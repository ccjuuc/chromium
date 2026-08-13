// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_inference_client.h"

#include <stddef.h>
#include <stdint.h>

#include <memory>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_file.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "base/values.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/win/scoped_handle.h"
#else
#include <unistd.h>
#endif

namespace xenon {

namespace {

constexpr std::string_view kLogTag = "[xenon_ai_inferencer] ";

base::FilePath ResolveInferencerExe(const base::FilePath& exe_dir) {
#if BUILDFLAG(IS_WIN)
  const base::FilePath candidate =
      exe_dir.Append(FILE_PATH_LITERAL("xenon_ai_inferencer_burn.exe"));
#else
  const base::FilePath candidate =
      exe_dir.Append(FILE_PATH_LITERAL("xenon_ai_inferencer_burn"));
#endif
  if (base::PathExists(candidate)) {
    return candidate;
  }
  return {};
}

bool ParseSidecarOutput(const std::string& output,
                        std::string* combined,
                        bool* success,
                        std::string* parse_error) {
  *success = false;
  combined->clear();
  std::vector<std::string> lines = base::SplitString(
      output, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  constexpr std::string_view kDeltaPrefix = "DELTA ";
  constexpr std::string_view kDonePrefix = "DONE ";
  bool saw_done = false;
  for (const std::string& line : lines) {
    if (base::StartsWith(line, kDeltaPrefix)) {
      combined->append(line.substr(kDeltaPrefix.size()));
    } else if (base::StartsWith(line, kDonePrefix)) {
      saw_done = true;
      int code = 0;
      std::string code_str = line.substr(kDonePrefix.size());
      base::TrimWhitespaceASCII(code_str, base::TRIM_ALL, &code_str);
      if (!base::StringToInt(code_str, &code)) {
        *parse_error = "invalid DONE line";
        return false;
      }
      *success = (code == 0);
    }
  }
  if (!saw_done) {
    *parse_error = "missing DONE line";
    return false;
  }
  return true;
}

// Long-lived `xenon_ai_inferencer_burn --stdio-server`: model stays loaded;
// one JSON object per line on stdin; responses are DELTA*/DONE on stdout.
class BurnInferencerHost {
 public:
  explicit BurnInferencerHost(base::FilePath exe) : exe_(std::move(exe)) {}
  BurnInferencerHost(const BurnInferencerHost&) = delete;
  BurnInferencerHost& operator=(const BurnInferencerHost&) = delete;

  ~BurnInferencerHost() { Shutdown(); }

  bool EnsureStarted(std::string* error_message) {
    // Do not gate reuse on Process::IsRunning() — on Windows it has been
    // unreliable with some child lifecycles and caused unnecessary Shutdown()
    // (killing a healthy inferencer) before the second request. Broken pipes or
    // EOF on the next read/write surface a dead child.
    const bool proc_ok = process_.IsValid();
    const bool stdin_ok = child_stdin_write_.IsValid();
    const bool stdout_ok = child_stdout_read_.IsValid();
    if (proc_ok && stdin_ok && stdout_ok) {
      VLOG(1) << kLogTag << "reuse existing stdio-server pid="
              << process_.Pid();
      return true;
    }
    VLOG(1) << kLogTag << "relaunch stdio-server: proc_ok=" << proc_ok
            << " stdin_ok=" << stdin_ok << " stdout_ok=" << stdout_ok;
    Shutdown();

    VLOG(1) << kLogTag << "starting stdio-server exe=" << exe_.AsUTF8Unsafe();

#if BUILDFLAG(IS_WIN)
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = nullptr;
    sa.bInheritHandle = TRUE;
    HANDLE stdin_read = nullptr;
    HANDLE stdin_write = nullptr;
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    if (!::CreatePipe(&stdin_read, &stdin_write, &sa, 0) ||
        !::CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
      *error_message = "CreatePipe failed for inferencer";
      PLOG(WARNING) << kLogTag << *error_message;
      return false;
    }

    // GUI Chrome often has NULL stderr; STARTF_USESTDHANDLES still needs a valid
    // writable handle or Rust dies on first eprintln!(). Console stderr handles
    // may also fail HANDLE_FLAG_INHERIT — always use an inheritable NUL.
    HANDLE stderr_for_child = ::CreateFileW(
        L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
        OPEN_EXISTING, 0, nullptr);
    if (stderr_for_child == INVALID_HANDLE_VALUE) {
      *error_message = "failed to open NUL for inferencer stderr";
      PLOG(WARNING) << kLogTag << *error_message;
      ::CloseHandle(stdin_read);
      ::CloseHandle(stdin_write);
      ::CloseHandle(stdout_read);
      ::CloseHandle(stdout_write);
      return false;
    }
    base::win::ScopedHandle child_stderr_closer(stderr_for_child);

    base::LaunchOptions options;
    options.start_hidden = true;
    options.stdin_handle = stdin_read;
    options.stdout_handle = stdout_write;
    options.stderr_handle = stderr_for_child;
    options.handles_to_inherit.push_back(stdin_read);
    options.handles_to_inherit.push_back(stdout_write);
    options.handles_to_inherit.push_back(stderr_for_child);

    base::CommandLine cmd(exe_);
    cmd.AppendSwitch("stdio-server");
    process_ = base::LaunchProcess(cmd, options);

    ::CloseHandle(stdin_read);
    ::CloseHandle(stdout_write);

    if (!process_.IsValid()) {
      ::CloseHandle(stdin_write);
      ::CloseHandle(stdout_read);
      *error_message = "failed to launch inferencer --stdio-server";
      LOG(WARNING) << kLogTag << *error_message << " exe=" << exe_.AsUTF8Unsafe();
      return false;
    }

    VLOG(1) << kLogTag << "launched stdio-server pid=" << process_.Pid()
            << " waiting READY...";

    child_stdin_write_ = base::File(stdin_write);
    child_stdout_read_ = base::File(stdout_read);
#else
    base::ScopedFD child_stdin_read_fd;
    base::ScopedFD parent_stdin_write_fd;
    base::ScopedFD parent_stdout_read_fd;
    base::ScopedFD child_stdout_write_fd;
    if (!base::CreatePipe(&child_stdin_read_fd, &parent_stdin_write_fd,
                          false) ||
        !base::CreatePipe(&parent_stdout_read_fd, &child_stdout_write_fd,
                          false)) {
      *error_message = "CreatePipe failed for inferencer";
      PLOG(WARNING) << kLogTag << *error_message;
      return false;
    }

    base::LaunchOptions options;
    options.fds_to_remap.emplace_back(child_stdin_read_fd.get(),
                                      STDIN_FILENO);
    options.fds_to_remap.emplace_back(child_stdout_write_fd.get(),
                                        STDOUT_FILENO);

    base::CommandLine cmd(exe_);
    cmd.AppendSwitch("stdio-server");
    process_ = base::LaunchProcess(cmd, options);

    child_stdin_read_fd.reset();
    child_stdout_write_fd.reset();

    if (!process_.IsValid()) {
      *error_message = "failed to launch inferencer --stdio-server";
      LOG(WARNING) << kLogTag << *error_message << " exe=" << exe_.AsUTF8Unsafe();
      return false;
    }

    VLOG(1) << kLogTag << "launched stdio-server pid=" << process_.Pid()
            << " waiting READY...";

    child_stdin_write_ = base::File(std::move(parent_stdin_write_fd));
    child_stdout_read_ = base::File(std::move(parent_stdout_read_fd));
#endif

    // llama-burn / dependencies may print progress to stdout (e.g. "Loading
    // record...") before our READY line; skip preamble until READY.
    constexpr int kMaxStdoutLinesBeforeReady = 65536;
    std::string ready_line;
    for (int preamble = 0;; ++preamble) {
      if (preamble >= kMaxStdoutLinesBeforeReady) {
        *error_message =
            "inferencer stdout: no READY line within preamble limit (malformed "
            "or stuck loader)";
        LOG(WARNING) << kLogTag << *error_message;
        Shutdown();
        return false;
      }
      if (!ReadLine(&ready_line, error_message)) {
        LOG(WARNING) << kLogTag << "while waiting READY: " << *error_message;
        Shutdown();
        return false;
      }
      base::TrimWhitespaceASCII(ready_line, base::TRIM_ALL, &ready_line);
      if (ready_line == "READY") {
        break;
      }
      VLOG(1) << kLogTag << "skip stdout before READY (" << preamble
              << "): " << ready_line.substr(0, 200);
    }
    VLOG(1) << kLogTag << "READY ok";
    return true;
  }

  void Shutdown() {
    VLOG(1) << kLogTag << "Shutdown()";
    child_stdin_write_.Close();
    child_stdout_read_.Close();
    if (process_.IsValid()) {
      process_.Terminate(1, /*wait=*/false);
    }
    process_.Close();
  }

  bool RunOneRequest(const base::FilePath& input_path,
                     const std::string& instruction_utf8,
                     std::string* combined_output,
                     std::string* error_message) {
    base::DictValue request =
        base::DictValue()
            .Set("input", input_path.AsUTF8Unsafe())
            .Set("instruction", instruction_utf8)
            .Set("task", "rewrite");
    auto json = base::WriteJson(request);
    if (!json.has_value()) {
      *error_message = "failed to serialize inferencer request";
      return false;
    }
    json->push_back('\n');
    VLOG(1) << kLogTag << "RunOneRequest json_bytes=" << json->size();

    if (!WriteAll(base::as_byte_span(*json), error_message)) {
      LOG(WARNING) << kLogTag << "stdin write: " << *error_message;
      return false;
    }
    if (!child_stdin_write_.Flush()) {
      VLOG(1) << kLogTag << "stdin Flush() failed (non-fatal)";
    }

    std::string stdout_batch;
    while (true) {
      std::string line;
      if (!ReadLine(&line, error_message)) {
        LOG(WARNING) << kLogTag << "read response line: " << *error_message;
        Shutdown();
        return false;
      }
      if (line.empty()) {
        continue;
      }
      stdout_batch.append(line);
      stdout_batch.push_back('\n');
      constexpr std::string_view kDone = "DONE ";
      if (base::StartsWith(line, kDone)) {
        break;
      }
    }

    bool ok = false;
    std::string parse_error;
    if (!ParseSidecarOutput(stdout_batch, combined_output, &ok, &parse_error)) {
      *error_message = parse_error;
      LOG(WARNING) << kLogTag << "parse: " << *error_message << " raw="
                     << stdout_batch.substr(0, 500);
      return false;
    }
    if (!ok) {
      *error_message = "inferencer returned failure DONE code";
      LOG(WARNING) << kLogTag << *error_message << " raw="
                     << stdout_batch.substr(0, 500);
      Shutdown();
      return false;
    }
    VLOG(1) << kLogTag << "RunOneRequest ok out_len=" << combined_output->size();
    return true;
  }

 private:
  bool ReadLine(std::string* line, std::string* error_message) {
    line->clear();
    while (true) {
      uint8_t c = 0;
      std::optional<size_t> n =
          child_stdout_read_.ReadAtCurrentPos(base::byte_span_from_ref(c));
      if (!n.has_value() || *n == 0) {
        *error_message = !n.has_value() ? "inferencer stdout read error"
                                        : "inferencer stdout closed";
        return false;
      }
      if (c == '\n') {
        while (!line->empty() && line->back() == '\r') {
          line->pop_back();
        }
        return true;
      }
      line->push_back(static_cast<char>(c));
    }
  }

  bool WriteAll(base::span<const uint8_t> data,
                std::string* error_message) {
    while (!data.empty()) {
      std::optional<size_t> n =
          child_stdin_write_.WriteAtCurrentPosNoBestEffort(data);
      if (!n.has_value() || *n == 0) {
        *error_message = "inferencer stdin write error";
        return false;
      }
      data = data.subspan(*n);
    }
    return true;
  }

  base::FilePath exe_;
  base::Process process_;
  base::File child_stdin_write_;
  base::File child_stdout_read_;
};

base::Lock& ResidentHostLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

std::unique_ptr<BurnInferencerHost>& ResidentHostInstance() {
  static base::NoDestructor<std::unique_ptr<BurnInferencerHost>> host;
  return *host;
}

}  // namespace

// static
bool XenonAiInferenceClient::RunRewrite(const std::string& instruction_utf8,
                                        const std::string& selection_utf8,
                                        std::string* combined_output,
                                        std::string* error_message) {
  VLOG(1) << kLogTag << "RunRewrite instruction_len=" << instruction_utf8.size()
          << " selection_len=" << selection_utf8.size();

  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    *error_message = "DIR_EXE unavailable";
    LOG(WARNING) << kLogTag << *error_message;
    return false;
  }
  base::FilePath exe = ResolveInferencerExe(exe_dir);
  if (exe.empty()) {
    *error_message =
        "no xenon_ai_inferencer_burn beside chrome.exe (build with "
        "build_xenon_ai_inferencer_burn and copy artifact to output dir)";
    LOG(WARNING) << kLogTag << *error_message
                 << " exe_dir=" << exe_dir.AsUTF8Unsafe();
    return false;
  }

  VLOG(1) << kLogTag << "exe=" << exe.AsUTF8Unsafe() << " mode=stdio-server";

  base::ScopedTempFile temp_file;
  if (!temp_file.Create()) {
    *error_message = "failed to create temp file for inferencer input";
    LOG(WARNING) << kLogTag << *error_message;
    return false;
  }
  if (!base::WriteFile(temp_file.path(), selection_utf8)) {
    *error_message = "failed to write inferencer input";
    LOG(WARNING) << kLogTag << *error_message;
    return false;
  }

  base::AutoLock guard(ResidentHostLock());
  std::unique_ptr<BurnInferencerHost>& host = ResidentHostInstance();
  if (!host) {
    host = std::make_unique<BurnInferencerHost>(exe);
  }
  if (!host->EnsureStarted(error_message)) {
    LOG(WARNING) << kLogTag << "EnsureStarted failed: " << *error_message;
    host.reset();
    return false;
  }
  if (!host->RunOneRequest(temp_file.path(), instruction_utf8, combined_output,
                           error_message)) {
    LOG(WARNING) << kLogTag << "RunOneRequest failed: " << *error_message;
    host.reset();
    return false;
  }
  VLOG(1) << kLogTag << "RunRewrite ok combined_len=" << combined_output->size();
  return true;
}

}  // namespace xenon
