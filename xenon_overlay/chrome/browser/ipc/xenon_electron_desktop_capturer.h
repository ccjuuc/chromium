// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_DESKTOP_CAPTURER_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_DESKTOP_CAPTURER_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace xenon::ipc {

// UI sequence; owner is the authenticated container/generation identity.
void GetElectronDesktopSources(
    const std::string& owner,
    const base::Value& options,
    base::OnceCallback<void(base::Value, std::string)> reply);
void CloseElectronDesktopSources(const std::string& owner);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_DESKTOP_CAPTURER_H_
