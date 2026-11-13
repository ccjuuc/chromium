// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_MEDIA_PERMISSION_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_MEDIA_PERMISSION_H_

#include <optional>

#include "content/public/browser/media_stream_request.h"

namespace content {
class RenderFrameHost;
class WebContents;
}  // namespace content

namespace xenon::ipc {

// Returns false without consuming callback only for an ordinary browser page.
// Authenticated Electron frames use their Session handlers and Chromium's real
// capture backend; a retired document cannot fall back to browser permissions.
bool HandleElectronMediaAccessRequest(
    content::WebContents* contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback& callback);
std::optional<bool> CheckElectronMediaAccessPermission(
    content::RenderFrameHost* frame,
    const url::Origin& origin,
    blink::mojom::MediaStreamType type);

}  // namespace xenon::ipc

#endif
