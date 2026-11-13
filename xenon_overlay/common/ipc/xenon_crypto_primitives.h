// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_IPC_XENON_CRYPTO_PRIMITIVES_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_CRYPTO_PRIMITIVES_H_

#include "v8/include/v8-forward.h"

namespace xenon::ipc {

// Installs a realm-local synchronous primitive. Both Node main contexts and
// renderer preloads use the same constant-time native comparison.
void InstallTimingSafeEqual(v8::Isolate* isolate,
                            v8::Local<v8::Context> context,
                            v8::Local<v8::Object> target,
                            const char* name);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_COMMON_IPC_XENON_CRYPTO_PRIMITIVES_H_
