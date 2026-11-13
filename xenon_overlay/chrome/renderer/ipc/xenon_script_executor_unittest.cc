// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/ipc/xenon_script_executor.h"

#include "gin/converter.h"
#include "gin/test/v8_test.h"
#include "v8/include/v8.h"

namespace xenon::ipc {
namespace {

class XenonScriptExecutorTest : public gin::V8Test {
 public:
  void SetUp() override {
    gin::V8Test::SetUp();
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    caller_.Reset(isolate, v8::Context::New(isolate));
    executor_ = base::MakeRefCounted<XenonScriptExecutor>(isolate);
    Evaluate("globalThis.realm = 'main'", true);
    Evaluate("globalThis.realm = 'preload'; globalThis.process = {secret: 42}");
  }
  void TearDown() override {
    executor_->Shutdown();
    executor_.reset();
    caller_.Reset();
    gin::V8Test::TearDown();
  }

 protected:
  std::string Evaluate(const std::string& source, bool main = false) {
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto context = main ? context_.Get(isolate) : caller_.Get(isolate);
    v8::Context::Scope scope(context);
    v8::TryCatch caught(isolate);
    v8::Local<v8::Script> script;
    v8::Local<v8::Value> result;
    if (!v8::Script::Compile(context,
                             gin::StringToV8(isolate, source).As<v8::String>())
             .ToLocal(&script) ||
        !script->Run(context).ToLocal(&result)) {
      v8::String::Utf8Value error(isolate, caught.Exception());
      ADD_FAILURE() << (*error ? *error : "Script failed");
      return {};
    }
    v8::String::Utf8Value text(isolate, result);
    return *text ? std::string(*text, text.length()) : std::string();
  }

  void Start(const std::string& source,
             uint64_t id = 1,
             const std::string& name = "result",
             bool same_world = false) {
    auto* isolate = instance_->isolate();
    v8::HandleScope handles(isolate);
    auto main = context_.Get(isolate);
    auto target = same_world ? main : caller_.Get(isolate);
    v8::Context::Scope scope(target);
    v8::Local<v8::Promise> promise;
    ASSERT_TRUE(executor_->Execute(id, main, target, source).ToLocal(&promise));
    ASSERT_TRUE(target->Global()
                    ->CreateDataProperty(
                        target, gin::StringToV8(isolate, "request"), promise)
                    .FromMaybe(false));
    Evaluate("globalThis." + name +
                 " = {state: 'pending'}; request.then("
                 "value => { " +
                 name +
                 " = {state: 'fulfilled', value}; },"
                 "error => { " +
                 name + " = {state: 'rejected', error}; })",
             same_world);
    isolate->PerformMicrotaskCheckpoint();
  }

  scoped_refptr<XenonScriptExecutor> executor_;
  v8::Global<v8::Context> caller_;
};

TEST_F(XenonScriptExecutorTest,
       MainWorldExecutionPreservesCspAndClonesIntoCallerRealm) {
  auto* isolate = instance_->isolate();
  {
    v8::HandleScope handles(isolate);
    context_.Get(isolate)->AllowCodeGenerationFromStrings(false);
  }
  Start(
      "globalThis.changed = 7; ({realm, node: typeof process, data: {value: "
      "42}})");
  EXPECT_EQ("fulfilled", Evaluate("result.state"));
  EXPECT_EQ("main", Evaluate("result.value.realm"));
  EXPECT_EQ("undefined", Evaluate("result.value.node"));
  EXPECT_EQ("7", Evaluate("changed", true));
  EXPECT_EQ("undefined", Evaluate("typeof changed"));
  EXPECT_EQ(
      "true",
      Evaluate("Object.getPrototypeOf(result.value) === Object.prototype"));
  EXPECT_EQ(
      "true",
      Evaluate(
          "Object.getPrototypeOf(result.value.data) === Object.prototype"));
  // Embedding a script does not grant the page permission to use eval.
  EXPECT_EQ("EvalError",
            Evaluate("try { eval('1') } catch (e) { e.name }", true));
}

TEST_F(XenonScriptExecutorTest,
       AwaitsPromisesAndThenablesWithoutPagePromiseHelpers) {
  Evaluate(
      "Promise.resolve = () => { throw Error('replaced resolve'); };"
      "Promise.prototype.then = () => { throw Error('replaced then'); }",
      true);
  Start("new Promise(resolve => resolve({answer: 42}))");
  EXPECT_EQ("fulfilled:42",
            Evaluate("result.state + ':' + result.value.answer"));
  Start("({then(resolve) { resolve(43); }})", 2);
  EXPECT_EQ("fulfilled:43", Evaluate("result.state + ':' + result.value"));
}

TEST_F(XenonScriptExecutorTest, PreservesUndefinedBigIntBinaryAndCycles) {
  Start("void 0");
  EXPECT_EQ(
      "true",
      Evaluate("result.state === 'fulfilled' && result.value === undefined"));
  Start(
      "(() => { const value = {n: 42n, bytes: new Uint8Array([0,255]),"
      "map: new Map([['x', 7]])}; value.self = value; return value; })()",
      2);
  EXPECT_EQ(
      "true",
      Evaluate(
          "result.value.n === 42n && result.value.bytes instanceof Uint8Array "
          "&&"
          "result.value.bytes[1] === 255 && result.value.map.get('x') === 7 &&"
          "result.value.self === result.value"));
  Start("globalThis.original = {x: 1}; original", 3, "same", true);
  EXPECT_EQ("true",
            Evaluate("same.value !== original && same.value.x === 1", true));
}

TEST_F(XenonScriptExecutorTest,
       RejectsScriptSyntaxErrorsThrownValuesAndUncloneableResults) {
  Start("throw new TypeError('expected')");
  EXPECT_EQ(
      "true",
      Evaluate(
          "result.state === 'rejected' && result.error instanceof TypeError &&"
          "result.error.message === 'expected'"));
  Start("(", 2);
  EXPECT_EQ("true", Evaluate("result.error instanceof SyntaxError"));
  Start("Promise.reject(new RangeError('async expected'))", 3);
  EXPECT_EQ("true", Evaluate("result.error instanceof RangeError && "
                             "result.error.message === 'async expected'"));
  Start("Promise.reject(17)", 4);
  EXPECT_EQ("17", Evaluate("result.error"));
  Start("(() => 42)", 5);
  EXPECT_EQ(
      "true",
      Evaluate("result.state === 'rejected' && result.error instanceof Error"));
}

TEST_F(XenonScriptExecutorTest,
       CancellationReleasesRequestAndLateSettlementCannotReuseId) {
  Start("new Promise(resolve => { globalThis.oldResolve = resolve; })", 5,
        "oldResult");
  EXPECT_EQ("pending", Evaluate("oldResult.state"));
  executor_->Cancel(5);
  instance_->isolate()->PerformMicrotaskCheckpoint();
  EXPECT_EQ("rejected", Evaluate("oldResult.state"));
  Start("new Promise(resolve => { globalThis.newResolve = resolve; })", 5,
        "newResult");
  Evaluate("oldResolve('obsolete')", true);
  instance_->isolate()->PerformMicrotaskCheckpoint();
  EXPECT_EQ("pending", Evaluate("newResult.state"));
  Evaluate("newResolve('current')", true);
  instance_->isolate()->PerformMicrotaskCheckpoint();
  EXPECT_EQ("current", Evaluate("newResult.value"));
}

TEST_F(XenonScriptExecutorTest, ShutdownRejectsPendingAndRevokesExecution) {
  Start("new Promise(() => {})");
  executor_->Shutdown();
  instance_->isolate()->PerformMicrotaskCheckpoint();
  EXPECT_EQ("rejected", Evaluate("result.state"));
  auto* isolate = instance_->isolate();
  v8::HandleScope handles(isolate);
  EXPECT_TRUE(
      executor_->Execute(2, context_.Get(isolate), caller_.Get(isolate), "42")
          .IsEmpty());
}

}  // namespace
}  // namespace xenon::ipc
