// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_OS_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_OS_BRIDGE_H_

#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Returns a fresh node:os networkInterfaces() snapshot. May block; callers on
// the browser UI sequence must dispatch this to a blocking task runner.
mojom::IpcResultPtr GetNetworkInterfaces();

// Reads an operating-system value for node:os. |request| must be a dictionary
// with a string "method". May block; dispatch off the browser UI sequence.
// Process-specific operations are deliberately unsupported until the caller's
// process identity can be supplied by the trusted IPC endpoint.
mojom::IpcResultPtr PerformOsCall(base::Value request);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_OS_BRIDGE_H_
