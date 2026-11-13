// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_v8_statistics.h"

#include <string>

#include "gin/converter.h"
#include "gin/test/v8_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"

namespace xenon::ipc {
namespace {

class XenonV8StatisticsTest : public gin::V8Test {
 protected:
  v8::Isolate* isolate() { return instance_->isolate(); }
  v8::Local<v8::Context> context() { return context_.Get(isolate()); }
  double Value(v8::Local<v8::Object> object, const char* name) {
    return object->Get(context(), gin::StringToV8(isolate(), name))
        .ToLocalChecked()
        ->NumberValue(context())
        .FromJust();
  }
};

TEST_F(XenonV8StatisticsTest,
       SnapshotsActualCompiledBytecodeAndDoNotShareObjects) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  auto before = GetHeapCodeStatistics(context()).ToLocalChecked();
  for (const auto* key :
       {"code_and_metadata_size", "bytecode_and_metadata_size",
        "external_script_source_size", "cpu_profiler_metadata_size"}) {
    EXPECT_GE(Value(before, key), 0);
  }
  // Keep freshly compiled functions reachable. A hardcoded zero/snapshot cannot
  // report the bytecode compiled between these two calls.
  std::string source = "globalThis.retainedCode = [];";
  for (int i = 0; i < 256; ++i) {
    source += "retainedCode.push(function code" + std::to_string(i) +
              "(n) { let sum = " + std::to_string(i) +
              "; for (let j = 0; j < n; ++j) sum += j; return sum; });";
  }
  source += "retainedCode.forEach(fn => fn(3));";
  ASSERT_FALSE(
      v8::Script::Compile(context(),
                          gin::StringToV8(isolate(), source).As<v8::String>())
          .ToLocalChecked()
          ->Run(context())
          .IsEmpty());
  auto after = GetHeapCodeStatistics(context()).ToLocalChecked();
  EXPECT_NE(before, after);
  EXPECT_GT(Value(after, "bytecode_and_metadata_size"),
            Value(before, "bytecode_and_metadata_size"));
  EXPECT_TRUE(before
                  ->CreateDataProperty(
                      context(),
                      gin::StringToV8(isolate(), "bytecode_and_metadata_size"),
                      v8::Number::New(isolate(), -1))
                  .FromJust());
  EXPECT_GT(Value(after, "bytecode_and_metadata_size"), 0);
}

}  // namespace
}  // namespace xenon::ipc
