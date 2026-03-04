// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_NOTIFICATION_CONTAINER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_NOTIFICATION_CONTAINER_H_

#include <map>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/view.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_types.h"

class Profile;

namespace xenon {

class XenonReminderCardWebView;
class XenonReminderNotificationGroup;

// 提醒弹窗内容容器：垂直排列多个消息组（按 type 分组）。
class XenonReminderNotificationContainer : public views::View {
  METADATA_HEADER(XenonReminderNotificationContainer, views::View)

 public:
  using LayoutChangedCallback = base::RepeatingClosure;

  explicit XenonReminderNotificationContainer(Profile* profile,
                                              bool is_desktop = false);
  XenonReminderNotificationContainer(
      const XenonReminderNotificationContainer&) = delete;
  XenonReminderNotificationContainer& operator=(
      const XenonReminderNotificationContainer&) = delete;
  ~XenonReminderNotificationContainer() override;

  void AddMessage(const ReminderMessage& message);
  void RemoveMessage(const std::string& message_id);
  void UpdateMessage(const ReminderMessage& message);
  void ClearMessages();

  void SetLayoutChangedCallback(LayoutChangedCallback callback);

  void Layout(PassKey) override;
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;

  bool DoesIntersectRect(const views::View* target,
                         const gfx::Rect& rect) const;
  std::vector<gfx::Rect> GetCardBounds() const;

 private:
  XenonReminderNotificationGroup* GetOrCreateGroup(const std::string& type);
  void OnCardDismissed(const std::string& message_id);

  raw_ptr<Profile> profile_ = nullptr;
  bool is_desktop_ = false;
  std::map<std::string, raw_ptr<XenonReminderNotificationGroup>> groups_;
  LayoutChangedCallback layout_changed_callback_;
  int last_preferred_height_ = 0;
  static constexpr int kContainerPadding = 12;
  static constexpr int kGroupSpacing = 12;
  static constexpr int kContainerWidth = 360;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_NOTIFICATION_CONTAINER_H_
