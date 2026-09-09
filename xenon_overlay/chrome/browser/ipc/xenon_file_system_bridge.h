// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_FILE_SYSTEM_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_FILE_SYSTEM_BRIDGE_H_

#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Performs one real filesystem operation for a trusted Electron-compatible
// renderer. Call this only from a sequence that permits blocking I/O.
xenon::ipc::mojom::IpcResultPtr PerformFileSystemCall(base::Value arguments);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_FILE_SYSTEM_BRIDGE_H_
