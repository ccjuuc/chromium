// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_FRAMELESS_SHADOW_VIEW_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_FRAMELESS_SHADOW_VIEW_H_

#include <memory>
#include <optional>

#include "base/memory/raw_ptr.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view.h"

namespace views {
class ViewShadow;
}  // namespace views

namespace xenon {

// A reusable Views wrapper that gives frameless translucent Widgets a stable
// compositor-drawn shadow.
//
// Windows frameless/native shadows can conflict with Views-drawn rounded
// corners, especially when Widget::InitParams::rounded_corners and
// WindowOpacity::kTranslucent are mixed. This view avoids that platform-specific
// path by:
//   * reserving transparent space around the content;
//   * positioning the real content inside that reserved space;
//   * optionally attaching views::ViewShadow to the content view.
//
// Typical Widget setup:
//   * Widget::InitParams::TYPE_WINDOW_FRAMELESS;
//   * params.remove_standard_frame = true;
//   * params.opacity = Widget::InitParams::WindowOpacity::kTranslucent;
//   * params.shadow_type = Widget::InitParams::ShadowType::kNone.
//
// The wrapped content is owned by this view after construction. The content
// continues to receive normal layout, painting, focus, and input events.
class XenonFramelessShadowView : public views::View {
  METADATA_HEADER(XenonFramelessShadowView, views::View)

 public:
  struct Params {
    // Transparent margin reserved around the content for the shadow. The Widget
    // bounds should include this inset area; otherwise the shadow can be clipped.
    gfx::Insets shadow_insets;

    // Rounded-corner radius used by the compositor shadow. This should match
    // the content background's visual radius.
    int corner_radius = 0;

    // Optional Views shadow elevation. When unset, this wrapper only reserves
    // transparent margins and does not draw a shadow.
    std::optional<int> shadow_elevation;
  };

  // Takes ownership of `content` and lays it out inside `params.shadow_insets`.
  XenonFramelessShadowView(std::unique_ptr<views::View> content,
                           Params params);
  XenonFramelessShadowView(const XenonFramelessShadowView&) = delete;
  XenonFramelessShadowView& operator=(const XenonFramelessShadowView&) =
      delete;
  ~XenonFramelessShadowView() override;

  views::View* content_view() { return content_view_; }
  const views::View* content_view() const { return content_view_; }

  // Returns the preferred outer size for a Widget that wraps content with the
  // given preferred size. Use this when calculating frameless Widget bounds so
  // the transparent shadow area is included.
  static gfx::Size GetPreferredSizeForContent(
      const gfx::Size& content_preferred_size,
      const gfx::Insets& shadow_insets);

  void Layout(PassKey) override;
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;

 private:
  gfx::Rect GetContentBounds() const;

  const gfx::Insets shadow_insets_;
  raw_ptr<views::View> content_view_ = nullptr;
  std::unique_ptr<views::ViewShadow> view_shadow_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_FRAMELESS_SHADOW_VIEW_H_
