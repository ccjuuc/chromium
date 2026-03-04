// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_reminder_notification_container.h"

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_manager.h"
#include "xenon_overlay/chrome/browser/ui/xenon_reminder_card_webview.h"
#include "xenon_overlay/chrome/browser/ui/xenon_reminder_notification_group.h"

namespace xenon {

XenonReminderNotificationContainer::XenonReminderNotificationContainer(
    Profile* profile,
    bool is_desktop)
    : profile_(profile), is_desktop_(is_desktop) {}

XenonReminderNotificationContainer::~XenonReminderNotificationContainer() =
    default;

void XenonReminderNotificationContainer::AddMessage(
    const ReminderMessage& message) {
  XenonReminderNotificationGroup* group =
      GetOrCreateGroup(message.type.empty() ? "default" : message.type);
  if (!group) {
    return;
  }
  auto card = std::make_unique<XenonReminderCardWebView>(profile_, message);
  card->SetDismissCallback(
      base::BindRepeating(&XenonReminderNotificationContainer::OnCardDismissed,
                          base::Unretained(this)));
  group->AddCard(std::move(card));
  InvalidateLayout();
}

void XenonReminderNotificationContainer::RemoveMessage(
    const std::string& message_id) {
  for (auto& [type, group] : groups_) {
    group->RemoveCard(message_id);
  }
  for (auto it = groups_.begin(); it != groups_.end();) {
    if (it->second->children().empty()) {
      RemoveChildView(it->second);
      it = groups_.erase(it);
    } else {
      ++it;
    }
  }
  InvalidateLayout();
}

void XenonReminderNotificationContainer::UpdateMessage(
    const ReminderMessage& message) {
  RemoveMessage(message.id);
  AddMessage(message);
}

void XenonReminderNotificationContainer::ClearMessages() {
  for (auto& [type, group] : groups_) {
    if (group) {
      group->ClearAllCardWebContents();
    }
  }
  while (!children().empty()) {
    RemoveChildView(children().front());
  }
  groups_.clear();
  InvalidateLayout();
}

void XenonReminderNotificationContainer::SetLayoutChangedCallback(
    LayoutChangedCallback callback) {
  layout_changed_callback_ = std::move(callback);
}

void XenonReminderNotificationContainer::Layout(PassKey key) {
  int y = kContainerPadding;
  const int w = width() - 2 * kContainerPadding;
  for (views::View* child : children()) {
    if (!child->GetVisible()) {
      continue;
    }
    gfx::Size preferred = child->GetPreferredSize();
    child->SetBounds(kContainerPadding, y, w, preferred.height());
    y += preferred.height() + kGroupSpacing;
  }

  gfx::Size preferred = CalculatePreferredSize({});
  if (preferred.height() != last_preferred_height_) {
    last_preferred_height_ = preferred.height();
    if (layout_changed_callback_) {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE, layout_changed_callback_);
    }
  }
}

gfx::Size XenonReminderNotificationContainer::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  int height = kContainerPadding;
  int max_width = kContainerWidth;
  bool has_visible_child = false;
  for (const views::View* child : children()) {
    if (child->GetVisible()) {
      has_visible_child = true;
      gfx::Size ps = child->GetPreferredSize();
      height += ps.height() + kGroupSpacing;
      if (ps.width() > max_width) {
        max_width = ps.width();
      }
    }
  }
  if (!has_visible_child) {
    return gfx::Size(0, 0);
  }
  height += kContainerPadding;
  return gfx::Size(max_width + 2 * kContainerPadding, height);
}

XenonReminderNotificationGroup*
XenonReminderNotificationContainer::GetOrCreateGroup(const std::string& type) {
  auto it = groups_.find(type);
  if (it != groups_.end()) {
    return it->second;
  }
  auto group = std::make_unique<XenonReminderNotificationGroup>(type);
  XenonReminderNotificationGroup* ptr = AddChildView(std::move(group));
  groups_[type] = ptr;
  return ptr;
}

bool XenonReminderNotificationContainer::DoesIntersectRect(
    const views::View* target,
    const gfx::Rect& rect) const {
  for (const views::View* child : children()) {
    if (!child->GetVisible()) {
      continue;
    }
    gfx::Rect rect_in_child = rect;
    rect_in_child.Offset(-child->x(), -child->y());
    if (child->HitTestRect(rect_in_child)) {
      return true;
    }
  }
  return false;
}

std::vector<gfx::Rect> XenonReminderNotificationContainer::GetCardBounds()
    const {
  std::vector<gfx::Rect> result;
  for (const views::View* group : children()) {
    if (!group->GetVisible()) {
      continue;
    }
    for (const views::View* card : group->children()) {
      if (!card->GetVisible()) {
        continue;
      }
      gfx::Rect card_bounds = card->bounds();
      card_bounds.Offset(group->x(), group->y());
      result.push_back(card_bounds);
    }
  }
  return result;
}

void XenonReminderNotificationContainer::OnCardDismissed(
    const std::string& message_id) {
  if (is_desktop_) {
    RemoveMessage(message_id);
  } else {
    XenonReminderNotificationManager::GetInstance()->RemoveMessage(message_id);
  }
}

BEGIN_METADATA(XenonReminderNotificationContainer)
END_METADATA

}  // namespace xenon
