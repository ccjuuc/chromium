// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_reminder_notification_group.h"

#include <algorithm>
#include <vector>

#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "xenon_overlay/chrome/browser/ui/xenon_reminder_card_webview.h"

namespace xenon {

XenonReminderNotificationGroup::XenonReminderNotificationGroup(
    const std::string& type)
    : type_(type) {}

XenonReminderNotificationGroup::~XenonReminderNotificationGroup() = default;

void XenonReminderNotificationGroup::AddCard(
    std::unique_ptr<XenonReminderCardWebView> card) {
  if (card) {
    AddChildView(std::move(card));
  }
}

void XenonReminderNotificationGroup::RemoveCard(
    const std::string& message_id) {
  for (views::View* child : children()) {
    auto* card = static_cast<XenonReminderCardWebView*>(child);
    if (card->message_id() == message_id) {
      card->ReleaseWebContents();
      RemoveChildView(card);
      break;
    }
  }
}

void XenonReminderNotificationGroup::ClearAllCardWebContents() {
  for (views::View* child : children()) {
    static_cast<XenonReminderCardWebView*>(child)->ReleaseWebContents();
  }
}

void XenonReminderNotificationGroup::SetCollapsed(bool collapsed) {
  if (is_collapsed_ != collapsed) {
    is_collapsed_ = collapsed;
    InvalidateLayout();
  }
}

void XenonReminderNotificationGroup::BringCardToFront(views::View* card) {
  if (card && Contains(card)) {
    for (views::View* child : children()) {
      if (!child->layer()) {
        child->SetPaintToLayer();
        child->layer()->SetFillsBoundsOpaquely(false);
      }
    }
    ReorderChildView(card, static_cast<int>(children().size()) - 1);
    if (is_collapsed_) {
      const int content_width = width() - 2 * kGroupPadding;
      for (size_t i = 0; i < children().size(); ++i) {
        views::View* child = children()[i];
        auto settings = std::make_unique<ui::ScopedLayerAnimationSettings>(
            child->layer()->GetAnimator());
        settings->SetTransitionDuration(base::Milliseconds(300));
        settings->SetTweenType(gfx::Tween::EASE_IN_OUT);
        int card_y =
            kGroupHeaderHeight + static_cast<int>(i) * kCollapsedOffset;
        child->SetBounds(kGroupPadding, card_y, content_width,
                         child->GetPreferredSize().height());
      }
    } else {
      InvalidateLayout();
    }
  }
}

void XenonReminderNotificationGroup::Layout(PassKey key) {
  const int content_width = width() - 2 * kGroupPadding;
  int y = kGroupHeaderHeight;

  std::vector<views::View*> visible_cards;
  for (views::View* child : children()) {
    auto* card = static_cast<XenonReminderCardWebView*>(child);
    if (card->is_loaded()) {
      visible_cards.push_back(child);
    } else {
      child->SetVisible(false);
    }
  }

  if (visible_cards.empty()) {
    return;
  }

  if (is_collapsed_) {
    for (size_t i = 0; i < visible_cards.size(); ++i) {
      views::View* card = visible_cards[i];
      card->SetVisible(true);
      gfx::Size size = card->GetPreferredSize();
      int card_w = std::max(content_width, size.width());
      int card_y = y + static_cast<int>(i) * kCollapsedOffset;
      card->SetBounds(kGroupPadding, card_y, card_w, size.height());
    }
  } else {
    for (size_t i = 0; i < visible_cards.size(); ++i) {
      views::View* card = visible_cards[i];
      card->SetVisible(true);
      gfx::Size size = card->GetPreferredSize();
      int card_w = std::max(content_width, size.width());
      card->SetBounds(kGroupPadding, y, card_w, size.height());
      y += size.height() + kCardOverlap;
    }
  }
}

gfx::Size XenonReminderNotificationGroup::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  std::vector<const views::View*> loaded_cards;
  for (const views::View* child : children()) {
    auto* card = static_cast<const XenonReminderCardWebView*>(child);
    if (card->is_loaded()) {
      loaded_cards.push_back(child);
    }
  }

  int height = kGroupHeaderHeight;
  if (is_collapsed_) {
    if (!loaded_cards.empty()) {
      int n = static_cast<int>(loaded_cards.size());
      int last_card_y = (n - 1) * kCollapsedOffset;
      height = kGroupHeaderHeight + last_card_y +
               loaded_cards.back()->GetPreferredSize().height();
    }
  } else {
    for (const views::View* child : loaded_cards) {
      height += child->GetPreferredSize().height() + kCardOverlap;
    }
    if (!loaded_cards.empty()) {
      height -= kCardOverlap;
    }
  }
  int max_width = 360;
  for (const views::View* child : loaded_cards) {
    int w = child->GetPreferredSize().width() + 2 * kGroupPadding;
    if (w > max_width) {
      max_width = w;
    }
  }
  return gfx::Size(max_width, height);
}

BEGIN_METADATA(XenonReminderNotificationGroup)
END_METADATA

}  // namespace xenon
