// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_ipc_value_codec.h"

#include <cstdlib>
#include <utility>

#include "base/containers/span.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-value-serializer.h"

namespace xenon::ipc {

namespace {

constexpr char kWireVersionKey[] = "__xenon_ipc_wire_version__";
constexpr char kWireDataKey[] = "data";
constexpr int kWireVersion = 1;

std::string ExceptionText(v8::Isolate* isolate,
                          const v8::TryCatch& try_catch,
                          const char* fallback) {
  if (try_catch.HasCaught()) {
    v8::String::Utf8Value text(isolate, try_catch.Exception());
    if (*text && text.length() > 0) {
      return std::string(*text, text.length());
    }
  }
  return fallback;
}

void RethrowCloneError(v8::Isolate* isolate,
                       v8::TryCatch* try_catch,
                       const std::string& message) {
  if (try_catch->HasCaught()) {
    try_catch->ReThrow();
    return;
  }
  isolate->ThrowException(v8::Exception::Error(
      v8::String::NewFromUtf8(isolate, message.c_str()).ToLocalChecked()));
}

}  // namespace

bool IsInternalIpcChannel(const std::string& channel) {
  return channel.starts_with("__xenon:");
}

bool SerializeIpcValue(v8::Isolate* isolate,
                       v8::Local<v8::Context> context,
                       v8::Local<v8::Value> value,
                       bool rethrow_exception,
                       base::Value* output,
                       std::string* error) {
  if (!isolate || context.IsEmpty() || value.IsEmpty() || !output) {
    if (error) {
      *error = "Invalid structured-clone input";
    }
    return false;
  }

  v8::TryCatch try_catch(isolate);
  v8::ValueSerializer serializer(isolate);
  serializer.WriteHeader();
  if (!serializer.WriteValue(context, value).FromMaybe(false)) {
    const std::string clone_error = ExceptionText(
        isolate, try_catch, "Value could not be cloned for IPC");
    if (error) {
      *error = clone_error;
    }
    if (rethrow_exception) {
      RethrowCloneError(isolate, &try_catch, clone_error);
    }
    return false;
  }

  auto [data, size] = serializer.Release();
  base::Value::BlobStorage bytes(size);
  if (size > 0) {
    // ValueSerializer owns this exact allocation and reports its extent. Wrap
    // that C API boundary once; all subsequent transport uses bounded spans.
    base::span(bytes).copy_from(UNSAFE_BUFFERS(base::span(data, size)));
  }
  std::free(data);

  base::DictValue envelope;
  envelope.Set(kWireVersionKey, kWireVersion);
  envelope.Set(kWireDataKey, std::move(bytes));
  *output = base::Value(std::move(envelope));
  if (error) {
    error->clear();
  }
  return true;
}

bool IsSerializedIpcValue(const base::Value& value) {
  if (!value.is_dict()) {
    return false;
  }
  const base::DictValue& envelope = value.GetDict();
  return envelope.FindInt(kWireVersionKey).value_or(0) == kWireVersion &&
         envelope.FindBlob(kWireDataKey) != nullptr;
}

v8::MaybeLocal<v8::Value> DeserializeIpcValue(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    const base::Value& input,
    std::string* error) {
  if (!isolate || context.IsEmpty() || !IsSerializedIpcValue(input)) {
    if (error) {
      *error = "Invalid structured-clone IPC envelope";
    }
    return {};
  }

  const base::Value::BlobStorage& bytes =
      *input.GetDict().FindBlob(kWireDataKey);
  v8::TryCatch try_catch(isolate);
  v8::ValueDeserializer deserializer(isolate, bytes.data(), bytes.size());
  if (!deserializer.ReadHeader(context).FromMaybe(false)) {
    if (error) {
      *error = ExceptionText(isolate, try_catch,
                             "Invalid structured-clone IPC header");
    }
    try_catch.Reset();
    return {};
  }

  v8::Local<v8::Value> value;
  if (!deserializer.ReadValue(context).ToLocal(&value)) {
    if (error) {
      *error = ExceptionText(isolate, try_catch,
                             "Invalid structured-clone IPC payload");
    }
    try_catch.Reset();
    return {};
  }
  if (error) {
    error->clear();
  }
  return value;
}

}  // namespace xenon::ipc
