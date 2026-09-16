// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_API_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_API_BRIDGE_H_

#include "base/functional/callback.h"
#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

using ElectronApiCallback =
    base::OnceCallback<void(xenon::ipc::mojom::IpcResultPtr)>;

// Calls clipboard/shell APIs for an already-authorized Electron endpoint.
// Accepts a direct main-process request or the renderer's one-argument list.
// Call on the Browser UI sequence. Replies run on that same sequence. Shell
// operations are delegated to the selected platform backend.
void CallElectronApi(base::Value arguments, ElectronApiCallback callback);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_API_BRIDGE_H_
