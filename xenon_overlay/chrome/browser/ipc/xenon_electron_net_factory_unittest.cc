// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_net_factory.h"

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/test/test_future.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "net/base/net_errors.h"
#include "net/base/network_change_notifier.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "services/network/test/test_network_connection_tracker.h"
#include "services/network/test/test_network_context.h"
#include "services/network/test/test_url_loader_client.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_web_request.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {
namespace {
class NetFactoryContext : public content::TestBrowserContext {
 public:
  NetFactoryContext() {
    BrowserContextDependencyManager::GetInstance()->MarkBrowserContextLive(
        this);
  }
  ~NetFactoryContext() override {
    NotifyWillBeDestroyed();
    BrowserContextDependencyManager::GetInstance()
        ->DestroyBrowserContextServices(this);
  }
};
class SessionNetworkContext : public network::TestNetworkContext {
 public:
  mojo::PendingRemote<network::mojom::NetworkContext> Bind() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void CreateURLLoaderFactory(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      network::mojom::URLLoaderFactoryParamsPtr params) override {
    ++calls;
    parameters = std::move(params);
    target.Clone(std::move(receiver));
  }
  int calls = 0;
  network::mojom::URLLoaderFactoryParamsPtr parameters;
  network::TestURLLoaderFactory target;

 private:
  mojo::Receiver<network::mojom::NetworkContext> receiver_{this};
};
class ElectronNetFactoryTest : public testing::Test {
 public:
  static void SetUpTestSuite() {
    ElectronSessionShutdownNotifierFactory::GetInstance();
  }

 protected:
  content::BrowserTaskEnvironment tasks_;
  NetFactoryContext context_;
};

TEST_F(ElectronNetFactoryTest, UsesExactSessionWithTrustedHeaders) {
  SessionNetworkContext first, second;
  auto* one = ResolveElectronSessionPartition(&context_, "app", "");
  auto* two = ResolveElectronSessionPartition(&context_, "app", "persist:two");
  ASSERT_NE(one, two);
  one->SetNetworkContextForTesting(first.Bind());
  two->SetNetworkContextForTesting(second.Bind());
  std::string error;
  auto factory =
      CreateElectronNetFactory(&context_, "app", "persist:two", &error);
  ASSERT_TRUE(factory) << error;
  tasks_.RunUntilIdle();
  EXPECT_EQ(0, first.calls);
  ASSERT_EQ(1, second.calls);
  ASSERT_TRUE(second.parameters);
  EXPECT_TRUE(second.parameters->header_client);
  EXPECT_TRUE(second.parameters->is_trusted);
  EXPECT_FALSE(second.parameters->disable_web_security);
  EXPECT_TRUE(second.parameters->automatically_assign_isolation_info);
  factory.reset();
  tasks_.RunUntilIdle();
  RemoveElectronWebRequestHandlers("app");
}

TEST_F(ElectronNetFactoryTest, InvalidOrShuttingDownContextReturnsNoFactory) {
  std::string error;
  EXPECT_FALSE(CreateElectronNetFactory(nullptr, "app", "", &error));
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(CreateElectronNetFactory(&context_, "", "", &error));
  EXPECT_FALSE(CreateElectronNetFactory(&context_, "app",
                                        std::string(1025, 'a'), &error));
}

class FinalHeaderObserver : public mojom::ElectronNetworkHeadersObserver {
 public:
  mojo::PendingRemote<mojom::ElectronNetworkHeadersObserver> Bind() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void OnResponseHeaders(const std::string& value,
                         OnResponseHeadersCallback callback) override {
    raw = value;
    acknowledgement = std::move(callback);
  }
  std::string raw;
  base::OnceClosure acknowledgement;

 private:
  mojo::Receiver<mojom::ElectronNetworkHeadersObserver> receiver_{this};
};
TEST_F(ElectronNetFactoryTest,
       RawHeadersFollowInterceptionAndWaitForMetadataAck) {
  SessionNetworkContext session_network;
  auto* partition = ResolveElectronSessionPartition(&context_, "app", "");
  partition->SetNetworkContextForTesting(session_network.Bind());
  std::string error;
  ASSERT_TRUE(SetElectronWebRequestHandler(
      &context_, "app", 1,
      base::Value(base::DictValue()
                      .Set("partition", "")
                      .Set("event", "onHeadersReceived")
                      .Set("registrationId", "cookie")
                      .Set("enabled", true)
                      .Set("filter", base::DictValue())),
      base::BindRepeating(
          [](uint64_t, base::Value, ElectronWebRequestReply reply) {
            std::move(reply).Run(
                base::Value(base::DictValue().Set(
                    "responseHeaders",
                    base::DictValue().Set(
                        "set-cookie", base::ListValue().Append("modified=1")))),
                "");
          }),
      base::DoNothing(), base::DoNothing(), &error));
  FinalHeaderObserver observer;
  mojo::Remote<network::mojom::URLLoaderFactory> factory(
      CreateElectronNetFactory(&context_, "app", "", &error, observer.Bind()));
  ASSERT_TRUE(factory) << error;
  network::ResourceRequest request;
  request.url = GURL("https://headers.test/");
  mojo::Remote<network::mojom::URLLoader> loader;
  network::TestURLLoaderClient client;
  factory->CreateLoaderAndStart(
      loader.BindNewPipeAndPassReceiver(), 42, 0, request,
      client.CreateRemote(),
      net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
  tasks_.RunUntilIdle();
  ASSERT_EQ(1, session_network.target.NumPending());
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> headers_factory(
      std::move(session_network.parameters->header_client));
  mojo::Remote<network::mojom::TrustedHeaderClient> headers;
  headers_factory->OnLoaderCreated(
      session_network.target.GetPendingRequest(0)->request_id,
      headers.BindNewPipeAndPassReceiver());
  auto original =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 200 OK");
  original->AddHeader("Set-Cookie", "original=1");
  base::test::TestFuture<int32_t, std::optional<std::string>,
                         std::optional<GURL>>
      future;
  headers->OnHeadersReceived(
      original->raw_headers(), {}, std::nullopt,
      future.GetCallback<int32_t, const std::optional<std::string>&,
                         const std::optional<GURL>&>());
  tasks_.RunUntilIdle();
  EXPECT_FALSE(future.IsReady());
  ASSERT_TRUE(observer.acknowledgement);
  auto final = base::MakeRefCounted<net::HttpResponseHeaders>(observer.raw);
  EXPECT_EQ("modified=1", final->GetNormalizedHeader("Set-Cookie"));
  std::move(observer.acknowledgement).Run();
  EXPECT_EQ(net::OK, std::get<0>(future.Take()));
  loader.reset();
  factory.reset();
  tasks_.RunUntilIdle();
  RemoveElectronWebRequestHandlers("app");
}

TEST_F(ElectronNetFactoryTest, OnlineUsesChromiumConnectionState) {
  auto tracker = network::TestNetworkConnectionTracker::CreateInstance();
  content::SetNetworkConnectionTrackerForTesting(tracker.get());
  tracker->SetConnectionType(net::NetworkChangeNotifier::CONNECTION_NONE);
  EXPECT_FALSE(IsElectronNetworkOnline());
  tracker->SetConnectionType(net::NetworkChangeNotifier::CONNECTION_WIFI);
  EXPECT_TRUE(IsElectronNetworkOnline());
  content::SetNetworkConnectionTrackerForTesting(nullptr);
}
}  // namespace
}  // namespace xenon::ipc
