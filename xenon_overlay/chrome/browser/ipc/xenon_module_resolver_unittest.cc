// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/values.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

namespace xenon::ipc {
namespace {

// Resolve and execute real files through the production container. These
// checks cover observable Node contracts, including cache identity and module
// format, rather than substituting a package-map callback in the resolver.
class XenonModuleResolutionIntegrationTest : public gin::V8Test,
                                             public mojom::IpcRenderer {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }

  void SetUp() override {
    gin::V8Test::SetUp();
    timeout_ = std::make_unique<base::test::ScopedRunLoopTimeout>(
        FROM_HERE, base::Seconds(5));
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ASSERT_TRUE(base::NormalizeFilePath(temp_.GetPath(), &app_path_));
    container_ = std::make_unique<XenonIpcMainContainer>();
  }

  void TearDown() override {
    container_.reset();
    receiver_.reset();
    timeout_.reset();
    gin::V8Test::TearDown();
  }

  void Dispatch(const std::string& channel, base::Value arguments) override {}
  void DispatchPostMessage(const std::string& channel,
                           blink::TransferableMessage message) override {
    ADD_FAILURE() << "Unexpected MessagePort IPC: " << channel;
  }

  bool Write(const std::string& relative, const std::string& source) {
    const auto path =
        app_path_.Append(base::FilePath::FromUTF8Unsafe(relative));
    return base::CreateDirectory(path.DirName()) &&
           base::WriteFile(path, source);
  }

  bool Start(const std::string& manifest) {
    if (!Write("package.json", manifest)) {
      return false;
    }
    XenonIpcMainContainer::AppMainModule config;
    config.app_path = app_path_;
    if (!container_->Initialize(std::move(config))) {
      return false;
    }
    container_->AddRenderer("renderer", receiver_.BindNewPipeAndPassRemote(), 1,
                            1, 0, "about:blank", true);
    return true;
  }

  mojom::IpcResultPtr Result() {
    base::test::TestFuture<mojom::IpcResultPtr> future;
    container_->Invoke("renderer", "test:result",
                       base::Value(base::ListValue()), future.GetCallback());
    return future.Take();
  }

  base::ScopedTempDir temp_;
  base::FilePath app_path_;
  mojo::Receiver<mojom::IpcRenderer> receiver_{this};
  std::unique_ptr<XenonIpcMainContainer> container_;
  std::unique_ptr<base::test::ScopedRunLoopTimeout> timeout_;
};

TEST_F(XenonModuleResolutionIntegrationTest,
       NativeWindowsPathsAreNotEsmFileUrls) {
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    ipcMain.handle('test:result', async () => {
      const result = {};
      const path = 'C:\\fixture\\entry.mjs';
      try { await import(path); } catch (error) { result.dynamic = error.code; }
      return result;
    });
  )JS"));
  ASSERT_TRUE(Start(R"({"type":"module","main":"main.mjs"})"))
      << container_->startup_error();
  auto result = Result();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  ASSERT_TRUE(value.FindString("dynamic"));
  EXPECT_EQ("ERR_UNSUPPORTED_ESM_URL_SCHEME", *value.FindString("dynamic"));
}

TEST_F(XenonModuleResolutionIntegrationTest,
       PackageImportSubpathsPreserveUrlEncodingQueryAndFragment) {
  ASSERT_TRUE(
      Write("node_modules/dependency/package.json", R"({"type":"module"})"));
  ASSERT_TRUE(Write("node_modules/dependency/value name.mjs", R"JS(
    globalThis.instances = (globalThis.instances || 0) + 1;
    export const instance = globalThis.instances;
    export const url = import.meta.url;
  )JS"));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import * as first from '#one';
    import * as repeated from '#one-again';
    import * as second from '#two';
    ipcMain.handle('test:result', async () => ({
      same: first === repeated && first === await import('#one'),
      distinct: first !== second, first: first.instance, second: second.instance,
      firstUrl: first.url, secondUrl: second.url,
      resolved: import.meta.resolve('#one') === first.url,
    }));
  )JS"));
  ASSERT_TRUE(Start(R"({"type":"module","main":"main.mjs","imports":{
    "#one":"dependency/value%20name.mjs?flavor=one#selected",
    "#one-again":"dependency/value%20name.mjs?flavor=one#selected",
    "#two":"dependency/value%20name.mjs?flavor=two#other"
  }})"))
      << container_->startup_error();
  auto result = Result();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  EXPECT_EQ(true, value.FindBool("same"));
  EXPECT_EQ(true, value.FindBool("distinct"));
  EXPECT_EQ(true, value.FindBool("resolved"));
  EXPECT_EQ(1, value.FindInt("first"));
  EXPECT_EQ(2, value.FindInt("second"));
  ASSERT_TRUE(value.FindString("firstUrl"));
  EXPECT_TRUE(
      value.FindString("firstUrl")
          ->ends_with("/dependency/value%20name.mjs?flavor=one#selected"));
  ASSERT_TRUE(value.FindString("secondUrl"));
  EXPECT_TRUE(value.FindString("secondUrl")
                  ->ends_with("/dependency/value%20name.mjs?flavor=two#other"));
}

TEST_F(XenonModuleResolutionIntegrationTest,
       RequirePackageImportsDecodeUrlSubpathsButBareRequiresKeepLiteralNames) {
  ASSERT_TRUE(
      Write("node_modules/dependency/package.json", R"({"type":"commonjs"})"));
  ASSERT_TRUE(Write("node_modules/dependency/value name.cjs",
                    "module.exports = {answer: 42};"));
  ASSERT_TRUE(Write("node_modules/dependency/value%20name.cjs",
                    "module.exports = {answer: 99};"));
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    ipcMain.handle('test:result', () => {
      const mapped = require('#value');
      return {mapped: mapped.answer,
        same: mapped === require('dependency/value name.cjs'),
        literal: require('dependency/value%20name.cjs').answer};
    });
  )JS"));
  ASSERT_TRUE(Start(R"({"main":"main.cjs","imports":{
    "#value":"dependency/value%20name.cjs?feature=one#selected"
  }})"))
      << container_->startup_error();
  auto result = Result();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  EXPECT_EQ(42, value.FindInt("mapped"));
  EXPECT_EQ(99, value.FindInt("literal"));
  EXPECT_EQ(true, value.FindBool("same"));
}

TEST_F(XenonModuleResolutionIntegrationTest,
       PackageImportSubpathsRejectEncodedSeparators) {
  ASSERT_TRUE(Write("node_modules/dependency/package.json", "{}"));
  ASSERT_TRUE(Write("node_modules/dependency/dir/value.cjs",
                    "module.exports = 'must not load';"));
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    ipcMain.handle('test:result', async () => {
      const result = {};
      for (const name of ['#slash', '#backslash']) {
        try { require(name); } catch (error) { result[name] = error.code; }
        try { await import(name); }
        catch (error) { result[name + '-import'] = error.code; }
      }
      return result;
    });
  )JS"));
  ASSERT_TRUE(Start(R"({"main":"main.cjs","imports":{
    "#slash":"dependency/dir%2Fvalue.cjs",
    "#backslash":"dependency/dir%5Cvalue.cjs"
  }})"))
      << container_->startup_error();
  auto result = Result();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  for (const char* key :
       {"#slash", "#backslash", "#slash-import", "#backslash-import"}) {
    ASSERT_TRUE(value.FindString(key));
    EXPECT_EQ("ERR_INVALID_MODULE_SPECIFIER", *value.FindString(key)) << key;
  }
}

TEST_F(XenonModuleResolutionIntegrationTest,
       RequireUnknownAndMissingExtensionsRemainCommonJsInsideModuleScope) {
  ASSERT_TRUE(Write("value.custom", "module.exports = {answer: 41};"));
  ASSERT_TRUE(Write("increment", "module.exports = {answer: 1};"));
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    ipcMain.handle('test:result', () => {
      const value = require('./value.custom');
      const increment = require('./increment');
      return {answer: value.answer + increment.answer,
        same: value === require('./value.custom') &&
          increment === require('./increment')};
    });
  )JS"));
  ASSERT_TRUE(Start(R"({"type":"module","main":"main.cjs"})"))
      << container_->startup_error();
  auto result = Result();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  EXPECT_EQ(42, result->value.GetDict().FindInt("answer"));
  EXPECT_EQ(true, result->value.GetDict().FindBool("same"));
}

TEST_F(XenonModuleResolutionIntegrationTest,
       ExtensionlessImportsUseTheirOwnPackageScope) {
  ASSERT_TRUE(Write("value", "export const answer = 40;"));
  ASSERT_TRUE(Write("legacy/package.json", R"({"type":"commonjs"})"));
  ASSERT_TRUE(Write("legacy/increment", "module.exports = {answer: 2};"));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import {answer} from './value';
    import increment from './legacy/increment';
    ipcMain.handle('test:result', async () => ({
      answer: answer + increment.answer,
      same: (await import('./value')).answer === answer &&
        (await import('./legacy/increment')).default === increment,
    }));
  )JS"));
  ASSERT_TRUE(Start(R"({"type":"module","main":"main.mjs"})"))
      << container_->startup_error();
  auto result = Result();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  EXPECT_EQ(42, result->value.GetDict().FindInt("answer"));
  EXPECT_EQ(true, result->value.GetDict().FindBool("same"));
}

TEST_F(XenonModuleResolutionIntegrationTest,
       CommonJsLookupOverridesShareParentsAndRespectLiveSearchOrder) {
  ASSERT_TRUE(
      Write("vendor-one/dependency/index.cjs", "module.exports={value:1};"));
  ASSERT_TRUE(
      Write("vendor-one/dependency/package.json", R"({"main":"index.cjs"})"));
  ASSERT_TRUE(
      Write("vendor-two/dependency/index.cjs", "module.exports={value:2};"));
  ASSERT_TRUE(
      Write("vendor-two/dependency/package.json", R"({"main":"index.cjs"})"));
  ASSERT_TRUE(Write("nested.cjs", "module.exports=require('dependency');"));
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    const Module = require('node:module');
    const path = require('node:path');
    const one = path.join(__dirname, 'vendor-one');
    const two = path.join(__dirname, 'vendor-two');
    const original = Module._resolveLookupPaths;
    const result = {fields: module.path === __dirname && Array.isArray(module.paths)};
    module.paths.unshift(one);
    const first = require('dependency');
    result.defaultIdentity = first === require('dependency');
    module.paths[0] = two;
    result.changedParentPaths = require('dependency').value === 2;
    module.paths[0] = one;
    result.originalIdentity = require('dependency') === first;
    const parents = [];
    let receiver = true;
    Module._resolveLookupPaths = function(request, parent) {
      receiver = receiver && this === Module;
      if (request === 'dependency') { parents.push(parent); return [two]; }
      return original.call(this, request, parent);
    };
    result.override = require('dependency').value === 2;
    result.parentIdentity = parents[0] === module;
    result.resolve = require.resolve('dependency') === path.join(two, 'dependency', 'index.cjs');
    result.resolvePaths = require.resolve.paths('dependency')[0] === two;
    const bound = Module.createRequire(path.join(__dirname, 'synthetic.cjs'));
    parents.length = 0;
    bound.resolve('dependency');
    bound.resolve.paths('dependency');
    const synthetic = parents[0];
    result.syntheticIdentity = synthetic === parents[1] && synthetic !== module &&
      synthetic.path === __dirname && Array.isArray(synthetic.paths);
    const another = Module.createRequire(path.join(__dirname, 'synthetic.cjs'));
    another.resolve('dependency');
    result.separateParents = parents[2] !== synthetic;
    const hooks = Module.registerHooks({resolve(specifier, context, next) {
      return next(specifier, context);
    }});
    result.hookDefault = bound('dependency').value === 2 && require('./nested.cjs').value === 2;
    hooks.deregister();
    Module._resolveLookupPaths = function(request, parent) {
      return request === 'dependency' ? [one] : original.call(this, request, parent);
    };
    result.replacedOverride = require('dependency') === first;
    result.receiver = receiver;
    Module._resolveLookupPaths = original;
    ipcMain.handle('test:result', () => result);
  )JS"));
  ASSERT_TRUE(Start(R"({"main":"main.cjs"})")) << container_->startup_error();
  auto result = Result();
  ASSERT_TRUE(result->success) << result->error;
  for (const char* field :
       {"fields", "defaultIdentity", "changedParentPaths", "originalIdentity",
        "override", "parentIdentity", "resolve", "resolvePaths",
        "syntheticIdentity", "separateParents", "hookDefault",
        "replacedOverride", "receiver"}) {
    EXPECT_EQ(true, result->value.GetDict().FindBool(field)) << field;
  }
}

TEST_F(XenonModuleResolutionIntegrationTest,
       CommonJsLookupOverridesPreserveExportsAndApplicationAuthority) {
  base::ScopedTempDir outside;
  ASSERT_TRUE(outside.CreateUniqueTempDir());
  ASSERT_TRUE(
      base::CreateDirectory(outside.GetPath().AppendASCII("dependency")));
  ASSERT_TRUE(base::WriteFile(
      outside.GetPath().AppendASCII("dependency").AppendASCII("index.js"),
      "module.exports='must not execute';"));
  ASSERT_TRUE(Write("outside.json", *base::WriteJson(base::Value(
                                        outside.GetPath().AsUTF8Unsafe()))));
  ASSERT_TRUE(
      Write("vendor/dependency/package.json", R"({"exports":"./public.cjs"})"));
  ASSERT_TRUE(Write("vendor/dependency/public.cjs", "module.exports=42;"));
  ASSERT_TRUE(
      Write("vendor/dependency/private.cjs", "module.exports='hidden';"));
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    const Module = require('node:module');
    const path = require('node:path');
    const outside = require('./outside.json');
    const original = Module._resolveLookupPaths;
    let paths = [path.join(__dirname, 'vendor')];
    Module._resolveLookupPaths = function(request, parent) {
      return request.startsWith('dependency') ? paths : original.call(this, request, parent);
    };
    ipcMain.handle('test:result', async () => {
      const result = {answer: require('dependency')};
      try { require('dependency/private.cjs'); } catch (e) { result.private = e.code; }
      try { await import('dependency'); } catch (e) { result.esm = e.code; }
      try { require('#mapped'); } catch (e) { result.mapped = e.code; }
      paths = [outside];
      try { require('dependency'); } catch (e) { result.outside = e.code; }
      try { require.resolve('dependency'); } catch (e) { result.outsideResolve = e.code; }
      return result;
    });
  )JS"));
  ASSERT_TRUE(
      Start(R"({"main":"main.cjs","imports":{"#mapped":"dependency"}})"))
      << container_->startup_error();
  auto result = Result();
  ASSERT_TRUE(result->success) << result->error;
  const auto& value = result->value.GetDict();
  EXPECT_EQ(42, value.FindInt("answer"));
  for (const auto& [key, expected] :
       {std::pair{"private", "ERR_PACKAGE_PATH_NOT_EXPORTED"},
        std::pair{"esm", "ERR_MODULE_NOT_FOUND"},
        std::pair{"mapped", "MODULE_NOT_FOUND"},
        std::pair{"outside", "MODULE_NOT_FOUND"},
        std::pair{"outsideResolve", "MODULE_NOT_FOUND"}}) {
    ASSERT_TRUE(value.FindString(key));
    EXPECT_EQ(expected, *value.FindString(key)) << key;
  }
}

TEST_F(XenonModuleResolutionIntegrationTest,
       CommonJsLookupOverridesHonorNodeModulePathsAndPreserveThrownValues) {
  ASSERT_TRUE(Write("vendor/dependency/index.js", "module.exports=42;"));
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    const Module = require('node:module');
    const path = require('node:path');
    const originalPaths = Module._nodeModulePaths;
    let sameReceiver = false;
    Module._nodeModulePaths = function(directory) {
      sameReceiver = this === Module && directory === __dirname;
      return [path.join(directory, 'vendor')];
    };
    const bound = Module.createRequire(path.join(__dirname, 'synthetic.cjs'));
    Module._nodeModulePaths = originalPaths;
    const result = {customPaths: bound('dependency') === 42, sameReceiver};
    const marker = {source: 'lookup'};
    Module._resolveLookupPaths = () => { throw marker; };
    for (const [name, operation] of [['require', () => bound('dependency')],
      ['resolve', () => bound.resolve('dependency')],
      ['paths', () => bound.resolve.paths('dependency')]]) {
      try { operation(); } catch (error) { result[name] = error === marker; }
    }
    ipcMain.handle('test:result', () => result);
  )JS"));
  ASSERT_TRUE(Start(R"({"main":"main.cjs"})")) << container_->startup_error();
  auto result = Result();
  ASSERT_TRUE(result->success) << result->error;
  for (const char* key :
       {"customPaths", "sameReceiver", "require", "resolve", "paths"}) {
    EXPECT_EQ(true, result->value.GetDict().FindBool(key)) << key;
  }
}

}  // namespace
}  // namespace xenon::ipc
