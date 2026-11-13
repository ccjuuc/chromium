// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_v8_statistics.h"

#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-statistics.h"

namespace xenon::ipc {

v8::MaybeLocal<v8::Object> GetHeapCodeStatistics(
    v8::Local<v8::Context> context) {
  auto* isolate = v8::Isolate::GetCurrent();
  v8::HeapCodeStatistics statistics;
  if (!isolate->GetHeapCodeAndMetadataStatistics(&statistics)) {
    auto error = v8::Exception::Error(v8::String::NewFromUtf8Literal(
        isolate, "Cannot read heap code statistics"));
    if (error.As<v8::Object>()
            ->CreateDataProperty(
                context, v8::String::NewFromUtf8Literal(isolate, "code"),
                v8::String::NewFromUtf8Literal(isolate, "ERR_OPERATION_FAILED"))
            .IsNothing()) {
      return {};
    }
    isolate->ThrowException(error);
    return {};
  }
  auto result = v8::Object::New(isolate);
  // Node exposes byte counts as Number, including counts above the int32 range.
  if (!result
           ->CreateDataProperty(
               context,
               v8::String::NewFromUtf8Literal(isolate,
                                              "code_and_metadata_size"),
               v8::Number::New(isolate, statistics.code_and_metadata_size()))
           .FromMaybe(false) ||
      !result
           ->CreateDataProperty(
               context,
               v8::String::NewFromUtf8Literal(isolate,
                                              "bytecode_and_metadata_size"),
               v8::Number::New(isolate,
                               statistics.bytecode_and_metadata_size()))
           .FromMaybe(false) ||
      !result
           ->CreateDataProperty(
               context,
               v8::String::NewFromUtf8Literal(isolate,
                                              "external_script_source_size"),
               v8::Number::New(isolate,
                               statistics.external_script_source_size()))
           .FromMaybe(false) ||
      !result
           ->CreateDataProperty(
               context,
               v8::String::NewFromUtf8Literal(isolate,
                                              "cpu_profiler_metadata_size"),
               v8::Number::New(isolate,
                               statistics.cpu_profiler_metadata_size()))
           .FromMaybe(false)) {
    return {};
  }
  return result;
}

void InstallHeapCodeStatistics(v8::Isolate* isolate,
                               v8::Local<v8::Context> context,
                               v8::Local<v8::Object> target,
                               const char* name) {
  auto callback = [](const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Local<v8::Object> statistics;
    if (GetHeapCodeStatistics(info.GetIsolate()->GetCurrentContext())
            .ToLocal(&statistics)) {
      info.GetReturnValue().Set(statistics);
    }
  };
  target
      ->CreateDataProperty(
          context, v8::String::NewFromUtf8(isolate, name).ToLocalChecked(),
          v8::Function::New(context, callback).ToLocalChecked())
      .Check();
}

}  // namespace xenon::ipc
