// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ZLIB_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ZLIB_BRIDGE_H_

#include <string>

#include "base/values.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {

// Runs entirely on plain owned data; safe to invoke on the worker pool.
mojom::IpcResultPtr PerformZlibCall(std::string operation,
                                    base::Value::BlobStorage input,
                                    base::DictValue options);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ZLIB_BRIDGE_H_
