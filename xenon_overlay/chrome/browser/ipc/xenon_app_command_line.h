// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_COMMAND_LINE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_COMMAND_LINE_H_

#include <string>

#include "base/command_line.h"
#include "base/values.h"

namespace xenon::ipc {

// Native Chromium argument parsing, independently owned by one application.
// Mutating it never changes the command line of another application or host.
class ElectronAppCommandLine {
 public:
  explicit ElectronAppCommandLine(base::CommandLine command_line);
  ~ElectronAppCommandLine();
  void EnableSandbox();
  bool Call(const base::Value& request,
            base::Value* result,
            std::string* error);
  const base::DictValue& renderer_overrides() const {
    return renderer_overrides_;
  }
  const base::CommandLine& command_line() const { return command_line_; }

 private:
  base::CommandLine command_line_;
  base::DictValue renderer_overrides_;
};

// Browser UI sequence, authenticated owner supplied by XenonManager.
bool CallElectronAppCommandLine(const std::string& owner,
                                const base::Value& request,
                                base::Value* result,
                                std::string* error);
void CloseElectronAppCommandLine(const std::string& owner);
void EnableSandboxInElectronAppCommandLine(const std::string& owner);
void AppendElectronAppRendererCommandLine(const std::string& owner,
                                          base::CommandLine* command_line);

}  // namespace xenon::ipc
#endif
