// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_NETWORK_REQUEST_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_NETWORK_REQUEST_BRIDGE_H_

#include "base/functional/callback_forward.h"
#include "base/memory/scoped_refptr.h"
#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace network {
class SharedURLLoaderFactory;
}

namespace xenon::ipc {

using NetworkRequestCallback =
    base::OnceCallback<void(xenon::ipc::mojom::IpcResultPtr)>;

// Performs one HTTP(S) request for a trusted Electron-compatible renderer.
// Electron/Node requests are not renderer fetches: routing through the
// browser network service preserves their headers and avoids renderer CORS.
void PerformNetworkRequest(
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory,
    base::Value arguments,
    NetworkRequestCallback callback);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_NETWORK_REQUEST_BRIDGE_H_
