// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_menu_shadow_modifier.h"

#include <utility>

#include "base/task/single_thread_task_runner.h"
#include "ui/color/color_id.h"
#include "ui/color/color_provider.h"
#include "ui/compositor/layer.h"
#include "ui/compositor_extra/shadow.h"
#include "ui/views/background.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/controls/menu/menu_config.h"
#include "ui/views/controls/menu/menu_scroll_view_container.h"
#include "ui/views/view_shadow.h"
#include "ui/views/widget/widget.h"
#include "xenon_overlay/chrome/browser/ui/xenon_menu_runner.h"
#include "xenon_overlay/chrome/browser/ui/xenon_menu_shadow_border.h"

namespace xenon {

MenuShadowModifier::MenuShadowModifier(const XenonMenuShadow& shadow)
    : shadow_(shadow) {}

MenuShadowModifier::~MenuShadowModifier() {
  if (parent_widget_) {
    parent_widget_->RemoveObserver(this);
  }
  if (menu_widget_) {
    menu_widget_->RemoveObserver(this);
  }
  if (bg_view_) {
    bg_view_->RemoveObserver(this);
  }
}

void MenuShadowModifier::SetParentWidget(views::Widget* parent) {
  parent_widget_ = parent;
}

void MenuShadowModifier::OnWidgetChildAdded(views::Widget* widget,
                                            views::Widget* child) {
  if (captured_) {
    return;
  }
  if (child->GetName() == "MenuHost") {
    captured_ = true;
    menu_widget_ = child;
    menu_widget_->AddObserver(this);

    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&MenuShadowModifier::ApplyShadow,
                                  weak_factory_.GetWeakPtr()));
  }
}

void MenuShadowModifier::OnWidgetDestroying(views::Widget* widget) {
  if (widget == menu_widget_) {
    weak_factory_.InvalidateWeakPtrs();
    menu_widget_ = nullptr;
    if (!deletion_scheduled_) {
      deletion_scheduled_ = true;
      base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                                    this);
    }
  } else if (widget == parent_widget_) {
    weak_factory_.InvalidateWeakPtrs();
    parent_widget_ = nullptr;
    if (!menu_widget_ && !deletion_scheduled_) {
      deletion_scheduled_ = true;
      base::SingleThreadTaskRunner::GetCurrentDefault()->DeleteSoon(FROM_HERE,
                                                                    this);
    }
  }
}

void MenuShadowModifier::OnViewLayerBoundsSet(views::View* view) {
  if (view == bg_view_ && compositor_shadow_) {
    gfx::Rect shadow_bounds = view->bounds();
    shadow_bounds.Offset(shadow_.x_offset, shadow_.y_offset);
    compositor_shadow_->SetContentBounds(shadow_bounds);
  }
}

void MenuShadowModifier::OnViewIsDeleting(views::View* view) {
  if (view == bg_view_) {
    bg_view_ = nullptr;
  }
}

void MenuShadowModifier::ApplyShadow() {
  views::Widget* widget = menu_widget_;
  if (!widget) {
    return;
  }
  views::View* contents_view = widget->GetContentsView();
  if (!contents_view) {
    return;
  }

  views::MenuScrollViewContainer* menu_scroll_container = nullptr;
  if (contents_view->GetClassName() == "MenuScrollViewContainer") {
    menu_scroll_container =
        static_cast<views::MenuScrollViewContainer*>(contents_view);
  }

  gfx::Insets old_total_insets = contents_view->GetInsets();
  const int kCornerRadius = XenonMenuRunner::kCornerRadius;

  switch (shadow_.style) {
    case ShadowStyle::kNone: {
      auto border = std::make_unique<views::BubbleBorder>(
          views::BubbleBorder::NONE, views::BubbleBorder::NO_SHADOW);
      border->set_rounded_corners(gfx::RoundedCornersF(kCornerRadius));
      contents_view->SetBackground(
          std::make_unique<views::BubbleBackground>(border.get()));
      contents_view->SetBorder(std::move(border));
      break;
    }

    case ShadowStyle::kBubbleBorder: {
      if (menu_scroll_container && menu_scroll_container->HasBubbleBorder() &&
          contents_view->GetBorder()) {
        views::BubbleBorder* bubble_border =
            static_cast<views::BubbleBorder*>(contents_view->GetBorder());
        bubble_border->set_md_shadow_elevation(shadow_.elevation);
      }
      break;
    }

    case ShadowStyle::kViewShadow: {
      views::View* bg_view = nullptr;
      if (menu_scroll_container && !menu_scroll_container->children().empty()) {
        bg_view = menu_scroll_container->children().front();
      }

      if (!bg_view) {
        auto border = std::make_unique<views::BubbleBorder>(
            views::BubbleBorder::NONE, views::BubbleBorder::NO_SHADOW);
        border->set_rounded_corners(gfx::RoundedCornersF(kCornerRadius));
        contents_view->SetBackground(
            std::make_unique<views::BubbleBackground>(border.get()));
        contents_view->SetBorder(std::move(border));

        view_shadow_ = std::make_unique<views::ViewShadow>(contents_view,
                                                           shadow_.elevation);
        view_shadow_->SetRoundedCornerRadius(kCornerRadius);

        if (contents_view->layer()) {
          contents_view->layer()->SetFillsBoundsOpaquely(false);
        }

        if (view_shadow_->shadow()) {
          SkColor color = ParseHexColor(shadow_.color_hex, 1.0);
          ui::Shadow::ElevationToColorsMap color_map;
          color_map[shadow_.elevation] = std::make_pair(color, color);
          view_shadow_->shadow()->SetElevationToColorsMap(color_map);

          if (view_shadow_->shadow()->layer()) {
            view_shadow_->shadow()->layer()->SetOpacity(shadow_.opacity);
          }
        }
      } else {
        gfx::Insets insets =
            views::BubbleBorder::GetBorderAndShadowInsets(shadow_.elevation);
        contents_view->SetBorder(views::CreateEmptyBorder(insets));
        contents_view->SetBackground(
            views::CreateSolidBackground(SK_ColorTRANSPARENT));

        if (!bg_view->layer()) {
          bg_view->SetPaintToLayer();
        }
        bg_view->layer()->SetFillsBoundsOpaquely(false);
        bg_view->layer()->SetRoundedCornerRadius(
            gfx::RoundedCornersF(kCornerRadius));

        const auto& menu_config = views::MenuConfig::instance();
        int vertical_inset =
            menu_config.rounded_menu_vertical_border_size.value_or(
                kCornerRadius);
        int horizontal_inset = menu_config.menu_horizontal_border_size;
        bg_view->SetBorder(views::CreateEmptyBorder(
            gfx::Insets::TLBR(vertical_inset, horizontal_inset, vertical_inset,
                              horizontal_inset)));

        SkColor bg_color = SK_ColorWHITE;
        if (contents_view->GetColorProvider()) {
          bg_color = contents_view->GetColorProvider()->GetColor(
              ui::kColorMenuBackground);
        }
        bg_view->SetBackground(
            views::CreateRoundedRectBackground(bg_color, kCornerRadius));

        view_shadow_ =
            std::make_unique<views::ViewShadow>(bg_view, shadow_.elevation);
        view_shadow_->SetRoundedCornerRadius(kCornerRadius);

        if (view_shadow_->shadow()) {
          SkColor color = ParseHexColor(shadow_.color_hex, 1.0);
          ui::Shadow::ElevationToColorsMap color_map;
          color_map[shadow_.elevation] = std::make_pair(color, color);
          view_shadow_->shadow()->SetElevationToColorsMap(color_map);

          if (view_shadow_->shadow()->layer()) {
            view_shadow_->shadow()->layer()->SetOpacity(shadow_.opacity);
          }
        }
      }
      break;
    }

    case ShadowStyle::kCompositorShadow: {
      views::View* bg_view = nullptr;
      if (menu_scroll_container && !menu_scroll_container->children().empty()) {
        bg_view = menu_scroll_container->children().front();
      }

      if (!bg_view) {
        auto border = std::make_unique<views::BubbleBorder>(
            views::BubbleBorder::NONE, views::BubbleBorder::NO_SHADOW);
        border->set_rounded_corners(gfx::RoundedCornersF(kCornerRadius));
        contents_view->SetBackground(
            std::make_unique<views::BubbleBackground>(border.get()));
        contents_view->SetBorder(std::move(border));

        if (!contents_view->layer()) {
          contents_view->SetPaintToLayer();
          contents_view->layer()->SetFillsBoundsOpaquely(false);
        }

        compositor_shadow_ = std::make_unique<ui::Shadow>();
        compositor_shadow_->Init(shadow_.elevation);
        compositor_shadow_->SetRoundedCornerRadius(kCornerRadius);

        {
          SkColor color = ParseHexColor(shadow_.color_hex, 1.0);
          ui::Shadow::ElevationToColorsMap color_map;
          color_map[shadow_.elevation] = std::make_pair(color, color);
          compositor_shadow_->SetElevationToColorsMap(color_map);
        }

        contents_view->AddLayerToRegion(compositor_shadow_->layer(),
                                        views::LayerRegion::kBelow);

        gfx::Rect shadow_bounds = contents_view->GetLocalBounds();
        shadow_bounds.Offset(shadow_.x_offset, shadow_.y_offset);
        compositor_shadow_->SetContentBounds(shadow_bounds);

        if (compositor_shadow_->layer()) {
          compositor_shadow_->layer()->SetOpacity(shadow_.opacity);
        }
      } else {
        gfx::Insets insets =
            views::BubbleBorder::GetBorderAndShadowInsets(shadow_.elevation);
        contents_view->SetBorder(views::CreateEmptyBorder(insets));
        contents_view->SetBackground(
            views::CreateSolidBackground(SK_ColorTRANSPARENT));

        bg_view_ = bg_view;
        bg_view_->AddObserver(this);

        if (!bg_view->layer()) {
          bg_view->SetPaintToLayer();
        }
        bg_view->layer()->SetFillsBoundsOpaquely(false);
        bg_view->layer()->SetRoundedCornerRadius(
            gfx::RoundedCornersF(kCornerRadius));

        const auto& menu_config = views::MenuConfig::instance();
        int vertical_inset =
            menu_config.rounded_menu_vertical_border_size.value_or(
                kCornerRadius);
        int horizontal_inset = menu_config.menu_horizontal_border_size;
        bg_view->SetBorder(views::CreateEmptyBorder(
            gfx::Insets::TLBR(vertical_inset, horizontal_inset, vertical_inset,
                              horizontal_inset)));

        SkColor bg_color = SK_ColorWHITE;
        if (contents_view->GetColorProvider()) {
          bg_color = contents_view->GetColorProvider()->GetColor(
              ui::kColorMenuBackground);
        }
        bg_view->SetBackground(
            views::CreateRoundedRectBackground(bg_color, kCornerRadius));

        compositor_shadow_ = std::make_unique<ui::Shadow>();
        compositor_shadow_->Init(shadow_.elevation);
        compositor_shadow_->SetRoundedCornerRadius(kCornerRadius);

        {
          SkColor color = ParseHexColor(shadow_.color_hex, 1.0);
          ui::Shadow::ElevationToColorsMap color_map;
          color_map[shadow_.elevation] = std::make_pair(color, color);
          compositor_shadow_->SetElevationToColorsMap(color_map);
        }

        bg_view->AddLayerToRegion(compositor_shadow_->layer(),
                                  views::LayerRegion::kBelow);

        gfx::Rect shadow_bounds = bg_view->bounds();
        shadow_bounds.Offset(shadow_.x_offset, shadow_.y_offset);
        compositor_shadow_->SetContentBounds(shadow_bounds);

        if (compositor_shadow_->layer()) {
          compositor_shadow_->layer()->SetOpacity(shadow_.opacity);
        }
      }
      break;
    }

    case ShadowStyle::kBoxShadow: {
      contents_view->SetBackground(
          views::CreateSolidBackground(SK_ColorTRANSPARENT));
      SkColor color = ParseHexColor(shadow_.color_hex, shadow_.opacity);
      auto border = std::make_unique<XenonBoxShadowBorder>(
          shadow_.x_offset, shadow_.y_offset, shadow_.elevation, shadow_.spread,
          color, kCornerRadius);
      contents_view->SetBorder(std::move(border));
      break;
    }
  }

  contents_view->InvalidateLayout();

  gfx::Rect bounds = widget->GetWindowBoundsInScreen();
  gfx::Size preferred_size = contents_view->GetPreferredSize();

  gfx::Insets additional_insets;
  if (menu_scroll_container && menu_scroll_container->HasBubbleBorder()) {
    const auto& menu_config = views::MenuConfig::instance();
    int vertical_inset =
        menu_config.rounded_menu_vertical_border_size.value_or(kCornerRadius);
    int horizontal_inset = menu_config.menu_horizontal_border_size;
    int border_thickness = menu_config.use_outer_border ? 1 : 0;
    additional_insets = gfx::Insets::VH(vertical_inset, horizontal_inset) -
                        gfx::Insets(border_thickness);
  }

  if (menu_scroll_container &&
      (shadow_.style == ShadowStyle::kViewShadow ||
       shadow_.style == ShadowStyle::kCompositorShadow)) {
    preferred_size.Enlarge(additional_insets.width(),
                           additional_insets.height());
  }

  if (bounds.size() != preferred_size) {
    gfx::Insets new_total_insets = contents_view->GetInsets();
    if (menu_scroll_container &&
        (shadow_.style == ShadowStyle::kViewShadow ||
         shadow_.style == ShadowStyle::kCompositorShadow)) {
      new_total_insets += additional_insets;
    }

    int dx = new_total_insets.left() - old_total_insets.left();
    int dy = new_total_insets.top() - old_total_insets.top();

    bounds.set_x(bounds.x() - dx);
    bounds.set_y(bounds.y() - dy);
    bounds.set_size(preferred_size);
    widget->SetBounds(bounds);
  }

  contents_view->SchedulePaint();
}

}  // namespace xenon
