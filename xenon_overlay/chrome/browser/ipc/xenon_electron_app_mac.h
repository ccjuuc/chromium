// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_MAC_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_MAC_H_

#include "base/values.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"

namespace xenon::ipc {

// Read-only login item query for the current browser's outer bundle. Call on
// the Browser UI thread, where the process and bundle identities are correct.
void GetElectronLoginItemSettings(const base::DictValue& arguments,
                                  ElectronApiCallback callback);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_MAC_H_
