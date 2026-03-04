// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_REMINDER_XENON_REMINDER_NOTIFICATION_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_REMINDER_XENON_REMINDER_NOTIFICATION_MANAGER_H_

#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/singleton.h"
#include "base/sequence_checker.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_observer.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_types.h"

namespace xenon {

// 提醒弹窗业务管理：消息存储、通知观察者。简化版，无模板下载/WebUI 注册。
class XenonReminderNotificationManager {
 public:
  static XenonReminderNotificationManager* GetInstance();
  void Initialize();

  XenonReminderNotificationManager(const XenonReminderNotificationManager&) =
      delete;
  XenonReminderNotificationManager& operator=(
      const XenonReminderNotificationManager&) = delete;

  void AddObserver(XenonReminderNotificationObserver* observer);
  void RemoveObserver(XenonReminderNotificationObserver* observer);

  void AddMessage(ReminderMessage message);
  void RemoveMessage(const std::string& message_id);
  void UpdateMessage(ReminderMessage message);
  void ClearMessages();

  std::vector<ReminderMessage> GetAllMessages() const;

  void AddTestMessages(const std::string& id);

  // 使用 url 作为卡片内嵌 iframe 的 src，创建并展示一条提醒。
  void HandlePushMessage(const std::string& url);

 private:
  XenonReminderNotificationManager();
  ~XenonReminderNotificationManager();

  void NotifyMessageAdded(const ReminderMessage& message);
  void NotifyMessageRemoved(const std::string& message_id);
  void NotifyMessageUpdated(const ReminderMessage& message);
  void NotifyMessagesCleared();
  void NotifyTipMessage(const std::string& tip_text);

  friend struct base::DefaultSingletonTraits<XenonReminderNotificationManager>;

  SEQUENCE_CHECKER(sequence_checker_);

  std::map<std::string, ReminderMessage> messages_;
  std::vector<raw_ptr<XenonReminderNotificationObserver>> observers_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_REMINDER_XENON_REMINDER_NOTIFICATION_MANAGER_H_
