// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_REMINDER_XENON_REMINDER_TYPES_H_
#define XENON_OVERLAY_CHROME_BROWSER_REMINDER_XENON_REMINDER_TYPES_H_

#include <string>

#include "base/time/time.h"

namespace xenon {

struct ReminderMessage;

// 提醒卡片消息：id、展示用 URL（卡片内嵌 iframe 加载）、可选 title 等。
struct ReminderMessage {
  std::string id;
  std::string type;
  std::string title;
  std::string js_component_url;  // iframe src URL
  base::Time timestamp = base::Time();
  base::Time expire_time = base::Time();
  bool is_read = false;
  bool is_dismissed = false;
  bool display_on_desktop = false;

  ReminderMessage();
  ReminderMessage(const ReminderMessage& other);
  ReminderMessage& operator=(const ReminderMessage& other);
  ReminderMessage(ReminderMessage&&) noexcept;
  ReminderMessage& operator=(ReminderMessage&&) noexcept;
  ~ReminderMessage();
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_REMINDER_XENON_REMINDER_TYPES_H_
