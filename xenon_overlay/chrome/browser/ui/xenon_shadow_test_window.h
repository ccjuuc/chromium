// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_SHADOW_TEST_WINDOW_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_SHADOW_TEST_WINDOW_H_

#include <string>

#include "ui/gfx/native_ui_types.h"

namespace xenon {

// Developer-only entry points for opening Xenon shadow inspection windows.
//
// This class owns no state. Each static method creates and shows a standalone
// Views Widget for manual shadow inspection. `parent_view` is optional; when it
// is non-null the test window is positioned near the same display and stacked
// relative to that parent where the platform supports it.
//
// Do not use this class as a production shadow implementation. Reusable
// frameless shadow behavior lives in XenonFramelessShadowView.
class XenonShadowTestWindow {
 public:
  XenonShadowTestWindow() = delete;
  XenonShadowTestWindow(const XenonShadowTestWindow&) = delete;
  XenonShadowTestWindow& operator=(const XenonShadowTestWindow&) = delete;

  // Opens the Widget shadow inspection window.
  //
  // The window lists all Widget::InitParams::ShadowType values and lets the
  // caller open both standard Widget samples and frameless Widget samples.
  // Frameless samples use XenonFramelessShadowView so compositor shadows can be
  // inspected without native/DWM rounded-corner artifacts.
  static void ShowWidgetShadowTestWindow(gfx::NativeView parent_view);

  // Directly opens a Widget shadow sample window by string type ("kDefault", "kNone", "kDrop").
  static void ShowWidgetShadowSample(gfx::NativeView parent_view,
                                     const std::string& shadow_type_str,
                                     bool borderless,
                                     bool show_backdrop);

  // Opens the View shadow inspection window.
  //
  // The window demonstrates View-level shadow APIs, including ui::Shadow,
  // views::ViewShadow, and views::BubbleBorder.
  static void ShowViewShadowTestWindow(gfx::NativeView parent_view);
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_SHADOW_TEST_WINDOW_H_
