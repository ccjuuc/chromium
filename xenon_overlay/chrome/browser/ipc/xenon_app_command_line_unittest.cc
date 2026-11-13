// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_command_line.h"

#include "base/base64.h"
#include "base/files/file_path.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "xenon_overlay/common/ipc/xenon_renderer_command_line.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

namespace xenon::ipc {
namespace {
TEST(ElectronRendererIpcHintTest, OrdinaryDocumentsSkipConfigurationIpc) {
  base::CommandLine browser(base::CommandLine::NO_PROGRAM);
  browser.AppendSwitch(switches::kEnable);
  browser.AppendSwitchASCII(switches::kAllowedOrigins,
                            "https://trusted.test, http://localhost:8123");
  base::CommandLine renderer(base::CommandLine::NO_PROGRAM);
  AppendElectronIpcRendererHints(browser, false, &renderer);
  ElectronRendererIpcHint hint(renderer);
  for (const char* url :
       {"https://ordinary.test", "http://ordinary.test",
        "file:///tmp/index.html", "about:blank",
        "https://trusted.test.evil.test", "http://localhost:8124"}) {
    EXPECT_FALSE(hint.MayRequestConfig(url::Origin::Create(GURL(url)), true));
  }
  EXPECT_TRUE(hint.MayRequestConfig(
      url::Origin::Create(GURL("https://trusted.test/path")), true));
  EXPECT_TRUE(hint.MayRequestConfig(
      url::Origin::Create(GURL("http://localhost:8123")), true));
  EXPECT_FALSE(hint.MayRequestConfig(
      url::Origin::Create(GURL("https://trusted.test")), false));
}

TEST(ElectronRendererIpcHintTest, ApplicationPartitionsStillAskBrowser) {
  base::CommandLine browser(base::CommandLine::NO_PROGRAM);
  browser.AppendSwitch(switches::kEnable);
  base::CommandLine renderer(base::CommandLine::NO_PROGRAM);
  AppendElectronIpcRendererHints(browser, true, &renderer);
  ElectronRendererIpcHint hint(renderer);
  for (const char* url : {"http://app.test", "file:///tmp/app/index.html",
                          "app-resource://ui/main", "about:blank"}) {
    EXPECT_TRUE(hint.MayRequestConfig(url::Origin::Create(GURL(url)), true));
    EXPECT_TRUE(hint.MayRequestConfig(url::Origin::Create(GURL(url)), false));
  }
  // The hint cannot be changed by app.commandLine renderer overrides.
  EXPECT_FALSE(IsElectronRendererSwitch(switches::kRendererIpcCandidate));
  EXPECT_FALSE(IsElectronRendererSwitch(switches::kAllowedOrigins));
  EXPECT_FALSE(IsElectronRendererSwitch(switches::kAllowAllOrigins));
}

TEST(ElectronRendererIpcHintTest, DisabledHostClearsInheritedHints) {
  base::CommandLine browser(base::CommandLine::NO_PROGRAM);
  base::CommandLine renderer(base::CommandLine::NO_PROGRAM);
  renderer.AppendSwitch(switches::kEnable);
  renderer.AppendSwitch(switches::kRendererIpcCandidate);
  renderer.AppendSwitch(switches::kAllowAllOrigins);
  renderer.AppendSwitchASCII(switches::kAllowedOrigins, "https://trusted.test");
  AppendElectronIpcRendererHints(browser, true, &renderer);
  ElectronRendererIpcHint hint(renderer);
  EXPECT_FALSE(hint.MayRequestConfig(
      url::Origin::Create(GURL("https://trusted.test")), true));
  EXPECT_FALSE(renderer.HasSwitch(switches::kRendererIpcCandidate));
  EXPECT_FALSE(renderer.HasSwitch(switches::kAllowedOrigins));
}

TEST(ElectronRendererIpcHintTest, DevelopmentAllowAllStillRequiresMainFrame) {
  base::CommandLine browser(base::CommandLine::NO_PROGRAM);
  browser.AppendSwitch(switches::kEnable);
  browser.AppendSwitch(switches::kAllowAllOrigins);
  base::CommandLine renderer(base::CommandLine::NO_PROGRAM);
  AppendElectronIpcRendererHints(browser, false, &renderer);
  ElectronRendererIpcHint hint(renderer);
  EXPECT_TRUE(hint.MayRequestConfig(url::Origin(), true));
  EXPECT_FALSE(hint.MayRequestConfig(url::Origin(), false));
}

TEST(ElectronRendererIpcHintTest, InProcessRendererStillAsksBrowser) {
  base::CommandLine browser(base::CommandLine::NO_PROGRAM);
  browser.AppendSwitch("single-process");
  browser.AppendSwitch(switches::kEnable);
  ElectronRendererIpcHint hint(browser);
  EXPECT_TRUE(hint.MayRequestConfig(
      url::Origin::Create(GURL("file:///tmp/app/index.html")), true));
  EXPECT_TRUE(hint.MayRequestConfig(url::Origin(), false));
  browser.RemoveSwitch(switches::kEnable);
  EXPECT_FALSE(ElectronRendererIpcHint(browser).MayRequestConfig(
      url::Origin(), true));
}

base::Value Request(const std::string& method,
                    const std::string& name,
                    const std::string& value = "") {
  return base::Value(base::DictValue()
                         .Set("method", method)
                         .Set("name", name)
                         .Set("value", value));
}

TEST(ElectronAppCommandLineTest, MutationsAreNativeAndApplicationLocal) {
  base::CommandLine original(base::FilePath(FILE_PATH_LITERAL("host")));
  original.AppendSwitchASCII("remote-debugging-port", "9259");
  ElectronAppCommandLine first(original);
  ElectronAppCommandLine second(original);
  base::Value result;
  std::string error;
  ASSERT_TRUE(first.Call(Request("getSwitchValue", "REMOTE-DEBUGGING-PORT"),
                         &result, &error));
  EXPECT_EQ(result.GetString(), "9259");
  ASSERT_TRUE(first.Call(Request("appendSwitch", "Testing-Switch", "a b=中"),
                         &result, &error));
  ASSERT_TRUE(
      first.Call(Request("getSwitchValue", "testing-switch"), &result, &error));
  EXPECT_EQ(result.GetString(), "a b=中");
  ASSERT_TRUE(
      second.Call(Request("hasSwitch", "testing-switch"), &result, &error));
  EXPECT_FALSE(result.GetBool());
  EXPECT_FALSE(original.HasSwitch("testing-switch"));
  ASSERT_TRUE(
      first.Call(Request("removeSwitch", "TESTING-SWITCH"), &result, &error));
  ASSERT_TRUE(
      first.Call(Request("getSwitchValue", "testing-switch"), &result, &error));
  EXPECT_EQ(result.GetString(), "");
}

TEST(ElectronAppCommandLineTest, AppendArgumentDoesNotReparseAsSwitch) {
  ElectronAppCommandLine command{
      base::CommandLine(base::CommandLine::NO_PROGRAM)};
  base::Value result;
  std::string error;
  ASSERT_TRUE(command.Call(Request("appendArgument", "", "--not-a-switch=a b"),
                           &result, &error));
  EXPECT_FALSE(command.command_line().HasSwitch("not-a-switch"));
  ASSERT_EQ(command.command_line().GetArgs().size(), 1u);
  EXPECT_EQ(command.command_line().GetArgs()[0],
            FILE_PATH_LITERAL("--not-a-switch=a b"));
}

TEST(ElectronAppCommandLineTest, FailureDoesNotPartiallyChangeState) {
  ElectronAppCommandLine command{
      base::CommandLine(base::CommandLine::NO_PROGRAM)};
  base::Value result;
  std::string error;
  ASSERT_TRUE(
      command.Call(Request("appendSwitch", "lang", "en-US"), &result, &error));
  EXPECT_FALSE(
      command.Call(Request("appendSwitch", "lang", std::string(20000, 'x')),
                   &result, &error));
  EXPECT_TRUE(error.starts_with("ERR_OUT_OF_RANGE:"));
  EXPECT_FALSE(command.Call(Request("appendSwitch", "invalid=name", "x"),
                            &result, &error));
  EXPECT_FALSE(
      command.Call(Request("appendSwitch", "lang", std::string("x\0y", 3)),
                   &result, &error));
  EXPECT_EQ(command.command_line().GetSwitchValueASCII("lang"), "en-US");
  EXPECT_EQ(*command.renderer_overrides().FindString("lang"), "en-US");
}

TEST(ElectronAppCommandLineTest, RendererReceivesOverridesAfterHostDefaults) {
  ElectronAppCommandLine command{
      base::CommandLine(base::CommandLine::NO_PROGRAM)};
  base::Value result;
  std::string error;
  ASSERT_TRUE(command.Call(
      Request("appendSwitch", "disable-blink-features", "IdleDetection"),
      &result, &error));
  ASSERT_TRUE(
      command.Call(Request("appendSwitch", "lang", "zh-CN"), &result, &error));
  ASSERT_TRUE(
      command.Call(Request("removeSwitch", "js-flags"), &result, &error));
  // Browser/GPU/sandbox switches never enter the renderer override payload.
  EXPECT_FALSE(
      command.Call(Request("appendSwitch", "no-sandbox"), &result, &error));
  EXPECT_FALSE(command.Call(Request("appendSwitch", "type", "utility"), &result,
                            &error));
  base::CommandLine child(base::CommandLine::NO_PROGRAM);
  child.AppendSwitchASCII("type", "renderer");
  AppendElectronRendererOverrides(command.renderer_overrides(), &child);
  child.AppendSwitchASCII("lang", "en-US");
  child.AppendSwitchASCII("js-flags", "--expose-gc");
  ApplyElectronRendererOverrides(&child);
  EXPECT_EQ(child.GetSwitchValueASCII("lang"), "zh-CN");
  EXPECT_EQ(child.GetSwitchValueASCII("disable-blink-features"),
            "IdleDetection");
  EXPECT_EQ(child.GetSwitchValueASCII("type"), "renderer");
  EXPECT_FALSE(child.HasSwitch("no-sandbox"));
  EXPECT_FALSE(child.HasSwitch("js-flags"));
  EXPECT_FALSE(child.HasSwitch("xenon-electron-renderer-overrides"));
}

TEST(ElectronAppCommandLineTest, MalformedOrNonRendererPayloadIsNotApplied) {
  base::CommandLine child(base::CommandLine::NO_PROGRAM);
  child.AppendSwitchASCII("type", "renderer");
  child.AppendSwitchASCII("lang", "en-US");
  child.AppendSwitchASCII(
      "xenon-electron-renderer-overrides",
      base::Base64Encode(R"({"lang":"zh-CN","no-sandbox":""})"));
  ApplyElectronRendererOverrides(&child);
  EXPECT_EQ(child.GetSwitchValueASCII("lang"), "en-US");
  EXPECT_FALSE(child.HasSwitch("no-sandbox"));
  child.AppendSwitchASCII("type", "utility");
  AppendElectronRendererOverrides(base::DictValue().Set("lang", "zh-CN"),
                                  &child);
  ApplyElectronRendererOverrides(&child);
  EXPECT_EQ(child.GetSwitchValueASCII("lang"), "en-US");
}
}  // namespace
}  // namespace xenon::ipc
