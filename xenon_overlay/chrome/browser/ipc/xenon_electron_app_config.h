// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_CONFIG_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_CONFIG_H_

#include <string>

#include "base/files/file_path.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Accepts an application directory/archive or an external host JSON descriptor.
// Resolves metadata without rewriting or extracting any application resources.
bool LoadElectronAppConfig(const base::FilePath& path,
                           const std::string& container_id,
                           mojom::IpcMainConfigPtr* config,
                           std::string* error);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_APP_CONFIG_H_
