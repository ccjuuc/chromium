// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_DOCUMENT_REGISTRATION_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_DOCUMENT_REGISTRATION_H_

#include <string>

#include "base/functional/callback.h"

namespace content {
class RenderFrameHost;
}

namespace xenon::ipc {

// Called only after Browser has authorized the document. The returned callback
// is completed by the actual Utility registration, not by queueing a message.
base::OnceCallback<void(bool)> BeginElectronDocumentRegistration(
    content::RenderFrameHost* frame,
    const std::string& endpoint_id,
    base::RepeatingCallback<bool()> owner_is_current = {});
void RevokeElectronDocumentRegistration(content::RenderFrameHost* frame,
                                        const std::string& endpoint_id);

// Returns this exact document's registered endpoint, or an empty string on
// failure, navigation, destruction or timeout. Never waits for another document
// or creates a runtime. Completion is asynchronous even for a ready document.
void WaitForElectronDocumentRegistration(
    content::RenderFrameHost* frame,
    base::OnceCallback<void(std::string)> callback);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_DOCUMENT_REGISTRATION_H_
