// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_OS_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_OS_BRIDGE_H_

#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Returns a fresh node:os networkInterfaces() snapshot. May block; callers on
// the browser UI sequence must dispatch this to a blocking task runner.
mojom::IpcResultPtr GetNetworkInterfaces();

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_OS_BRIDGE_H_
