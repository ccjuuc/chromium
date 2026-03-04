// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_NOTIFICATION_GROUP_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_NOTIFICATION_GROUP_H_

#include <memory>
#include <string>

#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/view.h"

namespace xenon {

class XenonReminderCardWebView;

// 同类型消息组：管理一组卡片的层叠布局（展开/收起）。
class XenonReminderNotificationGroup : public views::View {
  METADATA_HEADER(XenonReminderNotificationGroup, views::View)

 public:
  explicit XenonReminderNotificationGroup(const std::string& type);
  XenonReminderNotificationGroup(const XenonReminderNotificationGroup&) =
      delete;
  XenonReminderNotificationGroup& operator=(
      const XenonReminderNotificationGroup&) = delete;
  ~XenonReminderNotificationGroup() override;

  const std::string& type() const { return type_; }
  void AddCard(std::unique_ptr<XenonReminderCardWebView> card);
  void RemoveCard(const std::string& message_id);
  void ClearAllCardWebContents();
  void SetCollapsed(bool collapsed);
  bool is_collapsed() const { return is_collapsed_; }
  void BringCardToFront(views::View* card);

  void Layout(PassKey) override;
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;

 private:
  std::string type_;
  bool is_collapsed_ = true;
  static constexpr int kGroupHeaderHeight = 0;
  static constexpr int kGroupPadding = 8;
  static constexpr int kCardOverlap = 12;
  static constexpr int kCollapsedOffset = 15;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_NOTIFICATION_GROUP_H_
