// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

namespace xenon::ipc {
namespace {

class XenonIpcModuleLoaderTest : public gin::V8Test, public mojom::IpcRenderer {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }

  void SetUp() override {
    gin::V8Test::SetUp();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    app_path_ = temp_dir_.GetPath().AppendASCII("app");
    ASSERT_TRUE(base::CreateDirectory(app_path_));
    base::FilePath canonical_app_path;
    ASSERT_TRUE(base::NormalizeFilePath(app_path_, &canonical_app_path));
    app_path_ = std::move(canonical_app_path);
  }

  void TearDown() override {
    container_.reset();
    renderer_receiver_.reset();
    gin::V8Test::TearDown();
  }

  void Dispatch(const std::string& channel, base::Value arguments) override {}
  void DispatchPostMessage(const std::string& channel,
                           blink::TransferableMessage message) override {
    ADD_FAILURE() << "Unexpected MessagePort IPC: " << channel;
  }

  void WriteModule(const std::string& relative_path,
                   const std::string& source) {
    const base::FilePath path =
        app_path_.Append(base::FilePath::FromUTF8Unsafe(relative_path));
    ASSERT_TRUE(base::CreateDirectory(path.DirName()));
    ASSERT_TRUE(base::WriteFile(path, source));
  }

  void ExpectScript(const std::string& script,
                    XenonIpcMainContainer::NativeAddonHooks native_hooks = {}) {
    XenonIpcMainContainer::EmbeddedMainModule module;
    module.virtual_path = app_path_.AppendASCII("main.js");
    module.app_path = app_path_;
    module.source = R"JS(
      const {ipcMain} = require('electron');
      function errorCode(load) {
        try { load(); return 'no-error'; } catch (error) { return error.code; }
      }
      const result = (() => {
    )JS" + script + R"JS(
      })();
      ipcMain.on('test:module-result', event => { event.returnValue = result; });
    )JS";
    container_ = std::make_unique<XenonIpcMainContainer>();
    container_->SetNativeAddonHooks(std::move(native_hooks));
    ASSERT_TRUE(container_->Initialize(std::move(module)))
        << container_->startup_error();
    renderer_receiver_.reset();
    container_->AddRenderer("renderer",
                            renderer_receiver_.BindNewPipeAndPassRemote(), 1, 1,
                            0, "about:blank", true);
    auto result = container_->SendSync("renderer", "test:module-result",
                                       base::Value(base::ListValue()));
    ASSERT_TRUE(result->success) << result->error;
    ASSERT_TRUE(result->value.is_bool());
    EXPECT_TRUE(result->value.GetBool());
  }

  base::ScopedTempDir temp_dir_;
  base::FilePath app_path_;
  mojo::Receiver<mojom::IpcRenderer> renderer_receiver_{this};
  std::unique_ptr<XenonIpcMainContainer> container_;
};

TEST_F(XenonIpcModuleLoaderTest, BuiltinsHaveExactNamesAndSharedIdentity) {
  ExpectScript(R"JS(
    return require('fs') === require('node:fs') &&
        require('events') === require('node:events') &&
        ['tls', 'readline', 'zlib', 'http2', 'child_process', 'timers/promises', 'console'].every(name =>
            require(name) === require('node:' + name)) &&
        require('timers').promises === require('timers/promises') &&
        require.resolve('fs') === 'fs' &&
        require.resolve('node:fs') === 'node:fs' &&
        errorCode(() => require('node:electron')) === 'ERR_UNKNOWN_BUILTIN_MODULE' &&
        errorCode(() => require('node:FS')) === 'ERR_UNKNOWN_BUILTIN_MODULE' &&
        errorCode(() => require.resolve('node:electron')) ===
            'ERR_UNKNOWN_BUILTIN_MODULE' &&
        errorCode(() => require.resolve('node:not_a_builtin')) ===
            'ERR_UNKNOWN_BUILTIN_MODULE' &&
        errorCode(() => require('FS')) === 'MODULE_NOT_FOUND' &&
        errorCode(() => require(42)) === 'ERR_INVALID_ARG_TYPE' &&
        errorCode(() => require.resolve()) === 'ERR_INVALID_ARG_TYPE' &&
        errorCode(() => require('')) === 'ERR_INVALID_ARG_VALUE' &&
        errorCode(() => require.resolve('')) === 'ERR_INVALID_ARG_VALUE' &&
        errorCode(() => require('fs\0')) === 'ERR_INVALID_ARG_VALUE';
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, IsBuiltinUsesTheActualModuleRegistry) {
  ExpectScript(R"JS(
    const {isBuiltin} = require('node:module');
    return ['fs', 'fs/promises', 'stream/promises', 'original-fs',
        'original-fs/promises', 'util', 'module'].every(name =>
          isBuiltin(name) && isBuiltin('node:' + name) &&
          require(name) === require('node:' + name)) &&
        [undefined, null, 1, {}, Symbol(), '', 'electron', 'node:electron',
         'node:node:fs', 'node:missing', 'FS', './fs', 'fs\0'].every(name =>
          isBuiltin(name) === false);
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, UtilDeepStrictEqualUsesNativeBrands) {
  ExpectScript(R"JS(
    const util = require('node:util');
    const equal = util.isDeepStrictEqual;
    const promise = Promise.resolve(1);
    const fakePromise = Object.create(Promise.prototype);
    const fakeError = Object.create(Error.prototype);
    const nativeError = Object.setPrototypeOf(new Error('native'), null);
    const thenable = {get then() { throw new Error('must not inspect then'); }};
    const first = {items: new Map([[{key: 1}, new Set([1, 2])]])};
    const second = {items: new Map([[{key: 1}, new Set([2, 1])]])};
    first.self = first;
    second.self = second;
    class First { constructor() { this.value = 1; } }
    class Second { constructor() { this.value = 1; } }
    return util === require('util') && typeof equal === 'function' &&
        util.types.isPromise(promise) &&
        !util.types.isPromise(fakePromise) &&
        !util.types.isPromise(thenable) && !util.types.isPromise() &&
        !util.types.isPromise(new Proxy(promise, {})) &&
        util.types.isNativeError(nativeError) &&
        !util.types.isNativeError(fakeError) && !util.types.isNativeError() &&
        equal(first, second) && equal(NaN, NaN) && !equal(0, -0) &&
        equal(promise, promise) && !equal(promise, Promise.resolve(1)) &&
        equal(fakePromise, Object.create(Promise.prototype)) &&
        !equal(new First(), new Second()) &&
        equal(new First(), new Second(), true) &&
        !equal(new Error('first'), new Error('second'));
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, BuiltinObjectsRejectUnavailableOperations) {
  ExpectScript(R"JS(
    const childProcess = require('child_process');
    const http2 = require('http2');
    return http2 !== require('http') && http2 !== require('https') &&
        childProcess !== require('net') &&
        typeof childProcess.spawn === 'function' &&
        errorCode(() => childProcess.spawn('fixture', [], {shell: true})) === 'ERR_NOT_SUPPORTED' &&
        typeof childProcess.execFile === 'function' &&
        typeof childProcess.execFile[require('util').promisify.custom] === 'function' &&
        errorCode(() => childProcess.execFile('fixture', [], {timeout: -1})) === 'ERR_OUT_OF_RANGE' &&
        // This module-only fixture has no OS transport. A null killSignal
        // avoids native signal lookup while checking the unsupported shell.
        errorCode(() => childProcess.execFile('fixture', [],
            {shell: true, killSignal: null})) === 'ERR_NOT_SUPPORTED' &&
        ['spawnSync', 'exec', 'execSync', 'execFileSync', 'fork'].every(method =>
            typeof childProcess[method] === 'function' &&
            errorCode(() => childProcess[method]('fixture')) === 'ERR_NOT_SUPPORTED') &&
        ['connect', 'createServer', 'createSecureServer'].every(method =>
            typeof http2[method] === 'function' &&
            errorCode(() => http2[method]()) === 'ERR_NOT_SUPPORTED') &&
        ['http2', 'child_process'].every(name =>
            require.resolve(name) === name &&
            require.resolve('node:' + name) === 'node:' + name);
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, MissingAddonDoesNotResolveByBasename) {
  WriteModule("addon.node", "not a native library");
  WriteModule("build/Release/addon.node", "not a native library");
  WriteModule("Release/addon.node", "not a native library");
  ExpectScript(R"JS(
    return errorCode(() => require.resolve('./missing/addon.node')) ===
            'MODULE_NOT_FOUND' &&
        errorCode(() => require('./missing/addon.node')) === 'MODULE_NOT_FOUND' &&
        require.resolve('./addon.node').endsWith('addon.node');
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, AddonLoadErrorsDoNotLookLikeMissingFiles) {
  WriteModule("denied.node", "native loader fixture");
  WriteModule("invalid.node", "native loader fixture");
  XenonIpcMainContainer::NativeAddonHooks hooks;
  hooks.load =
      base::BindRepeating([](const std::string& path, std::string* error) {
        *error =
            base::FilePath::FromUTF8Unsafe(path).BaseName().AsUTF8Unsafe() ==
                    "denied.node"
                ? "ERR_ACCESS_DENIED: native library outside application"
                : "native library initialization failed";
        return false;
      });
  ExpectScript(R"JS(
    let denied, invalid;
    try { require('./denied.node'); } catch (error) { denied = error; }
    try { require('./invalid.node'); } catch (error) { invalid = error; }
    return denied.code === 'ERR_ACCESS_DENIED' &&
        denied.message.includes('outside application') &&
        invalid.code === 'ERR_DLOPEN_FAILED' &&
        invalid.message.includes('initialization failed') &&
        errorCode(() => require('./missing.node')) === 'MODULE_NOT_FOUND';
  )JS",
               std::move(hooks));
}

TEST_F(XenonIpcModuleLoaderTest, ResolveDoesNotExecuteAndRequireCachesExports) {
  WriteModule("counter.js", R"JS(
    global.moduleLoads = (global.moduleLoads || 0) + 1;
    module.exports = {count: global.moduleLoads};
  )JS");
  WriteModule("json-module.json", "{\"value\":42}");
  ExpectScript(R"JS(
    const resolved = require.resolve('./counter');
    if (global.moduleLoads !== undefined || !resolved.endsWith('counter.js'))
      return false;
    const first = require('./counter');
    return first === require('./counter.js') && first.count === 1 &&
        require('./json-module').value === 42 &&
        errorCode(() => require.resolve('./missing')) === 'MODULE_NOT_FOUND';
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, BareModulesSearchFromParentWithinApplication) {
  WriteModule("src/feature/check.js",
              "module.exports = require('dependency');");
  WriteModule("src/node_modules/dependency/index.js",
              "module.exports = 'near-parent';");
  WriteModule("node_modules/dependency/index.js",
              "module.exports = 'app-root';");
  WriteModule("node_modules/sqlite3/index.js",
              "module.exports = 'real-package';");
  const base::FilePath outside =
      temp_dir_.GetPath().AppendASCII("node_modules/outside/index.js");
  ASSERT_TRUE(base::CreateDirectory(outside.DirName()));
  ASSERT_TRUE(base::WriteFile(outside, "module.exports = 'outside-root';"));
  ExpectScript(R"JS(
    return require('./src/feature/check') === 'near-parent' &&
        require('dependency') === 'app-root' &&
        require('sqlite3') === 'real-package' &&
        errorCode(() => require('outside')) === 'MODULE_NOT_FOUND' &&
        errorCode(() => require.resolve('../node_modules/outside')) ===
            'MODULE_NOT_FOUND';
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, CommonJsFileAndDirectoryResolution) {
  WriteModule("explicit.cjs", "module.exports = 'explicit-cjs';");
  WriteModule("named.ext.js", "module.exports = 'extension-appended';");
  WriteModule("dir/index.json", "{\"value\":42}");
  WriteModule("addon-dir/index.node", "not a native library");
  WriteModule("cjs-dir/index.cjs", "module.exports = 'not-implicit';");
  WriteModule("pkg/package.json", "{\"main\":\"lib/entry\"}");
  WriteModule("pkg/lib/entry.js", "module.exports = 'package-main';");
  ExpectScript(R"JS(
    return require('./explicit.cjs') === 'explicit-cjs' &&
        errorCode(() => require('./explicit')) === 'MODULE_NOT_FOUND' &&
        require('./named.ext') === 'extension-appended' &&
        require('./dir').value === 42 &&
        require('./pkg') === 'package-main' &&
        require.resolve('./addon-dir').endsWith('index.node') &&
        errorCode(() => require('./cjs-dir')) === 'MODULE_NOT_FOUND';
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, PackageExportsAndImportsAreNotBypassed) {
  WriteModule("node_modules/guarded/package.json",
              "{\"exports\":{\".\":\"./public.js\"},\"main\":\"private.js\"}");
  WriteModule("node_modules/guarded/private.js", "module.exports = 'private';");
  WriteModule("node_modules/guarded/public.js", "module.exports = 'public';");
  ExpectScript(R"JS(
    return require('guarded') === 'public' &&
        errorCode(() => require.resolve('guarded/private.js')) ===
            'ERR_PACKAGE_PATH_NOT_EXPORTED' &&
        errorCode(() => require('#private')) === 'ERR_PACKAGE_IMPORT_NOT_DEFINED';
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest,
       PackageConditionsImportsPatternsAndSelfReference) {
  WriteModule("package.json", R"JSON({"name":"fixture","exports":{
    "./public":"./self.cjs"},"imports":{"#self":"./self.cjs",
    "#external":"ordered","#wild/*":"./lib/*.cjs"}})JSON");
  WriteModule("self.cjs", "module.exports = {answer:42};");
  WriteModule("lib/value.cjs", "module.exports = 42;");
  WriteModule("node_modules/ordered/package.json", R"JSON({"exports":{
    ".":{"default":"./default.cjs","require":"./wrong.cjs"},
    "./feature":{"node":{"require":"./feature.cjs"}},
    "./hidden":null,"./wild/*":"./lib/*.cjs"}})JSON");
  WriteModule("node_modules/ordered/default.cjs",
              "module.exports = 'default-first';");
  WriteModule("node_modules/ordered/wrong.cjs",
              "throw new Error('wrong condition');");
  WriteModule("node_modules/ordered/feature.cjs", "module.exports = 7;");
  WriteModule("node_modules/ordered/lib/value.cjs", "module.exports = 9;");
  ExpectScript(R"JS(
    return require('ordered') === 'default-first' &&
      require('ordered/feature') === 7 && require('ordered/wild/value') === 9 &&
      require('#external') === require('ordered') && require('#wild/value') === 42 &&
      require('#self') === require('fixture/public') &&
      errorCode(() => require('fixture')) === 'ERR_PACKAGE_PATH_NOT_EXPORTED' &&
      errorCode(() => require('ordered/hidden')) === 'ERR_PACKAGE_PATH_NOT_EXPORTED';
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, ExportTargetsAreExactAndStayInsideThePackage) {
  WriteModule("node_modules/guarded/package.json", R"JSON({"exports":{
    ".":["../invalid.cjs","./valid.cjs"],"./missing":["./missing.cjs","./valid.cjs"],
    "./extension":"./extension","./directory":"./directory",
    "./bad":"./lib/../../escape.cjs","./space":"./with%20space.cjs"}})JSON");
  WriteModule("node_modules/guarded/valid.cjs", "module.exports = 42;");
  WriteModule("node_modules/guarded/extension.js",
              "throw 'extension guessed';");
  WriteModule("node_modules/guarded/directory/index.js",
              "throw 'index guessed';");
  WriteModule("node_modules/guarded/with space.cjs", "module.exports = 7;");
  ExpectScript(R"JS(
    return require('guarded') === 42 && require('guarded/space') === 7 &&
      errorCode(() => require('guarded/missing')) === 'MODULE_NOT_FOUND' &&
      errorCode(() => require('guarded/extension')) === 'MODULE_NOT_FOUND' &&
      errorCode(() => require('guarded/directory')) === 'MODULE_NOT_FOUND' &&
      errorCode(() => require('guarded/bad')) === 'ERR_INVALID_PACKAGE_TARGET';
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, CircularRequiresKeepPartialExports) {
  WriteModule("a.js", "exports.name = 'a'; exports.b = require('./b');");
  WriteModule("b.js", "exports.name = 'b'; exports.a = require('./a');");
  ExpectScript(R"JS(
    const a = require('./a');
    return a.name === 'a' && a.b.name === 'b' && a.b.a === a;
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest,
       RequiredEsmSurvivesFileRemovalWithLiveBindings) {
  WriteModule("counter.mjs", R"JS(
    export let count = 1;
    export function increment() { ++count; }
    export default 'value';
  )JS");
  ExpectScript(R"JS(
    const first = require('./counter.mjs');
    require('fs').unlinkSync(require.resolve('./counter.mjs'));
    first.increment();
    const second = require('./counter.mjs');
    return first === second && second.count === 2 &&
      second.default === 'value' && second.__esModule === true;
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest,
       CircularRequiresObserveReassignedModuleExports) {
  WriteModule("a.js", R"JS(
    module.exports = {name: 'replacement'};
    module.exports.b = require('./b');
  )JS");
  WriteModule("b.js", "module.exports = {a: require('./a')};");
  ExpectScript(R"JS(
    const a = require('./a');
    return a.name === 'replacement' && a.b.a === a;
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest,
       RequireEsmRejectsCycleBackToExecutingCommonJs) {
  WriteModule("start.cjs", "exports.partial = 1; require('./bridge.mjs');");
  WriteModule("bridge.mjs",
              "import value from './start.cjs'; export default value;");
  ExpectScript(R"JS(
    return errorCode(() => require('./start.cjs')) === 'ERR_REQUIRE_CYCLE_MODULE';
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, CacheReadsCurrentExportsAfterEvaluation) {
  WriteModule("replace.js", R"JS(
    module.exports = {
      version: 1,
      replace() { module.exports = {version: 2}; },
    };
  )JS");
  ExpectScript(R"JS(
    const first = require('./replace');
    first.replace();
    const second = require('./replace');
    return first !== second && second.version === 2 &&
        second === require('./replace.js');
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, NestedRequirePreservesThrownObjectAndRetries) {
  WriteModule("failure.js", R"JS(
    const failure = new TypeError('original failure');
    failure.code = 'CUSTOM_FAILURE';
    module.exports = failure;
  )JS");
  WriteModule("thrower.js", R"JS(
    global.failedLoads = (global.failedLoads || 0) + 1;
    throw require('./failure');
  )JS");
  WriteModule("indirect.js", "module.exports = require('./thrower');");
  ExpectScript(R"JS(
    const expected = require('./failure');
    for (let i = 0; i < 2; i++) {
      try {
        require('./indirect');
        return false;
      } catch (error) {
        if (error !== expected || !(error instanceof TypeError) ||
            error.code !== 'CUSTOM_FAILURE') return false;
      }
    }
    return global.failedLoads === 2;
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, RequirePreservesPrimitiveAndSyntaxErrors) {
  WriteModule("primitive.js", "throw undefined;");
  WriteModule("syntax.js", "function ( {");
  WriteModule("syntax.json", "{invalid json}");
  ExpectScript(R"JS(
    try {
      require('./primitive');
      return false;
    } catch (error) {
      if (error !== undefined) return false;
    }
    for (const name of ['./syntax.js', './syntax.json']) {
      try {
        require(name);
        return false;
      } catch (error) {
        if (!(error instanceof SyntaxError) || error.code === 'MODULE_NOT_FOUND')
          return false;
      }
    }
    return true;
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, ExportsGetterKeepsItsOriginalException) {
  WriteModule("getter.js", R"JS(
    Object.defineProperty(module, 'exports', {
      get() { throw global.exportsError; },
    });
  )JS");
  ExpectScript(R"JS(
    global.exportsError = {original: true};
    try {
      require('./getter');
      return false;
    } catch (error) {
      return error === global.exportsError;
    }
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, LoadedModuleSurvivesItsFileBeingRemoved) {
  WriteModule("cached.js", "module.exports = {ready: true};");
  ExpectScript(R"JS(
    const fs = require('fs');
    const first = require('./cached');
    const filename = require.resolve('./cached');
    fs.unlinkSync(filename);
    for (let count = 0; count < 1000; ++count) {
      if (require('./cached') !== first || require.resolve('./cached') !== filename)
        return false;
    }
    return first.ready;
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, FailedModulesDoNotCacheTheirResolution) {
  WriteModule("retry.js", "throw new Error('retry this module');");
  ExpectScript(R"JS(
    const fs = require('fs');
    const path = require('path');
    try { require('./retry'); return false; } catch (error) {
      if (error.message !== 'retry this module') return false;
    }
    fs.unlinkSync(path.join(__dirname, 'retry.js'));
    fs.writeFileSync(path.join(__dirname, 'retry.json'), '42');
    if (require('./retry') !== 42) return false;
    if (errorCode(() => require('./created-later')) !== 'MODULE_NOT_FOUND') return false;
    fs.writeFileSync(path.join(__dirname, 'created-later.js'), 'module.exports = 73;');
    return require('./created-later') === 73;
  )JS");
}

TEST_F(XenonIpcModuleLoaderTest, NativeForwarderPreservesRealExportShape) {
  WriteModule("shape.node", "loaded by the test hook");
  int nested_inspections = 0;
  int getter_reads = 0;
  int native_calls = 0;
  XenonIpcMainContainer::NativeAddonHooks hooks;
  hooks.load = base::BindRepeating(
      [](const std::string&, std::string*) { return true; });
  hooks.describe = base::BindRepeating(
      [](int* nested_inspections, int* getter_reads, const std::string&,
         const std::string& path, base::Value* description,
         std::string* error) {
        std::string json;
        if (path.empty()) {
          json = R"JSON({"kind":"object","children":[
            {"name":"then","kind":"number","hasValue":true,"value":7,"enumerable":true},
            {"name":"default","kind":"string","hasValue":true,"value":"real","enumerable":true},
            {"name":"__esModule","kind":"boolean","hasValue":true,"value":false,"enumerable":true},
            {"name":"nil","kind":"null","hasValue":true,"value":null},
            {"name":"unset","kind":"undefined"},
            {"name":"nested","kind":"object","enumerable":true},
            {"name":"items","kind":"array","enumerable":true},
            {"name":"add","kind":"function","enumerable":true},
            {"name":"Counter","kind":"class","enumerable":true},
            {"name":"changing","kind":"property","enumerable":true},
            {"name":"changingObject","kind":"property","enumerable":true},
            {"name":"invalid.path","kind":"function","enumerable":true},
            {"name":"broken","kind":"property","enumerable":true}]})JSON";
        } else if (path == "nested") {
          ++*nested_inspections;
          json = R"JSON({"kind":"object","children":[
            {"name":"value","kind":"number","hasValue":true,"value":42,"enumerable":true},
            {"name":"deep","kind":"object","enumerable":true}]})JSON";
        } else if (path == "nested.deep") {
          json = R"JSON({"kind":"object","children":[
            {"name":"ok","kind":"boolean","hasValue":true,"value":true}]})JSON";
        } else if (path == "items") {
          json = R"JSON({"kind":"array","children":[
            {"name":"0","kind":"string","hasValue":true,"value":"first","enumerable":true},
            {"name":"length","kind":"number","hasValue":true,"value":1}]})JSON";
        } else if (path == "add") {
          json = R"JSON({"kind":"function","children":[
            {"name":"name","kind":"string","hasValue":true,"value":"add"},
            {"name":"length","kind":"number","hasValue":true,"value":2},
            {"name":"tag","kind":"string","hasValue":true,"value":"static","enumerable":true}]})JSON";
        } else if (path == "Counter") {
          json = R"JSON({"kind":"class","children":[
            {"name":"prototype","kind":"object"}]})JSON";
        } else if (path == "Counter.prototype") {
          json = R"JSON({"kind":"object","children":[]})JSON";
        } else if (path == "changingObject") {
          json = R"JSON({"kind":"object","children":[]})JSON";
        } else if (path == "changing") {
          ++*getter_reads;
          *description = base::Value(base::DictValue()
                                         .Set("kind", "number")
                                         .Set("hasValue", true)
                                         .Set("value", *getter_reads));
          return true;
        } else if (path == "broken") {
          *error = "native getter failed";
          return false;
        } else {
          *error = "unexpected inspection: " + path;
          return false;
        }
        auto parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
        if (!parsed) {
          return false;
        }
        *description = std::move(*parsed);
        return true;
      },
      &nested_inspections, &getter_reads);
  hooks.invoke = base::BindRepeating(
      [](int* calls, const std::string&, const std::string& path,
         const base::Value& args, base::Value* result, std::string*) {
        EXPECT_EQ(path, "add");
        ++*calls;
        *result = base::Value(args.GetList()[0].GetInt() +
                              args.GetList()[1].GetInt());
        return true;
      },
      &native_calls);
  hooks.construct = base::BindRepeating(
      [](const std::string&, const std::string& path, const base::Value&,
         base::Value* instance, std::string*) {
        EXPECT_EQ(path, "Counter");
        *instance = base::Value(
            base::DictValue()
                .Set("__xenon_node_wire_type__", "native_instance")
                .Set("instance_id", 17)
                .Set("fields", base::DictValue().Set("count", 5).Set("then", 9))
                .Set("prototype",
                     base::ListValue().Append(base::DictValue()
                                                  .Set("name", "add")
                                                  .Set("kind", "function"))));
        return true;
      });
  hooks.invoke_instance = base::BindRepeating(
      [](const std::string&, int32_t id, const std::string& method,
         const base::Value& args, base::Value* result, std::string*) {
        EXPECT_EQ(id, 17);
        EXPECT_EQ(method, "add");
        *result = base::Value(5 + args.GetList()[0].GetInt());
        return true;
      });
  ExpectScript(R"JS(
    const addon = require('./shape.node');
    if (typeof addon !== 'object' || addon.missing !== undefined ||
        'missing' in addon || Object.hasOwn(addon, 'missing') ||
        addon.then !== 7 || addon.default !== 'real' || addon.__esModule !== false ||
        addon.nil !== null || addon.unset !== undefined) return false;
    if (addon.nested.value !== 42 || !addon.nested.deep.ok ||
        addon.nested !== addon.nested || addon.nested.missing !== undefined) return false;
    if (!Array.isArray(addon.items) || addon.items.length !== 1 ||
        addon.items[0] !== 'first') return false;
    if (typeof addon.add !== 'function' || addon.add.name !== 'add' ||
        addon.add.length !== 2 || addon.add.tag !== 'static' ||
        addon.add.missing !== undefined || addon.add(2, 3) !== 5 ||
        addon.add.call(null, 3, 4) !== 7 || addon.add.apply(null, [4, 5]) !== 9 ||
        addon.add.bind(null, 5)(6) !== 11) return false;
    const counter = new addon.Counter();
    if (typeof counter !== 'object' || !(counter instanceof addon.Counter) ||
        counter.count !== 5 || counter.then !== 9 || counter.add(4) !== 9 ||
        counter.missing !== undefined) return false;
    if (addon.changing !== 1 || addon.changing !== 2) return false;
    if (errorCode(() => addon.changingObject) !== 'ERR_NOT_SUPPORTED' ||
        errorCode(() => addon['invalid.path']) !== 'ERR_NOT_SUPPORTED' ||
        errorCode(() => { addon.changing = 42; }) !== 'ERR_NOT_SUPPORTED') return false;
    try { addon.broken; return false; }
    catch (error) { if (!error.message.includes('native getter failed')) return false; }
    return require('./shape.node') === addon;
  )JS",
               std::move(hooks));
  EXPECT_EQ(nested_inspections, 1);
  EXPECT_EQ(getter_reads, 2);
  EXPECT_EQ(native_calls, 4);
}

TEST_F(XenonIpcModuleLoaderTest, NativeRootFunctionCallsTheActualRoot) {
  WriteModule("callable.node", "loaded by the test hook");
  XenonIpcMainContainer::NativeAddonHooks hooks;
  hooks.load = base::BindRepeating(
      [](const std::string&, std::string*) { return true; });
  hooks.describe =
      base::BindRepeating([](const std::string&, const std::string& path,
                             base::Value* description, std::string*) {
        EXPECT_TRUE(path.empty());
        *description = base::Value(base::DictValue().Set("kind", "function"));
        return true;
      });
  hooks.invoke = base::BindRepeating(
      [](const std::string&, const std::string& path, const base::Value& args,
         base::Value* result, std::string*) {
        EXPECT_TRUE(path.empty());
        *result = args.GetList()[0].Clone();
        return true;
      });
  ExpectScript(R"JS(
    const fn = require('./callable.node');
    return typeof fn === 'function' && fn(19) === 19 &&
        fn.missing === undefined && fn.then === undefined &&
        fn.default === undefined && fn.__esModule === undefined;
  )JS",
               std::move(hooks));
}

TEST_F(XenonIpcModuleLoaderTest, NativeForwarderPreservesNestedBinaryKinds) {
  WriteModule("binary.node", "loaded by the test hook");
  int binary_calls = 0;
  XenonIpcMainContainer::NativeAddonHooks hooks;
  hooks.load = base::BindRepeating(
      [](const std::string&, std::string*) { return true; });
  hooks.describe =
      base::BindRepeating([](const std::string&, const std::string&,
                             base::Value* description, std::string*) {
        *description = base::Value(base::DictValue().Set("kind", "function"));
        return true;
      });
  hooks.invoke = base::BindRepeating(
      [](int* calls, const std::string&, const std::string&,
         const base::Value& args, base::Value* result, std::string*) {
        ++*calls;
        const auto& row = args.GetList()[0].GetDict();
        const auto* binary = row.FindDict("data");
        EXPECT_NE(binary, nullptr);
        if (!binary) {
          return false;
        }
        EXPECT_EQ(*binary->FindString("__xenon_node_wire_type__"), "binary");
        EXPECT_EQ(*binary->FindString("kind"), *row.FindString("kind"));
        const auto* bytes = binary->Find("value");
        EXPECT_TRUE(bytes && bytes->is_blob());
        if (!bytes || !bytes->is_blob()) {
          return false;
        }
        EXPECT_EQ(bytes->GetBlob().size(),
                  static_cast<size_t>(*row.FindInt("byteLength")));
        EXPECT_EQ(row.FindList("nested")
                      ->front()
                      .GetDict()
                      .Find("data")
                      ->GetDict()
                      .Find("value")
                      ->GetBlob(),
                  bytes->GetBlob());
        *result = args.GetList()[0].Clone();
        return true;
      },
      &binary_calls);
  ExpectScript(R"JS(
    const echo = require('./binary.node');
    const names = ['Int8Array', 'Uint8Array', 'Uint8ClampedArray',
      'Int16Array', 'Uint16Array', 'Int32Array', 'Uint32Array',
      'Float16Array', 'Float32Array', 'Float64Array', 'BigInt64Array', 'BigUint64Array'];
    const samples = [
      ['Buffer', Buffer.from([91, 11, 22, 92]).subarray(1, 3)],
      ['Buffer', Buffer.alloc(0)],
      ['ArrayBuffer', new Uint8Array([11, 22]).buffer],
      ['ArrayBuffer', new ArrayBuffer(0)],
      ['DataView', new DataView(new Uint8Array([91, 11, 22, 92]).buffer, 1, 2)],
      ['DataView', new DataView(new ArrayBuffer(0))],
    ];
    for (const name of names) {
      const Type = globalThis[name];
      if (typeof Type !== 'function') continue;
      const source = new Type(4);
      source[1] = name.startsWith('Big') ? 11n : 11;
      source[2] = name.startsWith('Big') ? 22n : 22;
      samples.push([name, source.subarray(1, 3)], [name, new Type(0)]);
    }
    const bytes = value => value instanceof ArrayBuffer ? new Uint8Array(value) :
        new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    for (const [kind, data] of samples) {
      const row = echo({kind, byteLength: data.byteLength, data, nested: [{data}]});
      for (const actual of [row.data, row.nested[0].data]) {
        if (kind === 'Buffer' ? !Buffer.isBuffer(actual) :
            !(actual instanceof globalThis[kind]) || Buffer.isBuffer(actual)) return false;
        if (actual.byteLength !== data.byteLength ||
            actual === data || actual.buffer === data.buffer && !(data instanceof ArrayBuffer)) return false;
        if (bytes(actual).some((byte, index) => byte !== bytes(data)[index])) return false;
        const backing = actual instanceof ArrayBuffer ? actual : actual.buffer;
        if (backing.byteLength !== actual.byteLength) return false;
      }
    }
    return true;
  )JS",
               std::move(hooks));
  EXPECT_GE(binary_calls, 28);
}

TEST_F(XenonIpcModuleLoaderTest,
       NativeForwarderPreservesConstructorAndInstanceBinary) {
  WriteModule("binary-class.node", "loaded by the test hook");
  XenonIpcMainContainer::NativeAddonHooks hooks;
  hooks.load = base::BindRepeating(
      [](const std::string&, std::string*) { return true; });
  hooks.describe =
      base::BindRepeating([](const std::string&, const std::string&,
                             base::Value* description, std::string*) {
        *description = base::Value(base::DictValue().Set("kind", "class"));
        return true;
      });
  hooks.construct = base::BindRepeating(
      [](const std::string&, const std::string&, const base::Value& args,
         base::Value* instance, std::string*) {
        const auto& payload = args.GetList()[0];
        EXPECT_TRUE(payload.GetDict().Find("value")->is_blob());
        *instance = base::Value(
            base::DictValue()
                .Set("__xenon_node_wire_type__", "native_instance")
                .Set("instance_id", 23)
                .Set("fields",
                     base::DictValue().Set("payload", payload.Clone()))
                .Set("prototype",
                     base::ListValue().Append(base::DictValue()
                                                  .Set("name", "echo")
                                                  .Set("kind", "function"))));
        return true;
      });
  hooks.invoke_instance = base::BindRepeating([](const std::string&, int32_t id,
                                                 const std::string& method,
                                                 const base::Value& args,
                                                 base::Value* result,
                                                 std::string*) {
    EXPECT_EQ(id, 23);
    EXPECT_EQ(method, "echo");
    const auto& row = args.GetList()[0].GetDict();
    EXPECT_TRUE(
        row.FindDict("__proto__")->FindDict("data")->Find("value")->is_blob());
    *result = args.GetList()[0].Clone();
    return true;
  });
  ExpectScript(R"JS(
    const Binary = require('./binary-class.node');
    const instance = new Binary(Buffer.from([91, 11, 22, 92]).subarray(1, 3));
    if (!Buffer.isBuffer(instance.payload) ||
        instance.payload.toString('hex') !== '0b16') return false;
    const row = JSON.parse('{"__proto__":{"polluted":true}}');
    row.__proto__.data = instance.payload;
    row.nested = [{data: instance.payload}];
    const actual = instance.echo(row);
    return Object.getPrototypeOf(actual) === Object.prototype &&
        Object.hasOwn(actual, '__proto__') && actual.__proto__.polluted === true &&
        actual.polluted === undefined && ({}).polluted === undefined &&
        Buffer.isBuffer(actual.__proto__.data) &&
        Buffer.isBuffer(actual.nested[0].data) &&
        actual.__proto__.data.toString('hex') === '0b16' &&
        actual.nested[0].data.toString('hex') === '0b16';
  )JS",
               std::move(hooks));
}

}  // namespace
}  // namespace xenon::ipc
