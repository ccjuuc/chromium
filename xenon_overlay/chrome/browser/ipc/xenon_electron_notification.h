// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NOTIFICATION_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NOTIFICATION_H_

#include <string>

#include "base/functional/callback.h"
#include "base/types/expected.h"
#include "base/values.h"

class Profile;

namespace xenon::ipc {

using ElectronNotificationEvent = base::RepeatingCallback<void(base::Value)>;

// UI sequence only. The caller authenticates the owner, which must include the
// container generation. Neither the owner nor the profile comes from JS.
base::expected<base::Value, std::string> CallElectronNotification(
    Profile* profile,
    const std::string& owner,
    const std::string& app_name,
    const base::Value& request,
    ElectronNotificationEvent event_callback);

// Retires callback identities before closing the platform notifications, so a
// delayed OS click/close can never be delivered to a restarted application.
void CloseElectronNotifications(Profile* profile, const std::string& owner);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NOTIFICATION_H_
