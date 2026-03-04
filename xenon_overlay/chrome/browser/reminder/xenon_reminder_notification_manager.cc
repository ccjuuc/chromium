// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_manager.h"

#include "base/memory/singleton.h"
#include "base/time/time.h"
#include "base/uuid.h"

namespace xenon {

XenonReminderNotificationManager* XenonReminderNotificationManager::GetInstance() {
  return base::Singleton<XenonReminderNotificationManager>::get();
}

void XenonReminderNotificationManager::Initialize() {}

void XenonReminderNotificationManager::AddObserver(
    XenonReminderNotificationObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (observer) {
    observers_.push_back(observer);
  }
}

void XenonReminderNotificationManager::RemoveObserver(
    XenonReminderNotificationObserver* observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::erase(observers_, observer);
}

void XenonReminderNotificationManager::AddMessage(ReminderMessage message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (message.id.empty()) {
    return;
  }
  if (messages_.find(message.id) != messages_.end()) {
    return;
  }
  std::string id = message.id;
  messages_[id] = std::move(message);
  NotifyMessageAdded(messages_[id]);
}

void XenonReminderNotificationManager::RemoveMessage(
    const std::string& message_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  auto it = messages_.find(message_id);
  if (it == messages_.end()) {
    return;
  }
  messages_.erase(it);
  NotifyMessageRemoved(message_id);
}

void XenonReminderNotificationManager::UpdateMessage(ReminderMessage message) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (message.id.empty()) {
    return;
  }
  auto it = messages_.find(message.id);
  if (it == messages_.end()) {
    AddMessage(std::move(message));
    return;
  }
  it->second = std::move(message);
  NotifyMessageUpdated(it->second);
}

void XenonReminderNotificationManager::ClearMessages() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  messages_.clear();
  NotifyMessagesCleared();
}

std::vector<ReminderMessage> XenonReminderNotificationManager::GetAllMessages()
    const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<ReminderMessage> out;
  out.reserve(messages_.size());
  for (const auto& [id, msg] : messages_) {
    out.push_back(msg);
  }
  return out;
}

void XenonReminderNotificationManager::AddTestMessages(const std::string& id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  ReminderMessage msg;
  msg.id = id.empty() ? base::Uuid::GenerateRandomV4().AsLowercaseString() : id;
  msg.type = "test";
  msg.title = "Xenon 提醒测试";
  msg.timestamp = base::Time::Now();
  AddMessage(std::move(msg));
}

void XenonReminderNotificationManager::HandlePushMessage(
    const std::string& url) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (url.empty()) {
    return;
  }
  ReminderMessage msg;
  msg.id = base::Uuid::GenerateRandomV4().AsLowercaseString();
  msg.type = "reminder";
  msg.js_component_url = url;
  msg.timestamp = base::Time::Now();
  AddMessage(std::move(msg));
}

void XenonReminderNotificationManager::NotifyMessageAdded(
    const ReminderMessage& message) {
  for (XenonReminderNotificationObserver* observer : observers_) {
    observer->OnReminderMessageAdded(message);
  }
}

void XenonReminderNotificationManager::NotifyMessageRemoved(
    const std::string& message_id) {
  for (XenonReminderNotificationObserver* observer : observers_) {
    observer->OnReminderMessageRemoved(message_id);
  }
}

void XenonReminderNotificationManager::NotifyMessageUpdated(
    const ReminderMessage& message) {
  for (XenonReminderNotificationObserver* observer : observers_) {
    observer->OnReminderMessageUpdated(message);
  }
}

void XenonReminderNotificationManager::NotifyMessagesCleared() {
  for (XenonReminderNotificationObserver* observer : observers_) {
    observer->OnReminderMessagesCleared();
  }
}

void XenonReminderNotificationManager::NotifyTipMessage(
    const std::string& tip_text) {
  for (XenonReminderNotificationObserver* observer : observers_) {
    observer->OnTipMessageReceived(tip_text);
  }
}

XenonReminderNotificationManager::XenonReminderNotificationManager() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

XenonReminderNotificationManager::~XenonReminderNotificationManager() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

}  // namespace xenon
