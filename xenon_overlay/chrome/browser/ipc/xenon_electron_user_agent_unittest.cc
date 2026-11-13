// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_user_agent.h"

#include "base/strings/strcat.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/common/ipc/xenon_runtime_platform.h"

namespace xenon::ipc {
namespace {

constexpr std::string_view kChromiumUserAgent =
    "Mozilla/5.0 (Platform) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/153.0.0.0 Safari/537.36";

TEST(XenonElectronUserAgentTest, KeepsBrowserPrefixAndElectronProductOrder) {
  EXPECT_EQ(
      base::StrCat(
          {"Mozilla/5.0 (Platform) AppleWebKit/537.36 (KHTML, like Gecko) "
           "diagram-editor/2.3.4 Chrome/153.0.0.0 Electron/",
           kElectronCompatibilityVersion, " Safari/537.36"}),
      BuildElectronUserAgent(kChromiumUserAgent, "diagram-editor", "2.3.4"));
}

TEST(XenonElectronUserAgentTest, NormalizesDisplayNameSpaces) {
  EXPECT_EQ(BuildElectronUserAgent(kChromiumUserAgent, "DisplayApp", "2.3.4"),
            BuildElectronUserAgent(kChromiumUserAgent, "Display App", "2.3.4"));
}

TEST(XenonElectronUserAgentTest, MissingOrInvalidMetadataCannotInjectProducts) {
  const std::string without_app =
      BuildElectronUserAgent(kChromiumUserAgent, "", "");
  EXPECT_NE(std::string::npos, without_app.find(" Electron/"));
  EXPECT_EQ(without_app, BuildElectronUserAgent(kChromiumUserAgent, "", "1"));
  EXPECT_EQ(without_app, BuildElectronUserAgent(kChromiumUserAgent, "app", ""));
  EXPECT_EQ(without_app, BuildElectronUserAgent(kChromiumUserAgent,
                                                "app\r\nInjected: x", "1"));
  EXPECT_EQ(without_app,
            BuildElectronUserAgent(kChromiumUserAgent, "app", "1 Safari/9"));
  EXPECT_EQ(without_app,
            BuildElectronUserAgent(kChromiumUserAgent, "app\tname", "1"));
  EXPECT_EQ(without_app,
            BuildElectronUserAgent(kChromiumUserAgent, "app/name", "1"));
}

TEST(XenonElectronUserAgentTest, ApplicationCannotReplaceRuntimeIdentity) {
  EXPECT_EQ(BuildElectronUserAgent(kChromiumUserAgent, "", ""),
            BuildElectronUserAgent(kChromiumUserAgent, "Electron", "99.0.0"));
}

TEST(XenonElectronUserAgentTest, UnrecognizedBrowserProductsRemainIntact) {
  EXPECT_EQ(base::StrCat({"Mozilla/5.0 Alternate/1 sample/2 Electron/",
                          kElectronCompatibilityVersion}),
            BuildElectronUserAgent("Mozilla/5.0 Alternate/1", "sample", "2"));
}

}  // namespace
}  // namespace xenon::ipc
