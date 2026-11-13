// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_JUMP_LIST_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_JUMP_LIST_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace xenon::ipc {
// Validation is platform independent and completes before changing the OS list.
std::string ValidateElectronJumpList(const base::Value& categories);

// UI-sequence entry. The caller supplies the authenticated container's App ID.
// Windows COM runs on an STA; completion returns on the caller's sequence.
// The value is Electron's documented status string, including OS failures.
void SetElectronJumpList(
    std::string app_id,
    base::Value categories,
    base::OnceCallback<void(base::Value, const std::string&)> callback);
}  // namespace xenon::ipc
#endif
