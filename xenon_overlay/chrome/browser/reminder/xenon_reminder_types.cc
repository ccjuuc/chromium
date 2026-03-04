// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_types.h"

namespace xenon {

ReminderMessage::ReminderMessage() = default;

ReminderMessage::ReminderMessage(const ReminderMessage& other)
    : id(other.id),
      type(other.type),
      title(other.title),
      js_component_url(other.js_component_url),
      timestamp(other.timestamp),
      expire_time(other.expire_time),
      is_read(other.is_read),
      is_dismissed(other.is_dismissed),
      display_on_desktop(other.display_on_desktop) {}

ReminderMessage& ReminderMessage::operator=(const ReminderMessage& other) {
  id = other.id;
  type = other.type;
  title = other.title;
  js_component_url = other.js_component_url;
  timestamp = other.timestamp;
  expire_time = other.expire_time;
  is_read = other.is_read;
  is_dismissed = other.is_dismissed;
  display_on_desktop = other.display_on_desktop;
  return *this;
}

ReminderMessage::ReminderMessage(ReminderMessage&&) noexcept = default;

ReminderMessage& ReminderMessage::operator=(ReminderMessage&&) noexcept =
    default;

ReminderMessage::~ReminderMessage() = default;

}  // namespace xenon
