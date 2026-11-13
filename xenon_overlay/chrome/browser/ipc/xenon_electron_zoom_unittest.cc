// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_zoom.h"

#include <limits>

#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_renderer_host.h"
#include "content/public/test/web_contents_tester.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"

namespace xenon::ipc {
class ElectronPageZoomTest : public content::RenderViewHostTestHarness {};

TEST_F(ElectronPageZoomTest, PageZoomUsesChromiumsOriginState) {
  NavigateAndCommit(GURL("https://zoom.test/"));
  std::string error;
  base::Value result;
  ASSERT_TRUE(CallElectronPageZoom(
      web_contents(), "set-zoom-level",
      base::Value(base::DictValue().Set("level", 2.0)), &result, &error))
      << error;
  ASSERT_TRUE(CallElectronPageZoom(web_contents(), "get-zoom-level",
                                   base::Value(), &result, &error));
  EXPECT_DOUBLE_EQ(2.0, result.GetDouble());
  EXPECT_FALSE(CallElectronPageZoom(
      web_contents(), "set-zoom-level",
      base::Value(base::DictValue().Set("level", "bad")), &result, &error));
  ASSERT_TRUE(CallElectronPageZoom(web_contents(), "get-zoom-level",
                                   base::Value(), &result, &error));
  EXPECT_DOUBLE_EQ(2.0, result.GetDouble());
}
TEST(ElectronVisualZoomTest, LimitsUpdateActualPreferencesAndSurviveRefresh) {
  content::BrowserTaskEnvironment tasks;
  content::TestBrowserContext context;
  auto contents =
      content::WebContentsTester::CreateTestWebContents(&context, nullptr);
  auto other =
      content::WebContentsTester::CreateTestWebContents(&context, nullptr);
  std::string error;
  ASSERT_TRUE(SetElectronVisualZoomLimits(
      contents.get(),
      base::Value(base::DictValue().Set("minimum", 1.0).Set("maximum", 2.5)),
      &error))
      << error;
  EXPECT_EQ(
      1.f,
      contents->GetOrCreateWebPreferences().default_minimum_page_scale_factor);
  EXPECT_EQ(
      2.5f,
      contents->GetOrCreateWebPreferences().default_maximum_page_scale_factor);
  blink::web_pref::WebPreferences refreshed;
  refreshed.default_minimum_page_scale_factor = 0.5f;
  refreshed.default_maximum_page_scale_factor = 5.f;
  EXPECT_FALSE(ApplyElectronVisualZoomLimits(other.get(), &refreshed));
  EXPECT_TRUE(ApplyElectronVisualZoomLimits(contents.get(), &refreshed));
  EXPECT_EQ(1.f, refreshed.default_minimum_page_scale_factor);
  EXPECT_EQ(2.5f, refreshed.default_maximum_page_scale_factor);
  EXPECT_FALSE(ApplyElectronVisualZoomLimits(contents.get(), &refreshed));
}
TEST(ElectronVisualZoomTest, InvalidRangesDoNotReplaceExistingLimits) {
  content::BrowserTaskEnvironment tasks;
  content::TestBrowserContext context;
  auto contents =
      content::WebContentsTester::CreateTestWebContents(&context, nullptr);
  std::string error;
  ASSERT_TRUE(SetElectronVisualZoomLimits(
      contents.get(),
      base::Value(base::DictValue().Set("minimum", 1).Set("maximum", 1)),
      &error));
  for (const auto pair : {std::pair(0., 2.), std::pair(2., 1.),
                          std::pair(1., std::numeric_limits<double>::max())}) {
    EXPECT_FALSE(SetElectronVisualZoomLimits(
        contents.get(),
        base::Value(base::DictValue()
                        .Set("minimum", pair.first)
                        .Set("maximum", pair.second)),
        &error));
    EXPECT_EQ(1.f, contents->GetOrCreateWebPreferences()
                       .default_maximum_page_scale_factor);
  }
  EXPECT_FALSE(SetElectronVisualZoomLimits(
      contents.get(),
      base::Value(base::DictValue().Set("minimum", "1").Set("maximum", 2)),
      &error));
}
}  // namespace xenon::ipc
