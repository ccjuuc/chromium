// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_service_impl.h"

#include <memory>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/test/run_until.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "build/build_config.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "v8/include/v8.h"

namespace xenon {
namespace {

using NodeRemote = mojo::Remote<ipc::mojom::NodeAddonHost>;
using ResultFuture = base::test::TestFuture<ipc::mojom::IpcResultPtr>;
constexpr char kContext[] = "owner-lifecycle-test";
constexpr char kConstructChannel[] = "__xenon:node-addon:construct-export";
constexpr char kInvokeChannel[] = "__xenon:node-addon:invoke-instance";

struct InstanceHandle {
  int32_t id = 0;
  std::string token;
};

// Use the real service ReceiverSet and real addon. Calling the generated async
// overloads of [Sync] methods lets this same-thread fixture exercise Mojo's
// current receiver context without blocking its own service dispatch.
class XenonServiceOwnerTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }

  void SetUp() override {
    v8::V8::SetFlagsFromString("--no-freeze-flags-after-init");
    base::FilePath executable_dir;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
    addon_path_ = executable_dir.AppendASCII("test_addon.node").AsUTF8Unsafe();
    StartService();
  }

  void TearDown() override {
    service_.reset();
    browser_.reset();
    task_environment_.RunUntilIdle();
  }

  void StartService() {
    service_.reset();
    browser_.reset();
    service_ = std::make_unique<XenonServiceImpl>(
        browser_.BindNewPipeAndPassReceiver());
  }

  NodeRemote Bind(const std::string& endpoint) {
    NodeRemote node;
    browser_->BindNodeAddonHost(kContext, endpoint,
                                node.BindNewPipeAndPassReceiver());
    browser_.FlushForTesting();
    return node;
  }

  ipc::mojom::IpcResultPtr Finish(ResultFuture& future) {
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(future.IsReady());
    return future.IsReady() ? future.Take() : nullptr;
  }

  void Load(NodeRemote& node) {
    ResultFuture loaded;
    node->RequireNodeModuleSync(addon_path_, loaded.GetCallback());
    auto result = Finish(loaded);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->success) << result->error;
  }

  InstanceHandle ParseHandle(ipc::mojom::IpcResultPtr result) {
    EXPECT_TRUE(result);
    if (!result) {
      return {};
    }
    EXPECT_TRUE(result->success) << result->error;
    if (!result->success) {
      return {};
    }
    EXPECT_TRUE(result->value.is_dict());
    if (!result->value.is_dict()) {
      return {};
    }
    const auto& value = result->value.GetDict();
    const auto id = value.FindInt("instance_id");
    const auto* token = value.FindString("owner_token");
    EXPECT_TRUE(id.has_value());
    EXPECT_TRUE(token && !token->empty());
    return {id.value_or(0), token ? *token : std::string()};
  }

  InstanceHandle Construct(NodeRemote& node) {
    ResultFuture constructed;
    node->ConstructNodeExportSync(
        addon_path_, "OwnedHandle", base::Value(base::ListValue()),
        base::Value(base::DictValue()), constructed.GetCallback());
    return ParseHandle(Finish(constructed));
  }

  base::Value ConstructRequest() {
    base::DictValue request;
    request.Set("modulePath", addon_path_);
    request.Set("exportPath", "OwnedHandle");
    request.Set("arguments", base::ListValue());
    return base::Value(base::ListValue().Append(std::move(request)));
  }

  InstanceHandle ConstructViaBrowser(const std::string& endpoint) {
    ResultFuture constructed;
    browser_->ElectronIpcInvoke(kContext, endpoint, kConstructChannel,
                                ConstructRequest(), constructed.GetCallback());
    return ParseHandle(Finish(constructed));
  }

  ipc::mojom::IpcResultPtr Read(NodeRemote& node,
                                const InstanceHandle& handle,
                                const std::string& method = "read") {
    base::test::TestFuture<ipc::mojom::IpcResultPtr, uint64_t> invoked;
    node->InvokeNodeInstanceSync(addon_path_, handle.id, method,
                                 base::Value(base::ListValue()), handle.token,
                                 invoked.GetCallback());
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(invoked.IsReady());
    if (!invoked.IsReady()) {
      return nullptr;
    }
    auto [result, promise_id] = invoked.Take();
    EXPECT_EQ(0u, promise_id);
    return std::move(result);
  }

  ipc::mojom::IpcResultPtr ReadViaBrowser(const std::string& endpoint,
                                          const InstanceHandle& handle) {
    base::DictValue request;
    request.Set("modulePath", addon_path_);
    request.Set("instanceId", handle.id);
    request.Set("methodName", "read");
    request.Set("ownerToken", handle.token);
    request.Set("arguments", base::ListValue());
    ResultFuture invoked;
    browser_->ElectronIpcInvoke(
        kContext, endpoint, kInvokeChannel,
        base::Value(base::ListValue().Append(std::move(request))),
        invoked.GetCallback());
    return Finish(invoked);
  }

  void ExpectReadable(ipc::mojom::IpcResultPtr result) {
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->success) << result->error;
    ASSERT_TRUE(result->value.is_int());
    EXPECT_EQ(42, result->value.GetInt());
  }

  void ExpectInvalidOwner(ipc::mojom::IpcResultPtr result) {
    ASSERT_TRUE(result);
    EXPECT_FALSE(result->success);
    EXPECT_NE(std::string::npos,
              result->error.find("ERR_NATIVE_INSTANCE_INVALIDATED"))
        << result->error;
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO,
      base::test::TaskEnvironment::ThreadPoolExecutionMode::QUEUED};
  mojo::Remote<mojom::XenonMainService> browser_;
  std::unique_ptr<XenonServiceImpl> service_;
  std::string addon_path_;
};

TEST_F(XenonServiceOwnerTest, FirstDirectBindingKeepsCallbackPathOwner) {
  const auto callback_handle = ConstructViaBrowser("page-a");
  ASSERT_GT(callback_handle.id, 0);
  NodeRemote node = Bind("page-a");
  ExpectReadable(Read(node, callback_handle));
  const auto direct_handle = Construct(node);
  ASSERT_GT(direct_handle.id, 0);
  EXPECT_EQ(callback_handle.token, direct_handle.token);
  ExpectReadable(ReadViaBrowser("page-a", direct_handle));
}

TEST_F(XenonServiceOwnerTest,
       PrototypeCallbacksReachSyncAndColdAsyncConstructors) {
  const auto properties = [] {
    return base::DictValue().Set(
        "emit", base::DictValue()
                    .Set("__xenon_node_wire_type__", "callback")
                    .Set("callback_id", 71));
  };
  base::DictValue request;
  request.Set("modulePath", addon_path_);
  request.Set("exportPath", "PrototypeCallback");
  request.Set("arguments", base::ListValue());
  request.Set("prototypeProperties", properties());
  ResultFuture async_constructed;
  browser_->ElectronIpcInvoke(
      kContext, "prototype-page", kConstructChannel,
      base::Value(base::ListValue().Append(std::move(request))),
      async_constructed.GetCallback());
  const auto async_handle = ParseHandle(Finish(async_constructed));
  ASSERT_GT(async_handle.id, 0);
  NodeRemote node = Bind("prototype-page");
  const auto expect_emit = [&](const InstanceHandle& handle, bool expected) {
    auto result = Read(node, handle, "fire");
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->success) << result->error;
    ASSERT_TRUE(result->value.is_bool());
    EXPECT_EQ(expected, result->value.GetBool());
  };
  expect_emit(async_handle, true);
  ExpectReadable(Read(node, async_handle));
  for (bool with_prototype : {true, false}) {
    ResultFuture constructed;
    node->ConstructNodeExportSync(
        addon_path_, "PrototypeCallback", base::Value(base::ListValue()),
        base::Value(with_prototype ? properties() : base::DictValue()),
        constructed.GetCallback());
    const auto handle = ParseHandle(Finish(constructed));
    ASSERT_GT(handle.id, 0);
    EXPECT_EQ(async_handle.token, handle.token);
    expect_emit(handle, with_prototype);
    ExpectReadable(Read(node, handle));
  }
}

TEST_F(XenonServiceOwnerTest, RebindingInvalidatesOldTokenOnBothCallPaths) {
  NodeRemote first = Bind("page-a");
  Load(first);
  const auto old_handle = Construct(first);
  NodeRemote replacement = Bind("page-a");
  const auto current = Construct(replacement);
  ASSERT_GT(current.id, 0);
  EXPECT_NE(old_handle.token, current.token);
  ExpectInvalidOwner(Read(replacement, old_handle));
  ExpectInvalidOwner(ReadViaBrowser("page-a", old_handle));
  ExpectReadable(Read(replacement, current));
  ExpectReadable(ReadViaBrowser("page-a", current));
}

TEST_F(XenonServiceOwnerTest, DisconnectReleasesOnlyTheOwningPage) {
  NodeRemote first = Bind("page-a");
  Load(first);
  const auto first_handle = Construct(first);
  NodeRemote second = Bind("page-b");
  const auto second_handle = Construct(second);
  ExpectInvalidOwner(Read(second, first_handle));
  first.reset();
  task_environment_.RunUntilIdle();
  ExpectReadable(Read(second, second_handle));
  NodeRemote replacement = Bind("page-a");
  ExpectInvalidOwner(Read(replacement, first_handle));
  const auto current = Construct(replacement);
  EXPECT_NE(first_handle.token, current.token);
  ExpectReadable(Read(replacement, current));
  ExpectReadable(Read(second, second_handle));
  browser_->RemoveElectronIpcRenderer(kContext, "page-a");
  browser_.FlushForTesting();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(replacement.is_connected());
  ExpectReadable(Read(second, second_handle));
}

TEST_F(XenonServiceOwnerTest, GcReleaseRequiresMatchingTokenAndOwner) {
  NodeRemote first = Bind("page-a");
  Load(first);
  const auto handle = Construct(first);
  NodeRemote second = Bind("page-b");
  const auto other = Construct(second);
  first->ReleaseNodeInstance(addon_path_, handle.id, "stale-token");
  second->ReleaseNodeInstance(addon_path_, handle.id, handle.token);
  task_environment_.RunUntilIdle();
  ExpectReadable(Read(first, handle));
  ResultFuture inspected;
  first->InspectNodeInstanceMemberSync(addon_path_, handle.id, "value",
                                       "stale-token", inspected.GetCallback());
  ExpectInvalidOwner(Finish(inspected));
  first->ReleaseNodeInstance(addon_path_, handle.id, handle.token);
  first->ReleaseNodeInstance(addon_path_, handle.id, handle.token);
  auto released = Read(first, handle);
  ASSERT_TRUE(released);
  EXPECT_FALSE(released->success);
  ExpectReadable(Read(second, other));
}

TEST_F(XenonServiceOwnerTest,
       ServiceRestartRejectsStaleTokenEvenWhenIdIsReused) {
  NodeRemote first = Bind("page-a");
  Load(first);
  const auto old_handle = Construct(first);
  StartService();
  NodeRemote replacement = Bind("page-a");
  Load(replacement);
  const auto current = Construct(replacement);
  ASSERT_EQ(old_handle.id, current.id);
  EXPECT_NE(old_handle.token, current.token);
  replacement->ReleaseNodeInstance(addon_path_, current.id, old_handle.token);
  ExpectInvalidOwner(Read(replacement, old_handle));
  ExpectReadable(Read(replacement, current));
}

TEST_F(XenonServiceOwnerTest, ColdLoadCannotReviveOwnerClosedByRebinding) {
  NodeRemote first = Bind("page-a");
  ResultFuture old_construction;
  browser_->ElectronIpcInvoke(kContext, "page-a", kConstructChannel,
                              ConstructRequest(),
                              old_construction.GetCallback());
  // Only pump the Mojo endpoint. QUEUED ThreadPool keeps addon preparation
  // suspended until after the owner's pipe has been replaced below.
  browser_.FlushForTesting();
  ASSERT_FALSE(old_construction.IsReady());
  NodeRemote replacement = Bind("page-a");
  auto closed = Finish(old_construction);
  ASSERT_TRUE(closed);
  EXPECT_FALSE(closed->success);
  EXPECT_NE(std::string::npos, closed->error.find("owner was closed"));
  const auto current = Construct(replacement);
  ASSERT_GT(current.id, 0);
  ExpectReadable(Read(replacement, current));
}

TEST_F(XenonServiceOwnerTest, ObserverQueuePreservesCallbackReceiver) {
  class Observer : public mojom::NodeAddonObserver {
   public:
    void OnCallback(int32_t callback_id,
                    std::vector<base::Value> args,
                    base::Value receiver) override {
      EXPECT_EQ(7, callback_id);
      ASSERT_EQ(1u, args.size());
      EXPECT_EQ(receiver, args[0]);
      receivers.push_back(std::move(receiver));
    }
    void OnCallbackReleased(int32_t) override {}
    std::vector<base::Value> receivers;
  } observer;
  NodeRemote node = Bind("page-a");
  Load(node);
  const auto invoke = [&](const char* label) {
    std::vector<mojom::NodeInvokeArgPtr> args;
    auto callback = mojom::NodeInvokeArg::New();
    callback->is_callback = true;
    callback->callback_id = 7;
    args.push_back(std::move(callback));
    auto receiver = mojom::NodeInvokeArg::New();
    receiver->value = base::Value(base::DictValue().Set("label", label));
    args.push_back(std::move(receiver));
    base::test::TestFuture<bool, base::Value,
                           std::vector<mojom::NodeCallbackResultPtr>,
                           const std::string&>
        invoked;
    browser_->InvokeFunction(kContext, 17, addon_path_,
                             "InvokeCallbackWithReceiver", std::move(args),
                             invoked.GetCallback());
    task_environment_.RunUntilIdle();
    ASSERT_TRUE(invoked.IsReady());
    EXPECT_TRUE(invoked.Get<0>()) << invoked.Get<3>();
  };
  invoke("queued");
  EXPECT_TRUE(observer.receivers.empty());
  mojo::Receiver<mojom::NodeAddonObserver> receiver(&observer);
  browser_->SetNodeAddonObserver(kContext, 17,
                                 receiver.BindNewPipeAndPassRemote());
  task_environment_.RunUntilIdle();
  ASSERT_EQ(1u, observer.receivers.size());
  EXPECT_EQ("queued", *observer.receivers[0].GetDict().FindString("label"));
  invoke("connected");
  ASSERT_EQ(2u, observer.receivers.size());
  EXPECT_EQ("connected", *observer.receivers[1].GetDict().FindString("label"));
}

// These tests use the production main bootstrap and OS pipe bridge. The fake
// renderer implements only the Mojo endpoint, so it can detect accidental
// broadcast fallback without sharing the bootstrap's routing implementation.
class NetPipeRenderer : public ipc::mojom::IpcRenderer {
 public:
  mojo::PendingRemote<ipc::mojom::IpcRenderer> BindNewRemote() {
    return receiver_.BindNewPipeAndPassRemote();
  }

  void Dispatch(const std::string& channel, base::Value arguments) override {
    messages.emplace_back(channel, std::move(arguments));
  }

  const base::DictValue* Payload(const std::string& channel) const {
    for (const auto& [name, arguments] : messages) {
      if (name == channel && arguments.is_list() &&
          !arguments.GetList().empty()) {
        return arguments.GetList().front().GetIfDict();
      }
    }
    return nullptr;
  }

  std::string ReceivedBytes() const {
    std::string bytes;
    for (const auto& [channel, arguments] : messages) {
      if (channel != "__xenon:net:data" || !arguments.is_list() ||
          arguments.GetList().empty()) {
        continue;
      }
      const auto* payload = arguments.GetList().front().GetIfDict();
      const auto* wire = payload ? payload->FindDict("wire") : nullptr;
      const auto* data = wire ? wire->FindString("d") : nullptr;
      const auto* type = wire ? wire->FindString("t") : nullptr;
      if (!data || !type || *type != "b64") {
        ADD_FAILURE() << "OS pipe data must contain a base64 byte payload";
        continue;
      }
      std::string decoded;
      EXPECT_TRUE(base::Base64Decode(*data, &decoded));
      bytes.append(decoded);
    }
    return bytes;
  }

  std::vector<std::pair<std::string, base::Value>> messages;

 private:
  mojo::Receiver<ipc::mojom::IpcRenderer> receiver_{this};
};

constexpr char kMainNetSource[] = R"JS(
const {ipcMain} = require('electron');
const net = require('node:net');
let server;
let serverBytes = '';
const serverSockets = new Set();
ipcMain.handle('pipe:connect', (_event, path) => new Promise(resolve => {
  let returned = false;
  let connectedAfterReturn = false;
  let received = Buffer.alloc(0);
  const socket = net.connect(path);
  socket.on('connect', () => {
    connectedAfterReturn = returned;
    socket.write(Buffer.from([0, 255, 128, 65]));
  });
  socket.on('data', chunk => {
    received = Buffer.concat([received, chunk]);
    if (received.length >= 4) {
      socket.destroy();
      resolve({hex: received.toString('hex'), connectedAfterReturn});
    }
  });
  socket.on('error', error => resolve({code: error.code, afterReturn: returned}));
  returned = true;
}));
ipcMain.handle('pipe:listen', (_event, path) => new Promise((resolve, reject) => {
  let returned = false;
  server = net.createServer(socket => {
    serverSockets.add(socket);
    let received = Buffer.alloc(0);
    socket.on('data', chunk => {
      received = Buffer.concat([received, chunk]);
      if (received.length >= 4 && !serverBytes) {
        serverBytes = received.toString('hex');
        socket.write(Buffer.from([255, 0, 66, 128]));
      }
    });
    socket.on('close', () => serverSockets.delete(socket));
    socket.on('error', reject);
  });
  server.on('error', reject);
  server.listen(path, () => resolve({afterReturn: returned}));
  returned = true;
}));
ipcMain.handle('pipe:server-bytes', () => serverBytes);
ipcMain.handle('pipe:stop-server', () => new Promise(resolve => {
  for (const socket of serverSockets) socket.destroy();
  server.close(() => resolve(true));
}));
)JS";

class XenonServiceMainNetTest : public XenonServiceOwnerTest {
 protected:
  void InitializeEmbeddedMain() {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    // The virtual module has no disk file to canonicalize. Derive it from the
    // same canonical directory as app_path (Windows may return an 8.3 temp
    // directory spelling), so the production ancestry check compares peers.
    base::FilePath app_path;
    ASSERT_TRUE(base::NormalizeFilePath(temp_dir_.GetPath(), &app_path));
    auto config = ipc::mojom::IpcMainConfig::New();
    config->container_id = kContext;
    config->embedded_main_source = kMainNetSource;
    config->virtual_main_path = app_path.AppendASCII("main.js").AsUTF8Unsafe();
    config->app_path = app_path.AsUTF8Unsafe();
    config->app_name = "Main Net Pipe Test";
    base::test::TestFuture<bool, const std::string&> initialized;
    browser_->InitializeElectronIpc(std::move(config),
                                    initialized.GetCallback());
    ASSERT_TRUE(initialized.Wait());
    ASSERT_TRUE(initialized.Get<0>()) << initialized.Get<1>();
    task_environment_.RunUntilIdle();
  }

  void RegisterRenderer(const std::string& endpoint,
                        NetPipeRenderer& renderer) {
    browser_->RegisterElectronIpcRenderer(kContext, endpoint,
                                          renderer.BindNewRemote(), 1, 1, 1);
    browser_.FlushForTesting();
    task_environment_.RunUntilIdle();
  }

  std::string PipePath() const {
    const std::string name =
        "xenon-main-net-" + base::UnguessableToken::Create().ToString();
#if BUILDFLAG(IS_WIN)
    return "\\\\.\\pipe\\" + name;
#else
    return temp_dir_.GetPath().AppendASCII(name).AsUTF8Unsafe();
#endif
  }

  void SendNet(const std::string& endpoint,
               const std::string& channel,
               base::DictValue message) {
    browser_->ElectronIpcSend(
        kContext, endpoint, "__xenon:net:" + channel,
        base::Value(base::ListValue().Append(std::move(message))));
  }

  void SendBytes(const std::string& endpoint,
                 const std::string& socket_id,
                 const std::string& bytes) {
    SendNet(endpoint, "data",
            base::DictValue()
                .Set("toId", socket_id)
                .Set("wire", base::DictValue()
                                 .Set("t", "b64")
                                 .Set("d", base::Base64Encode(bytes))));
  }

  ipc::mojom::IpcResultPtr InvokeMain(const std::string& channel,
                                      base::ListValue args = {}) {
    ResultFuture result;
    browser_->ElectronIpcInvoke(kContext, "test-driver", channel,
                                base::Value(std::move(args)),
                                result.GetCallback());
    if (!result.Wait()) {
      ADD_FAILURE() << "Main IPC did not complete: " << channel;
      return nullptr;
    }
    return result.Take();
  }

  base::test::ScopedRunLoopTimeout timeout_{FROM_HERE, base::Seconds(5)};
  base::ScopedTempDir temp_dir_;
};

TEST_F(XenonServiceMainNetTest, MainClientExchangesBytesWithOnlyItsRenderer) {
  ASSERT_NO_FATAL_FAILURE(InitializeEmbeddedMain());
  NetPipeRenderer server_renderer;
  NetPipeRenderer unrelated_renderer;
  RegisterRenderer("pipe-server", server_renderer);
  RegisterRenderer("unrelated", unrelated_renderer);
  const std::string path = PipePath();
  SendNet(
      "pipe-server", "listen",
      base::DictValue().Set("serverId", "renderer-server").Set("path", path));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return server_renderer.Payload("__xenon:net:listening") != nullptr;
  }));

  ResultFuture result;
  browser_->ElectronIpcInvoke(kContext, "test-driver", "pipe:connect",
                              base::Value(base::ListValue().Append(path)),
                              result.GetCallback());
  const std::string request("\0\xff\x80\x41", 4);
  ASSERT_TRUE(base::test::RunUntil([&] {
    return server_renderer.ReceivedBytes().size() >= request.size();
  }));
  EXPECT_EQ(request, server_renderer.ReceivedBytes());
  const auto* connection = server_renderer.Payload("__xenon:net:connection");
  ASSERT_TRUE(connection);
  ASSERT_TRUE(connection->FindString("socketId"));
  SendBytes("pipe-server", *connection->FindString("socketId"),
            std::string("\xff\0\x42\x80", 4));
  ASSERT_TRUE(result.Wait());
  auto response = result.Take();
  ASSERT_TRUE(response);
  ASSERT_TRUE(response->success) << response->error;
  ASSERT_TRUE(response->value.is_dict());
  ASSERT_TRUE(response->value.GetDict().FindString("hex"));
  EXPECT_EQ("ff004280", *response->value.GetDict().FindString("hex"));
  EXPECT_EQ(true, response->value.GetDict().FindBool("connectedAfterReturn"));
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(unrelated_renderer.messages.empty());
}

TEST_F(XenonServiceMainNetTest, MainServerExchangesBytesWithRendererClient) {
  ASSERT_NO_FATAL_FAILURE(InitializeEmbeddedMain());
  NetPipeRenderer client_renderer;
  NetPipeRenderer unrelated_renderer;
  RegisterRenderer("pipe-client", client_renderer);
  RegisterRenderer("unrelated", unrelated_renderer);
  const std::string path = PipePath();
  auto listening = InvokeMain("pipe:listen", base::ListValue().Append(path));
  ASSERT_TRUE(listening);
  ASSERT_TRUE(listening->success) << listening->error;
  ASSERT_TRUE(listening->value.is_dict());
  EXPECT_EQ(true, listening->value.GetDict().FindBool("afterReturn"));
  SendNet("pipe-client", "connect",
          base::DictValue().Set("fromId", "renderer-client").Set("path", path));
  ASSERT_TRUE(base::test::RunUntil([&] {
    return client_renderer.Payload("__xenon:net:connected") != nullptr;
  }));
  const auto* connected = client_renderer.Payload("__xenon:net:connected");
  ASSERT_TRUE(connected->FindString("peerId"));
  SendBytes("pipe-client", *connected->FindString("peerId"),
            std::string("\0\xff\x80\x41", 4));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return client_renderer.ReceivedBytes().size() >= 4; }));
  EXPECT_EQ(std::string("\xff\0\x42\x80", 4), client_renderer.ReceivedBytes());
  auto received = InvokeMain("pipe:server-bytes");
  ASSERT_TRUE(received);
  ASSERT_TRUE(received->success) << received->error;
  ASSERT_TRUE(received->value.is_string());
  EXPECT_EQ("00ff8041", received->value.GetString());
  auto stopped = InvokeMain("pipe:stop-server");
  ASSERT_TRUE(stopped);
  ASSERT_TRUE(stopped->success) << stopped->error;
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(unrelated_renderer.messages.empty());
}

TEST_F(XenonServiceMainNetTest,
       MissingMainPipeFailsAsynchronouslyWithoutBroadcast) {
  ASSERT_NO_FATAL_FAILURE(InitializeEmbeddedMain());
  NetPipeRenderer first;
  NetPipeRenderer second;
  RegisterRenderer("first", first);
  RegisterRenderer("second", second);
  auto result =
      InvokeMain("pipe:connect", base::ListValue().Append(PipePath()));
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& error = result->value.GetDict();
  ASSERT_TRUE(error.FindString("code"));
  EXPECT_EQ("ECONNREFUSED", *error.FindString("code"));
  EXPECT_EQ(true, error.FindBool("afterReturn"));
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(first.messages.empty());
  EXPECT_TRUE(second.messages.empty());
}

}  // namespace
}  // namespace xenon
