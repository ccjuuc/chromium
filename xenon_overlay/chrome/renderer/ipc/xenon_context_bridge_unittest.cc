// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/ipc/xenon_context_bridge.h"

#include "gin/converter.h"
#include "gin/test/v8_test.h"
#include "v8/include/v8.h"

namespace xenon::ipc {
namespace {

class XenonContextBridgeTest : public gin::V8Test {
 public:
  void SetUp() override {
    gin::V8Test::SetUp();
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto main = context_.Get(isolate);
    auto preload = v8::Context::New(isolate);
    preload_.Reset(isolate, preload);
    bridge_ = XenonContextBridge::Create(preload, main);
    preload->Global()
        ->CreateDataProperty(preload, gin::StringToV8(isolate, "contextBridge"),
                             bridge_->GetApi())
        .Check();
  }

  void TearDown() override {
    bridge_->Shutdown();
    bridge_.reset();
    preload_.Reset();
    gin::V8Test::TearDown();
  }

 protected:
  std::string Evaluate(const std::string& text, bool main = false) {
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto context = main ? context_.Get(isolate) : preload_.Get(isolate);
    v8::Context::Scope scope(context);
    v8::TryCatch caught(isolate);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    if (!v8::Script::Compile(context,
                             gin::StringToV8(isolate, text).As<v8::String>())
             .ToLocal(&script) ||
        !script->Run(context).ToLocal(&result)) {
      v8::String::Utf8Value error(isolate, caught.Exception());
      ADD_FAILURE() << (*error ? *error : "Script failed");
      return {};
    }
    v8::String::Utf8Value value(isolate, result);
    return *value ? std::string(*value, value.length()) : std::string();
  }
  v8::Global<v8::Value> TrackGlobal(const char* name, bool main = false) {
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto context = main ? context_.Get(isolate) : preload_.Get(isolate);
    v8::Context::Scope scope(context);
    auto value = context->Global()
                     ->Get(context, gin::StringToV8(isolate, name))
                     .ToLocalChecked();
    EXPECT_TRUE(value->IsObject());
    v8::Global<v8::Value> weak(isolate, value);
    weak.SetWeak();
    return weak;
  }
  void CollectGarbage() {
    // The second collection also sweeps values released by weak callbacks.
    instance_->isolate()->LowMemoryNotification();
    task_environment_.RunUntilIdle();
    instance_->isolate()->LowMemoryNotification();
    task_environment_.RunUntilIdle();
  }
  scoped_refptr<XenonContextBridge> bridge_;
  v8::Global<v8::Context> preload_;
};

TEST_F(XenonContextBridgeTest, FunctionsAndObjectsCannotExposePreloadGlobal) {
  Evaluate(R"JS(
    globalThis.process = {secret: 'private'};
    contextBridge.exposeInMainWorld('api', {
      read: () => process.secret,
      nested: {values: [1, 2]},
      getFunction: () => () => process.secret,
    });
  )JS");
  EXPECT_EQ("private", Evaluate("api.read()", true));
  EXPECT_EQ("undefined", Evaluate("typeof process", true));
  EXPECT_EQ("undefined",
            Evaluate("api.read.constructor('return typeof process')()", true));
  EXPECT_EQ(
      "undefined",
      Evaluate("api.nested.constructor.constructor('return typeof process')()",
               true));
  EXPECT_EQ("undefined",
            Evaluate("api.getFunction().constructor('return typeof process')()",
                     true));
  EXPECT_EQ(
      "true",
      Evaluate("Object.isFrozen(api) && Object.isFrozen(api.nested.values)",
               true));
  EXPECT_EQ("[1,2]", Evaluate("JSON.stringify(api.nested.values)", true));
}

TEST_F(XenonContextBridgeTest, CopiesArgumentsAndPreservesCallbackIdentity) {
  Evaluate(R"JS(
    let callback;
    contextBridge.exposeInMainWorld('api', {
      remember: fn => { callback = fn; },
      same: fn => callback === fn,
      call: value => callback(value),
      mutate: value => { value.nested.value++; return value; },
      cyclic: () => { const value = {}; value.self = value; return value; },
    });
  )JS");
  EXPECT_EQ(
      "true",
      Evaluate("var cb = x => x + 1; api.remember(cb); api.same(cb)", true));
  EXPECT_EQ("42", Evaluate("api.call(41)", true));
  EXPECT_EQ(
      "[1,2]",
      Evaluate(
          "var data = {nested: {value: 1}}; var changed = api.mutate(data);"
          "JSON.stringify([data.nested.value, changed.nested.value])",
          true));
  EXPECT_EQ("true",
            Evaluate("var cycle = api.cyclic(); cycle.self === cycle", true));
}

TEST_F(XenonContextBridgeTest, ErrorsAndPromisesBelongToDestinationRealm) {
  Evaluate(R"JS(
    contextBridge.exposeInMainWorld('api', {
      fail: () => { throw new Error('expected'); },
      read: async () => ({value: 42}),
      reject: async () => { throw {error_description: 'denied'}; },
    });
  )JS");
  EXPECT_EQ(
      "expected",
      Evaluate("try { api.fail(); 'missing'; } catch (e) {"
               "if (!(e instanceof Error)) throw Error('Wrong error realm');"
               "e.message.replace(/^Error: /, ''); }",
               true));
  EXPECT_EQ("true", Evaluate("api.read() instanceof Promise", true));
  Evaluate(
      "var reply, rejection; api.read().then(v => reply = v);"
      "api.reject().catch(e => rejection = e);",
      true);
  instance_->isolate()->PerformMicrotaskCheckpoint();
  EXPECT_EQ("42", Evaluate("reply.value", true));
  EXPECT_EQ(
      "true",
      Evaluate("Object.getPrototypeOf(reply) === Object.prototype", true));
  EXPECT_EQ("denied", Evaluate("rejection.error_description", true));
}

TEST_F(XenonContextBridgeTest, UnretainedProxyReleasesPreloadClosure) {
  Evaluate(R"JS(
    contextBridge.exposeInMainWorld('api', {
      create: value => {
        const payload = {value};
        globalThis.observed = payload;
        return () => payload.value;
      },
    });
  )JS");
  Evaluate("var retained = api.create(41)", true);
  auto payload = TrackGlobal("observed");
  Evaluate("delete globalThis.observed");
  CollectGarbage();
  EXPECT_FALSE(payload.IsEmpty());
  EXPECT_EQ("41", Evaluate("retained()", true));

  // Keep another proxy alive while collecting the first, then add a third.
  // Collection must not renumber/reuse an ID belonging to a live callback.
  Evaluate("var second = api.create(42); retained = null", true);
  Evaluate("delete globalThis.observed");
  CollectGarbage();
  EXPECT_TRUE(payload.IsEmpty());
  EXPECT_EQ("42", Evaluate("second()", true));
  EXPECT_EQ("43", Evaluate("api.create(43)()", true));
  EXPECT_EQ("42", Evaluate("second()", true));
}

TEST_F(XenonContextBridgeTest, ReleasedCallbackFreesMainWorldClosure) {
  Evaluate(R"JS(
    let callback;
    contextBridge.exposeInMainWorld('api', {
      remember: value => { callback = value; },
      get: () => callback,
      call: () => callback(),
      release: () => { callback = null; },
    });
  )JS");
  Evaluate(R"JS(
    var payload = {value: 42};
    var original = ((value) => () => value.value)(payload);
    api.remember(original);
  )JS",
           true);
  auto payload = TrackGlobal("payload", true);
  Evaluate("payload = null", true);
  CollectGarbage();
  EXPECT_FALSE(payload.IsEmpty());
  EXPECT_EQ("true", Evaluate("api.get() === original", true));
  Evaluate("original = null", true);
  CollectGarbage();
  EXPECT_FALSE(payload.IsEmpty());
  EXPECT_EQ("42", Evaluate("api.call()", true));
  Evaluate("api.release()", true);
  CollectGarbage();
  EXPECT_TRUE(payload.IsEmpty());
}

TEST_F(XenonContextBridgeTest, ShutdownReleasesClosureBehindRetainedProxy) {
  Evaluate(R"JS(
    contextBridge.exposeInMainWorld('api', {
      create: () => {
        const payload = {value: 42};
        globalThis.observed = payload;
        return () => payload.value;
      },
    });
  )JS");
  Evaluate("var retained = api.create()", true);
  auto payload = TrackGlobal("observed");
  Evaluate("delete globalThis.observed");
  CollectGarbage();
  EXPECT_FALSE(payload.IsEmpty());
  bridge_->Shutdown();
  CollectGarbage();
  EXPECT_TRUE(payload.IsEmpty());
  EXPECT_EQ(
      "true",
      Evaluate("try { retained(); false } catch (e) { e instanceof Error }",
               true));
}

TEST_F(XenonContextBridgeTest,
       MainWorldExecutionUsesMainGlobalsAndCopiesResult) {
  Evaluate(
      "globalThis.location = {href: 'chrome-extension://fixture/worker.js'}",
      true);
  EXPECT_EQ("chrome-extension://fixture/worker.js", Evaluate(R"JS(
    contextBridge.executeInMainWorld({func: () => location.href})
  )JS"));
  EXPECT_EQ("7", Evaluate(R"JS(
    contextBridge.executeInMainWorld({func: (a, b) => a + b, args: [3, 4]})
  )JS"));
  EXPECT_EQ("true", Evaluate(R"JS(
    const hidden = 1;
    try {
      contextBridge.executeInMainWorld({func: () => hidden});
      false;
    } catch (error) { error instanceof Error; }
  )JS"));
}

TEST_F(XenonContextBridgeTest, RejectsReplacementAndRevokesRetainedFunctions) {
  Evaluate("contextBridge.exposeInMainWorld('api', {value: () => 1})");
  EXPECT_EQ("true", Evaluate("try {contextBridge.exposeInMainWorld('api', {}); "
                             "false} catch(e) {true}"));
  Evaluate("var retained = api.value", true);
  bridge_->Shutdown();
  EXPECT_EQ(
      "true",
      Evaluate("try {retained(); false} catch(e) {e instanceof Error}", true));
}

}  // namespace
}  // namespace xenon::ipc
