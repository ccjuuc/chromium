// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_sandbox_policy.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "build/build_config.h"

namespace xenon::ipc {
namespace {

const base::DictValue* SingleRequest(const base::Value& arguments) {
  const auto* list = arguments.GetIfList();
  return list && list->size() == 1 ? list->front().GetIfDict() : nullptr;
}

}  // namespace

ElectronSandboxPolicy::ElectronSandboxPolicy() = default;
ElectronSandboxPolicy::~ElectronSandboxPolicy() = default;

base::expected<void, std::string> ElectronSandboxPolicy::Enable(
    const std::string& owner,
    const base::CommandLine& host_command_line) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (owner.empty()) {
    return base::unexpected(
        "ERR_ACCESS_DENIED: missing Electron sandbox owner");
  }
  auto supported = CheckHostSandboxCompatibility(host_command_line);
  if (!supported.has_value()) {
    return supported;
  }
  if (!IsForced(owner) && owners_with_documents_.contains(owner)) {
    return base::unexpected(
        "ERR_INVALID_STATE: app.enableSandbox must precede renderer creation");
  }
  forced_owners_.insert(owner);
  return {};
}

bool ElectronSandboxPolicy::IsForced(const std::string& owner) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return forced_owners_.contains(owner);
}

void ElectronSandboxPolicy::NoteDocument(const std::string& owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!owner.empty()) {
    owners_with_documents_.insert(owner);
  }
}

void ElectronSandboxPolicy::ReleaseOwner(const std::string& owner) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  forced_owners_.erase(owner);
  owners_with_documents_.erase(owner);
}

base::expected<base::DictValue, std::string>
ElectronSandboxPolicy::NormalizePreferences(const std::string& owner,
                                            const base::DictValue& preferences,
                                            bool hosted) const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto result = preferences.Clone();
  if (!hosted && !IsForced(owner)) {
    return result;
  }
  for (const char* key : {"sandbox", "contextIsolation", "nodeIntegration",
                          "nodeIntegrationInWorker",
                          "nodeIntegrationInSubFrames", "webviewTag"}) {
    if (const auto* value = preferences.Find(key); value && !value->is_bool()) {
      return base::unexpected(std::string("ERR_INVALID_ARG_TYPE: ") + key +
                              " must be a boolean");
    }
  }
  if (const auto* value = preferences.Find("preload")) {
    const auto* preload = value->GetIfString();
    if (!preload || preload->find('\0') != std::string::npos ||
        (!preload->empty() &&
         !base::FilePath::FromUTF8Unsafe(*preload).IsAbsolute())) {
      return base::unexpected(
          "ERR_INVALID_ARG_VALUE: preload must be an absolute file path");
    }
  }
  const bool node_integration =
      preferences.FindBool("nodeIntegration").value_or(false);
  const bool worker_integration =
      preferences.FindBool("nodeIntegrationInWorker").value_or(false);
  const bool sandbox =
      IsForced(owner) || preferences.FindBool("sandbox").value_or(
                             !node_integration && !worker_integration);
  result.Set("sandbox", sandbox);
  result.Set("contextIsolation",
             preferences.FindBool("contextIsolation").value_or(true));
  // Reflect effective capabilities, including an explicit/global sandbox
  // overriding requested Node integration. Subframe preloads may still run,
  // but receive the same restricted module set as the primary frame.
  result.Set("nodeIntegration", !sandbox && node_integration);
  result.Set("nodeIntegrationInWorker", !sandbox && worker_integration);
  return result;
}

base::expected<void, std::string>
ElectronSandboxPolicy::CheckHostSandboxCompatibility(
    const base::CommandLine& command_line) {
  // A hosted application's main module runs after the shared Browser has
  // started. It cannot undo a global sandbox bypass or replace existing
  // renderer processes on behalf of other applications.
  for (const char* flag : {"no-sandbox", "single-process",
                           "renderer-cmd-prefix", "renderer-startup-dialog"}) {
    if (command_line.HasSwitch(flag)) {
      return base::unexpected(
          std::string("ERR_NOT_SUPPORTED: Electron sandbox is incompatible ") +
          "with the running host's --" + flag);
    }
  }
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  // Disabling the setuid helper alone can still leave the namespace sandbox
  // active, and vice versa. The renderer checks its actual two-layer status.
  if (command_line.HasSwitch("disable-seccomp-filter-sandbox") ||
      (command_line.HasSwitch("disable-namespace-sandbox") &&
       command_line.HasSwitch("disable-setuid-sandbox"))) {
    return base::unexpected(
        "ERR_NOT_SUPPORTED: the running host disables a required Linux "
        "renderer sandbox layer");
  }
#endif
  return {};
}

bool ElectronSandboxPolicy::AllowsChannel(std::string_view channel,
                                          const base::Value& arguments,
                                          bool webview_enabled) {
  if (!channel.starts_with("__xenon:")) {
    return true;
  }
  if (channel == "__xenon:preload-error" ||
      channel == "__xenon:execute-javascript-result" ||
      channel == "__xenon:renderer-web-preferences" ||
      channel == "__xenon:send-to-host") {
    return true;
  }
  if (channel == "__xenon:guest-call") {
    return webview_enabled;
  }
  const auto* request = SingleRequest(arguments);
  if (!request) {
    return false;
  }
  if (channel == "__xenon:os") {
    const auto* method = request->FindString("method");
    // Electron exposes process.getSystemVersion in sandboxed preloads.
    return method && *method == "release";
  }
  if (channel == "__xenon:electron-api") {
    const auto* operation = request->FindString("operation");
    // These operations decode/copy caller-supplied image bytes. They never
    // read a path, open a shell, change app state or write to the clipboard.
    return operation && (*operation == "nativeImage.decode" ||
                         *operation == "nativeImage.toJPEG" ||
                         *operation == "nativeImage.toBitmap");
  }
  return false;
}

}  // namespace xenon::ipc
