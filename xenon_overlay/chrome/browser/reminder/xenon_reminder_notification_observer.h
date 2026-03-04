// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_REMINDER_XENON_REMINDER_NOTIFICATION_OBSERVER_H_
#define XENON_OVERLAY_CHROME_BROWSER_REMINDER_XENON_REMINDER_NOTIFICATION_OBSERVER_H_

#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_types.h"

namespace xenon {

// 提醒弹窗 UI 观察者接口。Manager 通过此接口通知 UI 更新。
class XenonReminderNotificationObserver {
 public:
  virtual ~XenonReminderNotificationObserver() = default;

  virtual void OnReminderMessageAdded(const ReminderMessage& message) = 0;
  virtual void OnReminderMessageRemoved(const std::string& message_id) = 0;
  virtual void OnReminderMessageUpdated(const ReminderMessage& message) = 0;
  virtual void OnReminderMessagesCleared() = 0;

  virtual void OnTipMessageReceived(const std::string& tip_text) {}
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_REMINDER_XENON_REMINDER_NOTIFICATION_OBSERVER_H_
