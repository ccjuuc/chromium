// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/run_loop.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/values.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "net/base/filename_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"
#include "xenon_overlay/common/asar/test_support.h"

namespace xenon::ipc {
namespace {

// Exercise the actual package entry, resolver, CJS cache, V8 module graph and
// IPC facade together. Only a renderer endpoint is supplied by this fixture;
// application modules and their evaluation are never replaced with mocks.
class XenonIpcEsmIntegrationTest : public gin::V8Test,
                                   public mojom::IpcRenderer {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }

  void SetUp() override {
    gin::V8Test::SetUp();
    timeout_ = std::make_unique<base::test::ScopedRunLoopTimeout>(
        FROM_HERE, base::Seconds(5));
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ASSERT_TRUE(base::NormalizeFilePath(temp_.GetPath(), &root_));
    app_path_ = root_.AppendASCII("application");
    ASSERT_TRUE(base::CreateDirectory(app_path_));
    container_ = std::make_unique<XenonIpcMainContainer>();
    container_->SetAppExitHandler(base::BindRepeating(
        [](std::vector<int>* exits, int code) { exits->push_back(code); },
        &exit_codes_));
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
    const auto file =
        app_path_.Append(base::FilePath::FromUTF8Unsafe(relative));
    return base::CreateDirectory(file.DirName()) &&
           base::WriteFile(file, source);
  }

  bool StartAt(const base::FilePath& application) {
    XenonIpcMainContainer::AppMainModule config;
    config.app_path = application;
    if (!container_->Initialize(std::move(config))) {
      return false;
    }
    container_->AddRenderer("renderer", receiver_.BindNewPipeAndPassRemote(), 1,
                            1, 0, "about:blank", true);
    return true;
  }

  bool StartEntry(const std::string& entry, const std::string& type = {}) {
    const auto manifest = "{\"main\":\"" + entry + "\"" +
                          (type.empty() ? "" : ",\"type\":\"" + type + "\"") +
                          "}";
    return Write("package.json", manifest) && StartAt(app_path_);
  }

  mojom::IpcResultPtr Invoke(const std::string& channel) {
    base::test::TestFuture<mojom::IpcResultPtr> future;
    container_->Invoke("renderer", channel, base::Value(base::ListValue()),
                       future.GetCallback());
    return future.Take();
  }

  mojom::IpcResultPtr State() {
    return container_->SendSync("renderer", "test:state",
                                base::Value(base::ListValue()));
  }

  void Send(const std::string& channel) {
    container_->Send("renderer", channel, base::Value(base::ListValue()));
  }

  void MarkReadyOnEvaluationComplete() {
    container_->SetMainEvaluationCompleteHandler(base::BindRepeating(
        [](XenonIpcEsmIntegrationTest* self) {
          ++self->evaluation_completions_;
          EXPECT_TRUE(self->container_->MarkAppReady());
        },
        base::Unretained(this)));
  }

  base::ScopedTempDir temp_;
  base::FilePath root_;
  base::FilePath app_path_;
  std::vector<int> exit_codes_;
  int evaluation_completions_ = 0;
  mojo::Receiver<mojom::IpcRenderer> receiver_{this};
  std::unique_ptr<XenonIpcMainContainer> container_;
  std::unique_ptr<base::test::ScopedRunLoopTimeout> timeout_;
};

TEST_F(XenonIpcEsmIntegrationTest,
       CreateRequireNormalizesParentSegmentsBeforeRealpath) {
  ASSERT_TRUE(Write("product#percent%20.json", "{\"answer\":42}"));
  ASSERT_TRUE(base::WriteFile(root_.AppendASCII("outside.json"), "{}"));
  ASSERT_TRUE(Write("out/reader.cjs", R"JS(
    module.exports = require('../product#percent%20.json');
  )JS"));
  ASSERT_TRUE(Write("out/main.mjs", R"JS(
    import {createRequire} from 'node:module';
    import {ipcMain} from 'electron';
    const require = createRequire(import.meta.url);
    const value = require('../product#percent%20.json');
    const same = value === require('./missing/../../product#percent%20.json') &&
        value === require('./reader.cjs');
    let denied;
    try { require('../../outside.json'); } catch (error) { denied = error.code; }
    ipcMain.on('test:state', event => {
      event.returnValue = value.answer === 42 && same && denied === 'MODULE_NOT_FOUND';
    });
  )JS"));
  ASSERT_TRUE(StartEntry("out/main.mjs", "module"))
      << container_->startup_error();
  auto result = State();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
}

TEST_F(XenonIpcEsmIntegrationTest,
       ConsoleBuiltinSharesCjsEsmAndGlobalIdentity) {
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import nodeConsole, {Console, log} from 'node:console';
    import plainConsole from 'console';
    import {createRequire, isBuiltin} from 'node:module';
    import {ipcMain} from 'electron';
    import {EventEmitter} from 'node:events';
    const require = createRequire(import.meta.url);
    const output = [];
    const stream = new EventEmitter();
    stream.write = text => { output.push(text); return true; };
    const logger = new Console(stream);
    logger.group('group');
    logger.count('entry');
    logger.groupEnd();
    logger.log('done');
    ipcMain.on('test:state', event => {
      event.returnValue = nodeConsole === console && plainConsole === console &&
          require('node:console') === console && require('console') === console &&
          log === console.log && isBuiltin('console') && isBuiltin('node:console') &&
          JSON.stringify(output) === JSON.stringify(['group\n', '  entry: 1\n', 'done\n']);
    });
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs", "module")) << container_->startup_error();
  auto result = State();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
}

TEST_F(XenonIpcEsmIntegrationTest,
       WebStreamsBuiltinUsesRealByteOwnershipAndQueues) {
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import web, {ReadableStream, WritableStream, TransformStream} from 'node:stream/web';
    import {createRequire} from 'node:module';
    import {ipcMain} from 'electron';
    const require = createRequire(import.meta.url);
    ipcMain.handle('test:stream-web', async () => {
      const source = new Uint8Array([2, 3, 5]);
      const readable = new ReadableStream({type: 'bytes', start(controller) {
        controller.enqueue(source);
        controller.close();
      }});
      const reader = readable.getReader({mode: 'byob'});
      const target = new Uint8Array(8);
      const result = await reader.read(target);
      const input = [1, 2, 3];
      const output = [];
      await ReadableStream.from(input)
          .pipeThrough(new TransformStream({transform(value, controller) {
            controller.enqueue(value * 2);
          }}))
          .pipeTo(new WritableStream({write(value) { output.push(value); }}));
      const blob = new Blob(['blob']);
      const blobResult = await blob.stream().getReader({mode: 'byob'})
          .read(new Uint8Array(8));
      return web === require('stream/web') && web === require('node:stream/web') &&
          ReadableStream === globalThis.ReadableStream &&
          source.byteLength === 0 && target.byteLength === 0 &&
          JSON.stringify([...result.value]) === '[2,3,5]' &&
          JSON.stringify(output) === '[2,4,6]' &&
          JSON.stringify([...blobResult.value]) === '[98,108,111,98]';
    });
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs", "module")) << container_->startup_error();
  auto result = Invoke("test:stream-web");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
}

TEST_F(XenonIpcEsmIntegrationTest, MjsEntryPreservesLiveBindingsAndCycles) {
  ASSERT_TRUE(Write("counter.mjs", R"JS(
    import {read} from './reader.mjs';
    export let counter = 1;
    export function advance() { ++counter; return read(); }
  )JS"));
  ASSERT_TRUE(Write("reader.mjs", R"JS(
    import {counter} from './counter.mjs';
    export function read() { return counter; }
  )JS"));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import {counter, advance} from './counter.mjs';
    ipcMain.handle('test:counter', () => {
      const before = counter;
      return {before, after: advance(), current: counter};
    });
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  for (int expected = 1; expected <= 2; ++expected) {
    auto result = Invoke("test:counter");
    ASSERT_TRUE(result->success) << result->error;
    ASSERT_TRUE(result->value.is_dict());
    const auto& value = result->value.GetDict();
    EXPECT_EQ(expected, value.FindInt("before"));
    EXPECT_EQ(expected + 1, value.FindInt("after"));
    EXPECT_EQ(expected + 1, value.FindInt("current"));
  }
}

TEST_F(XenonIpcEsmIntegrationTest,
       BuiltinNamedExportsUseRealTimersAndFileStats) {
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import {EventEmitter, once} from 'node:events';
    import {setTimeout as delay} from 'node:timers/promises';
    import {Stats, statSync} from 'node:fs';
    ipcMain.handle('test:builtins', async () => {
      const emitter = new EventEmitter();
      const event = once(emitter, 'answer');
      await delay(1);
      emitter.emit('answer', 42);
      const stats = statSync(import.meta.filename);
      return {answer: (await event)[0], instance: stats instanceof Stats,
        file: stats.isFile(), size: stats.size > 0,
        remaining: emitter.listenerCount('answer')};
    });
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  auto result = Invoke("test:builtins");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  EXPECT_EQ(42, value.FindInt("answer"));
  EXPECT_EQ(true, value.FindBool("instance"));
  EXPECT_EQ(true, value.FindBool("file"));
  EXPECT_EQ(true, value.FindBool("size"));
  EXPECT_EQ(0, value.FindInt("remaining"));
}

TEST_F(XenonIpcEsmIntegrationTest,
       PackageTypeSelectsJsModulesAndNestedCommonJs) {
  ASSERT_TRUE(Write("value.js", "export const value = 40;"));
  ASSERT_TRUE(Write("legacy/package.json", R"({"type":"commonjs"})"));
  ASSERT_TRUE(Write("legacy/value.js", "exports.increment = 2;"));
  ASSERT_TRUE(Write("main.js", R"JS(
    import {ipcMain} from 'electron';
    import {value} from './value.js';
    import legacy, {increment} from './legacy/value.js';
    ipcMain.handle('test:types', () => ({answer: value + increment,
      identity: legacy.increment === increment}));
  )JS"));
  ASSERT_TRUE(StartEntry("main.js", "module")) << container_->startup_error();
  auto result = Invoke("test:types");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  EXPECT_EQ(42, result->value.GetDict().FindInt("answer"));
  EXPECT_EQ(true, result->value.GetDict().FindBool("identity"));
}

TEST_F(XenonIpcEsmIntegrationTest, ExtensionlessMainUsesModulePackageType) {
  ASSERT_TRUE(Write("entry", R"JS(
    import {ipcMain} from 'electron';
    ipcMain.handle('test:entry', () => 42);
  )JS"));
  ASSERT_TRUE(StartEntry("entry", "module")) << container_->startup_error();
  auto result = Invoke("test:entry");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonIpcEsmIntegrationTest,
       OriginalFsHasIndependentPhysicalArchiveView) {
  const auto archive = app_path_.AppendASCII("payload.asar");
  asar::TestArchiveBuilder builder;
  ASSERT_TRUE(
      builder.Write(archive, {{"value.txt", "virtual contents"}}, false));
  std::string physical;
  ASSERT_TRUE(base::ReadFileToString(archive, &physical));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import fs from 'node:fs';
    import original from 'original-fs';
    import originalPromises from 'node:original-fs/promises';
    import * as originalNamespace from 'original-fs';
    import * as prefixedNamespace from 'node:original-fs';
    import {fileURLToPath} from 'node:url';
    import {createRequire} from 'node:module';
    const require = createRequire(import.meta.url);
    const archive = fileURLToPath(new URL('./payload.asar', import.meta.url));
    ipcMain.handle('test:original-fs', async () => {
      const [member, bytes] = await Promise.all([
        fs.promises.readFile(archive + '/value.txt', 'utf8'),
        original.promises.readFile(archive)]);
      const rawChunks = [];
      const stream = original.createReadStream(archive, {start: 0, end: 7});
      await new Promise((resolve, reject) => {
        stream.on('data', chunk => rawChunks.push(chunk));
        stream.on('error', reject); stream.on('end', resolve);
      });
      return {same: original === require('original-fs'), distinct: original !== fs,
        promises: originalPromises === original.promises &&
          require('original-fs/promises') === originalPromises,
        alias: originalNamespace === prefixedNamespace &&
          require('original-fs') === require('node:original-fs'),
        file: original.statSync(archive).isFile(),
        directory: fs.statSync(archive).isDirectory(),
        hidden: !original.existsSync(archive + '/value.txt'), member,
        unchanged: fs.readFileSync(archive + '/value.txt', 'utf8') === member,
        bytes: bytes.toString('base64'), prefix: Buffer.concat(rawChunks).toString('base64')};
    });
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  auto result = Invoke("test:original-fs");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  for (const char* key : {"same", "distinct", "alias", "promises", "file",
                          "directory", "hidden", "unchanged"}) {
    EXPECT_EQ(true, value.FindBool(key)) << key;
  }
  ASSERT_TRUE(value.FindString("member"));
  ASSERT_TRUE(value.FindString("bytes"));
  ASSERT_TRUE(value.FindString("prefix"));
  EXPECT_EQ("virtual contents", *value.FindString("member"));
  EXPECT_EQ(base::Base64Encode(physical), *value.FindString("bytes"));
  EXPECT_EQ(base::Base64Encode(physical.substr(0, 8)),
            *value.FindString("prefix"));
}

TEST_F(XenonIpcEsmIntegrationTest, CommonJsDynamicImportReusesTheModuleGraph) {
  ASSERT_TRUE(Write("state.mjs", R"JS(
    globalThis.evaluations = (globalThis.evaluations || 0) + 1;
    export const count = globalThis.evaluations;
    export const object = {};
  )JS"));
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    ipcMain.handle('test:dynamic', async () => {
      const [first, second] = await Promise.all([
        import('./state.mjs'), import('./state.mjs')]);
      return {same: first === second, sameObject: first.object === second.object,
        count: second.count};
    });
  )JS"));
  ASSERT_TRUE(StartEntry("main.cjs")) << container_->startup_error();
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto result = Invoke("test:dynamic");
    ASSERT_TRUE(result->success) << result->error;
    ASSERT_TRUE(result->value.is_dict());
    EXPECT_EQ(true, result->value.GetDict().FindBool("same"));
    EXPECT_EQ(true, result->value.GetDict().FindBool("sameObject"));
    EXPECT_EQ(1, result->value.GetDict().FindInt("count"));
  }
}

TEST_F(XenonIpcEsmIntegrationTest, ImportMetaUsesCanonicalUrlsAndNativePaths) {
  ASSERT_TRUE(Write("answer.mjs", "export const value = 42;"));
  ASSERT_TRUE(Write("space name/main.mjs", R"JS(
    import {ipcMain} from 'electron';
    ipcMain.handle('test:meta', () => ({url: import.meta.url,
      filename: import.meta.filename, dirname: import.meta.dirname,
      answer: import.meta.resolve('../answer.mjs'),
      missing: import.meta.resolve('./missing.mjs'),
      builtin: import.meta.resolve('node:fs')}));
  )JS"));
  ASSERT_TRUE(StartEntry("space name/main.mjs")) << container_->startup_error();
  auto result = Invoke("test:meta");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  const auto directory = app_path_.AppendASCII("space name");
  const auto filename = directory.AppendASCII("main.mjs");
  ASSERT_TRUE(value.FindString("url"));
  EXPECT_EQ(net::FilePathToFileURL(filename).spec(), *value.FindString("url"));
  ASSERT_TRUE(value.FindString("filename"));
  EXPECT_EQ(filename.AsUTF8Unsafe(), *value.FindString("filename"));
  ASSERT_TRUE(value.FindString("dirname"));
  EXPECT_EQ(directory.AsUTF8Unsafe(), *value.FindString("dirname"));
  ASSERT_TRUE(value.FindString("answer"));
  EXPECT_EQ(net::FilePathToFileURL(app_path_.AppendASCII("answer.mjs")).spec(),
            *value.FindString("answer"));
  ASSERT_TRUE(value.FindString("missing"));
  EXPECT_EQ(net::FilePathToFileURL(directory.AppendASCII("missing.mjs")).spec(),
            *value.FindString("missing"));
  ASSERT_TRUE(value.FindString("builtin"));
  EXPECT_EQ("node:fs", *value.FindString("builtin"));
}

TEST_F(XenonIpcEsmIntegrationTest,
       JsonImportsShareCommonJsCacheAndRequireAttributes) {
  ASSERT_TRUE(Write("data.json", R"({"answer":41})"));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import {createRequire} from 'node:module';
    import data from './data.json' with {type: 'json'};
    const require = createRequire(import.meta.url);
    ipcMain.handle('test:json', async () => {
      const required = require('./data.json');
      const imported = await import('./data.json', {with: {type: 'json'}});
      required.answer = 42;
      let missingAttribute;
      try { await import('./data.json'); }
      catch (error) { missingAttribute = error.code; }
      return {same: required === data && imported.default === data,
        answer: data.answer, missingAttribute};
    });
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  auto result = Invoke("test:json");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  EXPECT_EQ(true, value.FindBool("same"));
  EXPECT_EQ(42, value.FindInt("answer"));
  ASSERT_TRUE(value.FindString("missingAttribute"));
  EXPECT_EQ("ERR_IMPORT_ATTRIBUTE_MISSING",
            *value.FindString("missingAttribute"));
}

TEST_F(XenonIpcEsmIntegrationTest,
       CommonJsReexportsShareDefaultAndSnapshotNames) {
  ASSERT_TRUE(Write("value.cjs", R"JS(
    exports.count = 1;
    exports.advance = () => ++exports.count;
    Object.defineProperty(exports, 'described', {value: 7, enumerable: true});
  )JS"));
  ASSERT_TRUE(
      Write("reexport.cjs", "module.exports = require('./value.cjs');"));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import {createRequire} from 'node:module';
    import value, {count, advance, described} from './reexport.cjs';
    const require = createRequire(import.meta.url);
    ipcMain.handle('test:cjs', () => ({same: value === require('./value.cjs'),
      before: count, advanced: advance(), snapshot: count,
      objectCount: value.count, described}));
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  auto result = Invoke("test:cjs");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  EXPECT_EQ(true, value.FindBool("same"));
  EXPECT_EQ(1, value.FindInt("before"));
  EXPECT_EQ(2, value.FindInt("advanced"));
  EXPECT_EQ(1, value.FindInt("snapshot"));
  EXPECT_EQ(2, value.FindInt("objectCount"));
  EXPECT_EQ(7, value.FindInt("described"));
}

TEST_F(XenonIpcEsmIntegrationTest,
       MissingCommonJsNamedExportFailsBeforeExecution) {
  ASSERT_TRUE(Write("side-effect.cjs", R"JS(
    require('fs').writeFileSync(require('path').join(__dirname, 'executed'), 'bad');
    module.exports = {};
  )JS"));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {notAnExport} from './side-effect.cjs';
    globalThis.unreachable = notAnExport;
  )JS"));
  EXPECT_FALSE(StartEntry("main.mjs"));
  EXPECT_NE(std::string::npos, container_->startup_error().find("notAnExport"));
  EXPECT_FALSE(base::PathExists(app_path_.AppendASCII("executed")));
}

TEST_F(XenonIpcEsmIntegrationTest,
       SynchronousRequireRejectsAsyncGraphsWithoutExecuting) {
  ASSERT_TRUE(Write("sync.mjs", "export const answer = 42;"));
  ASSERT_TRUE(Write("async.mjs", R"JS(
    globalThis.asyncExecuted = true;
    await Promise.resolve();
    export const answer = 99;
  )JS"));
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    ipcMain.handle('test:require-esm', async () => {
      const required = require('./sync.mjs');
      const imported = await import('./sync.mjs');
      let code;
      try { require('./async.mjs'); } catch (error) { code = error.code; }
      return {answer: required.answer, same: required === imported, code,
        executed: globalThis.asyncExecuted === true};
    });
  )JS"));
  ASSERT_TRUE(StartEntry("main.cjs")) << container_->startup_error();
  auto result = Invoke("test:require-esm");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  EXPECT_EQ(42, value.FindInt("answer"));
  EXPECT_EQ(true, value.FindBool("same"));
  EXPECT_EQ(false, value.FindBool("executed"));
  ASSERT_TRUE(value.FindString("code"));
  EXPECT_EQ("ERR_REQUIRE_ASYNC_MODULE", *value.FindString("code"));
}

TEST_F(XenonIpcEsmIntegrationTest,
       TopLevelAwaitDefersReadyUntilEvaluationCompletes) {
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {app, ipcMain} from 'electron';
    let release, completed = false, readyCount = 0;
    const pending = new Promise(resolve => { release = resolve; });
    app.on('ready', () => ++readyCount);
    ipcMain.on('test:state', event => {
      event.returnValue = {ready: app.isReady(), completed, readyCount};
    });
    ipcMain.on('test:release', () => release());
    await pending;
    completed = true;
  )JS"));
  MarkReadyOnEvaluationComplete();
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  EXPECT_FALSE(container_->MarkAppReady());
  auto before = State();
  ASSERT_TRUE(before->success) << before->error;
  ASSERT_TRUE(before->value.is_dict());
  EXPECT_EQ(false, before->value.GetDict().FindBool("ready"));
  EXPECT_EQ(false, before->value.GetDict().FindBool("completed"));
  EXPECT_EQ(0, before->value.GetDict().FindInt("readyCount"));
  EXPECT_EQ(0, evaluation_completions_);
  Send("test:release");
  auto after = State();
  ASSERT_TRUE(after->success) << after->error;
  ASSERT_TRUE(after->value.is_dict());
  EXPECT_EQ(true, after->value.GetDict().FindBool("ready"));
  EXPECT_EQ(true, after->value.GetDict().FindBool("completed"));
  EXPECT_EQ(1, after->value.GetDict().FindInt("readyCount"));
  EXPECT_EQ(1, evaluation_completions_);
  EXPECT_TRUE(container_->MarkAppReady());
  auto repeated = State();
  ASSERT_TRUE(repeated->success) << repeated->error;
  EXPECT_EQ(1, repeated->value.GetDict().FindInt("readyCount"));
}

TEST_F(XenonIpcEsmIntegrationTest, RejectedTopLevelAwaitReportsStartupFailure) {
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {app, ipcMain} from 'electron';
    let reject;
    const pending = new Promise((_resolve, fail) => { reject = fail; });
    ipcMain.on('test:reject', () => reject(new Error('fixture top-level rejection')));
    ipcMain.on('test:state', event => { event.returnValue = app.isReady(); });
    await pending;
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  EXPECT_FALSE(container_->MarkAppReady());
  Send("test:reject");
  EXPECT_NE(std::string::npos,
            container_->startup_error().find("fixture top-level rejection"));
  EXPECT_FALSE(container_->MarkAppReady());
  EXPECT_TRUE(exit_codes_.empty());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ((std::vector<int>{1}), exit_codes_);
  auto state = State();
  ASSERT_TRUE(state->success) << state->error;
  ASSERT_TRUE(state->value.is_bool());
  EXPECT_FALSE(state->value.GetBool());
}

TEST_F(XenonIpcEsmIntegrationTest,
       RejectedEntryCanDestroyContainerAfterIpcReturns) {
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    let reject;
    ipcMain.on('test:reject', () => reject(new Error('deferred exit')));
    await new Promise((_resolve, fail) => { reject = fail; });
  )JS"));
  bool returned_from_send = false;
  container_->SetAppExitHandler(base::BindRepeating(
      [](std::unique_ptr<XenonIpcMainContainer>* container, bool* returned,
         int code) {
        EXPECT_TRUE(*returned);
        EXPECT_EQ(1, code);
        container->reset();
      },
      &container_, &returned_from_send));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  Send("test:reject");
  returned_from_send = true;
  ASSERT_TRUE(container_);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(container_);
}

TEST_F(XenonIpcEsmIntegrationTest,
       EsmEntryAndDependenciesLoadInsideOriginalAsar) {
  const auto release = root_.AppendASCII("release");
  const auto archive = release.AppendASCII("resources").AppendASCII("app.asar");
  ASSERT_TRUE(base::CreateDirectory(archive.DirName()));
  asar::TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(
      archive,
      {
          {"package.json", R"({"type":"module","main":"entry.js"})"},
          {"entry.js", R"JS(
        import {app, ipcMain} from 'electron';
        import {answer} from './value.js';
        import extra from './extra.cjs';
        ipcMain.handle('test:asar', () => ({answer: answer + extra,
          appPath: app.getAppPath(), url: import.meta.url}));
      )JS"},
          {"value.js", "export const answer = 40;"},
          {"extra.cjs", "module.exports = 2;"},
      },
      false));
  ASSERT_TRUE(StartAt(release)) << container_->startup_error();
  auto result = Invoke("test:asar");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  EXPECT_EQ(42, value.FindInt("answer"));
  ASSERT_TRUE(value.FindString("appPath"));
  EXPECT_EQ(archive.AsUTF8Unsafe(), *value.FindString("appPath"));
  ASSERT_TRUE(value.FindString("url"));
  EXPECT_EQ(net::FilePathToFileURL(archive.AppendASCII("entry.js")).spec(),
            *value.FindString("url"));
}

TEST_F(XenonIpcEsmIntegrationTest,
       StreamPromisesCopyRealFilesAndPreserveNativeErrors) {
  const std::string payload = std::string(4096, 'x') + std::string("\0tail", 5);
  ASSERT_TRUE(Write("stream-source.bin", payload));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import {pipeline, finished} from 'node:stream/promises';
    import {createReadStream, createWriteStream, readFileSync} from 'node:fs';
    import {createRequire} from 'node:module';
    import {join} from 'node:path';
    const require = createRequire(import.meta.url);
    ipcMain.handle('test:stream-promises', async () => {
      const promises = require('stream/promises');
      const sourcePath = join(import.meta.dirname, 'stream-source.bin');
      const outputPath = join(import.meta.dirname, 'stream-output.bin');
      const source = createReadStream(sourcePath, {highWaterMark: 257});
      const destination = createWriteStream(outputPath, {highWaterMark: 1});
      let pauses = 0;
      source.on('pause', () => ++pauses);
      await pipeline(source, destination);
      // Node pipeline deliberately retains late-error guards. finished's
      // cleanup removes its own listeners without deleting those guards.
      const retainedListeners = [source.listeners('error'), destination.listeners('error')];
      await finished(source, {cleanup: true});
      await finished(destination, {cleanup: true});
      const missing = createReadStream(join(import.meta.dirname, 'missing.bin'));
      const failedOutput = createWriteStream(join(import.meta.dirname, 'failed-output.bin'));
      const failedClosed = new Promise(resolve => failedOutput.once('close', resolve));
      let originalError, pipelineError, finishedError;
      missing.on('error', error => { originalError = error; });
      try { await pipeline(missing, failedOutput); }
      catch (error) { pipelineError = error; }
      try { await finished(missing, {cleanup: true}); }
      catch (error) { finishedError = error; }
      // The failed reader can reject pipeline while the output's asynchronous
      // _construct is still running. Resource closure follows that completion.
      await failedClosed;
      return {
        aliases: promises === require('node:stream/promises') &&
          promises === require('stream').promises,
        namedExports: pipeline === promises.pipeline && finished === promises.finished,
        contents: readFileSync(outputPath).toString('base64'),
        backpressure: pauses > 1,
        closed: source.closed && destination.closed && missing.closed && failedOutput.closed,
        clean: [source, destination].every((stream, index) => {
          const current = stream.listeners('error'), previous = retainedListeners[index];
          return current.length === previous.length &&
            current.every((listener, position) => listener === previous[position]);
        }),
        errorIdentity: Boolean(originalError) && pipelineError === originalError &&
          finishedError === originalError,
        errorCode: pipelineError?.code,
      };
    });
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  auto result = Invoke("test:stream-promises");
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  for (const char* key : {"aliases", "namedExports", "backpressure", "closed",
                          "clean", "errorIdentity"}) {
    EXPECT_EQ(true, value.FindBool(key)) << key;
  }
  ASSERT_TRUE(value.FindString("contents"));
  EXPECT_EQ(base::Base64Encode(payload), *value.FindString("contents"));
  ASSERT_TRUE(value.FindString("errorCode"));
  EXPECT_EQ("ENOENT", *value.FindString("errorCode"));
  std::string copied;
  ASSERT_TRUE(base::ReadFileToString(app_path_.AppendASCII("stream-output.bin"),
                                     &copied));
  EXPECT_EQ(payload, copied);
}

TEST_F(XenonIpcEsmIntegrationTest,
       SynchronousHooksTransformCommonJsAndHonorConditionsAndCache) {
  ASSERT_TRUE(Write("node_modules/conditional/package.json", R"JSON({
    "exports":{"custom":"./custom.cjs","default":"./default.cjs"}
  })JSON"));
  ASSERT_TRUE(
      Write("node_modules/conditional/custom.cjs", "exports.answer = 40;"));
  ASSERT_TRUE(
      Write("node_modules/conditional/default.cjs", "exports.answer = 1;"));
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    const {registerHooks} = require('node:module');
    const trace = [];
    let loads = 0;
    const inner = registerHooks({
      resolve(specifier, context, next) {
        if (specifier === 'alias') {
          trace.push('inner:' + context.conditions.includes('require'));
          return next('conditional', {conditions: [...context.conditions, 'custom']});
        }
        return next(specifier, context);
      },
      load(url, context, next) {
        const result = next(url, context);
        if (url.endsWith('/custom.cjs')) {
          ++loads;
          result.source += '\nexports.answer += 2;';
        }
        return result;
      },
    });
    const outer = registerHooks({resolve(specifier, context, next) {
      if (specifier === 'alias') trace.push('outer');
      if (specifier === 'redirected-builtin') return next('node:path', context);
      return next(specifier, context);
    }});
    const first = require('alias');
    const same = first === require('alias');
    const builtin = require('redirected-builtin') === require('path');
    const resolvedBuiltin = require.resolve('redirected-builtin');
    outer.deregister();
    inner.deregister();
    const retained = require('./node_modules/conditional/custom.cjs') === first;
    let removed;
    try { require('alias'); } catch (error) { removed = error.code; }
    ipcMain.handle('test:hooks', () => ({answer: first.answer, same, builtin,
      resolvedBuiltin, retained, removed, loads, trace: trace.join(',')}));
  )JS"));
  ASSERT_TRUE(StartEntry("main.cjs")) << container_->startup_error();
  auto result = Invoke("test:hooks");
  ASSERT_TRUE(result->success) << result->error;
  const auto& value = result->value.GetDict();
  EXPECT_EQ(42, value.FindInt("answer"));
  EXPECT_EQ(1, value.FindInt("loads"));
  for (const char* key : {"same", "builtin", "retained"}) {
    EXPECT_EQ(true, value.FindBool(key)) << key;
  }
  ASSERT_TRUE(value.FindString("resolvedBuiltin"));
  EXPECT_EQ("path", *value.FindString("resolvedBuiltin"));
  ASSERT_TRUE(value.FindString("removed"));
  EXPECT_EQ("MODULE_NOT_FOUND", *value.FindString("removed"));
  ASSERT_TRUE(value.FindString("trace"));
  EXPECT_EQ("outer,inner:true,outer,inner:true", *value.FindString("trace"));
}

TEST_F(XenonIpcEsmIntegrationTest,
       SynchronousHooksLoadVirtualEsmAndCommonJsWithBinarySubviews) {
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import {registerHooks} from 'node:module';
    const trace = [];
    registerHooks({
      resolve(specifier, context, next) {
        if (specifier.startsWith('virtual:')) {
          trace.push(context.conditions.includes('import'));
          return {url: specifier, shortCircuit: true};
        }
        return next(specifier, context);
      },
      load(url, context, next) {
        if (url === 'virtual:esm') return {format: 'module', shortCircuit: true,
          source: "import legacy, {answer} from 'virtual:cjs'; export default {answer, same: legacy.answer === answer};"};
        if (url === 'virtual:cjs') {
          const text = Buffer.from('xxexports.answer = require("node:path").basename("/a/42");yy');
          return {format: 'commonjs', shortCircuit: true,
            source: new DataView(text.buffer, text.byteOffset + 2, text.byteLength - 4)};
        }
        return next(url, context);
      },
    });
    const first = await import('virtual:esm');
    const again = await import('virtual:esm');
    ipcMain.handle('test:virtual', () => ({...first.default,
      identity: first === again, importCondition: trace.every(Boolean)}));
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  auto result = Invoke("test:virtual");
  ASSERT_TRUE(result->success) << result->error;
  const auto& value = result->value.GetDict();
  ASSERT_TRUE(value.FindString("answer"));
  EXPECT_EQ("42", *value.FindString("answer"));
  for (const char* key : {"same", "identity", "importCondition"}) {
    EXPECT_EQ(true, value.FindBool(key)) << key;
  }
}

TEST_F(XenonIpcEsmIntegrationTest,
       SynchronousHooksRedirectIntoOriginalAsarAndValidateCachedJsonImports) {
  asar::TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(
      app_path_.AppendASCII("node_modules.asar"),
      {{"dependency/index.mjs", "export const answer = 42;"}}, false));
  ASSERT_TRUE(Write("value.json", R"({"answer":42})"));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import {registerHooks} from 'node:module';
    let attributeSeen = false;
    registerHooks({resolve(specifier, context, next) {
      if (specifier === 'dependency') return next(
        new URL('./node_modules.asar/dependency/index.mjs', context.parentURL).href,
        context);
      if (specifier === 'json-alias') {
        attributeSeen = Object.keys(context.importAttributes).length === 0;
        return {...next('./value.json', context), importAttributes: {type: 'json'}};
      }
      return next(specifier, context);
    }});
    const dependency = await import('dependency');
    const json = await import('json-alias');
    let missing;
    try { await import('./value.json'); } catch (error) { missing = error.code; }
    ipcMain.handle('test:archive-hooks', () => ({answer: dependency.answer,
      json: json.default.answer, attributeSeen, missing}));
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  auto result = Invoke("test:archive-hooks");
  ASSERT_TRUE(result->success) << result->error;
  const auto& value = result->value.GetDict();
  EXPECT_EQ(42, value.FindInt("answer"));
  EXPECT_EQ(42, value.FindInt("json"));
  EXPECT_EQ(true, value.FindBool("attributeSeen"));
  ASSERT_TRUE(value.FindString("missing"));
  EXPECT_EQ("ERR_IMPORT_ATTRIBUTE_MISSING", *value.FindString("missing"));
}

TEST_F(XenonIpcEsmIntegrationTest,
       SynchronousHooksPreserveThrownValuesAndRequireShortCircuit) {
  ASSERT_TRUE(Write("main.cjs", R"JS(
    const {ipcMain} = require('electron');
    const {registerHooks} = require('node:module');
    const marker = {reason: 'application hook failure'};
    registerHooks({resolve(specifier, context, next) {
      if (specifier === 'throwing') throw marker;
      if (specifier === 'incomplete') return {url: 'virtual:incomplete'};
      if (specifier === 'bad-load') return {url: 'virtual:bad', shortCircuit: true};
      return next(specifier, context);
    }, load(url, context, next) {
      if (url === 'virtual:bad') return {format: 'module', source: Promise.resolve(''), shortCircuit: true};
      return next(url, context);
    }});
    const errors = {};
    try { require('throwing'); } catch (error) { errors.identity = error === marker; }
    try { require('incomplete'); } catch (error) { errors.resolve = error.code; }
    try { require('bad-load'); } catch (error) { errors.load = error.code; }
    ipcMain.handle('test:hook-errors', () => errors);
  )JS"));
  ASSERT_TRUE(StartEntry("main.cjs")) << container_->startup_error();
  auto result = Invoke("test:hook-errors");
  ASSERT_TRUE(result->success) << result->error;
  const auto& value = result->value.GetDict();
  EXPECT_EQ(true, value.FindBool("identity"));
  for (const char* key : {"resolve", "load"}) {
    ASSERT_TRUE(value.FindString(key));
    EXPECT_EQ("ERR_INVALID_RETURN_PROPERTY_VALUE", *value.FindString(key));
  }
}

TEST_F(XenonIpcEsmIntegrationTest,
       SynchronousLoadHooksControlImportAttributesAndTypedModuleIdentity) {
  ASSERT_TRUE(Write("value.json", R"({"answer":42})"));
  ASSERT_TRUE(Write("main.mjs", R"JS(
    import {ipcMain} from 'electron';
    import {registerHooks} from 'node:module';
    let loads = 0;
    registerHooks({resolve(specifier, context, next) {
      if (specifier === 'virtual:typed') return {url: specifier, shortCircuit: true};
      return next(specifier, context);
    }, load(url, context, next) {
      if (url.endsWith('/value.json')) return next(url, {importAttributes: {type: 'json'}});
      if (url === 'virtual:typed') {
        ++loads;
        return {format: 'module', source: 'export default ' +
          JSON.stringify(context.importAttributes.type), shortCircuit: true};
      }
      return next(url, context);
    }});
    const json = await import('./value.json');
    const first = await import('virtual:typed', {with: {type: 'first'}});
    const second = await import('virtual:typed', {with: {type: 'second'}});
    const again = await import('virtual:typed', {with: {type: 'first'}});
    ipcMain.handle('test:load-attributes', () => ({answer: json.default.answer,
      first: first.default, second: second.default, same: first === again, loads}));
  )JS"));
  ASSERT_TRUE(StartEntry("main.mjs")) << container_->startup_error();
  auto result = Invoke("test:load-attributes");
  ASSERT_TRUE(result->success) << result->error;
  const auto& value = result->value.GetDict();
  EXPECT_EQ(42, value.FindInt("answer"));
  EXPECT_EQ(2, value.FindInt("loads"));
  EXPECT_EQ(true, value.FindBool("same"));
  ASSERT_TRUE(value.FindString("first"));
  ASSERT_TRUE(value.FindString("second"));
  EXPECT_EQ("first", *value.FindString("first"));
  EXPECT_EQ("second", *value.FindString("second"));
}

}  // namespace
}  // namespace xenon::ipc
