// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_BROKER_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_BROKER_H_

#include <string>

#include "base/functional/callback.h"
#include "base/types/expected.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "xenon_overlay/public/mojom/xenon_permission_check.mojom-forward.h"

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content

namespace xenon::ipc {

using ElectronPermissionRemoteFactory = base::RepeatingCallback<
    mojo::PendingRemote<mojom::ElectronPermissionCheck>()>;

// Called only by XenonManager with the authenticated application generation.
// The permission pipe is independent of the service's ordinary IPC pipe so a
// synchronous check deadline cannot disconnect the application's other APIs.
void BindElectronPermissionOwner(
    content::BrowserContext* context,
    const std::string& container,
    const std::string& owner,
    ElectronPermissionRemoteFactory remotes,
    base::RepeatingCallback<int(content::WebContents*)> contents_ids,
    base::RepeatingCallback<void(base::Value)> events);
void CloseElectronPermissionOwner(const std::string& owner);
// The manager may select the application's storage context before its first
// session is used. Keep registered handlers and the dedicated pipe, but revoke
// validators captured for the previous context.
base::expected<void, std::string> RebindElectronPermissionOwnerContext(
    const std::string& owner,
    content::BrowserContext* context);
base::expected<base::Value, std::string> CallElectronPermissions(
    const std::string& container,
    const std::string& owner,
    const base::Value& request);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_BROKER_H_
