// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_IPC_XENON_V8_STATISTICS_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_V8_STATISTICS_H_

#include "v8/include/v8-forward.h"

namespace xenon::ipc {

// Returns a fresh snapshot from the calling context's own isolate. No heap
// state or profiling metadata is shared with another application or worker.
v8::MaybeLocal<v8::Object> GetHeapCodeStatistics(
    v8::Local<v8::Context> context);

void InstallHeapCodeStatistics(v8::Isolate* isolate,
                               v8::Local<v8::Context> context,
                               v8::Local<v8::Object> target,
                               const char* name);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_COMMON_IPC_XENON_V8_STATISTICS_H_
