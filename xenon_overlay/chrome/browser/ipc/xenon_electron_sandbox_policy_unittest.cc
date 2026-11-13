// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_sandbox_policy.h"

#include "base/command_line.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

using Policy = ElectronSandboxPolicy;

base::Value Request(const char* field, const char* value) {
  return base::Value(
      base::ListValue().Append(base::DictValue().Set(field, value)));
}

TEST(XenonElectronSandboxPolicyTest, ModernDefaultsAndExplicitPreferences) {
  Policy policy;
  auto default_window = policy.NormalizePreferences("app:1", {}, true);
  ASSERT_TRUE(default_window.has_value());
  EXPECT_EQ(default_window->FindBool("sandbox"), true);
  EXPECT_EQ(default_window->FindBool("contextIsolation"), true);
  EXPECT_EQ(default_window->FindBool("nodeIntegration"), false);
  for (const char* key : {"nodeIntegration", "nodeIntegrationInWorker"}) {
    base::DictValue requested;
    requested.Set(key, true);
    EXPECT_EQ(policy.NormalizePreferences("app:1", requested, true)
                  ->FindBool("sandbox"),
              false);
    requested.Set("sandbox", true);
    auto restricted = policy.NormalizePreferences("app:1", requested, true);
    EXPECT_EQ(restricted->FindBool("sandbox"), true);
    EXPECT_EQ(restricted->FindBool(key), false);
  }
  base::DictValue no_sandbox;
  no_sandbox.Set("sandbox", false);
  no_sandbox.Set("contextIsolation", false);
  auto unrestricted = policy.NormalizePreferences("app:1", no_sandbox, true);
  EXPECT_EQ(unrestricted->FindBool("sandbox"), false);
  EXPECT_EQ(unrestricted->FindBool("contextIsolation"), false);
}

TEST(XenonElectronSandboxPolicyTest, ForceIsScopedToAuthenticatedGeneration) {
  Policy policy;
  base::CommandLine host(base::CommandLine::NO_PROGRAM);
  ASSERT_TRUE(policy.Enable("app:1", host));
  base::DictValue requested;
  requested.Set("sandbox", false);
  requested.Set("nodeIntegration", true);
  requested.Set("nodeIntegrationInSubFrames", true);
  requested.Set("contextIsolation", false);
  auto forced = policy.NormalizePreferences("app:1", requested, true);
  EXPECT_EQ(forced->FindBool("sandbox"), true);
  EXPECT_EQ(forced->FindBool("nodeIntegration"), false);
  EXPECT_EQ(forced->FindBool("nodeIntegrationInSubFrames"), true);
  EXPECT_EQ(forced->FindBool("contextIsolation"), false);
  EXPECT_EQ(policy.NormalizePreferences("app:2", requested, true)
                ->FindBool("sandbox"),
            false);
  EXPECT_EQ(policy.NormalizePreferences("other:1", requested, true)
                ->FindBool("sandbox"),
            false);
  policy.ReleaseOwner("app:1");
  EXPECT_FALSE(policy.IsForced("app:1"));
}

TEST(XenonElectronSandboxPolicyTest, DoesNotRevokeAnAlreadyExposedRenderer) {
  Policy policy;
  base::CommandLine host(base::CommandLine::NO_PROGRAM);
  policy.NoteDocument("app:1");
  auto late = policy.Enable("app:1", host);
  ASSERT_FALSE(late.has_value());
  EXPECT_TRUE(late.error().starts_with("ERR_INVALID_STATE:"));
  ASSERT_TRUE(policy.Enable("app:2", host));
  policy.NoteDocument("app:2");
  EXPECT_TRUE(policy.Enable("app:2", host));
  policy.ReleaseOwner("app:1");
  EXPECT_TRUE(policy.Enable("app:1", host));
}

TEST(XenonElectronSandboxPolicyTest, CannotClaimSandboxUnderHostBypasses) {
  for (const char* flag : {"no-sandbox", "single-process",
                           "renderer-cmd-prefix", "renderer-startup-dialog"}) {
    Policy policy;
    base::CommandLine host(base::CommandLine::NO_PROGRAM);
    host.AppendSwitch(flag);
    auto enabled = policy.Enable("app:1", host);
    ASSERT_FALSE(enabled.has_value());
    EXPECT_TRUE(enabled.error().starts_with("ERR_NOT_SUPPORTED:"));
    EXPECT_FALSE(policy.IsForced("app:1"));
  }
#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  Policy policy;
  base::CommandLine host(base::CommandLine::NO_PROGRAM);
  host.AppendSwitch("disable-setuid-sandbox");
  EXPECT_TRUE(policy.Enable("namespace:1", host));
  host.AppendSwitch("disable-namespace-sandbox");
  EXPECT_FALSE(policy.Enable("no-layer-one:1", host));
  host.RemoveSwitch("disable-namespace-sandbox");
  host.AppendSwitch("disable-seccomp-filter-sandbox");
  EXPECT_FALSE(policy.Enable("no-layer-two:1", host));
#endif
}

TEST(XenonElectronSandboxPolicyTest,
     OrdinaryWebUiDoesNotBecomeAnElectronWindow) {
  Policy policy;
  EXPECT_TRUE(policy.NormalizePreferences("app:1", {}, false)->empty());
  base::CommandLine host(base::CommandLine::NO_PROGRAM);
  ASSERT_TRUE(policy.Enable("app:1", host));
  EXPECT_EQ(
      policy.NormalizePreferences("app:1", {}, false)->FindBool("sandbox"),
      true);
  EXPECT_FALSE(policy.NormalizePreferences(
      "app:1", base::DictValue().Set("sandbox", "true"), true));
}

TEST(XenonElectronSandboxPolicyTest,
     NativeBrokerRejectsReservedPrivilegeBypasses) {
  for (const char* channel :
       {"__xenon:fs", "__xenon:sqlite", "__xenon:net-request",
        "__xenon:child-process", "__xenon:crypto",
        "__xenon:node-addon:invoke-export", "__xenon:register-guest",
        "__xenon:invented"}) {
    EXPECT_FALSE(Policy::AllowsChannel(channel, base::Value(), true));
  }
  EXPECT_TRUE(
      Policy::AllowsChannel("application:read-file", base::Value(), false));
  EXPECT_TRUE(
      Policy::AllowsChannel("__xenon:preload-error", base::Value(), false));
  EXPECT_FALSE(
      Policy::AllowsChannel("__xenon:guest-call", base::Value(), false));
  EXPECT_TRUE(Policy::AllowsChannel("__xenon:guest-call", base::Value(), true));
  EXPECT_FALSE(Policy::AllowsChannel(
      "__xenon:electron-api", Request("operation", "shell.openPath"), false));
  EXPECT_FALSE(Policy::AllowsChannel("__xenon:electron-api",
                                     Request("operation", "clipboard.readText"),
                                     false));
  EXPECT_FALSE(Policy::AllowsChannel(
      "__xenon:electron-api", Request("operation", "nativeImage.readFile"),
      false));
  EXPECT_TRUE(Policy::AllowsChannel("__xenon:electron-api",
                                    Request("operation", "nativeImage.decode"),
                                    false));
  EXPECT_TRUE(
      Policy::AllowsChannel("__xenon:os", Request("method", "release"), false));
  EXPECT_FALSE(
      Policy::AllowsChannel("__xenon:os", Request("method", "homedir"), false));
  EXPECT_FALSE(Policy::AllowsChannel("__xenon:os", base::Value(), false));
}

TEST(XenonElectronSandboxPolicyTest, PreloadMustBeAnApprovedAbsoluteFilePath) {
  Policy policy;
  for (const auto& path : {std::string("relative/preload.js"),
                           std::string("file:///application/preload.js"),
                           std::string("/app/preload.js\0tail", 20)}) {
    auto result = policy.NormalizePreferences(
        "app:1", base::DictValue().Set("preload", path), true);
    ASSERT_FALSE(result.has_value());
    EXPECT_TRUE(result.error().starts_with("ERR_INVALID_ARG_VALUE:"));
  }
  EXPECT_TRUE(policy.NormalizePreferences(
      "app:1", base::DictValue().Set("preload", ""), true));
#if BUILDFLAG(IS_WIN)
  constexpr char kAbsolute[] = "C:\\application\\app.asar\\preload.js";
#else
  constexpr char kAbsolute[] = "/application/app.asar/preload.js";
#endif
  EXPECT_TRUE(policy.NormalizePreferences(
      "app:1", base::DictValue().Set("preload", kAbsolute), true));
}

}  // namespace
}  // namespace xenon::ipc
