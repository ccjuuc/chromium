// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_DELEGATE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_DELEGATE_H_

#include <string_view>

#include "base/functional/callback.h"

namespace blink {
enum class PermissionType;
}
namespace content {
class BrowserContext;
class PermissionControllerDelegate;
}  // namespace content

namespace xenon::ipc {

// Electron's public permission names, not Chromium's UI/UMA spellings.
// Unsupported/invalid enum values return an empty view and must be denied.
std::string_view ElectronPermissionName(blink::PermissionType permission);

// One wrapper per BrowserContext. Only actual Electron StoragePartitions use
// the application policy. Every ordinary browser operation is delegated.
// The getter must not recreate a keyed service after context shutdown begins.
content::PermissionControllerDelegate* GetElectronPermissionControllerDelegate(
    content::BrowserContext* context,
    base::RepeatingCallback<content::PermissionControllerDelegate*()>
        fallback_getter);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PERMISSION_DELEGATE_H_
