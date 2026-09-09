// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

#include <memory>
#include <string>
#include <tuple>
#include <utility>

#include "base/base64.h"
#include "base/base_paths.h"
#include "build/build_config.h"
#if BUILDFLAG(IS_WIN)
#include "base/base_paths_win.h"
#endif
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "base/test/test_future.h"
#include "components/version_info/version_info.h"
#include "gin/test/v8_test.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"
#include "xenon_overlay/services/xenon_node_executor.h"

namespace xenon::ipc {

namespace {

constexpr char kTestMainSource[] = R"JS(
const {app, BrowserWindow, ipcMain, systemPreferences, Menu, nativeImage} = require('electron');
const net = require('net');
const path = require('node:path');
const os = require('os');
let persistentCounter = 0;
let pendingNetSocket;
let createdWindow;
let preloadFailure;
let observedContents;
let senderEventCount = 0;
let senderOnceCount = 0;
let deletedProcessIds = [];
let contentsDestroyedCount = 0;
ipcMain.on('test:increment', (_event, amount) => {
  persistentCounter += amount;
});
ipcMain.handle('test:get', () => persistentCounter);
ipcMain.handle('test:async-add', async (_event, left, right) => left + right);
ipcMain.handle('test:delayed-add', (_event, left, right) =>
    new Promise(resolve => setTimeout(() => resolve(left + right), 10)));
ipcMain.handle('test:next-tick', () => new Promise(resolve => {
  const value = {reading: false};
  let scheduled = false;
  const result = process.nextTick(function(first, missing, last) {
    resolve({
      asynchronous: scheduled,
      returnsUndefined: result === undefined,
      sameObject: first === value,
      missingIsUndefined: missing === undefined,
      argumentCount: arguments.length,
      last,
    });
  }, value, undefined, 42);
  scheduled = true;
}));
ipcMain.handle('test:next-tick-invalid', () =>
    [undefined, null, 1, 'callback', {}].every(callback => {
      try {
        process.nextTick(callback);
        return false;
      } catch (error) {
        return error instanceof TypeError && error.code === 'ERR_INVALID_ARG_TYPE';
      }
    }));
ipcMain.on('test:get-sync', event => {
  event.returnValue = persistentCounter;
});
ipcMain.handle('test:is-ready', () => app.isReady());
ipcMain.handle('test:sender', event => ({
  processId: event.processId,
  frameId: event.frameId,
  senderProcessId: event.sender.processId,
  senderFrameId: event.sender.frameId,
}));
ipcMain.handle('test:sender-window', event => {
  const window = BrowserWindow.fromWebContents(event.sender);
  return window ? window.id : 0;
});
ipcMain.handle('test:guest-info', (event, id) => {
  const contents = require('electron').webContents.fromId(id);
  return contents ? {sameSender: contents === event.sender,
    hasOwnerWindow: Boolean(contents.getOwnerBrowserWindow()),
    preferences: contents.getLastWebPreferences()} : null;
});
ipcMain.on('test:reply', (event, value) => {
  event.reply('test:reply-result', value + 1);
});
ipcMain.on('test:net-write-before-connect', () => {
  pendingNetSocket = net.connect('xenon-pending-write-test');
  pendingNetSocket.write('queued-before-connect');
});
app.whenReady().then(() => {
  createdWindow = new BrowserWindow({show: false, webPreferences: {
    preload: path.join(__dirname, 'preload', 'entry.js'),
    contextIsolation: false,
    nodeIntegration: true,
  }});
  createdWindow.webContents.on('preload-error', (event, file, error) => {
    preloadFailure = {file, message: error.message,
      correctSender: event.sender === createdWindow.webContents};
  });
});
ipcMain.on('test:preload-failure', event => { event.returnValue = preloadFailure; });
ipcMain.on('test:mutate-web-preferences', event => {
  const snapshot = createdWindow.webContents.getLastWebPreferences();
  snapshot.preload = 'changed.js';
  event.returnValue = createdWindow.webContents.getLastWebPreferences();
});
ipcMain.handle('test:observe-sender', event => {
  observedContents = event.sender;
  observedContents.on('test:event', () => ++senderEventCount);
  observedContents.once('test:event', () => ++senderOnceCount);
  observedContents.on('render-view-deleted', (_event, processId) => {
    deletedProcessIds.push(processId);
  });
  observedContents.once('destroyed', () => ++contentsDestroyedCount);
  return {
    sameContents: observedContents === createdWindow.webContents,
    sameOwner: observedContents.getOwnerBrowserWindow() === createdWindow,
    sameWindow: BrowserWindow.fromWebContents(observedContents) === createdWindow,
    sameId: require('electron').webContents.fromId(observedContents.id) ===
        observedContents,
  };
});
ipcMain.on('test:emit-sender', event => event.sender.emit('test:event'));
ipcMain.on('test:sender-state', event => {
  event.returnValue = {
    sameSender: event.sender === observedContents,
    senderEventCount, senderOnceCount, deletedProcessIds,
    contentsDestroyedCount, destroyed: observedContents.isDestroyed(),
  };
});
ipcMain.on('test:contents-send', (_event, value) => {
  createdWindow.webContents.send('test:contents-result', value);
});
ipcMain.on('test:sender-send-frame', (event, frameId, value) => {
  event.returnValue = event.sender.sendToFrame(
      frameId, 'test:frame-result', value);
});
ipcMain.handle('test:electron-runtime', () => ({
  joinedPath: path.join('parent', 'child'),
  osRelease: os.release(),
  windowCount: BrowserWindow.getAllWindows().length,
  aeroGlass: systemPreferences.isAeroGlassEnabled(),
}));
ipcMain.handle('test:network-interfaces', () => os.networkInterfaces());
ipcMain.handle('test:window-bounds', () => {
  const window = BrowserWindow.getAllWindows()[0];
  window.setBounds({x: 25, y: 40, width: 960, height: 540});
  return window.getBounds();
});
ipcMain.handle('test:web-contents-user-agent', () => {
  const contents = BrowserWindow.getAllWindows()[0].webContents;
  const before = contents.getUserAgent();
  contents.setUserAgent('XenonTest/1.0');
  return {before, after: contents.getUserAgent()};
});
ipcMain.handle('test:path-parse-win32', (_event, targetPath) => path.win32.parse(targetPath));
ipcMain.handle('test:path-parse-posix', (_event, targetPath) => path.posix.parse(targetPath));
ipcMain.handle('test:app-paths', () => ({
  home: app.getPath('home'),
  temp: app.getPath('temp'),
  userData: app.getPath('userData'),
  desktop: app.getPath('desktop'),
  exe: app.getPath('exe'),
  processExe: process.execPath,
  chromeVersion: process.versions.chrome,
}));
let menuClosed = false;
let menuClicked = '';
ipcMain.handle('test:menu-popup', () => {
  menuClosed = false;
  menuClicked = '';
  const menu = Menu.buildFromTemplate([
    {
      type: 'normal',
      label: 'hide',
      icon: nativeImage.createFromPath('C:/icon.png'),
      click: () => { menuClicked = 'hide'; },
    },
    {type: 'separator'},
    {
      type: 'submenu',
      label: 'settings',
      submenu: [{
        type: 'checkbox',
        label: 'always',
        checked: true,
        click: () => { menuClicked = 'always'; },
      }],
    },
  ]);
  menu.once('menu-will-close', () => { menuClosed = true; });
  menu.popup({window: BrowserWindow.getAllWindows()[0]});
  return true;
});
ipcMain.handle('test:menu-state', () => ({menuClosed, menuClicked}));
)JS";

base::Value Arguments(std::initializer_list<base::Value> values) {
  base::ListValue list;
  for (const base::Value& value : values) {
    list.Append(value.Clone());
  }
  return base::Value(std::move(list));
}

base::Value FileSystemArguments(const std::string& operation,
                                const base::FilePath& path,
                                base::DictValue options = {}) {
  options.Set("operation", operation);
  options.Set("path", path.AsUTF8Unsafe());
  base::ListValue arguments;
  arguments.Append(std::move(options));
  return base::Value(std::move(arguments));
}

class FakeIpcRenderer : public xenon::ipc::mojom::IpcRenderer {
 public:
  mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> BindNewRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  void Dispatch(const std::string& channel, base::Value arguments) override {
    dispatch_future_.SetValue(channel, std::move(arguments));
  }

  base::test::TestFuture<std::string, base::Value>& dispatch_future() {
    return dispatch_future_;
  }

 private:
  mojo::Receiver<xenon::ipc::mojom::IpcRenderer> receiver_{this};
  base::test::TestFuture<std::string, base::Value> dispatch_future_;
};

class XenonIpcMainContainerTest : public gin::V8Test {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }

  void SetUp() override {
    gin::V8Test::SetUp();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    main_script_ = temp_dir_.GetPath().AppendASCII("main.js");
    ASSERT_TRUE(base::WriteFile(main_script_, kTestMainSource));
    base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-main-js",
                                                             main_script_);
    container_ = std::make_unique<XenonIpcMainContainer>();
    XenonIpcMainContainer::WindowHooks window_hooks;
    window_hooks.create = base::BindRepeating(
        [](int width, int height, bool show, bool frame, bool transparent,
           int32_t parent_id, const std::string& title, int32_t* window_id,
           uint64_t* hwnd, std::string* error) {
          *window_id = 1;
          *hwnd = 1;
          return true;
        });
    window_bounds_.Set("x", 0);
    window_bounds_.Set("y", 0);
    window_bounds_.Set("width", 800);
    window_bounds_.Set("height", 600);
    window_hooks.call = base::BindRepeating(
        [](XenonIpcMainContainerTest* self, int32_t window_id,
           const std::string& command, const base::Value& arguments,
           base::Value* result, std::string* error) {
          self->last_window_command_ = command;
          self->last_window_arguments_ = arguments.Clone();
          if (window_id != 1) {
            *error = "unknown test window";
            return false;
          }
          if (command == "set-bounds") {
            self->window_bounds_ = arguments.GetDict().Clone();
            *result = base::Value(self->window_bounds_.Clone());
            return true;
          }
          if (command == "get-bounds") {
            *result = base::Value(self->window_bounds_.Clone());
            return true;
          }
          if (command == "center") {
            return true;
          }
          if (command == "popup-menu") {
            return true;
          }
          if (command == "set-user-agent") {
            const std::string* value = arguments.GetDict().FindString("value");
            if (!value) {
              *error = "missing test user agent";
              return false;
            }
            self->user_agent_ = *value;
            return true;
          }
          if (command == "get-user-agent") {
            *result = base::Value(self->user_agent_);
            return true;
          }
          *error = "unsupported test command";
          return false;
        },
        base::Unretained(this));
    container_->SetWindowHooks(std::move(window_hooks));
    ASSERT_TRUE(container_->Initialize()) << container_->startup_error();
  }

  void TearDown() override {
    container_.reset();
    base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");
    base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-electron-app");
    gin::V8Test::TearDown();
  }

  base::ScopedTempDir temp_dir_;
  base::FilePath main_script_;
  base::DictValue window_bounds_;
  std::string user_agent_ = "DefaultAgent/1.0";
  std::string last_window_command_;
  base::Value last_window_arguments_;
  std::unique_ptr<XenonIpcMainContainer> container_;
};

TEST_F(XenonIpcMainContainerTest, MainStateSurvivesIndependentMessages) {
  container_->Send("renderer-1", "test:increment", Arguments({base::Value(2)}));
  container_->Send("renderer-2", "test:increment", Arguments({base::Value(3)}));

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-3", "test:get", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(5, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, BrowserWindowBoundsUseNativeBridge) {
  container_->MarkAppReady();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:window-bounds", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  const base::DictValue& bounds = result->value.GetDict();
  EXPECT_EQ(bounds.FindInt("x"), 25);
  EXPECT_EQ(bounds.FindInt("y"), 40);
  EXPECT_EQ(bounds.FindInt("width"), 960);
  EXPECT_EQ(bounds.FindInt("height"), 540);
}

TEST_F(XenonIpcMainContainerTest, MenuPopupSerializesTemplateAndDispatchesClicks) {
  container_->MarkAppReady();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> popup_future;
  container_->Invoke("renderer-1", "test:menu-popup", Arguments({}),
                     popup_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr popup_result = popup_future.Take();
  ASSERT_TRUE(popup_result->success) << popup_result->error;
  EXPECT_EQ("popup-menu", last_window_command_);
  ASSERT_TRUE(last_window_arguments_.is_dict());
  const base::ListValue* items =
      last_window_arguments_.GetDict().FindList("items");
  ASSERT_TRUE(items);
  ASSERT_EQ(items->size(), 3u);
  const base::DictValue& hide = (*items)[0].GetDict();
  EXPECT_EQ("normal", *hide.FindString("type"));
  EXPECT_EQ("hide", *hide.FindString("label"));
  EXPECT_EQ("C:/icon.png", *hide.FindString("icon"));
  EXPECT_EQ("separator", *(*items)[1].GetDict().FindString("type"));
  const base::DictValue& settings = (*items)[2].GetDict();
  EXPECT_EQ("submenu", *settings.FindString("type"));
  EXPECT_EQ("settings", *settings.FindString("label"));
  const base::ListValue* submenu = settings.FindList("submenu");
  ASSERT_TRUE(submenu);
  ASSERT_EQ(submenu->size(), 1u);
  EXPECT_EQ("checkbox", *(*submenu)[0].GetDict().FindString("type"));
  EXPECT_TRUE((*submenu)[0].GetDict().FindBool("checked").value_or(false));

  const int hide_id = hide.FindInt("id").value_or(0);
  ASSERT_GT(hide_id, 0);
  base::DictValue command;
  command.Set("commandId", hide_id);
  container_->DispatchWindowEvent(1, "native-menu-command",
                                  base::Value(std::move(command)));
  container_->DispatchWindowEvent(1, "native-menu-closed", base::Value());

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> state_future;
  container_->Invoke("renderer-1", "test:menu-state", Arguments({}),
                     state_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr state = state_future.Take();
  ASSERT_TRUE(state->success) << state->error;
  ASSERT_TRUE(state->value.is_dict());
  EXPECT_TRUE(state->value.GetDict().FindBool("menuClosed").value_or(false));
  EXPECT_EQ("hide", *state->value.GetDict().FindString("menuClicked"));
}

TEST_F(XenonIpcMainContainerTest, WebContentsUserAgentUsesNativeBridge) {
  container_->MarkAppReady();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:web-contents-user-agent", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& user_agents = result->value.GetDict();
  EXPECT_EQ("DefaultAgent/1.0", *user_agents.FindString("before"));
  EXPECT_EQ("XenonTest/1.0", *user_agents.FindString("after"));
  EXPECT_EQ("XenonTest/1.0", user_agent_);
}

TEST_F(XenonIpcMainContainerTest, FileSystemCallUsesRealFilesystem) {
  const base::FilePath root = temp_dir_.GetPath().AppendASCII("fs-root");
  const base::FilePath nested = root.AppendASCII("nested");
  const base::FilePath source = nested.AppendASCII("source.bin");
  const base::FilePath destination = nested.AppendASCII("destination.bin");

  auto non_recursive =
      PerformFileSystemCall(FileSystemArguments("mkdir", nested));
  ASSERT_FALSE(non_recursive->success);
  EXPECT_TRUE(non_recursive->error.starts_with("ENOENT:"));

  base::DictValue recursive;
  recursive.Set("recursive", true);
  auto mkdir = PerformFileSystemCall(
      FileSystemArguments("mkdir", nested, std::move(recursive)));
  ASSERT_TRUE(mkdir->success) << mkdir->error;
  EXPECT_TRUE(base::DirectoryExists(nested));

  base::DictValue write_options;
  write_options.Set("dataBase64", base::Base64Encode("real filesystem data"));
  auto write_result = PerformFileSystemCall(
      FileSystemArguments("write_file", source, std::move(write_options)));
  ASSERT_TRUE(write_result->success) << write_result->error;

  auto read = PerformFileSystemCall(
      FileSystemArguments("read_file", source));
  ASSERT_TRUE(read->success) << read->error;
  EXPECT_EQ(read->value.GetString(), base::Base64Encode("real filesystem data"));

  auto stat = PerformFileSystemCall(FileSystemArguments("stat", source));
  ASSERT_TRUE(stat->success) << stat->error;
  EXPECT_TRUE(stat->value.GetDict().FindBool("isFile").value_or(false));

  base::DictValue rename_options;
  rename_options.Set("destination", destination.AsUTF8Unsafe());
  auto rename_result = PerformFileSystemCall(
      FileSystemArguments("rename", source, std::move(rename_options)));
  ASSERT_TRUE(rename_result->success) << rename_result->error;
  EXPECT_FALSE(base::PathExists(source));
  EXPECT_TRUE(base::PathExists(destination));

  auto readdir = PerformFileSystemCall(
      FileSystemArguments("readdir", nested));
  ASSERT_TRUE(readdir->success) << readdir->error;
  ASSERT_EQ(readdir->value.GetList().size(), 1u);
  EXPECT_EQ(readdir->value.GetList().front().GetString(), "destination.bin");

  base::DictValue remove_options;
  remove_options.Set("recursive", true);
  auto remove_result = PerformFileSystemCall(
      FileSystemArguments("rm", root, std::move(remove_options)));
  ASSERT_TRUE(remove_result->success) << remove_result->error;
  EXPECT_FALSE(base::PathExists(root));
}

TEST_F(XenonIpcMainContainerTest, InvokeAwaitsPromise) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:async-add",
                     Arguments({base::Value(20), base::Value(22)}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, InvokeAwaitsPromiseResolvedByTimer) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:delayed-add",
                     Arguments({base::Value(20), base::Value(22)}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, NextTickForwardsArgumentsAsynchronously) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:next-tick", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& state = result->value.GetDict();
  EXPECT_EQ(true, state.FindBool("asynchronous"));
  EXPECT_EQ(true, state.FindBool("returnsUndefined"));
  EXPECT_EQ(true, state.FindBool("sameObject"));
  EXPECT_EQ(true, state.FindBool("missingIsUndefined"));
  EXPECT_EQ(3, state.FindInt("argumentCount"));
  EXPECT_EQ(42, state.FindInt("last"));
}

TEST_F(XenonIpcMainContainerTest, NextTickRejectsInvalidCallbackSynchronously) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:next-tick-invalid", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_bool());
  EXPECT_TRUE(result->value.GetBool());
}

TEST_F(XenonIpcMainContainerTest, SendSyncUsesEventReturnValue) {
  container_->Send("renderer-1", "test:increment", Arguments({base::Value(7)}));
  xenon::ipc::mojom::IpcResultPtr result =
      container_->SendSync("renderer-1", "test:get-sync", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(7, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, AppReadyFollowsBrowserLifecycle) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> before_future;
  container_->Invoke("renderer-1", "test:is-ready", Arguments({}),
                     before_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr before = before_future.Take();
  ASSERT_TRUE(before->success) << before->error;
  ASSERT_TRUE(before->value.is_bool());
  EXPECT_FALSE(before->value.GetBool());

  container_->MarkAppReady();

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> after_future;
  container_->Invoke("renderer-1", "test:is-ready", Arguments({}),
                     after_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr after = after_future.Take();
  ASSERT_TRUE(after->success) << after->error;
  ASSERT_TRUE(after->value.is_bool());
  EXPECT_TRUE(after->value.GetBool());
}

TEST_F(XenonIpcMainContainerTest, StandardElectronMainBootstrapRunsUnchanged) {
  container_->MarkAppReady();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:electron-runtime", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& runtime = result->value.GetDict();
  const std::string* joined_path = runtime.FindString("joinedPath");
  ASSERT_TRUE(joined_path);
  EXPECT_EQ(base::FilePath(FILE_PATH_LITERAL("parent"))
                .Append(FILE_PATH_LITERAL("child"))
                .AsUTF8Unsafe(),
            *joined_path);
  const std::string* os_release = runtime.FindString("osRelease");
  ASSERT_TRUE(os_release);
  EXPECT_FALSE(os_release->empty());
  EXPECT_EQ(1, runtime.FindInt("windowCount"));
  EXPECT_EQ(false, runtime.FindBool("aeroGlass"));
}

TEST_F(XenonIpcMainContainerTest,
       LoadsElectronPackageMainWithoutSourceChanges) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  const base::FilePath app_dir =
      temp_dir_.GetPath().AppendASCII("electron-app");
  const base::FilePath dist_dir = app_dir.AppendASCII("dist");
  const base::FilePath dependency_dir = app_dir.AppendASCII("node_modules")
                                            .AppendASCII("fixture-dependency")
                                            .AppendASCII("lib");
  ASSERT_TRUE(base::CreateDirectory(dist_dir));
  ASSERT_TRUE(base::CreateDirectory(dependency_dir));
  ASSERT_TRUE(base::WriteFile(app_dir.AppendASCII("package.json"),
                              R"JSON({"main":"dist/main.js"})JSON"));
  ASSERT_TRUE(base::WriteFile(dist_dir.AppendASCII("answer.json"),
                              R"JSON({"value":40})JSON"));
  ASSERT_TRUE(
      base::WriteFile(dependency_dir.DirName().AppendASCII("package.json"),
                      R"JSON({"main":"lib/index.cjs"})JSON"));
  ASSERT_TRUE(base::WriteFile(dependency_dir.AppendASCII("index.cjs"),
                              "module.exports = {increment: 2};"));
  ASSERT_TRUE(base::WriteFile(
      dist_dir.AppendASCII("main.js"),
      "const answer = require('./answer.json').value + "
      "require('fixture-dependency').increment;"
      "const {app, ipcMain} = require('electron');"
      "ipcMain.handle('package:main', () => answer);"
      "ipcMain.handle('package:app-path', () => app.getAppPath());"));
  base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-electron-app",
                                                           app_dir);

  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize()) << container_->startup_error();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "package:main", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> path_future;
  container_->Invoke("renderer-1", "package:app-path", Arguments({}),
                     path_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr path_result = path_future.Take();
  ASSERT_TRUE(path_result->success) << path_result->error;
  ASSERT_TRUE(path_result->value.is_string());
  base::FilePath normalized_app_dir;
  ASSERT_TRUE(base::NormalizeFilePath(app_dir, &normalized_app_dir));
  EXPECT_EQ(normalized_app_dir.AsUTF8Unsafe(), path_result->value.GetString());
}

TEST_F(XenonIpcMainContainerTest, LoadsPackagedMainModuleWithoutDiskFile) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  const base::FilePath app_path =
      temp_dir_.GetPath().AppendASCII("packaged-app");
  const base::FilePath virtual_main = app_path.AppendASCII("main.js");
  XenonIpcMainContainer::EmbeddedMainModule main_module{
      .source =
          "const {app, ipcMain} = require('electron');"
          "ipcMain.handle('packaged:value', () => ({"
          "  answer: 42, appPath: app.getAppPath(), filename: __filename"
          "}));",
      .virtual_path = virtual_main,
      .app_path = app_path,
      .app_name = "Packaged Test",
      .app_version = "1.0.0",
  };
  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize(std::move(main_module)))
      << container_->startup_error();

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "packaged:value", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  EXPECT_EQ(42, result->value.GetDict().FindInt("answer"));
  EXPECT_EQ(app_path.AsUTF8Unsafe(),
            *result->value.GetDict().FindString("appPath"));
  EXPECT_EQ(virtual_main.AsUTF8Unsafe(),
            *result->value.GetDict().FindString("filename"));
  EXPECT_FALSE(base::PathExists(virtual_main));
}

TEST_F(XenonIpcMainContainerTest, UsesExplicitExecutableWithoutRenamingIt) {
  container_.reset();
  const auto executable =
      temp_dir_.GetPath().AppendASCII("actual-launcher.exe");
  ASSERT_TRUE(base::WriteFile(executable, "fixture"));
  XenonIpcMainContainer::EmbeddedMainModule module{
      .source =
          "const {app, ipcMain} = require('electron');"
          "ipcMain.handle('identity', () => ({"
          "exec: process.execPath, argv: process.argv[0],"
          "appExe: app.getPath('exe'), version: app.getVersion()}));",
      .virtual_path = main_script_,
      .app_path = temp_dir_.GetPath(),
      .executable_path = executable,
      .app_name = "DifferentDisplayName",
      .app_version = "3.2.1",
  };
  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize(std::move(module)))
      << container_->startup_error();
  base::test::TestFuture<mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "identity", Arguments({}),
                     future.GetCallback());
  const auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  base::FilePath normalized;
  ASSERT_TRUE(base::NormalizeFilePath(executable, &normalized));
  for (const char* key : {"exec", "argv", "appExe"}) {
    EXPECT_EQ(normalized.AsUTF8Unsafe(),
              *result->value.GetDict().FindString(key));
  }
  EXPECT_EQ("3.2.1", *result->value.GetDict().FindString("version"));
}

TEST_F(XenonIpcMainContainerTest,
       LoadsNativeAddonBesideExplicitExecutable) {
  container_.reset();
  base::FilePath build_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &build_dir));
  const base::FilePath app_dir =
      temp_dir_.GetPath().AppendASCII("resources").AppendASCII("app");
  const base::FilePath main_dir = app_dir.AppendASCII("out");
  const base::FilePath runtime_dir =
      temp_dir_.GetPath().AppendASCII("runtime");
  ASSERT_TRUE(base::CreateDirectory(main_dir));
  ASSERT_TRUE(base::CreateDirectory(runtime_dir));
  const base::FilePath executable =
      runtime_dir.AppendASCII("hosted-app.exe");
  ASSERT_TRUE(base::WriteFile(main_dir.AppendASCII("main.js"), "fixture"));
  ASSERT_TRUE(base::WriteFile(executable, "fixture"));
  ASSERT_TRUE(base::CopyFile(
      build_dir.Append(FILE_PATH_LITERAL("test_addon.node")),
      runtime_dir.Append(FILE_PATH_LITERAL("test_addon.node"))));

  XenonIpcMainContainer::EmbeddedMainModule module{
      .source =
          "const {ipcMain} = require('electron');"
          "const path = require('path');"
          "const native = require(path.join(path.dirname(process.execPath),"
          "  'test_addon.node'));"
          "ipcMain.handle('explicit-native:add',"
          "  (_event, a, b) => native.Add(a, b));",
      .virtual_path = main_dir.AppendASCII("main.js"),
      .app_path = app_dir,
      .executable_path = executable,
  };
  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize(std::move(module)))
      << container_->startup_error();

  base::test::TestFuture<mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "explicit-native:add",
                     Arguments({base::Value(20), base::Value(22)}),
                     future.GetCallback());
  const auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest,
       InvalidExplicitExecutableFailsInitialization) {
  container_.reset();
  XenonIpcMainContainer::EmbeddedMainModule module{
      .source = "module.exports = {};",
      .virtual_path = main_script_,
      .app_path = temp_dir_.GetPath(),
      .executable_path = temp_dir_.GetPath().AppendASCII("missing.exe"),
  };
  container_ = std::make_unique<XenonIpcMainContainer>();
  EXPECT_FALSE(container_->Initialize(std::move(module)));
  EXPECT_NE(std::string::npos, container_->startup_error().find("executable"));
}

TEST_F(XenonIpcMainContainerTest, ExecutableResolutionDoesNotFabricateFiles) {
  base::FilePath actual;
  std::string error;
  ASSERT_TRUE(ResolveAppExecutable({}, &actual, &error)) << error;
  EXPECT_TRUE(base::PathExists(actual));
  EXPECT_FALSE(ResolveAppExecutable(
      base::FilePath(FILE_PATH_LITERAL("relative.exe")), &actual, &error));
  EXPECT_FALSE(ResolveAppExecutable(temp_dir_.GetPath(), &actual, &error));
  const auto missing = temp_dir_.GetPath().AppendASCII("missing.exe");
  EXPECT_FALSE(ResolveAppExecutable(missing, &actual, &error));
  EXPECT_TRUE(GetAppExecutableVersion(missing).empty());
  EXPECT_TRUE(GetAppExecutableVersion(main_script_).empty());
}

TEST_F(XenonIpcMainContainerTest, NetworkInterfacesComeFromTheOperatingSystem) {
  auto native = GetNetworkInterfaces();
  ASSERT_TRUE(native->success) << native->error;
  ASSERT_TRUE(native->value.is_dict());
  base::test::TestFuture<mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:network-interfaces", Arguments({}),
                     future.GetCallback());
  const auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  for (auto [name, addresses] : result->value.GetDict()) {
    EXPECT_FALSE(name.empty());
    ASSERT_TRUE(addresses.is_list());
    for (const auto& address : addresses.GetList()) {
      const auto& entry = address.GetDict();
      ASSERT_TRUE(entry.FindString("address"));
      ASSERT_TRUE(entry.FindString("netmask"));
      ASSERT_TRUE(entry.FindString("mac"));
      EXPECT_EQ(17u, entry.FindString("mac")->size());
      ASSERT_TRUE(entry.FindBool("internal").has_value());
      const auto* family = entry.FindString("family");
      ASSERT_TRUE(family);
      EXPECT_TRUE(*family == "IPv4" || *family == "IPv6");
      const auto* cidr = entry.Find("cidr");
      ASSERT_TRUE(cidr);
      EXPECT_TRUE(cidr->is_none() || cidr->is_string());
      EXPECT_EQ(*family == "IPv6", entry.contains("scopeid"));
    }
  }
}

#if BUILDFLAG(IS_WIN)
TEST_F(XenonIpcMainContainerTest, ExecutableVersionUsesRealVersionResources) {
  base::FilePath source_root;
  ASSERT_TRUE(
      base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &source_root));
  const auto fixture = source_root.AppendASCII("base/test/data")
                           .AppendASCII("file_version_info_unittest")
                           .AppendASCII("FileVersionInfoTest1.dll");
  // The fixture's numeric version is 1.0.0.1; its localized display string
  // intentionally differs. Runtime identity uses the numeric version.
  EXPECT_EQ("1.0.0.1", GetAppExecutableVersion(fixture));
}
#endif

TEST_F(XenonIpcMainContainerTest, PreservesRendererFrameIdentity) {
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke(endpoint, "test:sender", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& sender = result->value.GetDict();
  EXPECT_EQ(17, sender.FindInt("processId"));
  EXPECT_EQ(23, sender.FindInt("frameId"));
  EXPECT_EQ(17, sender.FindInt("senderProcessId"));
  EXPECT_EQ(23, sender.FindInt("senderFrameId"));
}

TEST_F(XenonIpcMainContainerTest, ResolvesBrowserWindowFromRendererSender) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke(endpoint, "test:sender-window", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(1, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, MainCanReplyToOriginatingRenderer) {
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23);
  container_->Send(endpoint, "test:reply", Arguments({base::Value(41)}));

  EXPECT_EQ("test:reply-result", renderer.dispatch_future().Get<0>());
  const base::Value& arguments = renderer.dispatch_future().Get<1>();
  ASSERT_TRUE(arguments.is_list());
  ASSERT_EQ(1u, arguments.GetList().size());
  EXPECT_EQ(42, arguments.GetList()[0].GetInt());
}

TEST_F(XenonIpcMainContainerTest, PreloadPreferencesBelongToSenderWindow) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  auto result = container_->SendSync(
      endpoint, "__xenon:renderer-web-preferences", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  const auto expected_preload = temp_dir_.GetPath()
                                    .AppendASCII("preload")
                                    .AppendASCII("entry.js")
                                    .AsUTF8Unsafe();
  ASSERT_TRUE(result->value.is_dict());
  ASSERT_TRUE(result->value.GetDict().FindString("preload"));
  EXPECT_STRCASEEQ(expected_preload.c_str(),
                   result->value.GetDict().FindString("preload")->c_str());
  EXPECT_EQ(false, result->value.GetDict().FindBool("contextIsolation"));
  EXPECT_EQ(true, result->value.GetDict().FindBool("nodeIntegration"));

  result = container_->SendSync(endpoint, "test:mutate-web-preferences",
                                Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.GetDict().FindString("preload"));
  EXPECT_STRCASEEQ(expected_preload.c_str(),
                   result->value.GetDict().FindString("preload")->c_str());

  FakeIpcRenderer unattached_renderer;
  const std::string unattached =
      container_->AddRenderer(unattached_renderer.BindNewRemote(), 18, 24);
  result = container_->SendSync(unattached, "__xenon:renderer-web-preferences",
                                Arguments({base::Value(1)}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.is_none());
}

TEST_F(XenonIpcMainContainerTest,
       GuestIdentitySurvivesNavigationAndRoutesToHost) {
  container_->MarkAppReady();
  FakeIpcRenderer owner, guest, replacement, unrelated;
  const auto owner_id =
      container_->AddRenderer(owner.BindNewRemote(), 17, 23, 1);
  const auto unrelated_id =
      container_->AddRenderer(unrelated.BindNewRemote(), 19, 25, 2);
  base::DictValue preferences;
  preferences.Set("preload", "C:\\app\\renderer.asar\\preload.js");
  preferences.Set("contextIsolation", false);
  preferences.Set("nodeIntegration", false);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> registered;
  container_->Invoke(
      owner_id, "__xenon:register-guest",
      Arguments({base::Value(-1), base::Value(preferences.Clone())}),
      registered.GetCallback());
  auto result = registered.Take();
  ASSERT_TRUE(result->success) << result->error;
  const int public_id = result->value.GetInt();
  const auto guest_endpoint =
      container_->AddRenderer(guest.BindNewRemote(), 18, 24, -1);
  auto prefs =
      container_->SendSync(guest_endpoint, "__xenon:renderer-web-preferences",
                           Arguments({base::Value(true)}));
  ASSERT_TRUE(prefs->success) << prefs->error;
  EXPECT_EQ(prefs->value.GetDict(), preferences);
  container_->RemoveRenderer(guest_endpoint);
  const auto next_endpoint =
      container_->AddRenderer(replacement.BindNewRemote(), 20, 26, -1);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> state;
  container_->Invoke(next_endpoint, "test:guest-info",
                     Arguments({base::Value(public_id)}), state.GetCallback());
  result = state.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(result->value.GetDict().FindBool("sameSender"), true);
  EXPECT_EQ(result->value.GetDict().FindBool("hasOwnerWindow"), false);

  container_->Send(next_endpoint, "__xenon:send-to-host",
                   Arguments({base::Value("fixture"),
                              base::Value(base::ListValue().Append(42))}));
  auto [channel, args] = owner.dispatch_future().Take();
  EXPECT_EQ(channel, "__xenon:guest-event");
  EXPECT_EQ(args.GetList()[0].GetInt(), public_id);
  EXPECT_EQ(args.GetList()[1].GetString(), "ipc-message");

  container_->Invoke(
      unrelated_id, "__xenon:guest-call",
      Arguments(
          {base::Value(public_id), base::Value("loadURL"),
           base::Value(base::ListValue().Append("https://example.test/"))}),
      state.GetCallback());
  EXPECT_FALSE(state.Take()->success);
  container_->DispatchWindowEvent(-1, "guest-destroyed", base::Value());
  auto destroyed_event = owner.dispatch_future().Take();
  EXPECT_EQ(std::get<1>(destroyed_event).GetList()[1].GetString(), "destroyed");
  container_->Invoke(owner_id, "test:guest-info",
                     Arguments({base::Value(public_id)}), state.GetCallback());
  result = state.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.is_none());
}

TEST_F(XenonIpcMainContainerTest, PreloadErrorIsDeliveredToOwningWebContents) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  container_->Send(
      endpoint, "__xenon:preload-error",
      Arguments({base::Value("entry.js"), base::Value("fixture failure")}));
  auto result =
      container_->SendSync(endpoint, "test:preload-failure", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  EXPECT_EQ("entry.js", *result->value.GetDict().FindString("file"));
  EXPECT_EQ("fixture failure", *result->value.GetDict().FindString("message"));
  EXPECT_EQ(true, result->value.GetDict().FindBool("correctSender"));
}

TEST_F(XenonIpcMainContainerTest, SenderUsesStableWindowWebContents) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke(endpoint, "test:observe-sender", Arguments({}),
                     future.GetCallback());
  auto result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  for (const char* key :
       {"sameContents", "sameOwner", "sameWindow", "sameId"}) {
    EXPECT_EQ(true, result->value.GetDict().FindBool(key)) << key;
  }
  container_->Send(endpoint, "test:emit-sender", Arguments({}));
  container_->Send(endpoint, "test:emit-sender", Arguments({}));
  result = container_->SendSync(endpoint, "test:sender-state", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(true, result->value.GetDict().FindBool("sameSender"));
  EXPECT_EQ(2, result->value.GetDict().FindInt("senderEventCount"));
  EXPECT_EQ(1, result->value.GetDict().FindInt("senderOnceCount"));

  container_->RemoveRenderer(endpoint);
  FakeIpcRenderer next_renderer;
  const std::string next_endpoint =
      container_->AddRenderer(next_renderer.BindNewRemote(), 18, 24, 1);
  result = container_->SendSync(next_endpoint, "test:sender-state", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(true, result->value.GetDict().FindBool("sameSender"));
  EXPECT_EQ(false, result->value.GetDict().FindBool("destroyed"));
  const auto* deleted = result->value.GetDict().FindList("deletedProcessIds");
  ASSERT_TRUE(deleted);
  ASSERT_EQ(1u, deleted->size());
  EXPECT_EQ(17, (*deleted)[0].GetInt());

  container_->DispatchWindowEvent(1, "closed", base::Value());
  container_->DispatchWindowEvent(1, "closed", base::Value());
  result = container_->SendSync("observer", "test:sender-state", Arguments({}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(true, result->value.GetDict().FindBool("destroyed"));
  EXPECT_EQ(1, result->value.GetDict().FindInt("contentsDestroyedCount"));
}

TEST_F(XenonIpcMainContainerTest, WebContentsSendsOnlyToItsRegisteredRenderer) {
  container_->MarkAppReady();
  FakeIpcRenderer renderer;
  FakeIpcRenderer unrelated_renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23, 1);
  const std::string unrelated_endpoint =
      container_->AddRenderer(unrelated_renderer.BindNewRemote(), 18, 24, 2);
  // No incoming IPC from the destination is needed to establish routing.
  container_->Send(unrelated_endpoint, "test:contents-send",
                   Arguments({base::Value(42)}));
  auto [channel, arguments] = renderer.dispatch_future().Take();
  EXPECT_EQ("test:contents-result", channel);
  EXPECT_EQ(42, arguments.GetList()[0].GetInt());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(unrelated_renderer.dispatch_future().IsReady());

  auto result = container_->SendSync(
      endpoint, "test:sender-send-frame",
      Arguments({base::Value(23), base::Value(43)}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_TRUE(result->value.GetBool());
  auto [frame_channel, frame_arguments] = renderer.dispatch_future().Take();
  EXPECT_EQ("test:frame-result", frame_channel);
  EXPECT_EQ(43, frame_arguments.GetList()[0].GetInt());

  // A frame in a different WebContents must not receive this message.
  result = container_->SendSync(
      endpoint, "test:sender-send-frame",
      Arguments({Arguments({base::Value(18), base::Value(24)}), base::Value(44)}));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_FALSE(result->value.GetBool());
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(renderer.dispatch_future().IsReady());
  EXPECT_FALSE(unrelated_renderer.dispatch_future().IsReady());
}

TEST_F(XenonIpcMainContainerTest, NetSocketBuffersWritesUntilConnected) {
  FakeIpcRenderer renderer;
  const std::string endpoint =
      container_->AddRenderer(renderer.BindNewRemote(), 17, 23);

  container_->Send(endpoint, "test:net-write-before-connect", Arguments({}));
  auto [connect_channel, connect_arguments] =
      renderer.dispatch_future().Take();
  ASSERT_EQ("__xenon:net:connect", connect_channel);
  ASSERT_TRUE(connect_arguments.is_list());
  ASSERT_EQ(1u, connect_arguments.GetList().size());
  const base::DictValue& connect = connect_arguments.GetList()[0].GetDict();
  const std::string* from_id = connect.FindString("fromId");
  ASSERT_TRUE(from_id);

  base::DictValue connected;
  connected.Set("toId", *from_id);
  connected.Set("peerId", "renderer-test-peer");
  container_->Send(
      endpoint, "__xenon:net:connected",
      Arguments({base::Value(std::move(connected))}));

  auto [data_channel, data_arguments] = renderer.dispatch_future().Take();
  ASSERT_EQ("__xenon:net:data", data_channel);
  ASSERT_TRUE(data_arguments.is_list());
  ASSERT_EQ(1u, data_arguments.GetList().size());
  const base::DictValue& data = data_arguments.GetList()[0].GetDict();
  EXPECT_EQ("renderer-test-peer", *data.FindString("toId"));
  const base::DictValue* wire = data.FindDict("wire");
  ASSERT_TRUE(wire);
  const std::string* encoded = wire->FindString("d");
  ASSERT_TRUE(encoded);
  EXPECT_FALSE(encoded->empty());
}

TEST_F(XenonIpcMainContainerTest, CommonJsCanRequireNodeApiAddon) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  base::FilePath executable_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  const base::FilePath app_dir = temp_dir_.GetPath().AppendASCII("native-app");
  ASSERT_TRUE(base::CreateDirectory(app_dir));
  ASSERT_TRUE(base::CopyFile(
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node")),
      app_dir.Append(FILE_PATH_LITERAL("test_addon.node"))));
  ASSERT_TRUE(base::WriteFile(app_dir.AppendASCII("package.json"),
                              R"JSON({"main":"main.js"})JSON"));
  ASSERT_TRUE(base::WriteFile(
      app_dir.AppendASCII("main.js"),
      "const {ipcMain} = require('electron');"
      "const native = require('./test_addon.node');"
      "ipcMain.handle('native:add', (_event, a, b) => native.Add(a, b));"));
  base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-electron-app",
                                                           app_dir);

  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize()) << container_->startup_error();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "native:add",
                     Arguments({base::Value(20), base::Value(22)}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_int());
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonIpcMainContainerTest, NodeAddonCacheIsIsolatedByCanonicalPath) {
  base::FilePath executable_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  const base::FilePath executable_addon =
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node"));

  base::ScopedTempDir addon_dir;
  ASSERT_TRUE(addon_dir.CreateUniqueTempDirUnderPath(executable_dir));
  const base::FilePath nested_addon =
      addon_dir.GetPath().Append(FILE_PATH_LITERAL("test_addon.node"));
  ASSERT_TRUE(base::CopyFile(executable_addon, nested_addon));

  XenonNodeExecutor executor;
  std::string load_error;
  ASSERT_TRUE(executor.LoadAddonFromCurrentThread(nested_addon.AsUTF8Unsafe(),
                                                  &load_error))
      << load_error;
  EXPECT_TRUE(executor.HasModule(nested_addon.AsUTF8Unsafe()));
  EXPECT_FALSE(executor.HasModule(executable_addon.AsUTF8Unsafe()));
}

#if BUILDFLAG(IS_WIN)
TEST_F(XenonIpcMainContainerTest,
       NodeAddonRedirectsHostLibrariesToRuntimeDirectory) {
  base::FilePath executable_dir;
  base::FilePath system_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  ASSERT_TRUE(base::PathService::Get(base::DIR_SYSTEM, &system_dir));

  base::ScopedTempDir runtime_dir;
  ASSERT_TRUE(runtime_dir.CreateUniqueTempDir());
  const base::FilePath relative_library =
      base::FilePath(FILE_PATH_LITERAL("private-sdk"))
          .Append(FILE_PATH_LITERAL("version.dll"));
  const base::FilePath hosted_library =
      runtime_dir.GetPath().Append(relative_library);
  ASSERT_TRUE(base::CreateDirectory(hosted_library.DirName()));
  ASSERT_TRUE(base::CopyFile(
      system_dir.Append(FILE_PATH_LITERAL("version.dll")), hosted_library));

  const base::FilePath host_relative_request =
      executable_dir.Append(relative_library);
  ASSERT_FALSE(base::PathExists(host_relative_request));

  const base::FilePath addon_path =
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node"));
  XenonNodeExecutor executor;
  executor.SetRuntimeDirectory(runtime_dir.GetPath());
  base::ListValue arguments;
  arguments.Append(host_relative_request.AsUTF8Unsafe());
  base::Value result;
  std::string error;
  ASSERT_TRUE(executor.InvokeExportFromCurrentThread(
      addon_path.AsUTF8Unsafe(), "CanLoadLibrary",
      base::Value(std::move(arguments)), &result, &error))
      << error;
  ASSERT_TRUE(result.is_bool());
  EXPECT_TRUE(result.GetBool());
}
#endif

TEST_F(XenonIpcMainContainerTest, NodeAddonResolvesCallbacksNestedInObjects) {
  base::FilePath executable_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  const base::FilePath addon_path =
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node"));

  XenonNodeExecutor executor;
  std::string load_error;
  ASSERT_TRUE(executor.LoadAddonFromCurrentThread(addon_path.AsUTF8Unsafe(),
                                                  &load_error))
      << load_error;

  int32_t received_callback_id = 0;
  std::vector<base::Value> received_args;
  executor.SetCallbackHandlers(
      base::BindRepeating(
          [](int32_t* received_id, std::vector<base::Value>* received_args,
             int32_t client_id, int32_t callback_id,
             std::vector<base::Value> args) {
            EXPECT_EQ(17, client_id);
            *received_id = callback_id;
            *received_args = std::move(args);
          },
          &received_callback_id, &received_args),
      base::BindRepeating([](int32_t, int32_t) {}));

  base::DictValue callback_wire;
  callback_wire.Set("__xenon_node_wire_type__", "callback");
  callback_wire.Set("callback_id", 41);
  base::DictValue handler;
  handler.Set("onValue", std::move(callback_wire));
  base::ListValue handlers;
  handlers.Append(std::move(handler));
  base::DictValue options;
  options.Set("handlers", std::move(handlers));

  auto argument = xenon::mojom::NodeInvokeArg::New();
  argument->is_callback = false;
  argument->callback_id = 0;
  argument->value = base::Value(std::move(options));
  std::vector<xenon::mojom::NodeInvokeArgPtr> arguments;
  arguments.push_back(std::move(argument));

  base::test::TestFuture<
      bool, base::Value, std::vector<xenon::mojom::NodeCallbackResultPtr>,
      std::string>
      invoke_future;
  executor.InvokeFunction(addon_path.AsUTF8Unsafe(), "InvokeNestedCallback",
                          /*client_id=*/17, std::move(arguments),
                          base::BindOnce(
                              [](decltype(invoke_future)* future, bool success,
                                 base::Value result,
                                 std::vector<
                                     xenon::mojom::NodeCallbackResultPtr>
                                     callback_results,
                                 const std::string& error) {
                                future->SetValue(
                                    success, std::move(result),
                                    std::move(callback_results), error);
                              },
                              &invoke_future));
  auto [success, result, callback_results, invoke_error] = invoke_future.Take();
  EXPECT_TRUE(success) << invoke_error;
  EXPECT_EQ(41, received_callback_id);
  ASSERT_EQ(1u, received_args.size());
  ASSERT_TRUE(received_args[0].is_string());
  EXPECT_EQ("nested callback payload", received_args[0].GetString());
}

TEST_F(XenonIpcMainContainerTest, NodeAddonReturnsCallableFunctionHandles) {
  base::FilePath executable_dir;
  ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
  const base::FilePath addon_path =
      executable_dir.Append(FILE_PATH_LITERAL("test_addon.node"));

  XenonNodeExecutor executor;
  std::string load_error;
  ASSERT_TRUE(
      executor.LoadAddonFromCurrentThread(addon_path.AsUTF8Unsafe(), &load_error))
      << load_error;

  std::vector<base::Value> received_args;
  executor.SetCallbackHandlers(
      base::BindRepeating(
          [](std::vector<base::Value>* received_args, int32_t client_id,
             int32_t callback_id, std::vector<base::Value> args) {
            EXPECT_EQ(17, client_id);
            EXPECT_EQ(42, callback_id);
            *received_args = std::move(args);
          },
          &received_args),
      base::BindRepeating([](int32_t, int32_t) {}));

  auto callback_argument = xenon::mojom::NodeInvokeArg::New();
  callback_argument->is_callback = true;
  callback_argument->callback_id = 42;
  std::vector<xenon::mojom::NodeInvokeArgPtr> callback_arguments;
  callback_arguments.push_back(std::move(callback_argument));

  base::test::TestFuture<
      bool, base::Value, std::vector<xenon::mojom::NodeCallbackResultPtr>,
      std::string>
      invoke_future;
  executor.InvokeFunction(
      addon_path.AsUTF8Unsafe(), "InvokeCallbackWithFunction",
      /*client_id=*/17, std::move(callback_arguments),
      base::BindOnce(
          [](decltype(invoke_future)* future, bool success, base::Value result,
             std::vector<xenon::mojom::NodeCallbackResultPtr> callback_results,
             const std::string& error) {
            future->SetValue(success, std::move(result),
                             std::move(callback_results), error);
          },
          &invoke_future));
  auto [success, result, callback_results, invoke_error] = invoke_future.Take();
  ASSERT_TRUE(success) << invoke_error;
  ASSERT_EQ(1u, received_args.size());
  ASSERT_TRUE(received_args[0].is_dict());
  const base::DictValue& function_wire = received_args[0].GetDict();
  EXPECT_EQ("native_function",
            *function_wire.FindString("__xenon_node_wire_type__"));
  const std::optional<int> instance_id = function_wire.FindInt("instance_id");
  ASSERT_TRUE(instance_id);

  base::DictValue undefined_wire;
  undefined_wire.Set("__xenon_node_wire_type__", "undefined");
  auto this_argument = xenon::mojom::NodeInvokeArg::New();
  this_argument->value = base::Value(std::move(undefined_wire));
  auto value_argument = xenon::mojom::NodeInvokeArg::New();
  value_argument->value = base::Value(41);
  std::vector<xenon::mojom::NodeInvokeArgPtr> call_arguments;
  call_arguments.push_back(std::move(this_argument));
  call_arguments.push_back(std::move(value_argument));

  base::test::TestFuture<
      bool, base::Value, std::vector<xenon::mojom::NodeCallbackResultPtr>,
      std::string>
      call_future;
  executor.InvokeInstance(addon_path.AsUTF8Unsafe(), *instance_id, "call",
                          /*client_id=*/17, std::move(call_arguments),
                          base::BindOnce(
                              [](decltype(call_future)* future, bool success,
                                 base::Value result,
                                 std::vector<
                                     xenon::mojom::NodeCallbackResultPtr>
                                     callback_results,
                                 const std::string& error) {
                                future->SetValue(
                                    success, std::move(result),
                                    std::move(callback_results), error);
                              },
                              &call_future));
  auto [call_success, call_result, call_callbacks, call_error] =
      call_future.Take();
  ASSERT_TRUE(call_success) << call_error;
  ASSERT_TRUE(call_result.is_int());
  EXPECT_EQ(42, call_result.GetInt());
}

TEST_F(XenonIpcMainContainerTest, StandardPathModuleParsesRootsAndPosixWin32) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> win32_future;
  container_->Invoke("renderer-1", "test:path-parse-win32",
                     Arguments({base::Value("C:\\Windows\\System32\\cmd.exe")}),
                     win32_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr win32_result = win32_future.Take();
  ASSERT_TRUE(win32_result->success) << win32_result->error;
  ASSERT_TRUE(win32_result->value.is_dict());
  const base::DictValue& win32_dict = win32_result->value.GetDict();
  EXPECT_EQ("C:\\", *win32_dict.FindString("root"));
  EXPECT_EQ("C:\\Windows\\System32", *win32_dict.FindString("dir"));
  EXPECT_EQ("cmd.exe", *win32_dict.FindString("base"));
  EXPECT_EQ(".exe", *win32_dict.FindString("ext"));
  EXPECT_EQ("cmd", *win32_dict.FindString("name"));

  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> posix_future;
  container_->Invoke("renderer-1", "test:path-parse-posix",
                     Arguments({base::Value("/usr/local/bin/node")}),
                     posix_future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr posix_result = posix_future.Take();
  ASSERT_TRUE(posix_result->success) << posix_result->error;
  ASSERT_TRUE(posix_result->value.is_dict());
  const base::DictValue& posix_dict = posix_result->value.GetDict();
  EXPECT_EQ("/", *posix_dict.FindString("root"));
  EXPECT_EQ("/usr/local/bin", *posix_dict.FindString("dir"));
  EXPECT_EQ("node", *posix_dict.FindString("base"));
  EXPECT_EQ("", *posix_dict.FindString("ext"));
  EXPECT_EQ("node", *posix_dict.FindString("name"));
}

TEST_F(XenonIpcMainContainerTest, AppGetPathAndDynamicVersions) {
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "test:app-paths", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const base::DictValue& dict = result->value.GetDict();

  const std::string* home = dict.FindString("home");
  ASSERT_TRUE(home);
  EXPECT_FALSE(home->empty());

  const std::string* temp = dict.FindString("temp");
  ASSERT_TRUE(temp);
  EXPECT_FALSE(temp->empty());

  const std::string* userData = dict.FindString("userData");
  ASSERT_TRUE(userData);
  EXPECT_FALSE(userData->empty());

  const std::string* desktop = dict.FindString("desktop");
  ASSERT_TRUE(desktop);
  EXPECT_FALSE(desktop->empty());

  const std::string* exe = dict.FindString("exe");
  ASSERT_TRUE(exe);
  EXPECT_FALSE(exe->empty());
  EXPECT_EQ(*exe, *dict.FindString("processExe"));
  EXPECT_TRUE(base::PathExists(base::FilePath::FromUTF8Unsafe(*exe)));

  const std::string* chromeVersion = dict.FindString("chromeVersion");
  ASSERT_TRUE(chromeVersion);
  EXPECT_EQ(std::string(version_info::GetVersionNumber()), *chromeVersion);
}

TEST_F(XenonIpcMainContainerTest, InitializationFailsForInvalidMainScript) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  const base::FilePath invalid_main =
      temp_dir_.GetPath().AppendASCII("invalid-main.js");
  ASSERT_TRUE(base::WriteFile(invalid_main, "function broken("));
  base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-main-js",
                                                           invalid_main);

  container_ = std::make_unique<XenonIpcMainContainer>();
  EXPECT_FALSE(container_->Initialize());
  EXPECT_FALSE(container_->is_initialized());
  EXPECT_FALSE(container_->startup_error().empty());
}

TEST_F(XenonIpcMainContainerTest, PackageMainCanPointToItsOwnDirectory) {
  container_.reset();
  base::CommandLine::ForCurrentProcess()->RemoveSwitch("xenon-main-js");

  const base::FilePath app_dir =
      temp_dir_.GetPath().AppendASCII("self-main-app");
  ASSERT_TRUE(base::CreateDirectory(app_dir));
  ASSERT_TRUE(base::WriteFile(app_dir.AppendASCII("package.json"),
                              R"JSON({"main":"."})JSON"));
  ASSERT_TRUE(base::WriteFile(app_dir.AppendASCII("index.js"),
                              "const {ipcMain} = require('electron');"
                              "ipcMain.handle('self-main:value', () => 42);"));
  base::CommandLine::ForCurrentProcess()->AppendSwitchPath("xenon-electron-app",
                                                           app_dir);

  container_ = std::make_unique<XenonIpcMainContainer>();
  ASSERT_TRUE(container_->Initialize()) << container_->startup_error();
  base::test::TestFuture<xenon::ipc::mojom::IpcResultPtr> future;
  container_->Invoke("renderer-1", "self-main:value", Arguments({}),
                     future.GetCallback());
  xenon::ipc::mojom::IpcResultPtr result = future.Take();
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(42, result->value.GetInt());
}

}  // namespace

}  // namespace xenon::ipc
