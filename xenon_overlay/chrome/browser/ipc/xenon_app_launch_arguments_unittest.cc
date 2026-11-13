// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_launch_arguments.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

TEST(XenonAppLaunchArgumentsTest, NoSeparatorNeverLeaksBrowserArguments) {
  base::CommandLine browser(base::FilePath(FILE_PATH_LITERAL("host")));
  browser.AppendSwitchASCII("xenon-electron-app", "release");
  browser.AppendSwitchASCII("user-data-dir", "host-profile");
  browser.AppendSwitchASCII("remote-debugging-port", "9222");
  browser.AppendArg("https://browser-tab.example/");
  const auto identity =
      base::FilePath(FILE_PATH_LITERAL("configured-launcher"));
  EXPECT_EQ((std::vector<std::string>{"configured-launcher"}),
            BuildElectronAppLaunchArgv(browser, identity));
}

TEST(XenonAppLaunchArgumentsTest,
     ExplicitSuffixRetainsOrderValuesAndAnotherSeparator) {
  base::CommandLine browser(base::FilePath(FILE_PATH_LITERAL("host")));
  browser.AppendSwitchASCII("user-data-dir", "browser-profile");
  browser.AppendArg("browser-only-positional");
  browser.AppendArg("--");
  const std::vector<std::string> app_args = {"--new-window",
                                             "--user-data-dir=app-profile",
                                             "文档 名称.md",
                                             "",
                                             "--repeat=first",
                                             "--repeat=second",
                                             "--",
                                             " final literal "};
  for (const auto& arg : app_args) {
    browser.AppendArg(arg);
  }
  auto expected = app_args;
  expected.insert(expected.begin(), "host");
  EXPECT_EQ(expected,
            BuildElectronAppLaunchArgv(browser, browser.GetProgram()));
}

TEST(XenonAppLaunchArgumentsTest, EmptySuffixHasOnlyExecutableIdentity) {
  base::CommandLine browser(base::FilePath(FILE_PATH_LITERAL("host")));
  browser.AppendArg("--");
  EXPECT_EQ((std::vector<std::string>{"host"}),
            BuildElectronAppLaunchArgv(browser, browser.GetProgram()));
}

TEST(XenonAppLaunchArgumentsTest, SwitchValueContainingDashesIsNotASeparator) {
  base::CommandLine browser(base::FilePath(FILE_PATH_LITERAL("host")));
  browser.AppendSwitchASCII("some-browser-setting", "--");
  browser.AppendArg("--not-a-separator");
  EXPECT_EQ((std::vector<std::string>{"host"}),
            BuildElectronAppLaunchArgv(browser, browser.GetProgram()));
}

}  // namespace
}  // namespace xenon::ipc
