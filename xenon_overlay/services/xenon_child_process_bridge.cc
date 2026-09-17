// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_child_process_bridge.h"

#include <algorithm>
#include <array>
#include <csignal>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/check.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/time/time.h"

namespace xenon {
namespace {

base::DictValue Success() {
  return base::DictValue().Set("ok", true);
}

base::DictValue Failure(int status) {
  return base::DictValue()
      .Set("ok", false)
      .Set("code", uv_err_name(status))
      .Set("errno", status)
      .Set("message", uv_strerror(status));
}

bool ValidString(const std::string& value) {
  return value.find('\0') == std::string::npos;
}

struct SignalMapping {
  const char* name;
  int number;
};

// Signal numbers are platform ABI values (for example SIGABRT differs on
// Windows). Keep their translation beside libuv, never in shared JavaScript.
constexpr SignalMapping kSignals[] = {
    {"SIGINT", SIGINT}, {"SIGILL", SIGILL}, {"SIGABRT", SIGABRT},
    {"SIGFPE", SIGFPE}, {"SIGSEGV", SIGSEGV}, {"SIGTERM", SIGTERM},
    {"SIGHUP", SIGHUP}, {"SIGKILL", SIGKILL},
#ifdef SIGQUIT
    {"SIGQUIT", SIGQUIT},
#endif
#ifdef SIGTRAP
    {"SIGTRAP", SIGTRAP},
#endif
#ifdef SIGBUS
    {"SIGBUS", SIGBUS},
#endif
#ifdef SIGUSR1
    {"SIGUSR1", SIGUSR1},
#endif
#ifdef SIGUSR2
    {"SIGUSR2", SIGUSR2},
#endif
#ifdef SIGPIPE
    {"SIGPIPE", SIGPIPE},
#endif
#ifdef SIGALRM
    {"SIGALRM", SIGALRM},
#endif
#ifdef SIGCHLD
    {"SIGCHLD", SIGCHLD},
#endif
#ifdef SIGCONT
    {"SIGCONT", SIGCONT},
#endif
#ifdef SIGSTOP
    {"SIGSTOP", SIGSTOP},
#endif
#ifdef SIGTSTP
    {"SIGTSTP", SIGTSTP},
#endif
#ifdef SIGTTIN
    {"SIGTTIN", SIGTTIN},
#endif
#ifdef SIGTTOU
    {"SIGTTOU", SIGTTOU},
#endif
#ifdef SIGURG
    {"SIGURG", SIGURG},
#endif
#ifdef SIGXCPU
    {"SIGXCPU", SIGXCPU},
#endif
#ifdef SIGXFSZ
    {"SIGXFSZ", SIGXFSZ},
#endif
#ifdef SIGVTALRM
    {"SIGVTALRM", SIGVTALRM},
#endif
#ifdef SIGPROF
    {"SIGPROF", SIGPROF},
#endif
#ifdef SIGWINCH
    {"SIGWINCH", SIGWINCH},
#endif
#ifdef SIGIO
    {"SIGIO", SIGIO},
#endif
#ifdef SIGPWR
    {"SIGPWR", SIGPWR},
#endif
#ifdef SIGSYS
    {"SIGSYS", SIGSYS},
#endif
#ifdef SIGBREAK
    {"SIGBREAK", SIGBREAK},
#endif
};

int SignalNumber(std::string_view name) {
  for (const auto& mapping : kSignals) {
    if (mapping.name == name) {
      return mapping.number;
    }
  }
  return -1;
}

const char* SignalName(int number) {
  if (!number) {
    return "";
  }
  for (const auto& mapping : kSignals) {
    if (mapping.number == number) {
      return mapping.name;
    }
  }
  return "UNKNOWN";
}

struct WriteRequest {
  uv_write_t handle{};
  std::string bytes;
  base::OnceCallback<void(int)> completion;
};

struct ShutdownRequest {
  uv_shutdown_t handle{};
  base::OnceCallback<void(int)> completion;
};

}  // namespace

struct XenonChildProcessBridge::Pipe {
  raw_ptr<Child> child;
  int fd = 0;
  bool initialized = false;
  uv_pipe_t handle{};
};

struct XenonChildProcessBridge::Child {
  raw_ptr<XenonChildProcessBridge> owner;
  Key key;
  uv_process_t process{};
  std::array<Pipe, 3> pipes;
  int handles = 0;
  bool spawned = false;
  bool exited = false;
  bool suppressed = false;
  int64_t exit_status = 0;
  int signal = 0;
};

XenonChildProcessBridge::XenonChildProcessBridge(EventCallback callback)
    : event_callback_(std::move(callback)) {
  CHECK_EQ(0, uv_loop_init(&loop_));
  // The Chromium task loop owns service liveness. Keep uv_run polling even if
  // an application unref()s the only process in this private libuv loop.
  CHECK_EQ(0, uv_async_init(&loop_, &pump_handle_, [](uv_async_t*) {}));
}

XenonChildProcessBridge::~XenonChildProcessBridge() {
  shutting_down_ = true;
  loop_timer_.Stop();
  for (auto& [key, child] : children_) {
    Terminate(child.get());
  }
  // SIGKILL is unconditional. Drain process reaping and pending pipe closes
  // before destroying their backing storage, including unref'ed children.
  while (!children_.empty()) {
    uv_run(&loop_, UV_RUN_ONCE);
  }
  uv_close(reinterpret_cast<uv_handle_t*>(&pump_handle_), nullptr);
  uv_run(&loop_, UV_RUN_NOWAIT);
  CHECK_EQ(0, uv_loop_close(&loop_));
}

void XenonChildProcessBridge::PumpLoop() {
  uv_run(&loop_, UV_RUN_NOWAIT);
  // Failed spawns and exited children remain in the map until all native
  // handles finish closing. Stop polling only after those callbacks drain.
  if (children_.empty()) {
    loop_timer_.Stop();
  }
}

void XenonChildProcessBridge::Dispatch(Child* child, base::DictValue event) {
  if (shutting_down_ || child->suppressed) {
    return;
  }
  event.Set("id", std::get<2>(child->key));
  event_callback_.Run(std::get<0>(child->key), std::get<1>(child->key),
                      base::Value(std::move(event)));
}

void XenonChildProcessBridge::HandleClosed(Child* child) {
  CHECK_GT(child->handles, 0);
  if (--child->handles != 0) {
    return;
  }
  Dispatch(child, base::DictValue()
                      .Set("event", "close")
                      .Set("exitCode", static_cast<double>(child->exit_status))
                      .Set("signalName", SignalName(child->signal))
                      .Set("signal", child->signal));
  children_.erase(child->key);
}

// static
void XenonChildProcessBridge::OnPipeClosed(uv_handle_t* handle) {
  auto* pipe = static_cast<Pipe*>(handle->data);
  pipe->child->owner->HandleClosed(pipe->child);
}

// static
void XenonChildProcessBridge::OnProcessClosed(uv_handle_t* handle) {
  auto* child = static_cast<Child*>(handle->data);
  child->owner->HandleClosed(child);
}

// static
void XenonChildProcessBridge::ClosePipe(Pipe* pipe) {
  if (!pipe->initialized) {
    return;
  }
  auto* handle = reinterpret_cast<uv_handle_t*>(&pipe->handle);
  if (!uv_is_closing(handle)) {
    uv_read_stop(reinterpret_cast<uv_stream_t*>(&pipe->handle));
    uv_close(handle, &OnPipeClosed);
  }
}

// static
void XenonChildProcessBridge::AllocateReadBuffer(uv_handle_t*,
                                                size_t suggested_size,
                                                uv_buf_t* buffer) {
  const size_t size = std::clamp<size_t>(suggested_size, 4096, 65536);
  buffer->base = new char[size];
  buffer->len = static_cast<decltype(buffer->len)>(size);
}

// static
void XenonChildProcessBridge::OnRead(uv_stream_t* stream,
                                    ssize_t count,
                                    const uv_buf_t* buffer) {
  std::unique_ptr<char[]> storage(buffer->base);
  auto* pipe = static_cast<Pipe*>(stream->data);
  auto* child = pipe->child.get();
  if (count > 0) {
    child->owner->Dispatch(
        child, base::DictValue()
                   .Set("event", "data")
                   .Set("fd", pipe->fd)
                   .Set("data", base::Base64Encode(std::string_view(
                                    buffer->base, static_cast<size_t>(count)))));
  } else if (count < 0) {
    auto event = base::DictValue().Set("event", "end").Set("fd", pipe->fd);
    if (count != UV_EOF) {
      event.Set("code", uv_err_name(static_cast<int>(count)));
    }
    child->owner->Dispatch(child, std::move(event));
    ClosePipe(pipe);
  }
}

// static
void XenonChildProcessBridge::OnExit(uv_process_t* process,
                                    int64_t exit_status,
                                    int signal) {
  auto* child = static_cast<Child*>(process->data);
  child->exited = true;
  child->exit_status = exit_status;
  child->signal = signal;
  child->owner->Dispatch(
      child, base::DictValue()
                 .Set("event", "exit")
                 .Set("exitCode", static_cast<double>(exit_status))
                 .Set("signalName", SignalName(signal))
                 .Set("signal", signal));
  ClosePipe(&child->pipes[0]);
  uv_close(reinterpret_cast<uv_handle_t*>(process), &OnProcessClosed);
}

void XenonChildProcessBridge::Terminate(Child* child) {
  child->suppressed = true;
  if (child->spawned && !child->exited) {
    uv_ref(reinterpret_cast<uv_handle_t*>(&child->process));
    uv_process_kill(&child->process, SIGKILL);
  }
  for (auto& pipe : child->pipes) {
    ClosePipe(&pipe);
  }
}

void XenonChildProcessBridge::RemoveEndpoint(const std::string& container_id,
                                            const std::string& endpoint_id) {
  for (auto& [key, child] : children_) {
    if (std::get<0>(key) == container_id && std::get<1>(key) == endpoint_id) {
      Terminate(child.get());
    }
  }
}

base::DictValue XenonChildProcessBridge::Spawn(
    const Key& key, const base::DictValue& request) {
  if (children_.contains(key)) {
    return Failure(UV_EEXIST);
  }
  const auto* file = request.FindString("file");
  const auto* args = request.FindList("args");
  const auto* stdio = request.FindList("stdio");
  if (!file || file->empty() || !ValidString(*file) || !args || !stdio ||
      stdio->size() != 3) {
    return Failure(UV_EINVAL);
  }
  const auto* cwd = request.FindString("cwd");
  if (cwd && !ValidString(*cwd)) {
    return Failure(UV_EINVAL);
  }
  const auto* argv0 = request.FindString("argv0");
  if (argv0 && !ValidString(*argv0)) {
    return Failure(UV_EINVAL);
  }
  std::vector<std::string> argument_storage{argv0 ? *argv0 : *file};
  for (const auto& argument : *args) {
    if (!argument.is_string() || !ValidString(argument.GetString())) {
      return Failure(UV_EINVAL);
    }
    argument_storage.push_back(argument.GetString());
  }
  std::vector<char*> arguments;
  for (auto& argument : argument_storage) {
    arguments.push_back(argument.data());
  }
  arguments.push_back(nullptr);
  std::vector<std::string> environment_storage;
  std::vector<char*> environment;
  const base::DictValue* env = request.FindDict("env");
  if (request.contains("env") && !env) {
    return Failure(UV_EINVAL);
  }
  if (env) {
    for (auto [name, value] : *env) {
      if (name.empty() || name.find('=') != std::string::npos ||
          !ValidString(name) || !value.is_string() ||
          !ValidString(value.GetString())) {
        return Failure(UV_EINVAL);
      }
      environment_storage.push_back(name + "=" + value.GetString());
    }
    for (auto& entry : environment_storage) {
      environment.push_back(entry.data());
    }
    environment.push_back(nullptr);
  }
  for (const auto& mode : *stdio) {
    if (!mode.is_string() || (mode.GetString() != "pipe" &&
                             mode.GetString() != "ignore" &&
                             mode.GetString() != "inherit")) {
      return Failure(UV_EINVAL);
    }
  }

  auto storage = std::make_unique<Child>();
  Child* child = storage.get();
  child->owner = this;
  child->key = key;
  children_.emplace(key, std::move(storage));
  if (!loop_timer_.IsRunning()) {
    loop_timer_.Start(FROM_HERE, base::Milliseconds(5),
                      base::BindRepeating(&XenonChildProcessBridge::PumpLoop,
                                          base::Unretained(this)));
  }
  std::array<uv_stdio_container_t, 3> native_stdio{};
  int status = 0;
  for (int fd = 0; fd < 3; ++fd) {
    Pipe& pipe = child->pipes[fd];
    pipe.child = child;
    pipe.fd = fd;
    const auto& mode = (*stdio)[fd].GetString();
    if (mode == "ignore") {
      native_stdio[fd].flags = UV_IGNORE;
    } else if (mode == "inherit") {
      native_stdio[fd].flags = UV_INHERIT_FD;
      native_stdio[fd].data.fd = fd;
    } else {
      status = uv_pipe_init(&loop_, &pipe.handle, 0);
      if (status != 0) {
        break;
      }
      pipe.handle.data = &pipe;
      pipe.initialized = true;
      ++child->handles;
      native_stdio[fd].flags = static_cast<uv_stdio_flags>(
          UV_CREATE_PIPE | (fd == 0 ? UV_READABLE_PIPE : UV_WRITABLE_PIPE));
      native_stdio[fd].data.stream = reinterpret_cast<uv_stream_t*>(&pipe.handle);
    }
  }
  uv_process_options_t options{};
  options.file = file->c_str();
  options.args = arguments.data();
  options.env = env ? environment.data() : nullptr;
  options.cwd = cwd ? cwd->c_str() : nullptr;
  options.exit_cb = &OnExit;
  options.stdio = native_stdio.data();
  options.stdio_count = 3;
  // Electron enables Node's kHideConsoleWindows for hosted environments.
  // Keep this separate from windowsHide: suppressing a console must not hide
  // a child application's GUI. libuv ignores this flag on non-Windows hosts.
  options.flags |= UV_PROCESS_WINDOWS_HIDE_CONSOLE;
  if (request.FindBool("windowsHide").value_or(false)) {
    options.flags |= UV_PROCESS_WINDOWS_HIDE;
  }
  if (!status) {
    status = uv_spawn(&loop_, &child->process, &options);
    if (child->process.type == UV_PROCESS) {
      child->process.data = child;
      ++child->handles;
    }
  }
  if (status) {
    child->suppressed = true;
    for (auto& pipe : child->pipes) {
      ClosePipe(&pipe);
    }
    if (child->process.type == UV_PROCESS) {
      uv_close(reinterpret_cast<uv_handle_t*>(&child->process), &OnProcessClosed);
    }
    if (!child->handles) {
      children_.erase(key);
    }
    return Failure(status);
  }
  child->spawned = true;
  // Reads start only when JS requests data. This preserves pipe backpressure
  // instead of buffering unbounded output in IPC while no consumer is reading.
  return Success().Set("pid", static_cast<int>(child->process.pid));
}

base::DictValue XenonChildProcessBridge::Call(
    const std::string& container_id,
    const std::string& endpoint_id,
    const base::DictValue& request) {
  const auto* id = request.FindString("id");
  const auto* operation = request.FindString("op");
  if (!id || id->empty() || !operation) {
    return Failure(UV_EINVAL);
  }
  const Key key(container_id, endpoint_id, *id);
  if (*operation == "spawn") {
    return Spawn(key, request);
  }
  auto found = children_.find(key);
  if (found == children_.end()) {
    return Failure(UV_ESRCH);
  }
  Child* child = found->second.get();
  if (*operation == "kill") {
    const auto* signal_name = request.FindString("signal");
    const int signal = signal_name ? SignalNumber(*signal_name)
                                  : request.FindInt("signal").value_or(-1);
    if (signal < 0 || signal >= NSIG) {
      return Failure(UV_EINVAL);
    }
    const int status = child->exited ? UV_ESRCH :
        uv_process_kill(&child->process, signal);
    return status ? Failure(status) : Success();
  }
  if (*operation == "ref" || *operation == "unref") {
    auto* handle = reinterpret_cast<uv_handle_t*>(&child->process);
    if (*operation == "ref") {
      uv_ref(handle);
    } else {
      uv_unref(handle);
    }
    return Success();
  }
  const auto fd = request.FindInt("fd");
  if (!fd || *fd < 0 || *fd > 2) {
    return Failure(UV_EINVAL);
  }
  Pipe& pipe = child->pipes[*fd];
  auto* stream = reinterpret_cast<uv_stream_t*>(&pipe.handle);
  if (!pipe.initialized ||
      uv_is_closing(reinterpret_cast<uv_handle_t*>(&pipe.handle))) {
    return Failure(UV_EPIPE);
  }
  int status = 0;
  if (*operation == "resume" && *fd != 0) {
    status = uv_read_start(stream, &AllocateReadBuffer, &OnRead);
    if (status == UV_EALREADY) {
      status = 0;
    }
  } else if (*operation == "pause" && *fd != 0) {
    status = uv_read_stop(stream);
  } else if (*operation == "destroy") {
    ClosePipe(&pipe);
  } else if (*operation == "write" && *fd == 0) {
    auto write = std::make_unique<WriteRequest>();
    const auto* data = request.FindString("data");
    const auto token = request.FindInt("token");
    if (!data || !token || !base::Base64Decode(*data, &write->bytes) ||
        write->bytes.size() > std::numeric_limits<unsigned int>::max()) {
      return Failure(UV_EINVAL);
    }
    write->completion = base::BindOnce(
        [](Child* child, int token, int status) {
          auto event = base::DictValue().Set("event", "write").Set("token", token);
          if (status) {
            event.Set("code", uv_err_name(status));
          }
          child->owner->Dispatch(child, std::move(event));
        }, base::Unretained(child), *token);
    write->handle.data = write.get();
    uv_buf_t buffer = uv_buf_init(write->bytes.data(),
                                  static_cast<unsigned int>(write->bytes.size()));
    status = uv_write(&write->handle, stream, &buffer, 1,
                       [](uv_write_t* handle, int status) {
                         std::unique_ptr<WriteRequest> write(
                             static_cast<WriteRequest*>(handle->data));
                         std::move(write->completion).Run(status);
                       });
    if (!status) {
      write.release();
    }
  } else if (*operation == "end" && *fd == 0) {
    auto shutdown = std::make_unique<ShutdownRequest>();
    shutdown->completion = base::BindOnce(
        [](Pipe* pipe, int status) {
          auto event = base::DictValue().Set("event", "finish");
          if (status) {
            event.Set("code", uv_err_name(status));
          }
          pipe->child->owner->Dispatch(pipe->child, std::move(event));
          ClosePipe(pipe);
        }, base::Unretained(&pipe));
    shutdown->handle.data = shutdown.get();
    status = uv_shutdown(&shutdown->handle, stream,
                          [](uv_shutdown_t* handle, int status) {
                            std::unique_ptr<ShutdownRequest> shutdown(
                                static_cast<ShutdownRequest*>(handle->data));
                            std::move(shutdown->completion).Run(status);
                          });
    if (!status) {
      shutdown.release();
    }
  } else {
    return Failure(UV_EINVAL);
  }
  return status ? Failure(status) : Success();
}

}  // namespace xenon
