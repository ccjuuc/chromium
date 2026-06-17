// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_frameless_shadow_view.h"

#include <utility>

#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/view_shadow.h"

namespace xenon {

XenonFramelessShadowView::XenonFramelessShadowView(
    std::unique_ptr<views::View> content,
    Params params)
    : shadow_insets_(params.shadow_insets) {
  content_view_ = AddChildView(std::move(content));

  if (params.shadow_elevation) {
    view_shadow_ =
        std::make_unique<views::ViewShadow>(content_view_,
                                            *params.shadow_elevation);
    view_shadow_->SetRoundedCornerRadius(params.corner_radius);
  }
}

XenonFramelessShadowView::~XenonFramelessShadowView() = default;

// static
gfx::Size XenonFramelessShadowView::GetPreferredSizeForContent(
    const gfx::Size& content_preferred_size,
    const gfx::Insets& shadow_insets) {
  return gfx::Size(content_preferred_size.width() + shadow_insets.width(),
                   content_preferred_size.height() + shadow_insets.height());
}

void XenonFramelessShadowView::Layout(PassKey) {
  content_view_->SetBoundsRect(GetContentBounds());
}

gfx::Size XenonFramelessShadowView::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return GetPreferredSizeForContent(
      content_view_->GetPreferredSize(available_size), shadow_insets_);
}

gfx::Rect XenonFramelessShadowView::GetContentBounds() const {
  gfx::Rect bounds = GetLocalBounds();
  bounds.Inset(shadow_insets_);
  return bounds;
}

BEGIN_METADATA(XenonFramelessShadowView)
END_METADATA

}  // namespace xenon
