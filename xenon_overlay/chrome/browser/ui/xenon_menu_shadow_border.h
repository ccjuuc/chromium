// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_SHADOW_BORDER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_SHADOW_BORDER_H_

#include <string>

#include "third_party/skia/include/core/SkColor.h"
#include "ui/views/border.h"

namespace xenon {

// Helper to parse hex color string into SkColor.
SkColor ParseHexColor(const std::string& hex, double opacity);

class XenonBoxShadowBorder : public views::Border {
 public:
  XenonBoxShadowBorder(int x_offset,
                       int y_offset,
                       int blur_radius,
                       int spread_radius,
                       SkColor color,
                       int corner_radius);

  XenonBoxShadowBorder(const XenonBoxShadowBorder&) = delete;
  XenonBoxShadowBorder& operator=(const XenonBoxShadowBorder&) = delete;

  ~XenonBoxShadowBorder() override;

  // views::Border:
  void Paint(const views::View& view, gfx::Canvas* canvas) override;
  gfx::Insets GetInsets() const override;
  gfx::Size GetMinimumSize() const override;

 private:
  const int x_offset_;
  const int y_offset_;
  const int blur_radius_;
  const int spread_radius_;
  const SkColor color_;
  const int corner_radius_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MENU_SHADOW_BORDER_H_
