// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_IPC_XENON_IPC_VALUE_CODEC_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_IPC_VALUE_CODEC_H_

#include <string>

#include "base/values.h"
#include "v8/include/v8-forward.h"

namespace xenon::ipc {

// Private transport channels are decoded by the Browser process and retain
// their typed base::Value representation. Application channels are opaque to
// the Browser and use V8's structured-clone wire format end to end.
bool IsInternalIpcChannel(const std::string& channel);

// Serializes one JavaScript value into an opaque, tagged base::Value payload.
// V8's ValueSerializer implements the HTML structured clone algorithm and
// preserves undefined, BigInt, Date, RegExp, Map, Set, cycles and binary data.
// If |rethrow_exception| is true, DataCloneError remains visible to JavaScript.
bool SerializeIpcValue(v8::Isolate* isolate,
                       v8::Local<v8::Context> context,
                       v8::Local<v8::Value> value,
                       bool rethrow_exception,
                       base::Value* output,
                       std::string* error);

bool IsSerializedIpcValue(const base::Value& value);

// Decodes a payload produced by SerializeIpcValue. Decode errors are consumed
// and returned as text because wire data may arrive after the sending realm is
// gone; callers decide whether to throw, reject, or drop the message.
v8::MaybeLocal<v8::Value> DeserializeIpcValue(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    const base::Value& input,
    std::string* error);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_COMMON_IPC_XENON_IPC_VALUE_CODEC_H_
