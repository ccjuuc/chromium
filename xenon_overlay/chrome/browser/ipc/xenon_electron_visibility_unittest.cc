// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_visibility_policy.h"
#include "content/public/test/test_renderer_host.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

class ElectronVisibilityTest : public content::RenderViewHostTestHarness {};

TEST_F(ElectronVisibilityTest, OcclusionPolicyPreservesActualHideAndShow) {
  using content::Visibility;
  web_contents()->UpdateWebContentsVisibility(Visibility::VISIBLE);
  web_contents()->UpdateWebContentsVisibility(Visibility::OCCLUDED);
  EXPECT_EQ(web_contents()->GetVisibility(), Visibility::OCCLUDED);

  content::SetWebContentsOcclusionEnabled(web_contents(), false);
  EXPECT_EQ(web_contents()->GetVisibility(), Visibility::VISIBLE);
  web_contents()->UpdateWebContentsVisibility(Visibility::OCCLUDED);
  EXPECT_EQ(web_contents()->GetVisibility(), Visibility::VISIBLE);
  web_contents()->UpdateWebContentsVisibility(Visibility::HIDDEN);
  EXPECT_EQ(web_contents()->GetVisibility(), Visibility::HIDDEN);
  web_contents()->UpdateWebContentsVisibility(Visibility::VISIBLE);
  EXPECT_EQ(web_contents()->GetVisibility(), Visibility::VISIBLE);

  content::SetWebContentsOcclusionEnabled(web_contents(), true);
  web_contents()->UpdateWebContentsVisibility(Visibility::OCCLUDED);
  EXPECT_EQ(web_contents()->GetVisibility(), Visibility::OCCLUDED);
}

TEST_F(ElectronVisibilityTest, PolicyDoesNotChangeOtherContents) {
  using content::Visibility;
  auto other = content::WebContents::Create(
      content::WebContents::CreateParams(browser_context()));
  other->UpdateWebContentsVisibility(Visibility::VISIBLE);
  content::SetWebContentsOcclusionEnabled(web_contents(), false);
  other->UpdateWebContentsVisibility(Visibility::OCCLUDED);
  EXPECT_EQ(other->GetVisibility(), Visibility::OCCLUDED);
}

}  // namespace
}  // namespace xenon::ipc
