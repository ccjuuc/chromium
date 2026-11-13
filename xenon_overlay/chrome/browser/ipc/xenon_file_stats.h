// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_FILE_STATS_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_FILE_STATS_H_

#include "base/values.h"
#include "xenon_overlay/third_party/libuv/include/uv.h"

namespace xenon::ipc {

// Internal to the libuv filesystem implementations. BigInt requests carry
// exact decimal integers and nanoseconds over the existing value transport.
base::DictValue FileSystemStatValue(const uv_stat_t& stat, bool bigint);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_FILE_STATS_H_
