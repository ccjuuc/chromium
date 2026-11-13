// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_crypto_primitives.h"

#include <cstddef>
#include <cstdint>
#include <memory>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "crypto/secure_util.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-object.h"

namespace xenon::ipc {
namespace {

struct Bytes {
  std::shared_ptr<v8::BackingStore> owner;
  size_t offset = 0;
  size_t size = 0;

  base::span<const uint8_t> span() const {
    if (size == 0) {
      return {};
    }
    // V8 supplies both bounds and owns the allocation, retained for this call.
    return UNSAFE_BUFFERS(base::span(
        static_cast<const uint8_t*>(owner->Data()), owner->ByteLength()))
        .subspan(offset, size);
  }
};

bool GetBytes(v8::Local<v8::Value> value, Bytes* bytes) {
  if (value->IsArrayBufferView()) {
    auto view = value.As<v8::ArrayBufferView>();
    bytes->owner = view->Buffer()->GetBackingStore();
    bytes->offset = view->ByteOffset();
    bytes->size = view->ByteLength();
    return true;
  }
  if (value->IsArrayBuffer()) {
    auto buffer = value.As<v8::ArrayBuffer>();
    bytes->owner = buffer->GetBackingStore();
    bytes->size = buffer->ByteLength();
    return true;
  }
  if (value->IsSharedArrayBuffer()) {
    auto buffer = value.As<v8::SharedArrayBuffer>();
    bytes->owner = buffer->GetBackingStore();
    bytes->size = buffer->ByteLength();
    return true;
  }
  return false;
}

void Throw(v8::Isolate* isolate,
           bool range,
           const char* code,
           const char* message) {
  auto error = range ? v8::Exception::RangeError(gin::StringToV8(isolate, message))
                    : v8::Exception::TypeError(gin::StringToV8(isolate, message));
  error.As<v8::Object>()
      ->CreateDataProperty(isolate->GetCurrentContext(),
                           gin::StringToV8(isolate, "code"),
                           gin::StringToV8(isolate, code))
      .Check();
  isolate->ThrowException(error);
}

void TimingSafeEqual(gin::Arguments* args) {
  v8::Local<v8::Value> first;
  v8::Local<v8::Value> second;
  Bytes a;
  Bytes b;
  if (!args->GetNext(&first) || !args->GetNext(&second) ||
      !GetBytes(first, &a) || !GetBytes(second, &b)) {
    Throw(args->isolate(), false, "ERR_INVALID_ARG_TYPE",
          "Arguments must be ArrayBuffers, SharedArrayBuffers, or ArrayBufferViews");
    return;
  }
  if (a.size != b.size) {
    Throw(args->isolate(), true, "ERR_CRYPTO_TIMING_SAFE_EQUAL_LENGTH",
          "Input buffers must have the same byte length");
    return;
  }
  args->Return(crypto::SecureMemEqual(a.span(), b.span()));
}

}  // namespace

void InstallTimingSafeEqual(v8::Isolate* isolate,
                            v8::Local<v8::Context> context,
                            v8::Local<v8::Object> target,
                            const char* name) {
  target
      ->CreateDataProperty(
          context, gin::StringToV8(isolate, name),
          gin::CreateFunctionTemplate(isolate, base::BindRepeating(&TimingSafeEqual))
              ->GetFunction(context)
              .ToLocalChecked())
      .Check();
}

}  // namespace xenon::ipc
