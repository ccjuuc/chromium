// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_zlib_bridge.h"

#include <utility>

#include "xenon_overlay/chrome/browser/ipc/xenon_zlib_stream.h"

namespace xenon::ipc {
mojom::IpcResultPtr PerformZlibCall(std::string operation,
                                    base::Value::BlobStorage input,
                                    base::DictValue options) {
  return PerformStreamingZlibCall(std::move(operation), std::move(input),
                                  std::move(options));
}
}  // namespace xenon::ipc
