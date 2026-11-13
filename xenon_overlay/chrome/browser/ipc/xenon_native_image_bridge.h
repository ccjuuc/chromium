// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_NATIVE_IMAGE_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_NATIVE_IMAGE_BRIDGE_H_

#include <string>

#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Stateless PNG/JPEG conversion for authorized Electron endpoints. Image
// descriptions contain canonical PNG bytes and dimensions, never native
// handles.
mojom::IpcResultPtr CallNativeImageApi(const std::string& operation,
                                       const base::DictValue& arguments);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_NATIVE_IMAGE_BRIDGE_H_
