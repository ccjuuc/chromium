// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SHELL_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SHELL_H_

#include "base/values.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"

namespace xenon::ipc {

// Platform implementation for a validated shell operation name. Call on the
// Browser UI sequence; completion runs on the same sequence. The backend must
// return actual operation results, or ERR_NOT_SUPPORTED when unavailable.
void CallElectronShell(base::DictValue arguments, ElectronApiCallback callback);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_SHELL_H_
