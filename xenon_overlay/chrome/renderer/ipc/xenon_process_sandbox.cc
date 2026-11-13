// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/ipc/xenon_process_sandbox.h"

#include "base/command_line.h"
#include "build/build_config.h"
#include "sandbox/policy/sandbox.h"
#include "sandbox/policy/switches.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/win/access_token.h"
#endif

namespace xenon::ipc {

bool HasActiveProcessSandbox() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          sandbox::policy::switches::kNoSandbox)) {
    return false;
  }
  if (sandbox::policy::Sandbox::IsProcessSandboxed()) {
    return true;
  }
#if BUILDFLAG(IS_WIN)
  // In the Windows executable/DLL build, the launcher initializes the
  // executable's TargetServices and passes it into the DLL. The DLL's local
  // SandboxFactory may have a different shared-section global. Query the
  // executable export, as ChromeMainDelegate does, then independently require
  // a restricted token below medium integrity. Neither preferences nor the
  // mere presence of a renderer command-line switch establishes a sandbox.
  using IsSandboxedProcessFunction = bool (*)();
  const auto is_sandboxed = reinterpret_cast<IsSandboxedProcessFunction>(
      ::GetProcAddress(::GetModuleHandle(nullptr), "IsSandboxedProcess"));
  if (!is_sandboxed || !is_sandboxed()) {
    return false;
  }
  const auto token = base::win::AccessToken::FromCurrentProcess();
  return token && token->IsRestricted() &&
         token->IntegrityLevel() < SECURITY_MANDATORY_MEDIUM_RID;
#else
  return false;
#endif
}

}  // namespace xenon::ipc
