// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSIONS_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSIONS_H_

#include <string>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/values.h"

class GURL;
namespace content {
class RenderFrameHost;
class RenderProcessHost;
}  // namespace content

namespace xenon::ipc {

// Browser UI sequence only. Identity comes from the actual StoragePartition,
// never an origin supplied by a page or an application-provided container id.
bool IsElectronPermissionFrame(content::RenderFrameHost* frame);
bool IsElectronPermissionProcess(content::RenderProcessHost* process);
bool IsActiveElectronPermissionFrame(content::RenderFrameHost* frame);
// Capture before starting an asynchronous native operation. The validator also
// remembers the exact application generation, so a replacement owner cannot
// authorize completion of an operation requested by the preceding instance.
base::RepeatingCallback<bool()> GetElectronPermissionFrameValidator(
    content::RenderFrameHost* frame);

// Notifications are coalesced onto the Browser UI task queue so changing a
// handler inside its synchronous check cannot recursively refresh subscribers.
// The wrapper refreshes only its authenticated Electron subscriptions.
base::CallbackListSubscription ObserveElectronPermissionChanges(
    base::RepeatingClosure callback);

// Electron's default permission check grants except deprecated synchronous
// clipboard reads. The wrapper supplies this explicit default. A registered
// handler always receives the real check; transport errors and stale documents
// deny, and a retired application can never receive the permissive default.
bool CheckElectronPermission(content::RenderFrameHost* frame,
                             const std::string& permission,
                             const GURL& requesting_origin,
                             base::DictValue details,
                             bool default_granted = true);
bool CheckElectronWorkerPermission(content::RenderProcessHost* process,
                                   const std::string& permission,
                                   const GURL& requesting_origin,
                                   base::DictValue details,
                                   bool default_granted = true);

// Call only for a positively identified Electron frame. Requests complete
// exactly once, denying on document retirement, handler replacement, deadline
// expiry, or application exit. No handler follows Electron's grant default.
void RequestElectronPermission(content::RenderFrameHost* frame,
                               const std::string& permission,
                               base::DictValue details,
                               base::OnceCallback<void(bool)> callback);

// Display selection runs only after the ordinary "media" permission decision.
// An empty result denies. Result fields are video: {id, name} (a real desktop
// source), audio: "loopback" or "loopbackWithMute", enableLocalEcho: bool.
// The media adapter validates the source id/platform capabilities before use.
void RequestElectronDisplayMedia(
    content::RenderFrameHost* frame,
    base::DictValue details,
    base::OnceCallback<void(base::DictValue)> callback);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSIONS_H_
