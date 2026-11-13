// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_esm_module_loader.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "gin/converter.h"
#include "gin/test/v8_test.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-json.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"

namespace xenon::ipc {
namespace {

using Loader = XenonEsmModuleLoader;

class XenonEsmModuleLoaderTest : public gin::V8Test {
 protected:
  struct Fixture {
    std::string source;
    Loader::Format format = Loader::Format::kModule;
    std::vector<std::string> exports;
  };

  void SetUp() override {
    gin::V8Test::SetUp();
    v8::HandleScope handles(isolate());
    loader_ = std::make_unique<Loader>(
        isolate(), context(),
        base::BindRepeating(&XenonEsmModuleLoaderTest::Resolve,
                            base::Unretained(this)),
        base::BindRepeating(&XenonEsmModuleLoaderTest::Read,
                            base::Unretained(this)),
        base::BindRepeating(&XenonEsmModuleLoaderTest::LoadValue,
                            base::Unretained(this)),
        base::BindRepeating(&XenonEsmModuleLoaderTest::Names,
                            base::Unretained(this)));
  }

  void TearDown() override {
    loader_.reset();
    gin::V8Test::TearDown();
  }

  v8::Isolate* isolate() { return instance_->isolate(); }
  v8::Local<v8::Context> context() { return context_.Get(isolate()); }
  v8::Local<v8::String> String(const std::string& value) {
    return gin::StringToV8(isolate(), value).As<v8::String>();
  }

  Loader::ModuleInfo Info(const std::string& url) {
    Loader::ModuleInfo info;
    info.url = url;
    if (fixtures_.contains(url)) {
      info.format = fixtures_.at(url).format;
    }
    if (GURL(url).SchemeIsFile()) {
      info.filename = base::FilePath::FromUTF8Unsafe(GURL(url).path());
    }
    return info;
  }

  bool Resolve(const std::string& request,
               const std::string& parent,
               Loader::ResolutionMode mode,
               Loader::ModuleInfo* info,
               std::string* error) {
    const std::string url = request.starts_with("node:")
                                ? request
                                : GURL(parent).Resolve(request).spec();
    if (!fixtures_.contains(url) &&
        mode != Loader::ResolutionMode::kMetaResolve) {
      *error = "ERR_MODULE_NOT_FOUND: Missing fixture " + url;
      return false;
    }
    *info = Info(url);
    return true;
  }

  bool Read(const Loader::ModuleInfo& info,
            std::string* source,
            std::string* error) {
    auto found = fixtures_.find(info.url);
    if (found == fixtures_.end()) {
      *error = "ERR_MODULE_NOT_FOUND: Missing source";
      return false;
    }
    ++read_counts_[info.url];
    *source = found->second.source;
    return true;
  }

  v8::MaybeLocal<v8::Value> Script(
      const std::string& source,
      const std::string& url = "file:///fixture/entry.cjs") {
    v8::ScriptOrigin origin(String(url));
    v8::Local<v8::Script> script;
    if (!v8::Script::Compile(context(), String(source), &origin)
             .ToLocal(&script)) {
      return {};
    }
    return script->Run(context());
  }

  v8::MaybeLocal<v8::Value> LoadValue(const Loader::ModuleInfo& info,
                                      std::string* error) {
    const auto& fixture = fixtures_.at(info.url);
    if (fixture.format == Loader::Format::kJson) {
      return v8::JSON::Parse(context(), String(fixture.source));
    }
    return Script(fixture.source, info.url);
  }

  bool Names(const Loader::ModuleInfo& info,
             std::vector<std::string>* names,
             std::string* error) {
    *names = fixtures_.at(info.url).exports;
    return true;
  }

  std::string Text(v8::Local<v8::Value> value) {
    v8::String::Utf8Value text(isolate(), value);
    return *text ? std::string(*text, text.length()) : std::string();
  }

  std::string ErrorCode(v8::Local<v8::Value> error) {
    v8::Local<v8::Value> code;
    if (!error->IsObject() || !error.As<v8::Object>()
                                   ->Get(context(), String("code"))
                                   .ToLocal(&code)) {
      return {};
    }
    return Text(code);
  }

  std::map<std::string, Fixture> fixtures_;
  std::map<std::string, int> read_counts_;
  std::unique_ptr<Loader> loader_;
};

TEST_F(XenonEsmModuleLoaderTest,
       StaticCyclesKeepLiveBindingsAndModuleIdentity) {
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/a.js"] = {
      "import {read} from './b.js'; export let count=1;"
      "export function inc(){++count;} export {read};"};
  fixtures_["file:///fixture/b.js"] = {
      "import {count} from './a.js'; export function read(){return count;}"};
  std::string error;
  v8::Local<v8::Value> first;
  ASSERT_TRUE(
      loader_->Require(Info("file:///fixture/a.js"), &error).ToLocal(&first))
      << error;
  ASSERT_TRUE(context()
                  ->Global()
                  ->Set(context(), String("ns"), first)
                  .FromMaybe(false));
  v8::Local<v8::Value> value;
  ASSERT_TRUE(Script("ns.inc(); ns.read()").ToLocal(&value));
  EXPECT_EQ(2, value->Int32Value(context()).FromMaybe(-1));
  v8::Local<v8::Value> second;
  ASSERT_TRUE(
      loader_->Require(Info("file:///fixture/a.js"), &error).ToLocal(&second));
  EXPECT_TRUE(first->StrictEquals(second));
  EXPECT_EQ(1, read_counts_["file:///fixture/a.js"]);
  EXPECT_EQ(1, read_counts_["file:///fixture/b.js"]);
}

TEST_F(XenonEsmModuleLoaderTest,
       TopLevelAwaitCompletesDynamicImportAfterSettlement) {
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/async.js"] = {
      "export const answer=await new "
      "Promise(resolve=>globalThis.release=resolve);"};
  v8::Local<v8::Value> value;
  ASSERT_TRUE(Script("import('./async.js')").ToLocal(&value));
  ASSERT_TRUE(value->IsPromise());
  auto promise = value.As<v8::Promise>();
  EXPECT_EQ(v8::Promise::kPending, promise->State());
  ASSERT_TRUE(Script("release(42)").ToLocal(&value));
  isolate()->PerformMicrotaskCheckpoint();
  ASSERT_EQ(v8::Promise::kFulfilled, promise->State());
  ASSERT_TRUE(promise->Result()
                  .As<v8::Object>()
                  ->Get(context(), String("answer"))
                  .ToLocal(&value));
  EXPECT_EQ(42, value->Int32Value(context()).FromMaybe(-1));
}

TEST_F(XenonEsmModuleLoaderTest, RequireAsyncGraphRejectsBeforeExecutingIt) {
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/async.js"] = {
      "globalThis.ran=true; await Promise.resolve(); export const value=1;"};
  std::string error;
  {
    v8::TryCatch caught(isolate());
    EXPECT_TRUE(
        loader_->Require(Info("file:///fixture/async.js"), &error).IsEmpty());
    ASSERT_TRUE(caught.HasCaught());
    EXPECT_EQ("ERR_REQUIRE_ASYNC_MODULE", ErrorCode(caught.Exception()));
  }
  v8::Local<v8::Value> value;
  ASSERT_TRUE(Script("typeof ran").ToLocal(&value));
  EXPECT_EQ("undefined", Text(value));
}

TEST_F(XenonEsmModuleLoaderTest, RequireUsesExplicitCommonJsInteropExport) {
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/interop.js"] = {
      "const value={answer:42}; export {value as 'module.exports'};"};
  std::string error;
  v8::Local<v8::Value> value;
  ASSERT_TRUE(loader_->Require(Info("file:///fixture/interop.js"), &error)
                  .ToLocal(&value));
  ASSERT_TRUE(
      value.As<v8::Object>()->Get(context(), String("answer")).ToLocal(&value));
  EXPECT_EQ(42, value->Int32Value(context()).FromMaybe(-1));
}

TEST_F(XenonEsmModuleLoaderTest,
       RequireDefaultFacadePreservesLiveBindingsAndCache) {
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/default.js"] = {
      "globalThis.runs=(globalThis.runs||0)+1;"
      "export let count=1; let value=2; export {value as default};"
      "export function inc(){++count;++value;}"};
  std::string error;
  v8::Local<v8::Value> required;
  ASSERT_TRUE(loader_->Require(Info("file:///fixture/default.js"), &error)
                  .ToLocal(&required))
      << error;
  ASSERT_TRUE(context()
                  ->Global()
                  ->Set(context(), String("required"), required)
                  .FromMaybe(false));
  v8::Local<v8::Value> value;
  ASSERT_TRUE(Script("required.__esModule===true && required.default===2 && "
                     "Object.getPrototypeOf(required)===null && "
                     "!Object.isExtensible(required)")
                  .ToLocal(&value));
  EXPECT_TRUE(value->IsTrue());
  ASSERT_TRUE(
      Script("required.inc(); required.count===2 && required.default===3")
          .ToLocal(&value));
  EXPECT_TRUE(value->IsTrue());
  v8::Local<v8::Value> repeated;
  ASSERT_TRUE(loader_->Require(Info("file:///fixture/default.js"), &error)
                  .ToLocal(&repeated));
  EXPECT_TRUE(required->StrictEquals(repeated));
  ASSERT_TRUE(Script("import('./default.js')").ToLocal(&value));
  auto imported = value.As<v8::Promise>();
  isolate()->PerformMicrotaskCheckpoint();
  ASSERT_EQ(v8::Promise::kFulfilled, imported->State());
  EXPECT_FALSE(imported->Result()->StrictEquals(required));
  EXPECT_FALSE(imported->Result()
                   .As<v8::Object>()
                   ->HasOwnProperty(context(), String("__esModule"))
                   .FromMaybe(true));
  ASSERT_TRUE(Script("runs").ToLocal(&value));
  EXPECT_EQ(1, value->Int32Value(context()).FromMaybe(-1));
}

TEST_F(XenonEsmModuleLoaderTest,
       RequireDoesNotReplaceApplicationEsModuleMarker) {
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/marker.js"] = {
      "export default 42; export const __esModule=false;"};
  fixtures_["file:///fixture/named.js"] = {"export const answer=42;"};
  std::string error;
  v8::Local<v8::Value> value;
  ASSERT_TRUE(loader_->Require(Info("file:///fixture/marker.js"), &error)
                  .ToLocal(&value));
  ASSERT_TRUE(value.As<v8::Object>()
                  ->Get(context(), String("__esModule"))
                  .ToLocal(&value));
  EXPECT_TRUE(value->IsFalse());
  ASSERT_TRUE(loader_->Require(Info("file:///fixture/named.js"), &error)
                  .ToLocal(&value));
  EXPECT_FALSE(value.As<v8::Object>()
                   ->HasOwnProperty(context(), String("__esModule"))
                   .FromMaybe(true));
}

TEST_F(XenonEsmModuleLoaderTest,
       DynamicImportRejectsResolutionAndEvaluationErrors) {
  v8::HandleScope handles(isolate());
  v8::Local<v8::Value> value;
  ASSERT_TRUE(Script("import('./missing.js')").ToLocal(&value));
  auto missing = value.As<v8::Promise>();
  missing->MarkAsHandled();
  isolate()->PerformMicrotaskCheckpoint();
  ASSERT_EQ(v8::Promise::kRejected, missing->State());
  EXPECT_EQ("ERR_MODULE_NOT_FOUND", ErrorCode(missing->Result()));

  fixtures_["file:///fixture/error.js"] = {
      "await Promise.reject(Object.assign(new "
      "Error('failure'),{code:'FIXTURE'}));"};
  ASSERT_TRUE(Script("import('./error.js')").ToLocal(&value));
  auto rejected = value.As<v8::Promise>();
  rejected->MarkAsHandled();
  isolate()->PerformMicrotaskCheckpoint();
  ASSERT_EQ(v8::Promise::kRejected, rejected->State());
  EXPECT_EQ("FIXTURE", ErrorCode(rejected->Result()));
}

TEST_F(XenonEsmModuleLoaderTest, CommonJsLinkingDoesNotExecuteSideEffects) {
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/legacy.cjs"] = {
      "globalThis.cjsRan=true; ({answer:42})",
      Loader::Format::kCommonJs,
      {"answer"}};
  fixtures_["file:///fixture/bad.js"] = {
      "import {absent} from './legacy.cjs'; export {absent};"};
  std::string error;
  {
    v8::TryCatch caught(isolate());
    EXPECT_TRUE(
        loader_->Evaluate(Info("file:///fixture/bad.js"), &error).IsEmpty());
    EXPECT_TRUE(caught.HasCaught());
  }
  v8::Local<v8::Value> value;
  ASSERT_TRUE(Script("typeof cjsRan").ToLocal(&value));
  EXPECT_EQ("undefined", Text(value));

  fixtures_["file:///fixture/good.js"] = {
      "import legacy,{answer} from './legacy.cjs'; export const "
      "valid=legacy.answer===answer;"};
  ASSERT_TRUE(
      loader_->Require(Info("file:///fixture/good.js"), &error).ToLocal(&value))
      << error;
  ASSERT_TRUE(
      value.As<v8::Object>()->Get(context(), String("valid")).ToLocal(&value));
  EXPECT_TRUE(value->IsTrue());
}

TEST_F(XenonEsmModuleLoaderTest, JsonAttributesAreRequiredAndTypeChecked) {
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/data.json"] = {"{\"answer\":42}",
                                            Loader::Format::kJson};
  fixtures_["file:///fixture/json.js"] = {
      "import data from './data.json' with {type:'json'}; export const "
      "answer=data.answer;"};
  std::string error;
  v8::Local<v8::Value> value;
  ASSERT_TRUE(
      loader_->Require(Info("file:///fixture/json.js"), &error).ToLocal(&value))
      << error;
  ASSERT_TRUE(
      value.As<v8::Object>()->Get(context(), String("answer")).ToLocal(&value));
  EXPECT_EQ(42, value->Int32Value(context()).FromMaybe(-1));
  ASSERT_TRUE(Script("import('./data.json')").ToLocal(&value));
  auto rejected = value.As<v8::Promise>();
  rejected->MarkAsHandled();
  isolate()->PerformMicrotaskCheckpoint();
  ASSERT_EQ(v8::Promise::kRejected, rejected->State());
  EXPECT_EQ("ERR_IMPORT_ATTRIBUTE_MISSING", ErrorCode(rejected->Result()));
  ASSERT_TRUE(
      Script("import('./data.json',{with:{type:'json'}})").ToLocal(&value));
  auto loaded = value.As<v8::Promise>();
  isolate()->PerformMicrotaskCheckpoint();
  EXPECT_EQ(v8::Promise::kFulfilled, loaded->State());
}

TEST_F(XenonEsmModuleLoaderTest,
       ImportMetaUsesModuleIdentityAndResolutionMode) {
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/meta.js"] = {
      "export const url=import.meta.url;"
      "export const filename=import.meta.filename;"
      "export const resolved=import.meta.resolve('./missing.js');"};
  std::string error;
  v8::Local<v8::Value> ns;
  ASSERT_TRUE(
      loader_->Require(Info("file:///fixture/meta.js"), &error).ToLocal(&ns));
  v8::Local<v8::Value> value;
  ASSERT_TRUE(
      ns.As<v8::Object>()->Get(context(), String("url")).ToLocal(&value));
  EXPECT_EQ("file:///fixture/meta.js", Text(value));
  ASSERT_TRUE(
      ns.As<v8::Object>()->Get(context(), String("filename")).ToLocal(&value));
  EXPECT_EQ(Info("file:///fixture/meta.js").filename.AsUTF8Unsafe(),
            Text(value));
  ASSERT_TRUE(
      ns.As<v8::Object>()->Get(context(), String("resolved")).ToLocal(&value));
  EXPECT_EQ("file:///fixture/missing.js", Text(value));
}

TEST_F(XenonEsmModuleLoaderTest,
       BuiltinNamespacesContainPublicEnumerableExports) {
  v8::HandleScope handles(isolate());
  fixtures_["node:fixture"] = {
      "Object.defineProperty({answer:42},'internal',{value:7})",
      Loader::Format::kBuiltin};
  fixtures_["file:///fixture/builtin.js"] = {
      "import value,{answer} from 'node:fixture';"
      "import * as all from 'node:fixture';"
      "export const valid=value.answer===answer && !('internal' in all);"};
  std::string error;
  v8::Local<v8::Value> value;
  ASSERT_TRUE(loader_->Require(Info("file:///fixture/builtin.js"), &error)
                  .ToLocal(&value));
  ASSERT_TRUE(
      value.As<v8::Object>()->Get(context(), String("valid")).ToLocal(&value));
  EXPECT_TRUE(value->IsTrue());
}

TEST_F(XenonEsmModuleLoaderTest,
       SourcePhaseImportsFailWithoutCallingNullCallbacks) {
  v8::V8::SetFlagsFromString("--js-source-phase-imports");
  v8::HandleScope handles(isolate());
  fixtures_["file:///fixture/source.js"] = {
      "import source wasm from './fixture.wasm'; export {wasm};"};
  std::string error;
  {
    v8::TryCatch caught(isolate());
    EXPECT_TRUE(
        loader_->Require(Info("file:///fixture/source.js"), &error).IsEmpty());
    ASSERT_TRUE(caught.HasCaught());
    EXPECT_EQ("ERR_NOT_SUPPORTED", ErrorCode(caught.Exception()));
  }
  v8::Local<v8::Value> value;
  ASSERT_TRUE(Script("import.source('./fixture.wasm')").ToLocal(&value));
  auto rejected = value.As<v8::Promise>();
  rejected->MarkAsHandled();
  isolate()->PerformMicrotaskCheckpoint();
  ASSERT_EQ(v8::Promise::kRejected, rejected->State());
  EXPECT_EQ("ERR_NOT_SUPPORTED", ErrorCode(rejected->Result()));
}

}  // namespace
}  // namespace xenon::ipc
