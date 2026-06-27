// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_menu_shadow_border.h"

#include <algorithm>
#include <string_view>

#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "cc/paint/paint_filter.h"
#include "third_party/skia/include/effects/SkImageFilters.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view.h"

namespace xenon {

SkColor ParseHexColor(const std::string& hex, double opacity) {
  const int alpha = static_cast<int>(opacity * 255);
  if (hex.empty()) {
    return SkColorSetARGB(38, 0, 0, 0);
  }
  std::string_view s = hex;
  if (base::StartsWith(s, "#")) {
    s.remove_prefix(1);
  }
  uint32_t rgb = 0;
  if (s.size() == 6 && base::HexStringToUInt(s, &rgb)) {
    return SkColorSetARGB(alpha, (rgb >> 16) & 0xff, (rgb >> 8) & 0xff,
                          rgb & 0xff);
  }
  return SkColorSetARGB(alpha, 0, 0, 0);
}

XenonBoxShadowBorder::XenonBoxShadowBorder(int x_offset,
                                           int y_offset,
                                           int blur_radius,
                                           int spread_radius,
                                           SkColor color,
                                           int corner_radius)
    : x_offset_(x_offset),
      y_offset_(y_offset),
      blur_radius_(blur_radius),
      spread_radius_(spread_radius),
      color_(color),
      corner_radius_(corner_radius) {}

XenonBoxShadowBorder::~XenonBoxShadowBorder() = default;

void XenonBoxShadowBorder::Paint(const views::View& view, gfx::Canvas* canvas) {
  gfx::Rect content_bounds = view.GetLocalBounds();
  content_bounds.Inset(GetInsets());
  if (content_bounds.IsEmpty()) {
    return;
  }

  cc::PaintFlags flags;
  flags.setStyle(cc::PaintFlags::kFill_Style);

  SkColor bg_color = SK_ColorWHITE;
  if (view.GetColorProvider()) {
    bg_color = view.GetColorProvider()->GetColor(ui::kColorMenuBackground);
  }
  flags.setColor(bg_color);
  flags.setAntiAlias(true);

  float sigma = blur_radius_ * 0.5f;
  if (sigma > 0.0f) {
    flags.setImageFilter(sk_make_sp<cc::DropShadowPaintFilter>(
        x_offset_, y_offset_, sigma, sigma, SkColor4f::FromColor(color_),
        cc::DropShadowPaintFilter::ShadowMode::kDrawShadowAndForeground,
        nullptr));
  }

  canvas->DrawRoundRect(content_bounds, corner_radius_, flags);
}

gfx::Insets XenonBoxShadowBorder::GetInsets() const {
  int extra_left = std::max(0, blur_radius_ + spread_radius_ - x_offset_);
  int extra_right = std::max(0, blur_radius_ + spread_radius_ + x_offset_);
  int extra_top = std::max(0, blur_radius_ + spread_radius_ - y_offset_);
  int extra_bottom = std::max(0, blur_radius_ + spread_radius_ + y_offset_);
  return gfx::Insets::TLBR(extra_top, extra_left, extra_bottom, extra_right);
}

gfx::Size XenonBoxShadowBorder::GetMinimumSize() const {
  gfx::Insets insets = GetInsets();
  return gfx::Size(insets.width() + corner_radius_ * 2,
                   insets.height() + corner_radius_ * 2);
}

}  // namespace xenon
