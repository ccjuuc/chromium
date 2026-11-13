// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_utility_process_host.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/process/kill.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "content/public/browser/child_process_data.h"
#include "content/public/browser/child_process_termination_info.h"
#include "content/public/browser/service_process_info.h"
#include "xenon_overlay/public/utility_process_launch_options.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>

#include "base/posix/eintr_wrapper.h"
#endif

namespace xenon::ipc {
namespace {

base::File StandardFile(int fd, const std::string& mode, std::string* error) {
  if (mode == "inherit") {
#if BUILDFLAG(IS_WIN)
    const std::array<DWORD, 3> ids = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE,
                                      STD_ERROR_HANDLE};
    HANDLE handle = GetStdHandle(ids[fd]);
    HANDLE copy = INVALID_HANDLE_VALUE;
    if (handle && handle != INVALID_HANDLE_VALUE &&
        DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(), &copy,
                        0, FALSE, DUPLICATE_SAME_ACCESS)) {
      return base::File(copy);
    }
#else
    const int copy = HANDLE_EINTR(dup(fd));
    if (copy >= 0) {
      return base::File(copy);
    }
#endif
    // A desktop host commonly has no console. Inherited absent standard
    // streams are null devices, not invalid handles supplied to the launcher.
  }
#if BUILDFLAG(IS_WIN)
  const base::FilePath null_path(FILE_PATH_LITERAL("NUL"));
#else
  const base::FilePath null_path(FILE_PATH_LITERAL("/dev/null"));
#endif
  base::File file(null_path,
                  base::File::FLAG_OPEN | (fd == 0 ? base::File::FLAG_READ
                                                   : base::File::FLAG_WRITE));
  if (!file.IsValid()) {
    *error = "ERR_FAILED: Cannot open utility standard stream";
  }
  return file;
}

bool ValidText(const std::string& text) {
  return text.find('\0') == std::string::npos;
}

}  // namespace

struct XenonUtilityProcessHost::PreparedLaunch {
  std::unique_ptr<UtilityProcessLaunchOptions> launch =
      std::make_unique<UtilityProcessLaunchOptions>();
  mojom::UtilityProcessBootstrapPtr bootstrap =
      mojom::UtilityProcessBootstrap::New();
  std::array<scoped_refptr<UtilityProcessPipe>, 2> output;
  std::string service_name;
  std::string error;
};

XenonUtilityProcessHost::XenonUtilityProcessHost(
    mojo::PendingReceiver<mojom::UtilityProcessControl> control,
    mojo::PendingRemote<mojom::UtilityProcessClient> client,
    ElectronNetEngine::FactoryProvider network_factory,
    ElectronNetEngine::OnlineProvider network_online,
    base::OnceClosure disconnected)
    : pending_control_(std::move(control)),
      client_(std::move(client)),
      network_factory_(std::move(network_factory)),
      network_online_(std::move(network_online)),
      disconnected_(std::move(disconnected)) {
  content::BrowserChildProcessObserver::Add(this);
  client_.set_disconnect_handler(
      base::BindOnce(&XenonUtilityProcessHost::OnParentDisconnected,
                     weak_factory_.GetWeakPtr()));
}

XenonUtilityProcessHost::~XenonUtilityProcessHost() {
  content::BrowserChildProcessObserver::Remove(this);
  content::ServiceProcessHost::ClearInstanceObserver(this);
  for (const auto& pipe : output_) {
    if (pipe) {
      pipe->Cancel();
    }
  }
  // The owning application is gone. No utility may outlive that generation.
  // Normal exits run native cleanup in the child before closing its service.
  if (!finished_ && process_.IsValid()) {
    process_.Terminate(1, false);
  }
}

void XenonUtilityProcessHost::Start(
    mojom::IpcMainConfigPtr application,
    mojom::UtilityProcessOptionsPtr options,
    blink::MessagePortDescriptor parent_port,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> network_factory,
    StartCallback callback) {
  start_callback_ = std::move(callback);
  if (!base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce(&XenonUtilityProcessHost::Prepare,
                         std::move(application), std::move(options),
                         std::move(parent_port), std::move(network_factory)),
          base::BindOnce(&XenonUtilityProcessHost::Launch,
                         weak_factory_.GetWeakPtr()))) {
    std::move(start_callback_)
        .Run(false, "ERR_ABORTED: Utility launcher is shutting down");
  }
}

std::unique_ptr<XenonUtilityProcessHost::PreparedLaunch>
XenonUtilityProcessHost::Prepare(
    mojom::IpcMainConfigPtr application,
    mojom::UtilityProcessOptionsPtr options,
    blink::MessagePortDescriptor parent_port,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> network_factory) {
  auto result = std::make_unique<PreparedLaunch>();
  auto& error = result->error;
  const base::FilePath module =
      base::FilePath::FromUTF8Unsafe(options->module_path);
  const base::FilePath cwd =
      base::FilePath::FromUTF8Unsafe(options->working_directory);
  if (!parent_port.IsValid() || !module.IsAbsolute() ||
      module.ReferencesParent() || !cwd.IsAbsolute() ||
      !ValidText(options->module_path) ||
      !ValidText(options->working_directory) || !base::DirectoryExists(cwd)) {
    error = "ERR_INVALID_ARG_VALUE: Invalid utility module, cwd or parent port";
    return result;
  }
  if (!options->exec_argv.empty() ||
      options->allow_loading_unsigned_libraries ||
      options->respond_to_auth_requests_from_main_process) {
    error =
        "ERR_NOT_SUPPORTED: Utility execArgv, unsigned libraries and "
        "main-process authentication are unavailable";
    return result;
  }
  if (!ValidText(options->service_name) ||
      options->service_name.size() > 1024 || options->args.size() > 4096 ||
      options->environment.size() > 4096) {
    error = "ERR_INVALID_ARG_VALUE: Invalid utility launch arguments";
    return result;
  }
  size_t bytes = 0;
  for (const auto& argument : options->args) {
    bytes += argument.size();
    if (!ValidText(argument) || bytes > 1024 * 1024) {
      error =
          "ERR_INVALID_ARG_VALUE: Utility argument contains NUL or exceeds "
          "limit";
      return result;
    }
  }
  for (const auto& [key, value] : options->environment) {
    bytes += key.size() + value.size();
    if (bytes > 1024 * 1024) {
      error = "ERR_OUT_OF_RANGE: Utility environment exceeds limit";
      return result;
    }
#if BUILDFLAG(IS_WIN)
    result->launch->environment.emplace(base::UTF8ToWide(key),
                                        base::UTF8ToWide(value));
#else
    result->launch->environment.emplace(key, value);
#endif
  }
  result->launch->working_directory = cwd;
  const std::array<std::string, 3> modes = {"ignore", options->stdout_mode,
                                            options->stderr_mode};
  for (int fd = 0; fd < 3; ++fd) {
    if (modes[fd] == "pipe" && fd > 0) {
      result->output[fd - 1] =
          UtilityProcessPipe::Create(&result->launch->stdio[fd], &error);
    } else if (modes[fd] == "inherit" || modes[fd] == "ignore") {
      result->launch->stdio[fd] = StandardFile(fd, modes[fd], &error);
    } else {
      error = "ERR_INVALID_ARG_VALUE: Unsupported utility stdio mode";
    }
    if (!error.empty()) {
      return result;
    }
  }
  if (!result->launch->Validate(&error)) {
    return result;
  }
  application->embedded_main_source.clear();
  application->virtual_main_path.clear();
  application->main_script_path = options->module_path;
  application->working_directory = options->working_directory;
  application->app_argv = {application->executable_path, options->module_path};
  application->app_argv.insert(application->app_argv.end(),
                               options->args.begin(), options->args.end());
  application->native_addon_execution = mojom::NativeAddonExecution::kStandard;
  application->renderer_url_mappings.clear();
  application->renderer_base_url.clear();
  application->parent_window_pairing_urls.clear();
  result->bootstrap->argv = application->app_argv;
  result->bootstrap->exec_argv = options->exec_argv;
  result->bootstrap->environment = std::move(options->environment);
  result->bootstrap->working_directory = options->working_directory;
  result->bootstrap->parent_port = std::move(parent_port);
  result->bootstrap->main = std::move(application);
  result->bootstrap->network_factory = std::move(network_factory);
#if BUILDFLAG(IS_WIN)
  result->bootstrap->stdin_file = result->launch->stdio[0].Duplicate();
  result->bootstrap->stdout_file = result->launch->stdio[1].Duplicate();
  result->bootstrap->stderr_file = result->launch->stdio[2].Duplicate();
#endif
  result->service_name = options->service_name.empty() ? "Node Utility Process"
                                                       : options->service_name;
  return result;
}

void XenonUtilityProcessHost::Launch(std::unique_ptr<PreparedLaunch> prepared) {
  if (cancelled_ || finished_) {
    return;
  }
  // Leave reads queued while stdio is prepared. Applications may consume
  // stdout immediately after fork(), before the OS has launched the child.
  control_.Bind(std::move(pending_control_));
  control_.set_disconnect_handler(
      base::BindOnce(&XenonUtilityProcessHost::OnParentDisconnected,
                     weak_factory_.GetWeakPtr()));
  if (!prepared->error.empty()) {
    prepare_failed_ = true;
    std::move(start_callback_).Run(false, prepared->error);
    return;
  }
  output_ = std::move(prepared->output);
  service_ = content::ServiceProcessHost::Launch<mojom::UtilityProcessService>(
      content::ServiceProcessHost::Options()
          .WithDisplayName(base::UTF8ToUTF16(prepared->service_name))
          .WithExtraCommandLineSwitches({switches::kUtilityProcess})
          .WithObserver(weak_factory_.GetWeakPtr())
          .WithXenonUtilityProcessOptions(std::move(prepared->launch))
          .Pass());
  service_.set_disconnect_handler(
      base::BindOnce(&XenonUtilityProcessHost::OnServiceDisconnected,
                     weak_factory_.GetWeakPtr()));
  service_->Start(std::move(prepared->bootstrap),
                  observer_.BindNewPipeAndPassRemote(),
                  base::BindOnce(&XenonUtilityProcessHost::OnStarted,
                                 weak_factory_.GetWeakPtr()));
  startup_timeout_.Start(
      FROM_HERE, base::Seconds(30),
      base::BindOnce(
          [](base::WeakPtr<XenonUtilityProcessHost> self) {
            if (!self || self->finished_) {
              return;
            }
            self->client_->OnError(
                "ERR_TIMED_OUT: Utility initialization did not finish");
            if (self->process_.IsValid()) {
              self->process_.Terminate(1, false);
            }
            self->service_.reset();
          },
          weak_factory_.GetWeakPtr()));
  std::move(start_callback_).Run(true, "");
}

void XenonUtilityProcessHost::OnStarted(bool success,
                                        const std::string& error) {
  startup_timeout_.Stop();
  if (!success && !finished_) {
    client_->OnError(error.empty() ? "ERR_FAILED: Utility initialization failed"
                                   : error);
    if (service_.is_bound()) {
      service_->Shutdown(1);
    }
  }
}

void XenonUtilityProcessHost::OnServiceProcessLaunched(
    const content::ServiceProcessInfo& info) {
  process_ = info.GetProcess().Duplicate();
  if (cancelled_ || finished_) {
    process_.Terminate(1, false);
    return;
  }
  client_->OnSpawn(static_cast<int32_t>(process_.Pid()));
}

void XenonUtilityProcessHost::OnExitRequested(int32_t exit_code) {
  exit_requested_ = true;
  // This is intent, not evidence that the process has terminated.
}

void XenonUtilityProcessHost::CreateNetworkFactory(
    const std::string& partition,
    mojo::PendingRemote<mojom::ElectronNetworkHeadersObserver> headers,
    CreateNetworkFactoryCallback callback) {
  if (cancelled_ || finished_ || !network_factory_) {
    std::move(callback).Run({}, "ERR_ABORTED: Utility network owner stopped");
    return;
  }
  network_factory_.Run(
      partition, std::move(headers),
      base::BindOnce(
          [](CreateNetworkFactoryCallback done,
             mojo::PendingRemote<network::mojom::URLLoaderFactory> factory,
             std::string error) {
            std::move(done).Run(std::move(factory), error);
          },
          std::move(callback)));
}

void XenonUtilityProcessHost::GetNetworkOnline(
    GetNetworkOnlineCallback callback) {
  bool online = false;
  std::string error;
  if (cancelled_ || finished_ || !network_online_) {
    error = "ERR_ABORTED: Utility network owner stopped";
  } else if (!network_online_.Run(&online, &error) && error.empty()) {
    error = "ERR_FAILED: Utility network state unavailable";
  }
  std::move(callback).Run(online, error);
}

void XenonUtilityProcessHost::OnServiceDisconnected() {
  startup_timeout_.Stop();
  if (!process_.IsValid() && !finished_) {
    client_->OnError("ERR_FAILED: Utility process failed to launch");
    Finish(-1, "launch-failed");
  }
}

void XenonUtilityProcessHost::OnParentDisconnected() {
  cancelled_ = true;
  if (disconnected_) {
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, std::move(disconnected_));
  }
}

void XenonUtilityProcessHost::Kill(KillCallback callback) {
  const bool killed =
      !finished_ && process_.IsValid() && process_.Terminate(1, false);
  std::move(callback).Run(killed);
}

void XenonUtilityProcessHost::ReadOutput(int32_t fd,
                                         uint32_t maximum_bytes,
                                         ReadOutputCallback callback) {
  if (prepare_failed_ && (fd == 1 || fd == 2)) {
    // fork's error/exit events report the launch failure. No OS pipe existed,
    // so a queued standard-stream read observes EOF rather than a second error.
    std::move(callback).Run({}, true, "");
    return;
  }
  if (fd < 1 || fd > 2 || !output_[fd - 1]) {
    std::move(callback).Run(
        {}, false, "ERR_INVALID_ARG_VALUE: Utility stream is not piped");
    return;
  }
  auto callbacks = base::SplitOnceCallback(std::move(callback));
  if (!base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock()},
          base::BindOnce(&UtilityProcessPipe::Read, output_[fd - 1],
                         maximum_bytes),
          base::BindOnce(
              [](ReadOutputCallback done,
                 UtilityProcessPipe::ReadResult result) {
                std::move(done).Run(result.data, result.eof, result.error);
              },
              std::move(callbacks.first)))) {
    std::move(callbacks.second)
        .Run({}, false, "ERR_ABORTED: Utility reader is shutting down");
  }
}

void XenonUtilityProcessHost::CloseOutput(int32_t fd) {
  if (fd < 1 || fd > 2 || !output_[fd - 1]) {
    return;
  }
  output_[fd - 1]->Cancel();
  output_[fd - 1].reset();
}

void XenonUtilityProcessHost::ObserveExit(
    const content::ChildProcessData& data,
    const content::ChildProcessTerminationInfo& info,
    const char* reason) {
  if (!process_.IsValid() || !data.GetProcess().IsValid() ||
      data.GetProcess().Pid() != process_.Pid() || finished_) {
    return;
  }
  if (info.status == base::TERMINATION_STATUS_STILL_RUNNING) {
    // Disconnection can precede OS termination. Wait outside Browser UI; never
    // report the child's requested exit code as an observed successful exit.
    if (!base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE, {base::MayBlock(), base::WithBaseSyncPrimitives()},
            base::BindOnce(
                [](base::Process process) {
                  int code = -1;
                  const bool exited =
                      process.WaitForExitWithTimeout(base::Seconds(5), &code);
                  return std::make_pair(exited, code);
                },
                process_.Duplicate()),
            base::BindOnce(
                [](base::WeakPtr<XenonUtilityProcessHost> self,
                   std::pair<bool, int> result) {
                  if (!self || self->finished_) {
                    return;
                  }
                  if (result.first) {
                    self->Finish(result.second, "exited");
                  } else {
                    self->client_->OnError(
                        "ERR_TIMED_OUT: Utility OS exit has not been "
                        "confirmed");
                  }
                },
                weak_factory_.GetWeakPtr()))) {
      client_->OnError("ERR_ABORTED: Utility exit observer is shutting down");
    }
    return;
  }
  int code = info.exit_code;
#if BUILDFLAG(IS_POSIX)
  if (WIFEXITED(code)) {
    code = WEXITSTATUS(code);
  } else if (WIFSIGNALED(code)) {
    code = 128 + WTERMSIG(code);
  }
#endif
  Finish(code, reason);
}

void XenonUtilityProcessHost::Finish(int code, const std::string& reason) {
  if (finished_) {
    return;
  }
  finished_ = true;
  startup_timeout_.Stop();
  service_.reset();
  observer_.reset();
  client_->OnExit(code, reason);
  // Keep read endpoints available for draining buffered stdout/stderr. The
  // parent releases Control after EOF; process exit must not truncate output.
}

void XenonUtilityProcessHost::BrowserChildProcessCrashed(
    const content::ChildProcessData& data,
    const content::ChildProcessTerminationInfo& info) {
  ObserveExit(data, info, exit_requested_ ? "exited" : "crashed");
}
void XenonUtilityProcessHost::BrowserChildProcessKilled(
    const content::ChildProcessData& data,
    const content::ChildProcessTerminationInfo& info) {
  ObserveExit(data, info, "killed");
}
void XenonUtilityProcessHost::BrowserChildProcessExitedNormally(
    const content::ChildProcessData& data,
    const content::ChildProcessTerminationInfo& info) {
  ObserveExit(data, info, "exited");
}

}  // namespace xenon::ipc
