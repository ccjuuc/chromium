// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_service_impl.h"

#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/scoped_environment_variable_override.h"
#include "base/test/run_until.h"
#include "base/test/multiprocess_test.h"
#include "base/test/scoped_command_line.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "build/build_config.h"
#include "mojo/core/embedder/embedder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"
#include "v8/include/v8.h"
#include "xenon_overlay/chrome/browser/napi/napi_switches.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/asar/test_support.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace xenon {

class XenonServiceImplTestPeer {
 public:
  static std::vector<mojom::NodeInvokeArgPtr> TakeNodeInvokeArgs(
      base::Value arguments) {
    return XenonServiceImpl::TakeNodeInvokeArgs(std::move(arguments));
  }

  static bool HasMainContainer(const XenonServiceImpl& service) {
    return !service.ipc_main_containers_.empty();
  }

  static void BindUtilityObserver(
      XenonServiceImpl& service,
      mojo::PendingRemote<ipc::mojom::UtilityProcessServiceObserver> observer) {
    service.utility_observer_.Bind(std::move(observer));
  }

  static void CreateUtilityNetworkFactory(
      XenonServiceImpl& service,
      const std::string& partition,
      mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver> headers,
      base::OnceCallback<
          void(mojo::PendingRemote<network::mojom::URLLoaderFactory>,
               std::string)> callback) {
    service.CreateUtilityNetworkFactory(partition, std::move(headers),
                                        std::move(callback));
  }

  static bool GetUtilityNetworkOnline(XenonServiceImpl& service,
                                      bool* online,
                                      std::string* error) {
    return service.GetUtilityNetworkOnline(online, error);
  }

  static bool HasNodeExecutor(XenonServiceImpl& service,
                              const std::string& context_id) {
    return service.GetNodeExecutor(context_id) != nullptr;
  }

  static void ReleaseCallback(XenonServiceImpl& service,
                              const std::string& context_id,
                              const std::string& endpoint_id,
                              int32_t callback_id) {
    const auto client =
        service.renderer_node_clients_.find({context_id, endpoint_id});
    ASSERT_NE(service.renderer_node_clients_.end(), client);
    service.OnNodeCallbackReleased(context_id, client->second, callback_id);
  }
};

namespace {

using NodeRemote = mojo::Remote<ipc::mojom::NodeAddonHost>;
using ResultFuture = base::test::TestFuture<ipc::mojom::IpcResultPtr>;
constexpr char kContext[] = "owner-lifecycle-test";
constexpr char kConstructChannel[] = "__xenon:node-addon:construct-export";
constexpr char kInvokeChannel[] = "__xenon:node-addon:invoke-instance";
constexpr char kExportChannel[] = "__xenon:node-addon:invoke-export";

base::Value BinaryWire(std::string kind, base::Value::BlobStorage bytes = {}) {
  return base::Value(base::DictValue()
                         .Set("__xenon_node_wire_type__", "binary")
                         .Set("kind", std::move(kind))
                         .Set("value", base::Value(std::move(bytes))));
}

base::Value CallbackWire(int callback_id) {
  return base::Value(base::DictValue()
                         .Set("__xenon_node_wire_type__", "callback")
                         .Set("callback_id", callback_id));
}

TEST(XenonServiceArgumentTest, OwnedArgumentsKeepNestedBlobStorage) {
  base::Value::BlobStorage bytes(1024 * 1024, 0xa5);
  const auto* original_storage = bytes.data();
  base::ListValue arguments;
  arguments.Append(42);
  arguments.Append(base::DictValue().Set(
      "payload",
      base::ListValue().Append(BinaryWire("Buffer", std::move(bytes)))));
  arguments.Append(BinaryWire("DataView"));
  arguments.Append(CallbackWire(9));
  const auto expected = arguments.Clone();

  auto converted = XenonServiceImplTestPeer::TakeNodeInvokeArgs(
      base::Value(std::move(arguments)));
  ASSERT_EQ(expected.size(), converted.size());
  for (size_t i = 0; i < converted.size(); ++i) {
    ASSERT_TRUE(converted[i]);
    EXPECT_FALSE(converted[i]->is_callback);
    EXPECT_EQ(0, converted[i]->callback_id);
    EXPECT_EQ(expected[i], converted[i]->value);
  }
  const auto* payload = converted[1]->value.GetDict().FindList("payload");
  ASSERT_TRUE(payload);
  ASSERT_EQ(1u, payload->size());
  const auto* moved_blob = (*payload)[0].GetDict().FindBlob("value");
  ASSERT_TRUE(moved_blob);
  EXPECT_EQ(1024u * 1024u, moved_blob->size());
  // This guards the allocation/copy reduction directly, without a timing
  // threshold that depends on the machine or the addon workload.
  EXPECT_EQ(original_storage, moved_blob->data());
}

TEST(XenonServiceArgumentTest,
     TakingNestedArgumentsKeepsBorrowedMetadataValid) {
  base::Value::BlobStorage bytes(1024 * 1024, 0x5a);
  const auto* original_storage = bytes.data();
  base::DictValue request;
  request.Set("modulePath", std::string(200, 'm'));
  request.Set("functionName", "EchoOwnedHandle");
  request.Set("ownerToken", std::string(100, 'o'));
  request.Set("arguments", base::ListValue().Append(
                               BinaryWire("Uint8Array", std::move(bytes))));
  const auto* module_path = request.FindString("modulePath");
  const auto* function_name = request.FindString("functionName");
  const auto* owner_token = request.FindString("ownerToken");
  ASSERT_TRUE(module_path && function_name && owner_token);

  auto converted = XenonServiceImplTestPeer::TakeNodeInvokeArgs(
      std::move(*request.Find("arguments")));
  ASSERT_EQ(1u, converted.size());
  EXPECT_EQ(module_path, request.FindString("modulePath"));
  EXPECT_EQ(function_name, request.FindString("functionName"));
  EXPECT_EQ(owner_token, request.FindString("ownerToken"));
  EXPECT_EQ(std::string(200, 'm'), *module_path);
  EXPECT_EQ("EchoOwnedHandle", *function_name);
  EXPECT_EQ(std::string(100, 'o'), *owner_token);
  const auto* moved_blob = converted[0]->value.GetDict().FindBlob("value");
  ASSERT_TRUE(moved_blob);
  EXPECT_EQ(original_storage, moved_blob->data());
}

class XenonUtilityServiceTest
    : public testing::Test,
      public ipc::mojom::UtilityProcessServiceObserver {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }

  void SetUp() override {
    service_ = std::make_unique<XenonServiceImpl>(
        remote_.BindNewPipeAndPassReceiver());
    service_->SetUtilityExitHandler(base::BindRepeating(
        [](std::vector<int>* exits, int code) { exits->push_back(code); },
        &exits_));
  }

  void TearDown() override {
    service_.reset();
    remote_.reset();
    environment_.RunUntilIdle();
  }

  ipc::mojom::UtilityProcessBootstrapPtr Bootstrap() {
    auto config = ipc::mojom::UtilityProcessBootstrap::New();
    config->main = ipc::mojom::IpcMainConfig::New();
    config->main->native_addon_execution =
        ipc::mojom::NativeAddonExecution::kStandard;
    blink::MessagePortDescriptorPair pair;
    config->parent_port = pair.TakePort0();
    peer_port_ = pair.TakePort1();
    return config;
  }

  std::string Reject(ipc::mojom::UtilityProcessBootstrapPtr config) {
    observer_.reset();
    base::test::TestFuture<bool, std::string> result;
    service_->Start(std::move(config), observer_.BindNewPipeAndPassRemote(),
                    result.GetCallback<bool, const std::string&>());
    EXPECT_FALSE(result.Get<0>());
    EXPECT_FALSE(XenonServiceImplTestPeer::HasMainContainer(*service_));
    return result.Get<1>();
  }

  void OnExitRequested(int32_t code) override {
    requested_exits_.push_back(code);
  }

  void CreateNetworkFactory(
      const std::string& partition,
      mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver> headers,
      CreateNetworkFactoryCallback callback) override {
    requested_partitions_.push_back(partition);
    received_headers_ = std::move(headers);
    if (hold_network_factory_) {
      pending_network_callback_ = std::move(callback);
      return;
    }
    std::move(callback).Run(std::move(network_factory_), network_error_);
  }

  void GetNetworkOnline(GetNetworkOnlineCallback callback) override {
    std::move(callback).Run(true, "");
  }

  base::test::TaskEnvironment environment_;
  mojo::Remote<ipc::mojom::UtilityProcessService> remote_;
  mojo::Receiver<ipc::mojom::UtilityProcessServiceObserver> observer_{this};
  blink::MessagePortDescriptor peer_port_;
  std::unique_ptr<XenonServiceImpl> service_;
  std::vector<int> exits_;
  std::vector<int> requested_exits_;
  std::vector<std::string> requested_partitions_;
  mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver>
      received_headers_;
  mojo::PendingRemote<network::mojom::URLLoaderFactory> network_factory_;
  std::string network_error_;
  bool hold_network_factory_ = false;
  CreateNetworkFactoryCallback pending_network_callback_;
};

TEST_F(XenonUtilityServiceTest, NetworkFactoryForwardsPartitionAndOwnedPipes) {
  XenonServiceImplTestPeer::BindUtilityObserver(
      *service_, observer_.BindNewPipeAndPassRemote());
  auto factory_receiver = network_factory_.InitWithNewPipeAndPassReceiver();
  mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver> headers;
  auto headers_receiver = headers.InitWithNewPipeAndPassReceiver();
  base::test::TestFuture<mojo::PendingRemote<network::mojom::URLLoaderFactory>,
                         std::string>
      result;
  XenonServiceImplTestPeer::CreateUtilityNetworkFactory(
      *service_, "persist:isolated-network", std::move(headers),
      result.GetCallback());
  ASSERT_TRUE(result.Wait());
  auto [factory, error] = result.Take();
  EXPECT_TRUE(factory.is_valid());
  EXPECT_TRUE(error.empty());
  EXPECT_EQ(requested_partitions_,
            std::vector<std::string>({"persist:isolated-network"}));
  EXPECT_TRUE(received_headers_.is_valid());
}

TEST_F(XenonUtilityServiceTest, NetworkFactoryPreservesBrowserErrors) {
  XenonServiceImplTestPeer::BindUtilityObserver(
      *service_, observer_.BindNewPipeAndPassRemote());
  mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver> headers;
  auto headers_receiver = headers.InitWithNewPipeAndPassReceiver();
  network_error_ = "ERR_ABORTED: Parent generation changed";
  base::test::TestFuture<mojo::PendingRemote<network::mojom::URLLoaderFactory>,
                         std::string>
      result;
  XenonServiceImplTestPeer::CreateUtilityNetworkFactory(
      *service_, "", std::move(headers), result.GetCallback());
  ASSERT_TRUE(result.Wait());
  auto [factory, error] = result.Take();
  EXPECT_FALSE(factory.is_valid());
  EXPECT_EQ(network_error_, error);
}

TEST_F(XenonUtilityServiceTest,
       NetworkFactoryDisconnectCompletesPendingRequest) {
  XenonServiceImplTestPeer::BindUtilityObserver(
      *service_, observer_.BindNewPipeAndPassRemote());
  mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver> headers;
  auto headers_receiver = headers.InitWithNewPipeAndPassReceiver();
  hold_network_factory_ = true;
  base::test::TestFuture<mojo::PendingRemote<network::mojom::URLLoaderFactory>,
                         std::string>
      result;
  XenonServiceImplTestPeer::CreateUtilityNetworkFactory(
      *service_, "", std::move(headers), result.GetCallback());
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return !pending_network_callback_.is_null(); }));
  EXPECT_FALSE(result.IsReady());
  observer_.reset();
  pending_network_callback_.Reset();
  ASSERT_TRUE(result.Wait());
  auto [factory, error] = result.Take();
  EXPECT_FALSE(factory.is_valid());
  EXPECT_TRUE(error.starts_with("ERR_ABORTED"));
}

TEST_F(XenonUtilityServiceTest, NetworkWithoutOwnerReportsUnavailable) {
  base::test::TestFuture<mojo::PendingRemote<network::mojom::URLLoaderFactory>,
                         std::string>
      result;
  XenonServiceImplTestPeer::CreateUtilityNetworkFactory(*service_, "", {},
                                                        result.GetCallback());
  ASSERT_TRUE(result.Wait());
  auto [factory, error] = result.Take();
  EXPECT_FALSE(factory.is_valid());
  EXPECT_TRUE(error.starts_with("ERR_ABORTED"));
  bool online = false;
  error.clear();
  EXPECT_FALSE(XenonServiceImplTestPeer::GetUtilityNetworkOnline(
      *service_, &online, &error));
  EXPECT_TRUE(error.starts_with("ERR_ABORTED"));
}

TEST_F(XenonUtilityServiceTest, RejectsInvalidBootstrapBeforeMutatingStdio) {
  EXPECT_TRUE(Reject(nullptr).starts_with("ERR_INVALID_ARG_VALUE"));
  auto config = Bootstrap();
  config->parent_port = blink::MessagePortDescriptor();
  EXPECT_TRUE(Reject(std::move(config)).starts_with("ERR_INVALID_ARG_VALUE"));
  config = Bootstrap();
  config->main->native_addon_execution =
      ipc::mojom::NativeAddonExecution::kShared;
  EXPECT_TRUE(Reject(std::move(config)).starts_with("ERR_INVALID_ARG_VALUE"));
  config = Bootstrap();
  config->exec_argv.push_back("--inspect=0");
  EXPECT_TRUE(Reject(std::move(config)).starts_with("ERR_NOT_SUPPORTED"));
  EXPECT_TRUE(exits_.empty());
}

TEST_F(XenonUtilityServiceTest, MainServiceCannotAcquireUtilityRole) {
  mojo::Remote<mojom::XenonMainService> main;
  XenonServiceImpl service(main.BindNewPipeAndPassReceiver());
  base::test::TestFuture<bool, std::string> result;
  service.Start(Bootstrap(), observer_.BindNewPipeAndPassRemote(),
                result.GetCallback<bool, const std::string&>());
  EXPECT_FALSE(result.Get<0>());
  EXPECT_TRUE(result.Get<1>().starts_with("ERR_INVALID_STATE"));
  EXPECT_FALSE(XenonServiceImplTestPeer::HasMainContainer(service));
}

TEST_F(XenonUtilityServiceTest, ShutdownIsOnceAndRejectsRestart) {
  service_->Shutdown(17);
  service_->Shutdown(3);
  EXPECT_EQ(exits_, std::vector<int>({17}));
  EXPECT_TRUE(Reject(Bootstrap()).starts_with("ERR_INVALID_STATE"));
  remote_.reset();
  environment_.RunUntilIdle();
  EXPECT_EQ(exits_, std::vector<int>({17}));
}

TEST_F(XenonUtilityServiceTest, ParentDisconnectReleasesIdleProcess) {
  remote_.reset();
  ASSERT_TRUE(base::test::RunUntil([&] { return !exits_.empty(); }));
  EXPECT_EQ(exits_, std::vector<int>({0}));
}

#if BUILDFLAG(ENABLE_XENON_SERVICE)
TEST(XenonUtilityLaunchEnvironmentTest, EmptyValuesReachTheOperatingSystem) {
  base::ScopedEnvironmentVariableOverride parent_only(
      "XENON_UTILITY_PARENT_ONLY", "must-not-leak");
  for (bool preserve_empty : {false, true}) {
    auto command = base::GetMultiProcessTestChildBaseCommandLine();
    if (preserve_empty) command.AppendSwitch("expect-empty-environment-value");
    base::LaunchOptions options;
    options.clear_environment = true;
    options.preserve_empty_environment_values = preserve_empty;
    options.environment.emplace(FILE_PATH_LITERAL("XENON_UTILITY_EMPTY"),
                                FILE_PATH_LITERAL(""));
    options.environment.emplace(FILE_PATH_LITERAL("XENON_UTILITY_KEPT"),
                                FILE_PATH_LITERAL("kept"));
#if BUILDFLAG(IS_WIN)
    options.start_hidden = true;
#endif
    auto child = base::SpawnMultiProcessTestChild(
        "XenonUtilityEnvironmentChild", command, options);
    ASSERT_TRUE(child.IsValid());
    int exit_code = -1;
    ASSERT_TRUE(child.WaitForExitWithTimeout(base::Seconds(15), &exit_code));
    EXPECT_EQ(0, exit_code) << "preserve_empty=" << preserve_empty;
  }
}
#endif

class NodeCallbackRecorder : public ipc::mojom::IpcRenderer {
 public:
  void DispatchPostMessage(const std::string& channel,
                           blink::TransferableMessage message) override {
    ADD_FAILURE() << "Unexpected MessagePort IPC: " << channel;
  }
  struct Call {
    int32_t id;
    std::vector<base::Value> arguments;
    base::Value receiver;
  };
  void Dispatch(const std::string& channel, base::Value arguments) override {
    if (channel == "__xenon:node-addon:callback-released") {
      ASSERT_TRUE(arguments.is_list());
      ASSERT_EQ(1u, arguments.GetList().size());
      ASSERT_TRUE(arguments.GetList()[0].is_int());
      released.push_back(arguments.GetList()[0].GetInt());
      return;
    }
    if (channel != "__xenon:node-addon:callback") {
      return;
    }
    ASSERT_TRUE(arguments.is_list());
    auto& event = arguments.GetList();
    ASSERT_EQ(3u, event.size());
    ASSERT_TRUE(event[0].is_int());
    ASSERT_TRUE(event[1].is_list());
    std::vector<base::Value> args;
    for (auto& argument : event[1].GetList()) {
      args.push_back(std::move(argument));
    }
    calls.push_back({event[0].GetInt(), std::move(args), std::move(event[2])});
  }
  std::vector<Call> calls;
  std::vector<int32_t> released;
};

struct InstanceHandle {
  int32_t id = 0;
  std::string token;
};

// Use the real service ReceiverSet and real addon. Calling the generated async
// overloads of [Sync] methods lets this same-thread fixture exercise Mojo's
// current receiver context without blocking its own service dispatch.
class XenonServiceOwnerTest : public testing::Test {
 protected:
  using ThreadPoolExecutionMode =
      base::test::TaskEnvironment::ThreadPoolExecutionMode;

  explicit XenonServiceOwnerTest(ThreadPoolExecutionMode thread_pool_mode =
                                     ThreadPoolExecutionMode::QUEUED)
      : task_environment_(base::test::TaskEnvironment::MainThreadType::IO,
                          thread_pool_mode) {}

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
    main_driver_receivers_.Clear();
    service_ = std::make_unique<XenonServiceImpl>(
        browser_.BindNewPipeAndPassReceiver());
  }

  void RegisterMainDriver(const std::string& container_id,
                          const std::string& endpoint) {
    mojo::PendingRemote<ipc::mojom::IpcRenderer> remote;
    main_driver_receivers_.Add(&main_driver_,
                               remote.InitWithNewPipeAndPassReceiver());
    base::test::TestFuture<bool> registered;
    browser_->RegisterElectronIpcRenderer(
        container_id, endpoint, std::move(remote), 1, 1, 0,
        "file:///service-test-driver.html", true, registered.GetCallback());
    ASSERT_TRUE(registered.Get());
  }

  NodeRemote Bind(
      const std::string& endpoint,
      mojo::PendingRemote<ipc::mojom::IpcRenderer> callback_renderer = {}) {
    NodeRemote node;
    browser_->BindNodeAddonHost(kContext, endpoint,
                                node.BindNewPipeAndPassReceiver(),
                                std::move(callback_renderer));
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
                                const std::string& method = "read",
                                base::ListValue arguments = base::ListValue()) {
    base::test::TestFuture<ipc::mojom::IpcResultPtr, uint64_t> invoked;
    node->InvokeNodeInstanceSync(addon_path_, handle.id, method,
                                 base::Value(std::move(arguments)),
                                 handle.token, invoked.GetCallback());
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(invoked.IsReady());
    if (!invoked.IsReady()) {
      return nullptr;
    }
    auto [result, promise_id] = invoked.Take();
    EXPECT_EQ(0u, promise_id);
    return std::move(result);
  }

  std::pair<ipc::mojom::IpcResultPtr, uint64_t> CallExport(
      NodeRemote& node,
      const std::string& name,
      base::ListValue arguments = base::ListValue()) {
    base::test::TestFuture<ipc::mojom::IpcResultPtr, uint64_t> invoked;
    node->InvokeNodeExportSync(addon_path_, name,
                               base::Value(std::move(arguments)),
                               invoked.GetCallback());
    task_environment_.RunUntilIdle();
    EXPECT_TRUE(invoked.IsReady());
    if (!invoked.IsReady()) {
      return {};
    }
    auto [result, promise_id] = invoked.Take();
    return {std::move(result), promise_id};
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

  base::test::TaskEnvironment task_environment_;
  mojo::Remote<mojom::XenonMainService> browser_;
  std::unique_ptr<XenonServiceImpl> service_;
  NodeCallbackRecorder main_driver_;
  mojo::ReceiverSet<ipc::mojom::IpcRenderer> main_driver_receivers_;
  std::string addon_path_;
};

class XenonServiceArchiveTest : public XenonServiceOwnerTest {
 protected:
  // Failed initialization destroys the V8 isolate inside Mojo dispatch. Its
  // teardown may wait for worker tasks, so these workers must be allowed to
  // run. Owner-lifecycle race tests retain the default QUEUED mode.
  XenonServiceArchiveTest()
      : XenonServiceOwnerTest(ThreadPoolExecutionMode::ASYNC) {}

  void SetUp() override {
    XenonServiceOwnerTest::SetUp();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    ASSERT_TRUE(base::NormalizeFilePath(temp_dir_.GetPath(), &app_path_));
    archive_path_ = app_path_.AppendASCII("sealed.asar");
    ASSERT_TRUE(builder_.Write(archive_path_,
                               {{"main.js", R"JS(
            const data = require('./value.json');
            require('electron').ipcMain.handle('archive-answer', () => data.answer);
          )JS",
                                 true},
                                {"fail.js", R"JS(
            const data = require('./value.json');
            if (data.answer === 42) throw new Error('encrypted startup rejected');
          )JS",
                                 true},
                                {"value.json", R"({"answer":42})", true}}));
  }

  std::pair<bool, std::string> InitializeArchiveApp(const char* member,
                                                    bool configure_key) {
    auto config = ipc::mojom::IpcMainConfig::New();
    config->container_id = kContext;
    config->embedded_main_source =
        "require('./sealed.asar/" + std::string(member) + "');";
    config->virtual_main_path = app_path_.AppendASCII("main.js").AsUTF8Unsafe();
    config->app_path = app_path_.AsUTF8Unsafe();
    config->app_name = "Encrypted archive service test";
    if (configure_key) {
      config->archive_public_keys.push_back(
          ipc::mojom::IpcArchivePublicKey::New(app_path_.AsUTF8Unsafe(),
                                               builder_.public_key_pem()));
    }
    base::test::TestFuture<bool, const std::string&> initialized;
    browser_->InitializeElectronIpc(std::move(config),
                                    initialized.GetCallback());
    if (!initialized.Wait()) {
      ADD_FAILURE() << "Electron main initialization did not finish";
      return {false, "Initialization callback missing"};
    }
    return {initialized.Get<0>(), initialized.Get<1>()};
  }

  void ExpectArchiveReply() {
    RegisterMainDriver(kContext, "archive-page");
    ResultFuture reply;
    browser_->ElectronIpcInvoke(kContext, "archive-page", "archive-answer",
                                base::Value(base::ListValue()),
                                reply.GetCallback());
    auto result = Finish(reply);
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->success) << result->error;
    ASSERT_TRUE(result->value.is_int());
    EXPECT_EQ(42, result->value.GetInt());
  }

  base::ScopedEnvironmentVariableOverride hosted_directory_{
      "XENON_HOSTED_APP_DIR", "previous-runtime"};
  base::ScopedTempDir temp_dir_;
  base::FilePath app_path_;
  base::FilePath archive_path_;
  asar::TestArchiveBuilder builder_;
};

TEST_F(XenonServiceArchiveTest,
       EncryptedMainRequiresKeyAndServiceOwnsItsLifetime) {
  const auto denied = InitializeArchiveApp("main.js", false);
  EXPECT_FALSE(denied.first);
  EXPECT_FALSE(denied.second.empty());
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive_path_));

  const auto initialized = InitializeArchiveApp("main.js", true);
  ASSERT_TRUE(initialized.first) << initialized.second;
  ASSERT_NO_FATAL_FAILURE(ExpectArchiveReply());
  // This also populates the shared archive cache before destroying its owner.
  EXPECT_TRUE(asar::GetOrCreateAsarArchive(archive_path_));
  service_.reset();
  browser_.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive_path_));
}

TEST_F(XenonServiceArchiveTest, FailedEncryptedMainRevokesKeyAndAllowsRetry) {
  const auto failed = InitializeArchiveApp("fail.js", true);
  ASSERT_FALSE(failed.first);
  EXPECT_NE(std::string::npos, failed.second.find("encrypted startup rejected"))
      << failed.second;
  // The module was decrypted before it threw. A subsequent independent reader
  // must nevertheless fail after initialization rolls back the public key.
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive_path_));

  const auto initialized = InitializeArchiveApp("main.js", true);
  ASSERT_TRUE(initialized.first) << initialized.second;
  ASSERT_NO_FATAL_FAILURE(ExpectArchiveReply());
}

TEST_F(XenonServiceArchiveTest,
       IsolatedAddonRuntimeOwnsApplicationScopeAndArchiveKeys) {
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->RemoveSwitch(
      napi_switches::kAllowExternalNodeAddons);
  const auto archive = app_path_.AppendASCII("native.asar");
  std::string native_bytes;
  ASSERT_TRUE(base::ReadFileToString(
      base::FilePath::FromUTF8Unsafe(addon_path_), &native_bytes));
  ASSERT_TRUE(
      builder_.Write(archive, {{"bin/addon.node", native_bytes, false, true}}));
  const auto module = archive.AppendASCII("bin").AppendASCII("addon.node");
  auto configure = [&](bool with_key) {
    std::vector<ipc::mojom::IpcArchivePublicKeyPtr> keys;
    if (with_key) {
      keys.push_back(ipc::mojom::IpcArchivePublicKey::New(
          app_path_.AsUTF8Unsafe(), builder_.public_key_pem()));
    }
    browser_->InitializeNodeAddonRuntime(kContext, app_path_.AsUTF8Unsafe(),
                                         archive.AsUTF8Unsafe(),
                                         std::move(keys));
    browser_.FlushForTesting();
  };
  configure(false);
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive));
  NodeRemote node = Bind("native-page");
  ResultFuture denied;
  node->RequireNodeModuleSync(module.AsUTF8Unsafe(), denied.GetCallback());
  auto result = Finish(denied);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->success);

  configure(true);
  ResultFuture loaded;
  node->RequireNodeModuleSync(module.AsUTF8Unsafe(), loaded.GetCallback());
  result = Finish(loaded);
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_FALSE(XenonServiceImplTestPeer::HasMainContainer(*service_));
  EXPECT_TRUE(asar::GetOrCreateAsarArchive(archive));

  // Removing keys must revoke even the cached native-load path.
  configure(false);
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive));
  ResultFuture revoked;
  node->RequireNodeModuleSync(module.AsUTF8Unsafe(), revoked.GetCallback());
  result = Finish(revoked);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->error.starts_with("ERR_ACCESS_DENIED:")) << result->error;

  configure(true);
  EXPECT_TRUE(asar::GetOrCreateAsarArchive(archive));
  node.reset();
  service_.reset();
  browser_.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(asar::GetOrCreateAsarArchive(archive));
}

TEST_F(XenonServiceOwnerTest,
       DiskApplicationConfigOverridesCommandLineAndPreservesRuntimeMetadata) {
  base::ScopedEnvironmentVariableOverride hosted_directory(
      "XENON_HOSTED_APP_DIR", "previous-runtime");
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath root;
  ASSERT_TRUE(base::NormalizeFilePath(temp.GetPath(), &root));
  const auto app = root.AppendASCII("configured-app");
  const auto resources = root.AppendASCII("shared-resources");
  const auto working_directory = root.AppendASCII("launch-directory");
  ASSERT_TRUE(base::CreateDirectory(app));
  ASSERT_TRUE(base::CreateDirectory(resources));
  ASSERT_TRUE(base::CreateDirectory(working_directory));
  ASSERT_TRUE(
      base::WriteFile(app.AppendASCII("package.json"),
                      R"({"name":"disk-app","version":"4.3","main":"entry"})"));
  ASSERT_TRUE(
      base::WriteFile(app.AppendASCII("value.json"), R"({"answer":42})"));
  ASSERT_TRUE(base::WriteFile(app.AppendASCII("entry.js"), R"JS(
const {app, ipcMain} = require('electron');
const data = require('./value.json');
ipcMain.handle('disk-app:metadata', () => ({
  answer: data.answer, appPath: app.getAppPath(), filename: __filename,
  name: app.getName(), version: app.getVersion(), packaged: app.isPackaged,
  resources: process.resourcesPath, cwd: process.cwd(), argv: process.argv
}));
)JS"));
  const auto decoy = root.AppendASCII("command-line-main.js");
  ASSERT_TRUE(
      base::WriteFile(decoy, "throw new Error('wrong command-line main');"));
  base::test::ScopedCommandLine command_line;
  command_line.GetProcessCommandLine()->AppendSwitchPath("xenon-main-js",
                                                         decoy);

  auto config = ipc::mojom::IpcMainConfig::New();
  config->container_id = kContext;
  config->app_path = app.AsUTF8Unsafe();
  config->runtime_directory = root.AsUTF8Unsafe();
  config->resources_directory = resources.AsUTF8Unsafe();
  config->working_directory = working_directory.AsUTF8Unsafe();
  config->app_argv = {"trusted-browser-launcher", "--new-window",
                      "文档 名称.md", "--user-data-dir=application-profile"};
  config->is_packaged = true;
  ASSERT_TRUE(config->embedded_main_source.empty());
  ASSERT_TRUE(config->renderer_url_mappings.empty());
  base::test::TestFuture<bool, const std::string&> initialized;
  browser_->InitializeElectronIpc(std::move(config), initialized.GetCallback());
  ASSERT_TRUE(initialized.Wait());
  ASSERT_TRUE(initialized.Get<0>()) << initialized.Get<1>();

  RegisterMainDriver(kContext, "disk-page");
  ResultFuture reply;
  browser_->ElectronIpcInvoke(kContext, "disk-page", "disk-app:metadata",
                              base::Value(base::ListValue()),
                              reply.GetCallback());
  auto result = Finish(reply);
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success) << result->error;
  ASSERT_TRUE(result->value.is_dict());
  const auto& value = result->value.GetDict();
  EXPECT_EQ(42, value.FindInt("answer"));
  EXPECT_EQ(true, value.FindBool("packaged"));
  for (const char* key :
       {"appPath", "filename", "name", "version", "resources", "cwd"}) {
    ASSERT_TRUE(value.FindString(key)) << key;
  }
  EXPECT_EQ(app.AsUTF8Unsafe(), *value.FindString("appPath"));
  EXPECT_EQ(app.AppendASCII("entry.js").AsUTF8Unsafe(),
            *value.FindString("filename"));
  EXPECT_EQ("disk-app", *value.FindString("name"));
  EXPECT_EQ("4.3", *value.FindString("version"));
  EXPECT_EQ(resources.AsUTF8Unsafe(), *value.FindString("resources"));
  EXPECT_EQ(working_directory.AsUTF8Unsafe(), *value.FindString("cwd"));
  ASSERT_TRUE(value.FindList("argv"));
  EXPECT_EQ((base::ListValue()
                 .Append("trusted-browser-launcher")
                 .Append("--new-window")
                 .Append("文档 名称.md")
                 .Append("--user-data-dir=application-profile")),
            *value.FindList("argv"));
}

TEST_F(XenonServiceOwnerTest, AddonRuntimeDoesNotInitializeElectronMain) {
  base::ScopedEnvironmentVariableOverride hosted_directory(
      "XENON_HOSTED_APP_DIR", "previous-runtime");
  base::ScopedTempDir runtime;
  ASSERT_TRUE(runtime.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(runtime.GetPath().AppendASCII("package.json"),
                              R"({"main":"main.js"})"));
  ASSERT_TRUE(base::WriteFile(
      runtime.GetPath().AppendASCII("main.js"),
      "require('fs').writeFileSync(__dirname + '/main-executed', 'yes');"));

  browser_->InitializeNodeAddonRuntime(kContext,
                                       runtime.GetPath().AsUTF8Unsafe(),
                                       runtime.GetPath().AsUTF8Unsafe(), {});
  browser_.FlushForTesting();
  EXPECT_EQ(runtime.GetPath().AsUTF8Unsafe(),
            base::Environment::Create()->GetVar("XENON_HOSTED_APP_DIR"));
  EXPECT_TRUE(XenonServiceImplTestPeer::HasNodeExecutor(*service_, kContext));
  EXPECT_FALSE(XenonServiceImplTestPeer::HasMainContainer(*service_));
  EXPECT_FALSE(
      base::PathExists(runtime.GetPath().AppendASCII("main-executed")));

  ResultFuture main_call;
  browser_->ElectronIpcInvoke(kContext, "page", "ordinary-ipc-channel",
                              base::Value(base::ListValue()),
                              main_call.GetCallback());
  auto result = Finish(main_call);
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->success);
  EXPECT_NE(std::string::npos, result->error.find("ipcMain"));

  NodeRemote node = Bind("page");
  Load(node);
  ExpectReadable(Read(node, Construct(node)));
  EXPECT_FALSE(XenonServiceImplTestPeer::HasMainContainer(*service_));
  browser_->InitializeNodeAddonRuntime(kContext, "", "", {});
  browser_.FlushForTesting();
  EXPECT_FALSE(base::Environment::Create()->GetVar("XENON_HOSTED_APP_DIR"));
}

TEST_F(XenonServiceOwnerTest, DirectCallbacksWorkWithoutMainAndKeepOwner) {
  NodeCallbackRecorder first;
  NodeCallbackRecorder second;
  mojo::Receiver<ipc::mojom::IpcRenderer> first_receiver(&first);
  mojo::Receiver<ipc::mojom::IpcRenderer> second_receiver(&second);
  NodeRemote first_node =
      Bind("first", first_receiver.BindNewPipeAndPassRemote());
  NodeRemote second_node =
      Bind("second", second_receiver.BindNewPipeAndPassRemote());
  Load(first_node);
  EXPECT_FALSE(XenonServiceImplTestPeer::HasMainContainer(*service_));

  base::Value payload(base::DictValue().Set("label", "first-page"));
  for (bool direct : {true, false}) {
    SCOPED_TRACE(direct);
    base::ListValue args;
    args.Append(CallbackWire(81));
    args.Append(payload.Clone());
    ipc::mojom::IpcResultPtr result;
    if (direct) {
      auto [reply, promise_id] =
          CallExport(first_node, "InvokeCallbackWithReceiver", std::move(args));
      EXPECT_EQ(0u, promise_id);
      result = std::move(reply);
    } else {
      base::DictValue request;
      request.Set("modulePath", addon_path_);
      request.Set("functionName", "InvokeCallbackWithReceiver");
      request.Set("arguments", std::move(args));
      ResultFuture invoked;
      browser_->ElectronIpcInvoke(
          kContext, "first", kExportChannel,
          base::Value(base::ListValue().Append(std::move(request))),
          invoked.GetCallback());
      result = Finish(invoked);
    }
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->success) << result->error;
    EXPECT_EQ(payload, result->value);
  }
  ASSERT_EQ(2u, first.calls.size());
  for (const auto& call : first.calls) {
    EXPECT_EQ(81, call.id);
    ASSERT_EQ(1u, call.arguments.size());
    EXPECT_EQ(payload, call.arguments[0]);
    EXPECT_EQ(payload, call.receiver);
  }
  EXPECT_TRUE(second.calls.empty());

  auto [second_result, promise_id] =
      CallExport(second_node, "InvokeCallbackWithReceiver",
                 base::ListValue()
                     .Append(CallbackWire(81))
                     .Append(base::DictValue().Set("label", "second-page")));
  ASSERT_TRUE(second_result);
  ASSERT_TRUE(second_result->success) << second_result->error;
  EXPECT_EQ(0u, promise_id);
  ASSERT_EQ(1u, second.calls.size());
  ASSERT_TRUE(second.calls[0].receiver.is_dict());
  EXPECT_EQ("second-page",
            *second.calls[0].receiver.GetDict().FindString("label"));
  EXPECT_EQ(2u, first.calls.size());

  const auto direct_handle = Construct(first_node);
  const auto async_handle = ConstructViaBrowser("first");
  EXPECT_EQ(direct_handle.token, async_handle.token);
  ExpectReadable(Read(first_node, async_handle));
  ExpectReadable(ReadViaBrowser("first", direct_handle));
  ExpectInvalidOwner(Read(second_node, direct_handle));

  // Exercise the service's GC notification endpoint over the same real Mojo
  // pipe without making this routing test depend on V8's collection timing.
  XenonServiceImplTestPeer::ReleaseCallback(*service_, kContext, "first", 81);
  task_environment_.RunUntilIdle();
  EXPECT_EQ((std::vector<int32_t>{81}), first.released);
  EXPECT_TRUE(second.released.empty());
}

TEST_F(XenonServiceOwnerTest, RebindingDisconnectsOldCallbackPipe) {
  NodeCallbackRecorder old_callbacks;
  NodeCallbackRecorder new_callbacks;
  mojo::Receiver<ipc::mojom::IpcRenderer> old_receiver(&old_callbacks);
  mojo::Receiver<ipc::mojom::IpcRenderer> new_receiver(&new_callbacks);
  NodeRemote old_node = Bind("page", old_receiver.BindNewPipeAndPassRemote());
  Load(old_node);
  const auto old_handle = Construct(old_node);
  auto [retained, promise_id] =
      CallExport(old_node, "RetainCallback",
                 base::ListValue().Append(1).Append(CallbackWire(7)));
  ASSERT_TRUE(retained);
  ASSERT_TRUE(retained->success) << retained->error;
  EXPECT_EQ(0u, promise_id);

  base::test::TestFuture<void> old_disconnected;
  old_receiver.set_disconnect_handler(old_disconnected.GetCallback());
  NodeRemote new_node = Bind("page", new_receiver.BindNewPipeAndPassRemote());
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(old_node.is_connected());
  EXPECT_TRUE(old_disconnected.IsReady());
  ExpectInvalidOwner(Read(new_node, old_handle));

  auto [called, call_promise_id] =
      CallExport(new_node, "CallRetainedCallback", base::ListValue().Append(1));
  ASSERT_TRUE(called);
  ASSERT_TRUE(called->success) << called->error;
  EXPECT_EQ(0u, call_promise_id);
  EXPECT_EQ(0, called->value.GetInt());
  EXPECT_TRUE(old_callbacks.calls.empty());
  EXPECT_TRUE(new_callbacks.calls.empty());

  auto [new_result, new_promise_id] =
      CallExport(new_node, "InvokeCallbackWithReceiver",
                 base::ListValue()
                     .Append(CallbackWire(7))
                     .Append(base::DictValue().Set("label", "replacement")));
  ASSERT_TRUE(new_result);
  ASSERT_TRUE(new_result->success) << new_result->error;
  EXPECT_EQ(0u, new_promise_id);
  ASSERT_EQ(1u, new_callbacks.calls.size());
  ASSERT_TRUE(new_callbacks.calls[0].receiver.is_dict());
  EXPECT_EQ("replacement",
            *new_callbacks.calls[0].receiver.GetDict().FindString("label"));
  EXPECT_TRUE(old_callbacks.calls.empty());
  const auto new_handle = Construct(new_node);
  EXPECT_NE(old_handle.token, new_handle.token);
  ExpectReadable(ReadViaBrowser("page", new_handle));
}

TEST_F(XenonServiceOwnerTest, EitherPipeOrEndpointRemovalReleasesCallbacks) {
  for (const std::string close_path : {"native", "callback", "remove"}) {
    SCOPED_TRACE(close_path);
    const std::string endpoint = "page-" + close_path;
    NodeCallbackRecorder callbacks;
    NodeCallbackRecorder sibling_callbacks;
    mojo::Receiver<ipc::mojom::IpcRenderer> callback_receiver(&callbacks);
    mojo::Receiver<ipc::mojom::IpcRenderer> sibling_receiver(
        &sibling_callbacks);
    NodeRemote node =
        Bind(endpoint, callback_receiver.BindNewPipeAndPassRemote());
    NodeRemote sibling =
        Bind("sibling", sibling_receiver.BindNewPipeAndPassRemote());
    Load(node);
    const auto handle = Construct(node);
    const auto sibling_handle = Construct(sibling);
    for (int slot : {1, 2}) {
      auto [result, promise_id] =
          CallExport(slot == 1 ? node : sibling, "RetainCallback",
                     base::ListValue().Append(slot).Append(CallbackWire(7)));
      ASSERT_TRUE(result);
      ASSERT_TRUE(result->success) << result->error;
      EXPECT_EQ(0u, promise_id);
    }
    base::test::TestFuture<void> callbacks_disconnected;
    callback_receiver.set_disconnect_handler(
        callbacks_disconnected.GetCallback());
    if (close_path == "native") {
      node.reset();
    } else if (close_path == "callback") {
      callback_receiver.reset();
    } else {
      browser_->RemoveElectronIpcRenderer(kContext, endpoint);
      browser_.FlushForTesting();
    }
    task_environment_.RunUntilIdle();
    if (close_path != "native") {
      EXPECT_FALSE(node.is_connected());
    }
    if (close_path != "callback") {
      EXPECT_TRUE(callbacks_disconnected.IsReady());
    }
    for (int slot : {1, 2}) {
      auto [result, promise_id] = CallExport(sibling, "CallRetainedCallback",
                                             base::ListValue().Append(slot));
      ASSERT_TRUE(result);
      ASSERT_TRUE(result->success) << result->error;
      EXPECT_EQ(0u, promise_id);
    }
    EXPECT_TRUE(callbacks.calls.empty());
    ASSERT_EQ(1u, sibling_callbacks.calls.size());
    ExpectReadable(Read(sibling, sibling_handle));
    NodeRemote replacement = Bind(endpoint);
    ExpectInvalidOwner(Read(replacement, handle));
  }
}

TEST_F(XenonServiceOwnerTest,
       DirectAndBrowserExportsKeepBinaryAndArgumentOrder) {
  NodeRemote node = Bind("binary-page");
  Load(node);
  base::ListValue payload;
  payload.Append(BinaryWire("Buffer", {0, 255, 128, 1}));
  payload.Append(BinaryWire("Uint16Array", {0, 128, 254, 255}));
  payload.Append(BinaryWire("DataView"));
  base::Value nested(base::DictValue().Set("payload", std::move(payload)));
  for (bool direct : {true, false}) {
    SCOPED_TRACE(direct);
    base::ListValue arguments;
    arguments.Append(nested.Clone());
    ipc::mojom::IpcResultPtr result;
    if (direct) {
      auto [reply, promise_id] =
          CallExport(node, "EchoOwnedHandle", std::move(arguments));
      EXPECT_EQ(0u, promise_id);
      result = std::move(reply);
    } else {
      base::DictValue request;
      request.Set("modulePath", addon_path_);
      request.Set("functionName", "EchoOwnedHandle");
      request.Set("arguments", std::move(arguments));
      ResultFuture invoked;
      browser_->ElectronIpcInvoke(
          kContext, "binary-page", kExportChannel,
          base::Value(base::ListValue().Append(std::move(request))),
          invoked.GetCallback());
      result = Finish(invoked);
    }
    ASSERT_TRUE(result);
    ASSERT_TRUE(result->success) << result->error;
    EXPECT_EQ(nested, result->value);
  }
  auto [sum, promise_id] =
      CallExport(node, "Add", base::ListValue().Append(19).Append(23));
  ASSERT_TRUE(sum);
  ASSERT_TRUE(sum->success) << sum->error;
  EXPECT_EQ(0u, promise_id);
  EXPECT_EQ(42.0, sum->value.GetDouble());
}

TEST_F(XenonServiceOwnerTest,
       StandardMainNativeModulesKeepSynchronousCallbacksAndObjectIdentity) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath app;
  ASSERT_TRUE(base::NormalizeFilePath(temp.GetPath(), &app));
  ASSERT_TRUE(base::CopyFile(base::FilePath::FromUTF8Unsafe(addon_path_),
                             app.AppendASCII("test_addon.node")));
  auto config = ipc::mojom::IpcMainConfig::New();
  config->container_id = kContext;
  config->app_path = app.AsUTF8Unsafe();
  config->virtual_main_path = app.AppendASCII("main.js").AsUTF8Unsafe();
  config->embedded_main_source = R"JS(
    const {ipcMain} = require('electron');
    const native = require('./test_addon.node');
    ipcMain.handle('native-contract', () => {
      const object = {value: 42};
      const events = [];
      let returned;
      native.InvokeCallbackWithFunction(fn => {
        events.push('callback');
        returned = fn(41);
      });
      events.push('returned');
      let sameReceiver = false;
      native.InvokeCallbackWithReceiver(function(value) {
        sameReceiver = this === object && value === object;
      }, object);
      let caught = false;
      const error = new Error('identity');
      try { native.InvokeCallbackWithFunction(() => { throw error; }); }
      catch (value) { caught = value === error; }
      return {events, returned, sameReceiver, caught,
              sameObject: native.EchoOwnedHandle(object) === object,
              sameModule: native === require('./test_addon.node')};
    });
  )JS";
  base::test::TestFuture<bool, const std::string&> initialized;
  browser_->InitializeElectronIpc(std::move(config), initialized.GetCallback());
  ASSERT_TRUE(initialized.Wait());
  ASSERT_TRUE(initialized.Get<0>()) << initialized.Get<1>();
  RegisterMainDriver(kContext, "main-driver");
  ResultFuture future;
  browser_->ElectronIpcInvoke(kContext, "main-driver", "native-contract",
                              base::Value(base::ListValue()),
                              future.GetCallback());
  auto result = Finish(future);
  ASSERT_TRUE(result && result->success)
      << (result ? result->error : "no result");
  const auto& values = result->value.GetDict();
  EXPECT_EQ(42, values.FindInt("returned"));
  EXPECT_EQ(true, values.FindBool("sameReceiver"));
  EXPECT_EQ(true, values.FindBool("sameObject"));
  EXPECT_EQ(true, values.FindBool("sameModule"));
  EXPECT_EQ(true, values.FindBool("caught"));
  const auto* events = values.FindList("events");
  ASSERT_TRUE(events);
  EXPECT_EQ(base::ListValue().Append("callback").Append("returned"), *events);
}

TEST_F(
    XenonServiceOwnerTest,
    NativeExecutionModeControlsEnvironmentSharingAndCannotChangeWhileRunning) {
  base::ScopedTempDir temp;
  ASSERT_TRUE(temp.CreateUniqueTempDir());
  base::FilePath app;
  ASSERT_TRUE(base::NormalizeFilePath(temp.GetPath(), &app));
  ASSERT_TRUE(base::CopyFile(base::FilePath::FromUTF8Unsafe(addon_path_),
                             app.AppendASCII("test_addon.node")));
  addon_path_ = app.AppendASCII("test_addon.node").AsUTF8Unsafe();
  for (const auto mode : {ipc::mojom::NativeAddonExecution::kStandard,
                          ipc::mojom::NativeAddonExecution::kShared}) {
    SCOPED_TRACE(static_cast<int>(mode));
    StartService();
    auto config = ipc::mojom::IpcMainConfig::New();
    config->container_id = kContext;
    config->app_path = app.AsUTF8Unsafe();
    config->virtual_main_path = app.AppendASCII("main.js").AsUTF8Unsafe();
    config->native_addon_execution = mode;
    config->embedded_main_source = R"JS(
      const native = require('./test_addon.node');
      require('electron').ipcMain.handle('native-counter',
        () => native.nextEnvironmentCounter());
    )JS";
    base::test::TestFuture<bool, const std::string&> initialized;
    browser_->InitializeElectronIpc(config.Clone(), initialized.GetCallback());
    ASSERT_TRUE(initialized.Wait());
    ASSERT_TRUE(initialized.Get<0>()) << initialized.Get<1>();
    RegisterMainDriver(kContext, "main-driver");
    auto invoke_main = [&]() {
      ResultFuture future;
      browser_->ElectronIpcInvoke(kContext, "main-driver", "native-counter",
                                  base::Value(base::ListValue()),
                                  future.GetCallback());
      return Finish(future);
    };
    auto first = invoke_main();
    ASSERT_TRUE(first && first->success);
    EXPECT_EQ(1, first->value.GetInt());
    NodeRemote node = Bind("renderer-page");
    Load(node);
    auto [renderer, promise_id] = CallExport(node, "nextEnvironmentCounter");
    ASSERT_TRUE(renderer && renderer->success);
    EXPECT_EQ(0u, promise_id);
    EXPECT_EQ(mode == ipc::mojom::NativeAddonExecution::kShared ? 2 : 1,
              renderer->value.GetInt());
    auto second = invoke_main();
    ASSERT_TRUE(second && second->success);
    EXPECT_EQ(mode == ipc::mojom::NativeAddonExecution::kShared ? 3 : 2,
              second->value.GetInt());
    config->native_addon_execution =
        mode == ipc::mojom::NativeAddonExecution::kShared
            ? ipc::mojom::NativeAddonExecution::kStandard
            : ipc::mojom::NativeAddonExecution::kShared;
    base::test::TestFuture<bool, const std::string&> changed;
    browser_->InitializeElectronIpc(std::move(config), changed.GetCallback());
    ASSERT_TRUE(changed.Wait());
    EXPECT_FALSE(changed.Get<0>());
    EXPECT_NE(std::string::npos,
              changed.Get<1>().find("while the container is running"));
  }
}

TEST_F(XenonServiceOwnerTest,
       MovedArgumentsKeepCallbacksAndConstructorReceivers) {
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  base::FilePath app_path;
  ASSERT_TRUE(base::NormalizeFilePath(temp_dir.GetPath(), &app_path));
  auto config = ipc::mojom::IpcMainConfig::New();
  config->container_id = kContext;
  config->embedded_main_source = "module.exports = {};";
  config->virtual_main_path = app_path.AppendASCII("main.js").AsUTF8Unsafe();
  config->app_path = app_path.AsUTF8Unsafe();
  config->app_name = "Native callback argument test";
  base::test::TestFuture<bool, const std::string&> initialized;
  browser_->InitializeElectronIpc(std::move(config), initialized.GetCallback());
  ASSERT_TRUE(initialized.Wait());
  ASSERT_TRUE(initialized.Get<0>()) << initialized.Get<1>();
  NodeCallbackRecorder observer;
  mojo::Receiver<ipc::mojom::IpcRenderer> receiver(&observer);
  base::test::TestFuture<bool> registered;
  browser_->RegisterElectronIpcRenderer(
      kContext, "callback-page", receiver.BindNewPipeAndPassRemote(), 1, 1, 1,
      "file:///callback-page.html", true, registered.GetCallback());
  ASSERT_TRUE(registered.Get());
  NodeRemote node = Bind("callback-page");
  Load(node);
  base::Value payload(
      base::DictValue().Set("bytes", BinaryWire("Buffer", {0, 255, 128})));
  auto [result, promise_id] = CallExport(
      node, "InvokeCallbackWithReceiver",
      base::ListValue().Append(CallbackWire(81)).Append(payload.Clone()));
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(0u, promise_id);
  EXPECT_EQ(payload, result->value);
  ASSERT_EQ(1u, observer.calls.size());
  EXPECT_EQ(81, observer.calls[0].id);
  EXPECT_EQ(payload, observer.calls[0].receiver);
  ASSERT_EQ(1u, observer.calls[0].arguments.size());
  EXPECT_EQ(payload, observer.calls[0].arguments[0]);

  ResultFuture constructed;
  node->ConstructNodeExportSync(
      addon_path_, "OwnedHandle",
      base::Value(base::ListValue().Append(CallbackWire(82))),
      base::Value(base::DictValue()), constructed.GetCallback());
  const auto handle = ParseHandle(Finish(constructed));
  ASSERT_GT(handle.id, 0);
  ASSERT_EQ(2u, observer.calls.size());
  EXPECT_EQ(82, observer.calls[1].id);
  ASSERT_TRUE(observer.calls[1].receiver.is_dict());
  const auto& callback_receiver = observer.calls[1].receiver.GetDict();
  EXPECT_EQ(handle.id, callback_receiver.FindInt("instance_id"));
  ASSERT_TRUE(callback_receiver.FindString("owner_token"));
  EXPECT_EQ(handle.token, *callback_receiver.FindString("owner_token"));
  ASSERT_EQ(1u, observer.calls[1].arguments.size());
  EXPECT_EQ(observer.calls[1].receiver, observer.calls[1].arguments[0]);
  ExpectReadable(Read(node, handle));
}

TEST_F(XenonServiceOwnerTest,
       MovedInstanceArgumentsPreserveNativeFunctionCalls) {
  NodeRemote node = Bind("function-page");
  Load(node);
  auto [returned, promise_id] = CallExport(node, "MakeOwnedReturns");
  ASSERT_TRUE(returned);
  ASSERT_TRUE(returned->success) << returned->error;
  EXPECT_EQ(0u, promise_id);
  ASSERT_TRUE(returned->value.is_list());
  ASSERT_EQ(2u, returned->value.GetList().size());
  const auto& function = returned->value.GetList()[1].GetDict();
  ASSERT_TRUE(function.FindInt("instance_id"));
  ASSERT_TRUE(function.FindString("owner_token"));
  InstanceHandle handle{*function.FindInt("instance_id"),
                        *function.FindString("owner_token")};
  auto result = Read(node, handle, "call",
                     base::ListValue()
                         .Append(base::DictValue().Set(
                             "__xenon_node_wire_type__", "undefined"))
                         .Append(41));
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(42, result->value.GetInt());
}

TEST_F(XenonServiceOwnerTest,
       MovedArgumentsKeepPendingPromisesWithoutReinvoking) {
  NodeRemote node = Bind("promise-page");
  Load(node);
  auto [started, promise_id] = CallExport(node, "BeginControlledPromise");
  ASSERT_TRUE(started);
  ASSERT_TRUE(started->success) << started->error;
  ASSERT_NE(0u, promise_id);
  ResultFuture settled;
  node->AwaitNodePromise(promise_id, settled.GetCallback());
  node.FlushForTesting();
  EXPECT_FALSE(settled.IsReady());
  auto [completed, immediate_id] =
      CallExport(node, "SettleControlledPromise",
                 base::ListValue().Append(1).Append(false));
  ASSERT_TRUE(completed);
  ASSERT_TRUE(completed->success) << completed->error;
  EXPECT_EQ(0u, immediate_id);
  EXPECT_EQ(1, completed->value.GetInt());
  auto result = Finish(settled);
  ASSERT_TRUE(result);
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(1, result->value.GetInt());
  auto [count, count_promise_id] =
      CallExport(node, "ControlledPromiseCallCount");
  ASSERT_TRUE(count);
  ASSERT_TRUE(count->success) << count->error;
  EXPECT_EQ(0u, count_promise_id);
  EXPECT_EQ(1, count->value.GetInt());
}

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

constexpr char kMainAppEventSource[] = R"JS(
const {app, ipcMain} = require('electron');
const state = {readyCount: 0, whenReadyCount: 0, calls: [], microtasks: 0};
app.on('ready', () => ++state.readyCount);
app.whenReady().then(() => {
  ++state.whenReadyCount;
  app.on('activate', (_event, ...args) => {
    state.calls.push(args);
    queueMicrotask(() => ++state.microtasks);
  });
});
ipcMain.handle('app:event-state', () => state);
)JS";

class XenonServiceAppEventTest : public XenonServiceOwnerTest {
 protected:
  void SetUp() override {
    XenonServiceOwnerTest::SetUp();
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    ASSERT_TRUE(base::NormalizeFilePath(temp_dir_.GetPath(), &app_path_));
  }

  void InitializeMain(const std::string& container_id) {
    auto config = ipc::mojom::IpcMainConfig::New();
    config->container_id = container_id;
    config->embedded_main_source = kMainAppEventSource;
    config->virtual_main_path =
        app_path_.AppendASCII(container_id + ".js").AsUTF8Unsafe();
    config->app_path = app_path_.AsUTF8Unsafe();
    config->app_name = container_id;
    base::test::TestFuture<bool, const std::string&> initialized;
    browser_->InitializeElectronIpc(std::move(config),
                                    initialized.GetCallback());
    ASSERT_TRUE(initialized.Wait());
    ASSERT_TRUE(initialized.Get<0>()) << initialized.Get<1>();
    RegisterMainDriver(container_id, "test-driver");
    task_environment_.RunUntilIdle();
  }

  ipc::mojom::IpcResultPtr ReadState(const std::string& container_id) {
    ResultFuture result;
    browser_->ElectronIpcInvoke(container_id, "test-driver", "app:event-state",
                                base::Value(base::ListValue()),
                                result.GetCallback());
    if (!result.Wait()) {
      ADD_FAILURE() << "App event state query did not complete";
      return nullptr;
    }
    return result.Take();
  }

  base::test::ScopedRunLoopTimeout timeout_{FROM_HERE, base::Seconds(5)};
  base::ScopedTempDir temp_dir_;
  base::FilePath app_path_;
};

TEST_F(XenonServiceAppEventTest,
       RepeatedActivateTargetsOnlyTheRequestedRunningContainer) {
  ASSERT_NO_FATAL_FAILURE(InitializeMain("app-a"));
  ASSERT_NO_FATAL_FAILURE(InitializeMain("app-b"));
  browser_->DispatchElectronAppEvent(
      "app-a", "activate",
      base::Value(base::ListValue().Append(false).Append("first")));
  browser_->DispatchElectronAppEvent(
      "app-a", "activate",
      base::Value(base::ListValue().Append(true).Append("second")));
  browser_.FlushForTesting();

  const auto selected = ReadState("app-a");
  ASSERT_TRUE(selected);
  ASSERT_TRUE(selected->success) << selected->error;
  const auto& state = selected->value.GetDict();
  EXPECT_EQ(1, state.FindInt("readyCount"));
  EXPECT_EQ(1, state.FindInt("whenReadyCount"));
  EXPECT_EQ(2, state.FindInt("microtasks"));
  const auto* calls = state.FindList("calls");
  ASSERT_TRUE(calls);
  ASSERT_EQ(2u, calls->size());
  EXPECT_EQ((base::ListValue().Append(false).Append("first")),
            (*calls)[0].GetList());
  EXPECT_EQ((base::ListValue().Append(true).Append("second")),
            (*calls)[1].GetList());

  const auto unrelated = ReadState("app-b");
  ASSERT_TRUE(unrelated);
  ASSERT_TRUE(unrelated->success) << unrelated->error;
  const auto& other_state = unrelated->value.GetDict();
  EXPECT_EQ(1, other_state.FindInt("readyCount"));
  EXPECT_EQ(1, other_state.FindInt("whenReadyCount"));
  EXPECT_EQ(0, other_state.FindInt("microtasks"));
  ASSERT_TRUE(other_state.FindList("calls"));
  EXPECT_TRUE(other_state.FindList("calls")->empty());
}

TEST_F(XenonServiceAppEventTest,
       UnknownActivateNeitherStartsAContainerNorBroadcastsOrReplays) {
  ASSERT_NO_FATAL_FAILURE(InitializeMain("app-a"));
  browser_->DispatchElectronAppEvent(
      "missing-app", "activate", base::Value(base::ListValue().Append(false)));
  browser_->DispatchElectronAppEvent(
      "", "activate", base::Value(base::ListValue().Append(false)));
  browser_.FlushForTesting();
  for (const char* container_id : {"missing-app", ""}) {
    SCOPED_TRACE(container_id);
    const auto missing = ReadState(container_id);
    ASSERT_TRUE(missing);
    EXPECT_FALSE(missing->success);
    EXPECT_EQ("Utility ipcMain container is unavailable", missing->error);
  }
  const auto existing = ReadState("app-a");
  ASSERT_TRUE(existing);
  ASSERT_TRUE(existing->success) << existing->error;
  ASSERT_TRUE(existing->value.GetDict().FindList("calls"));
  EXPECT_TRUE(existing->value.GetDict().FindList("calls")->empty());
  EXPECT_EQ(1, existing->value.GetDict().FindInt("readyCount"));

  ASSERT_NO_FATAL_FAILURE(InitializeMain("missing-app"));
  const auto created_later = ReadState("missing-app");
  ASSERT_TRUE(created_later);
  ASSERT_TRUE(created_later->success) << created_later->error;
  ASSERT_TRUE(created_later->value.GetDict().FindList("calls"));
  EXPECT_TRUE(created_later->value.GetDict().FindList("calls")->empty());
  EXPECT_EQ(1, created_later->value.GetDict().FindInt("readyCount"));
}

// These tests use the production main bootstrap and OS pipe bridge. The fake
// renderer implements only the Mojo endpoint, so it can detect accidental
// broadcast fallback without sharing the bootstrap's routing implementation.
class NetPipeRenderer : public ipc::mojom::IpcRenderer {
 public:
  void DispatchPostMessage(const std::string& channel,
                           blink::TransferableMessage message) override {
    ADD_FAILURE() << "Unexpected MessagePort IPC: " << channel;
  }
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
#if BUILDFLAG(IS_MAC)
    // Keep generated Unix socket endpoints within Darwin's sun_path capacity.
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDirUnderPath(base::FilePath("/tmp")));
#else
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
#endif
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
    RegisterMainDriver(kContext, "test-driver");
    task_environment_.RunUntilIdle();
  }

  void RegisterRenderer(const std::string& endpoint,
                        NetPipeRenderer& renderer) {
    base::test::TestFuture<bool> registered;
    browser_->RegisterElectronIpcRenderer(kContext, endpoint,
                                          renderer.BindNewRemote(), 1, 1, 1,
                                          "file:///net-pipe.html", true,
                                          registered.GetCallback());
    ASSERT_TRUE(registered.Get());
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

TEST_F(XenonServiceMainNetTest, TcpOptionsAndCancellationRequireTheSocketOwner) {
  ASSERT_NO_FATAL_FAILURE(InitializeEmbeddedMain());
  NetPipeRenderer owner;
  NetPipeRenderer other;
  RegisterRenderer("socket-owner", owner);
  RegisterRenderer("other-page", other);
  SendNet("socket-owner", "listen",
          base::DictValue().Set("serverId", "listener").Set("port", 0)
              .Set("host", "127.0.0.1"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return owner.Payload("__xenon:net:listening"); }));
  const auto* address = owner.Payload("__xenon:net:listening")->FindDict("address");
  ASSERT_TRUE(address);
  const int port = address->FindInt("port").value_or(0);
  ASSERT_GT(port, 0);
  SendNet("socket-owner", "connect",
          base::DictValue().Set("fromId", "client-js").Set("port", port)
              .Set("host", "127.0.0.1"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return owner.Payload("__xenon:net:connected"); }));
  const std::string socket =
      *owner.Payload("__xenon:net:connected")->FindString("peerId");
  // An invalid option would report EINVAL for the owner. Another renderer
  // must not even reach option validation or cancellation on this socket.
  SendNet("other-page", "no-delay", base::DictValue().Set("toId", socket));
  SendNet("other-page", "cancel", base::DictValue().Set("fromId", "client-js"));
  SendNet("socket-owner", "no-delay",
          base::DictValue().Set("toId", socket).Set("fromId", "client-js")
              .Set("noDelay", true));
  SendBytes("socket-owner", socket, "owner still connected");
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return owner.ReceivedBytes() == "owner still connected"; }));
  EXPECT_TRUE(other.messages.empty());
  EXPECT_FALSE(owner.Payload("__xenon:net:error"));
  EXPECT_FALSE(owner.Payload("__xenon:net:close"));
  SendNet("socket-owner", "cancel", base::DictValue().Set("fromId", "client-js"));
  ASSERT_TRUE(base::test::RunUntil(
      [&] { return owner.Payload("__xenon:net:close"); }));
}

}  // namespace

#if BUILDFLAG(ENABLE_XENON_SERVICE)
MULTIPROCESS_TEST_MAIN(XenonUtilityEnvironmentChild) {
  bool empty_present = false;
#if BUILDFLAG(IS_WIN)
  wchar_t value[2] = {};
  ::SetLastError(ERROR_SUCCESS);
  const DWORD length = ::GetEnvironmentVariableW(
      L"XENON_UTILITY_EMPTY", value, std::size(value));
  empty_present = length == 0 && ::GetLastError() == ERROR_SUCCESS;
#else
  const char* value = std::getenv("XENON_UTILITY_EMPTY");
  empty_present = value && *value == '\0';
#endif
  const bool expected = base::CommandLine::ForCurrentProcess()->HasSwitch(
      "expect-empty-environment-value");
  auto environment = base::Environment::Create();
  if (empty_present != expected) return 1;
  if (environment->HasVar("XENON_UTILITY_PARENT_ONLY")) return 2;
  if (environment->GetVar("XENON_UTILITY_KEPT") != "kept") return 3;
  return 0;
}
#endif

}  // namespace xenon
