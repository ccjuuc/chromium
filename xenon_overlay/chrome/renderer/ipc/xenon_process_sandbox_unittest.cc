// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/ipc/xenon_process_sandbox.h"

#include "base/command_line.h"
#include "base/test/scoped_command_line.h"
#include "sandbox/policy/sandbox.h"
#include "sandbox/policy/switches.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {

TEST(XenonProcessSandboxTest, UnsandboxedTestProcessIsNotReportedAsSandboxed) {
  base::test::ScopedCommandLine scoped_command_line;
  scoped_command_line.GetProcessCommandLine()->RemoveSwitch(
      sandbox::policy::switches::kNoSandbox);
  EXPECT_FALSE(HasActiveProcessSandbox());
}

TEST(XenonProcessSandboxTest, NoSandboxDoesNotInheritDcheckHelpersTrueResult) {
  base::test::ScopedCommandLine scoped_command_line;
  auto* command_line = scoped_command_line.GetProcessCommandLine();
  command_line->AppendSwitchASCII(sandbox::policy::switches::kProcessType,
                                  "renderer");
  command_line->AppendSwitch(sandbox::policy::switches::kNoSandbox);
  ASSERT_TRUE(sandbox::policy::Sandbox::IsProcessSandboxed());
  EXPECT_FALSE(HasActiveProcessSandbox());
}

}  // namespace xenon::ipc
