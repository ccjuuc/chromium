// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_worker_message.h"

#include "gin/converter.h"
#include "gin/test/v8_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-script.h"
#include "xenon_overlay/common/ipc/xenon_message_port_codec.h"

namespace xenon::ipc {
namespace {
class XenonWorkerMessageTest : public gin::V8Test {
 protected:
  v8::Isolate* isolate() { return instance_->isolate(); }
  v8::Local<v8::Context> context() { return context_.Get(isolate()); }
  v8::Local<v8::Value> Run(const char* source) {
    return v8::Script::Compile(
               context(), gin::StringToV8(isolate(), source).As<v8::String>())
        .ToLocalChecked()
        ->Run(context())
        .ToLocalChecked();
  }
};

TEST_F(XenonWorkerMessageTest, ClonesCyclesAndCollections) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  auto value =
      Run("(() => { const a = {n: 42n, map: new Map([[1, 'x']]), "
          "set: new Set([7]), bytes: new Uint8Array([1,2,3])}; "
          "a.self = a; return a; })()");
  auto prepared = PreparedWorkerMessage::Serialize(context(), value,
                                                   v8::Undefined(isolate()));
  ASSERT_TRUE(prepared);
  auto message = prepared->Commit(isolate());
  auto clone = message->Deserialize(context()).ToLocalChecked();
  ASSERT_TRUE(context()
                  ->Global()
                  ->Set(context(), gin::StringToV8(isolate(), "c"), clone)
                  .FromJust());
  EXPECT_TRUE(Run("c !== undefined && c.self === c && c.n === 42n && "
                  "c.map.get(1) === 'x' && c.set.has(7) && c.bytes[2] === 3")
                  ->IsTrue());
  EXPECT_NE(value, clone);
}

TEST_F(XenonWorkerMessageTest,
       UncloneableNestedObjectsDoNotRunTheirGettersOrDetachTransfers) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  auto value = Run(
      "globalThis.hits = 0; globalThis.marked = {get secret() {hits += 100;}};"
      "({get nested() {++hits; return marked;}})");
  ASSERT_TRUE(MarkAsUncloneable(context(), Run("marked")).FromMaybe(false));
  auto buffer = v8::ArrayBuffer::New(isolate(), 8);
  auto transfers = v8::Array::New(isolate(), 1);
  ASSERT_TRUE(transfers->Set(context(), 0, buffer).FromJust());
  {
    v8::TryCatch caught(isolate());
    EXPECT_FALSE(PreparedWorkerMessage::Serialize(context(), value, transfers));
    EXPECT_TRUE(caught.HasCaught());
    EXPECT_TRUE(IsWorkerDataCloneError(context(), caught.Exception()));
    EXPECT_FALSE(buffer->WasDetached());
  }
  EXPECT_TRUE(Run("hits === 1 && Reflect.ownKeys(marked).join() === 'secret'")
                  ->IsTrue());
  // Private markings belong to the object and do not follow its prototype.
  EXPECT_TRUE(PreparedWorkerMessage::Serialize(
      context(), Run("Object.create(marked)"), v8::Undefined(isolate())));
}

TEST_F(XenonWorkerMessageTest,
       UncloneablePolicyMatchesNodeBuiltInAndProxyExceptions) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  for (const char* source : {"[]", "new Date(0)", "new Map([[1, 2]])",
                              "new Set([1])", "/test/", "new Number(1)",
                              "new Uint8Array([1, 2])",
                              "new DataView(new ArrayBuffer(2))",
                              "new ArrayBuffer(2)"}) {
    SCOPED_TRACE(source);
    auto value = Run(source);
    ASSERT_TRUE(MarkAsUncloneable(context(), value).FromMaybe(false));
    EXPECT_TRUE(PreparedWorkerMessage::Serialize(
        context(), value, v8::Undefined(isolate())));
  }
  for (const char* source : {"Object.freeze({})", "Object.seal({})",
                              "Object.create(null)", "new Error('marked')"}) {
    SCOPED_TRACE(source);
    auto value = Run(source);
    ASSERT_TRUE(MarkAsUncloneable(context(), value).FromMaybe(false));
    v8::TryCatch caught(isolate());
    EXPECT_FALSE(PreparedWorkerMessage::Serialize(
        context(), value, v8::Undefined(isolate())));
    EXPECT_TRUE(caught.HasCaught());
  }
  auto proxy = Run("globalThis.traps = 0; new Proxy({}, {"
                   "get() {++traps;}, set() {++traps;}})");
  {
    v8::TryCatch caught(isolate());
    EXPECT_TRUE(MarkAsUncloneable(context(), proxy).IsNothing());
    EXPECT_TRUE(caught.HasCaught());
  }
  EXPECT_TRUE(Run("traps === 0")->IsTrue());
}

TEST_F(XenonWorkerMessageTest, TransferDetachesOnlyOnCommit) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  auto value = Run("new Uint8Array([13, 27]).buffer").As<v8::ArrayBuffer>();
  // Node explicitly allows ArrayBuffer transfer despite this clone marker.
  ASSERT_TRUE(MarkAsUncloneable(context(), value).FromMaybe(false));
  auto transfers = v8::Array::New(isolate(), 1);
  ASSERT_TRUE(transfers->Set(context(), 0, value).FromJust());
  auto prepared = PreparedWorkerMessage::Serialize(context(), value, transfers);
  ASSERT_TRUE(prepared);
  EXPECT_FALSE(value->WasDetached());
  auto message = prepared->Commit(isolate());
  EXPECT_TRUE(value->WasDetached());
  auto clone =
      message->Deserialize(context()).ToLocalChecked().As<v8::ArrayBuffer>();
  EXPECT_EQ(clone->ByteLength(), 2u);
  EXPECT_EQ(static_cast<uint8_t*>(clone->Data())[0], 13);
}

TEST_F(XenonWorkerMessageTest, FailedCloneDoesNotDetachTransfers) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  auto buffer = v8::ArrayBuffer::New(isolate(), 8);
  auto transfers = v8::Array::New(isolate(), 1);
  ASSERT_TRUE(transfers->Set(context(), 0, buffer).FromJust());
  v8::TryCatch caught(isolate());
  EXPECT_FALSE(PreparedWorkerMessage::Serialize(context(), Run("(() => {})"),
                                                transfers));
  EXPECT_TRUE(caught.HasCaught());
  EXPECT_FALSE(buffer->WasDetached());
}

TEST_F(XenonWorkerMessageTest, RejectsDuplicateTransfersWithoutDetaching) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  auto buffer = v8::ArrayBuffer::New(isolate(), 8);
  auto transfers = v8::Array::New(isolate(), 2);
  ASSERT_TRUE(transfers->Set(context(), 0, buffer).FromJust());
  ASSERT_TRUE(transfers->Set(context(), 1, buffer).FromJust());
  v8::TryCatch caught(isolate());
  EXPECT_FALSE(PreparedWorkerMessage::Serialize(context(), buffer, transfers));
  EXPECT_TRUE(caught.HasCaught());
  EXPECT_FALSE(buffer->WasDetached());
}

TEST_F(XenonWorkerMessageTest, SharedArrayBufferRetainsSharedBackingStore) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  auto buffer = v8::SharedArrayBuffer::New(isolate(), 8);
  auto prepared = PreparedWorkerMessage::Serialize(context(), buffer,
                                                   v8::Undefined(isolate()));
  ASSERT_TRUE(prepared);
  auto message = prepared->Commit(isolate());
  auto clone = message->Deserialize(context())
                   .ToLocalChecked()
                   .As<v8::SharedArrayBuffer>();
  EXPECT_EQ(buffer->GetBackingStore(), clone->GetBackingStore());
  EXPECT_NE(buffer, clone);
}

TEST_F(XenonWorkerMessageTest,
       QueueSizeIncludesTransferredAndSharedStorageOnce) {
  v8::HandleScope handles(isolate());
  v8::Context::Scope scope(context());
  constexpr size_t kBytes = 1024 * 1024;
  auto buffer = v8::ArrayBuffer::New(isolate(), kBytes);
  auto shared = v8::SharedArrayBuffer::New(isolate(), kBytes);
  auto transfers = v8::Array::New(isolate(), 1);
  ASSERT_TRUE(transfers->Set(context(), 0, buffer).FromJust());
  auto value = v8::Array::New(isolate(), 3);
  ASSERT_TRUE(value->Set(context(), 0, buffer).FromJust());
  ASSERT_TRUE(value->Set(context(), 1, shared).FromJust());
  ASSERT_TRUE(value->Set(context(), 2, shared).FromJust());
  auto prepared = PreparedWorkerMessage::Serialize(context(), value, transfers);
  ASSERT_TRUE(prepared);
  EXPECT_GE(prepared->message_size(), 2 * kBytes);
  EXPECT_LT(prepared->message_size(), 2 * kBytes + 4096);
  EXPECT_FALSE(buffer->WasDetached());
}
}  // namespace
}  // namespace xenon::ipc
