// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_SHADOW_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_SHADOW_H_

#include <string>

namespace xenon {

inline constexpr int kDefaultElevation = 12;
inline constexpr double kDefaultOpacity = 0.2;
inline constexpr int kDefaultXOffset = 0;
inline constexpr int kDefaultYOffset = 0;
inline constexpr int kDefaultSpread = 0;
inline constexpr char kDefaultColorHex[] = "#000000";

enum class ShadowStyle {
  kNone,
  kBubbleBorder,
  kViewShadow,
  kCompositorShadow,
  kBoxShadow,
};

struct XenonMenuShadow {
  ShadowStyle style = ShadowStyle::kBubbleBorder;
  int elevation = kDefaultElevation;
  double opacity = kDefaultOpacity;
  int x_offset = kDefaultXOffset;
  int y_offset = kDefaultYOffset;
  int spread = kDefaultSpread;
  std::string color_hex = kDefaultColorHex;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_SHADOW_H_
