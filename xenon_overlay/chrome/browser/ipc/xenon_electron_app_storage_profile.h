// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_STORAGE_PROFILE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_STORAGE_PROFILE_H_

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_storage.h"

namespace xenon::ipc {

// Chrome-specific lifetime adapter. Kept outside the storage registry so the
// session/permission layer does not depend back on Profile implementation.
void CreateElectronAppStorageProfile(const base::FilePath& path,
                                     ElectronAppStorageContextCreated callback);
void DestroyElectronAppStorageProfile(
    std::unique_ptr<content::BrowserContext> context);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_STORAGE_PROFILE_H_
