// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_menu_runner.h"

#include <utility>

#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/controls/menu/menu_item_view.h"
#include "ui/views/widget/widget.h"
#include "xenon_overlay/chrome/browser/ui/xenon_menu_shadow_modifier.h"

namespace xenon {

XenonMenuRunner::XenonMenuRunner(
    ui::MenuModel* menu_model,
    int32_t run_types,
    base::RepeatingClosure on_menu_closed_callback)
    : menu_runner_(menu_model,
                   run_types,
                   std::move(on_menu_closed_callback)) {}

XenonMenuRunner::XenonMenuRunner(std::unique_ptr<views::MenuItemView> menu,
                                 int32_t run_types)
    : menu_runner_(std::move(menu), run_types) {}

XenonMenuRunner::~XenonMenuRunner() = default;

void XenonMenuRunner::RunMenuAt(
    views::Widget* parent,
    views::MenuButtonController* button_controller,
    const gfx::Rect& bounds,
    views::MenuAnchorPosition anchor,
    ui::mojom::MenuSourceType source_type,
    const XenonMenuShadow& shadow,
    gfx::NativeView native_view_for_gestures,
    std::optional<std::string> show_menu_host_duration_histogram) {
  if (parent) {
    auto* modifier = new MenuShadowModifier(shadow);
    modifier->SetParentWidget(parent);
    parent->AddObserver(modifier);
  }

  // To support translucent window and custom shadows (including kViewShadow, kBoxShadow, kCompositorShadow),
  // we must ensure the MenuHost is initialized with a translucent opacity and no native shadow.
  // In MenuHost::InitMenuHost, this is automatically set if bubble_border is true.
  // We can force bubble_border to be true by mapping non-bubble anchors to bubble anchors.
  views::MenuAnchorPosition mapped_anchor = anchor;
  if (shadow.style != ShadowStyle::kBubbleBorder || shadow.elevation == 0) {
    if (anchor == views::MenuAnchorPosition::kTopLeft) {
      mapped_anchor = views::MenuAnchorPosition::kBubbleBottomRight;
    } else if (anchor == views::MenuAnchorPosition::kTopRight) {
      mapped_anchor = views::MenuAnchorPosition::kBubbleBottomLeft;
    } else if (anchor == views::MenuAnchorPosition::kBottomCenter) {
      mapped_anchor = views::MenuAnchorPosition::kBubbleBottomLeft;
    }
  }

  menu_runner_.RunMenuAt(parent, button_controller, bounds, mapped_anchor, source_type,
                         native_view_for_gestures,
                         gfx::RoundedCornersF(kCornerRadius),
                         std::move(show_menu_host_duration_histogram));
}

bool XenonMenuRunner::IsRunning() const {
  return menu_runner_.IsRunning();
}

void XenonMenuRunner::Cancel() {
  menu_runner_.Cancel();
}

base::TimeTicks XenonMenuRunner::closing_event_time() const {
  return menu_runner_.closing_event_time();
}

}  // namespace xenon
