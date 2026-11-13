// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SANDBOX_POLICY_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SANDBOX_POLICY_H_

#include <set>
#include <string>
#include <string_view>

#include "base/sequence_checker.h"
#include "base/types/expected.h"
#include "base/values.h"

namespace base {
class CommandLine;
}

namespace xenon::ipc {

// Browser UI sequence only. Owner is the authenticated application identity
// plus its service generation, never a value supplied by a renderer.
class ElectronSandboxPolicy {
 public:
  ElectronSandboxPolicy();
  ~ElectronSandboxPolicy();
  ElectronSandboxPolicy(const ElectronSandboxPolicy&) = delete;
  ElectronSandboxPolicy& operator=(const ElectronSandboxPolicy&) = delete;

  base::expected<void, std::string> Enable(
      const std::string& owner,
      const base::CommandLine& host_command_line);
  bool IsForced(const std::string& owner) const;
  void NoteDocument(const std::string& owner);
  void ReleaseOwner(const std::string& owner);

  // Authorized WebUI documents without an Electron WebContents retain their
  // existing policy unless their application explicitly enables the sandbox.
  base::expected<base::DictValue, std::string> NormalizePreferences(
      const std::string& owner,
      const base::DictValue& preferences,
      bool hosted) const;

  static base::expected<void, std::string> CheckHostSandboxCompatibility(
      const base::CommandLine& host_command_line);
  // Only Electron's supported sandboxed-renderer operations may cross the
  // built-in broker. Application-defined ipcMain channels are unaffected.
  static bool AllowsChannel(std::string_view channel,
                            const base::Value& arguments,
                            bool webview_enabled);

 private:
  std::set<std::string> forced_owners_;
  std::set<std::string> owners_with_documents_;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SANDBOX_POLICY_H_
