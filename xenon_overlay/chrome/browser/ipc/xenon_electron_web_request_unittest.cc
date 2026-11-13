// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_web_request.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/test_future.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "content/public/test/browser_task_environment.h"
#include "content/public/test/test_browser_context.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/parsed_headers.mojom.h"
#include "services/network/test/test_data_pipe_getter.h"
#include "services/network/test/test_url_loader_client.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"

namespace xenon::ipc {
namespace {
using RequestHeaderFuture =
    base::test::TestFuture<int32_t,
                           std::optional<net::HttpRequestHeaders>,
                           std::optional<base::DictValue>>;
class WebRequestTestContext : public content::TestBrowserContext {
 public:
  WebRequestTestContext() {
    BrowserContextDependencyManager::GetInstance()->MarkBrowserContextLive(
        this);
  }
  ~WebRequestTestContext() override { ShutdownServices(); }
  void ShutdownServices() {
    if (shutdown_) {
      return;
    }
    shutdown_ = true;
    NotifyWillBeDestroyed();
    BrowserContextDependencyManager::GetInstance()
        ->DestroyBrowserContextServices(this);
  }

 private:
  bool shutdown_ = false;
};

class DownstreamHeaders final
    : public network::mojom::TrustedURLLoaderHeaderClient,
      public network::mojom::TrustedHeaderClient {
 public:
  mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient> Bind() {
    return factory_receiver_.BindNewPipeAndPassRemote();
  }
  void OnLoaderCreated(
      int32_t id,
      mojo::PendingReceiver<network::mojom::TrustedHeaderClient> receiver)
      override {
    original_id = id;
    receivers_.Add(this, std::move(receiver));
  }
  void OnLoaderForCorsPreflightCreated(
      const network::ResourceRequest& request,
      mojo::PendingReceiver<network::mojom::TrustedHeaderClient> receiver)
      override {
    receivers_.Add(this, std::move(receiver));
  }
  void OnBeforeSendHeaders(const GURL& url,
                           const net::HttpRequestHeaders& headers,
                           OnBeforeSendHeadersCallback callback) override {
    auto replacement = headers;
    replacement.SetHeader("X-Existing-Interceptor", "retained");
    std::move(callback).Run(error, replacement,
                            base::DictValue().Set("downstream", true));
  }
  void OnHeadersReceived(const std::string& raw,
                         const net::IPEndPoint& endpoint,
                         const std::optional<net::SSLInfo>& ssl,
                         OnHeadersReceivedCallback callback) override {
    auto headers = base::MakeRefCounted<net::HttpResponseHeaders>(raw);
    headers->AddHeader("X-Existing-Interceptor", "retained");
    std::move(callback).Run(error, headers->raw_headers(),
                            GURL("https://example.test/redirect#kept"));
  }
  int error = net::OK;
  int32_t original_id = 0;

 private:
  mojo::Receiver<network::mojom::TrustedURLLoaderHeaderClient>
      factory_receiver_{this};
  mojo::ReceiverSet<network::mojom::TrustedHeaderClient> receivers_;
};

class ElectronWebRequestProxyTest : public testing::Test {
 public:
  static void SetUpTestSuite() {
    ElectronSessionShutdownNotifierFactory::GetInstance();
  }

 protected:
  struct Pending {
    base::Value request;
    ElectronWebRequestReply reply;
  };
  void SetUp() override {
    target_.SetInterceptor(base::BindRepeating(
        [](int* count, const network::ResourceRequest&) { ++*count; },
        &target_calls_));
  }
  void TearDown() override {
    RemoveElectronWebRequestHandlers("app");
    RemoveElectronWebRequestHandlers("other");
    context_.ShutdownServices();
    tasks_.RunUntilIdle();
  }
  void Register(const std::string& event = "onBeforeRequest",
                const std::string& id = "registration-1",
                bool enabled = true,
                const std::string& partition = "",
                base::DictValue filter = {}) {
    std::string error;
    EXPECT_TRUE(SetElectronWebRequestHandler(
        &context_, "app", 7,
        base::Value(base::DictValue()
                        .Set("partition", partition)
                        .Set("event", event)
                        .Set("registrationId", id)
                        .Set("enabled", enabled)
                        .Set("filter", std::move(filter))),
        base::BindRepeating(&ElectronWebRequestProxyTest::Dispatch,
                            base::Unretained(this)),
        base::BindRepeating(&ElectronWebRequestProxyTest::Cancel,
                            base::Unretained(this)),
        base::BindRepeating(&ElectronWebRequestProxyTest::Release,
                            base::Unretained(this)),
        &error))
        << error;
  }
  void Dispatch(uint64_t generation,
                base::Value request,
                ElectronWebRequestReply reply) {
    EXPECT_EQ(7u, generation);
    pending_.push_back({std::move(request), std::move(reply)});
  }
  void Cancel(uint64_t generation, const std::string& id) {
    cancelled_.push_back(id);
  }
  void Release(uint64_t generation, const std::string& id) {
    released_.push_back(id);
  }
  mojo::PendingRemote<network::mojom::URLLoaderFactory> TargetRemote() {
    mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
    target_.Clone(remote.InitWithNewPipeAndPassReceiver());
    return remote;
  }
  mojo::Remote<network::mojom::URLLoaderFactory> Factory(
      const std::string& container = "app",
      const std::string& partition = "") {
    auto* storage =
        ResolveElectronSessionPartition(&context_, container, partition);
    return mojo::Remote<network::mojom::URLLoaderFactory>(
        WrapElectronWebRequestFactory(&context_, storage, TargetRemote()));
  }
  mojo::Remote<network::mojom::URLLoader> Begin(
      network::mojom::URLLoaderFactory* factory,
      network::TestURLLoaderClient* client,
      const char* url = "file:///fixture/private.txt") {
    network::ResourceRequest request;
    request.url = GURL(url);
    request.destination = network::mojom::RequestDestination::kEmpty;
    return BeginRequest(factory, client, request);
  }
  mojo::Remote<network::mojom::URLLoader> BeginRequest(
      network::mojom::URLLoaderFactory* factory,
      network::TestURLLoaderClient* client,
      const network::ResourceRequest& request) {
    mojo::Remote<network::mojom::URLLoader> loader;
    factory->CreateLoaderAndStart(
        loader.BindNewPipeAndPassReceiver(), 42, 0, request,
        client->CreateRemote(),
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
    tasks_.RunUntilIdle();
    return loader;
  }
  mojo::Remote<network::mojom::URLLoaderFactory> NetworkFactory(
      mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient>* headers,
      DownstreamHeaders* downstream = nullptr) {
    auto* storage = ResolveElectronSessionPartition(&context_, "app", "");
    auto remote = downstream
                      ? downstream->Bind()
                      : mojo::PendingRemote<
                            network::mojom::TrustedURLLoaderHeaderClient>();
    network::URLLoaderFactoryBuilder builder;
    EXPECT_TRUE(AppendElectronWebRequestProxy(&context_, storage, builder, true,
                                              &remote));
    headers->Bind(std::move(remote));
    return mojo::Remote<network::mojom::URLLoaderFactory>(
        std::move(builder)
            .Finish<mojo::PendingRemote<network::mojom::URLLoaderFactory>>(
                TargetRemote()));
  }
  mojo::Remote<network::mojom::TrustedHeaderClient> NetworkHeaders(
      network::mojom::TrustedURLLoaderHeaderClient* factory) {
    mojo::Remote<network::mojom::TrustedHeaderClient> headers;
    factory->OnLoaderCreated(target_.GetPendingRequest(0)->request_id,
                             headers.BindNewPipeAndPassReceiver());
    return headers;
  }
  void SendHeaders(network::mojom::TrustedHeaderClient* client,
                   const char* url,
                   const net::HttpRequestHeaders& headers,
                   RequestHeaderFuture* future) {
    client->OnBeforeSendHeaders(
        GURL(url), headers,
        future->GetCallback<int32_t,
                            const std::optional<net::HttpRequestHeaders>&,
                            std::optional<base::DictValue>>());
    tasks_.RunUntilIdle();
  }
  void Reply(size_t index, base::DictValue value = {}) {
    ASSERT_LT(index, pending_.size());
    std::move(pending_[index].reply).Run(base::Value(std::move(value)), "");
    tasks_.RunUntilIdle();
  }
  content::BrowserTaskEnvironment tasks_;
  WebRequestTestContext context_;
  network::TestURLLoaderFactory target_{true};
  int target_calls_ = 0;
  std::vector<Pending> pending_;
  std::vector<std::string> cancelled_;
  std::vector<std::string> released_;
};

TEST_F(ElectronWebRequestProxyTest,
       FileCancellationNeverCallsUnderlyingFactory) {
  Register();
  auto factory = Factory();
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client);
  ASSERT_EQ(1u, pending_.size());
  EXPECT_EQ(0, target_calls_);
  EXPECT_FALSE(client.has_received_completion());
  Reply(0, base::DictValue().Set("cancel", true));
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_BLOCKED_BY_CLIENT, client.completion_status().error_code);
  EXPECT_EQ(0, target_calls_);
  EXPECT_FALSE(client.has_received_response());
}

TEST_F(ElectronWebRequestProxyTest,
       DelayedCallbackHoldsTargetThenReallyStartsIt) {
  Register();
  auto factory = Factory();
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client);
  ASSERT_EQ(1u, pending_.size());
  tasks_.RunUntilIdle();
  EXPECT_EQ(0, target_calls_);
  Reply(0);
  EXPECT_EQ(1, target_calls_);
  EXPECT_EQ(1, target_.NumPending());
}

TEST_F(ElectronWebRequestProxyTest,
       LoaderPipeClosingCannotRaceClientCompletion) {
  auto factory = Factory();
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client);
  ASSERT_EQ(1, target_.NumPending());
  auto* pending = target_.GetPendingRequest(0);
  pending->test_url_loader.reset();
  tasks_.RunUntilIdle();
  EXPECT_FALSE(client.has_received_completion());
  pending->client->OnComplete(
      network::URLLoaderCompletionStatus(net::ERR_CONNECTION_RESET));
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_CONNECTION_RESET, client.completion_status().error_code);
}

TEST_F(ElectronWebRequestProxyTest,
       PreviouslyCreatedFactoryObservesNewListener) {
  auto factory = Factory();
  Register();
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client);
  ASSERT_EQ(1u, pending_.size());
  EXPECT_EQ(0, target_calls_);
  Reply(0, base::DictValue().Set("cancel", true));
  EXPECT_EQ(0, target_calls_);
}

TEST_F(ElectronWebRequestProxyTest,
       ReplacedRegistrationRetainsOnlyItsInflightCallback) {
  Register();
  auto factory = Factory();
  network::TestURLLoaderClient first;
  auto loader1 = Begin(factory.get(), &first);
  Register("onBeforeRequest", "registration-2");
  EXPECT_TRUE(released_.empty());
  network::TestURLLoaderClient second;
  auto loader2 = Begin(factory.get(), &second);
  ASSERT_EQ(2u, pending_.size());
  EXPECT_EQ("registration-1",
            *pending_[0].request.GetDict().FindString("registrationId"));
  EXPECT_EQ("registration-2",
            *pending_[1].request.GetDict().FindString("registrationId"));
  Reply(0, base::DictValue().Set("cancel", true));
  ASSERT_EQ(1u, released_.size());
  EXPECT_EQ("registration-1", released_[0]);
  Reply(1);
  EXPECT_EQ(1, target_calls_);
  Register("onBeforeRequest", "registration-2", false);
  ASSERT_EQ(2u, released_.size());
  EXPECT_EQ("registration-2", released_[1]);
  network::TestURLLoaderClient third;
  auto loader3 = Begin(factory.get(), &third);
  EXPECT_EQ(2u, pending_.size());
  EXPECT_EQ(2, target_calls_);
}

TEST_F(ElectronWebRequestProxyTest,
       DefaultAndNamedSessionsAndContainersRemainIsolated) {
  Register();
  auto other_partition = Factory("app", "separate");
  auto other_container = Factory("other");
  network::TestURLLoaderClient first, second;
  auto loader1 = Begin(other_partition.get(), &first);
  auto loader2 = Begin(other_container.get(), &second);
  EXPECT_TRUE(pending_.empty());
  EXPECT_EQ(2, target_calls_);
}

TEST_F(ElectronWebRequestProxyTest,
       ContainerRemovalAbortsPendingRequestAndLateReplyIsIgnored) {
  Register();
  auto factory = Factory();
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client);
  ASSERT_EQ(1u, pending_.size());
  RemoveElectronWebRequestHandlers("app");
  tasks_.RunUntilIdle();
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, client.completion_status().error_code);
  EXPECT_EQ(1u, cancelled_.size());
  Reply(0);
  EXPECT_EQ(0, target_calls_);
}

TEST_F(ElectronWebRequestProxyTest,
       ClientDisconnectCancelsCallbackWithoutOpeningFile) {
  Register();
  auto factory = Factory();
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client);
  ASSERT_EQ(1u, pending_.size());
  loader.reset();
  tasks_.RunUntilIdle();
  EXPECT_EQ(1u, cancelled_.size());
  Reply(0);
  EXPECT_EQ(0, target_calls_);
}

TEST_F(ElectronWebRequestProxyTest,
       ProfileShutdownAbortsPendingRequestsBeforePartitionDestruction) {
  Register();
  auto factory = Factory();
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client);
  context_.ShutdownServices();
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, client.completion_status().error_code);
  EXPECT_EQ(0, target_calls_);
  EXPECT_EQ(1u, cancelled_.size());
}

TEST_F(ElectronWebRequestProxyTest,
       SameSchemeRedirectRematchesBeforeOpeningTarget) {
  Register();
  auto factory = Factory();
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client);
  Reply(0, base::DictValue().Set("redirectURL", "file:///fixture/allowed.txt"));
  client.RunUntilRedirectReceived();
  EXPECT_EQ(0, target_calls_);
  loader->FollowRedirect(network::HttpRequestHeadersUpdateParams(),
                         std::nullopt);
  tasks_.RunUntilIdle();
  ASSERT_EQ(2u, pending_.size());
  EXPECT_EQ(
      "file:///fixture/allowed.txt",
      *pending_[1].request.GetDict().FindDict("details")->FindString("url"));
  Reply(1);
  EXPECT_EQ(1, target_calls_);
  ASSERT_EQ(1, target_.NumPending());
  EXPECT_EQ(GURL("file:///fixture/allowed.txt"),
            target_.GetPendingRequest(0)->request.url);
}

TEST_F(ElectronWebRequestProxyTest,
       MissingTrustedHeadersFailsBeforeIssuingNetworkRequest) {
  auto* storage = ResolveElectronSessionPartition(&context_, "app", "");
  network::URLLoaderFactoryBuilder builder;
  ASSERT_TRUE(AppendElectronWebRequestProxy(&context_, storage, builder, true,
                                            nullptr));
  mojo::Remote<network::mojom::URLLoaderFactory> factory(
      std::move(builder)
          .Finish<mojo::PendingRemote<network::mojom::URLLoaderFactory>>(
              TargetRemote()));
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client, "https://example.test/");
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_NOT_IMPLEMENTED, client.completion_status().error_code);
  EXPECT_EQ(0, target_calls_);
}

TEST_F(ElectronWebRequestProxyTest,
       TrustedHeadersPreserveDownstreamChangesAndErrors) {
  Register("onHeadersReceived");
  auto* storage = ResolveElectronSessionPartition(&context_, "app", "");
  DownstreamHeaders downstream;
  auto header_remote = downstream.Bind();
  network::URLLoaderFactoryBuilder builder;
  ASSERT_TRUE(AppendElectronWebRequestProxy(&context_, storage, builder, true,
                                            &header_remote));
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> header_factory(
      std::move(header_remote));
  mojo::Remote<network::mojom::URLLoaderFactory> factory(
      std::move(builder)
          .Finish<mojo::PendingRemote<network::mojom::URLLoaderFactory>>(
              TargetRemote()));
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client, "https://example.test/source");
  ASSERT_EQ(1, target_.NumPending());
  mojo::Remote<network::mojom::TrustedHeaderClient> headers;
  header_factory->OnLoaderCreated(target_.GetPendingRequest(0)->request_id,
                                  headers.BindNewPipeAndPassReceiver());
  auto original =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 200 OK");
  base::test::TestFuture<int32_t, std::optional<std::string>,
                         std::optional<GURL>>
      future;
  headers->OnHeadersReceived(
      original->raw_headers(), {}, std::nullopt,
      future.GetCallback<int32_t, const std::optional<std::string>&,
                         const std::optional<GURL>&>());
  tasks_.RunUntilIdle();
  EXPECT_EQ(42, downstream.original_id);
  ASSERT_EQ(1u, pending_.size());
  const auto* observed =
      pending_[0].request.GetDict().FindDict("details")->FindDict(
          "responseHeaders");
  ASSERT_TRUE(observed);
  ASSERT_TRUE(observed->FindList("x-existing-interceptor"));
  auto replacement = observed->Clone();
  replacement.Set("x-application", base::ListValue().Append("added"));
  Reply(0, base::DictValue().Set("responseHeaders", std::move(replacement)));
  auto [error, raw, fragment] = future.Take();
  EXPECT_EQ(net::OK, error);
  ASSERT_TRUE(raw);
  auto result = base::MakeRefCounted<net::HttpResponseHeaders>(*raw);
  EXPECT_EQ("retained", result->GetNormalizedHeader("X-Existing-Interceptor"));
  EXPECT_EQ("added", result->GetNormalizedHeader("X-Application"));
  EXPECT_EQ(GURL("https://example.test/redirect#kept"), fragment);
  downstream.error = net::ERR_ACCESS_DENIED;
  base::test::TestFuture<int32_t, std::optional<std::string>,
                         std::optional<GURL>>
      failed;
  headers->OnHeadersReceived(
      original->raw_headers(), {}, std::nullopt,
      failed.GetCallback<int32_t, const std::optional<std::string>&,
                         const std::optional<GURL>&>());
  EXPECT_EQ(net::ERR_ACCESS_DENIED, std::get<0>(failed.Take()));
  EXPECT_EQ(1u, pending_.size());
}

TEST_F(ElectronWebRequestProxyTest,
       ResponseHeadersDoNotInspectOrRejectStreamingUpload) {
  Register("onHeadersReceived");
  auto* storage = ResolveElectronSessionPartition(&context_, "app", "");
  mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>
      header_remote;
  network::URLLoaderFactoryBuilder builder;
  ASSERT_TRUE(AppendElectronWebRequestProxy(&context_, storage, builder, true,
                                            &header_remote));
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> header_factory(
      std::move(header_remote));
  mojo::Remote<network::mojom::URLLoaderFactory> factory(
      std::move(builder)
          .Finish<mojo::PendingRemote<network::mojom::URLLoaderFactory>>(
              TargetRemote()));

  mojo::PendingRemote<network::mojom::DataPipeGetter> upload_remote;
  network::TestDataPipeGetter upload(
      "streaming body", upload_remote.InitWithNewPipeAndPassReceiver());
  network::ResourceRequest request;
  request.url = GURL("https://example.test/upload");
  request.method = "POST";
  request.request_body = base::MakeRefCounted<network::ResourceRequestBody>();
  request.request_body->AppendDataPipe(std::move(upload_remote));
  network::TestURLLoaderClient client;
  mojo::Remote<network::mojom::URLLoader> loader;
  factory->CreateLoaderAndStart(
      loader.BindNewPipeAndPassReceiver(), 42, 0, request,
      client.CreateRemote(),
      net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS));
  tasks_.RunUntilIdle();
  ASSERT_EQ(1, target_.NumPending());
  const auto* target = target_.GetPendingRequest(0);
  ASSERT_TRUE(target->request.request_body);
  ASSERT_EQ(1u, target->request.request_body->elements()->size());
  EXPECT_EQ(network::DataElement::Tag::kDataPipe,
            target->request.request_body->elements()->front().type());

  mojo::Remote<network::mojom::TrustedHeaderClient> headers;
  header_factory->OnLoaderCreated(target->request_id,
                                  headers.BindNewPipeAndPassReceiver());
  auto original =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 200 OK");
  base::test::TestFuture<int32_t, std::optional<std::string>,
                         std::optional<GURL>>
      future;
  headers->OnHeadersReceived(
      original->raw_headers(), {}, std::nullopt,
      future.GetCallback<int32_t, const std::optional<std::string>&,
                         const std::optional<GURL>&>());
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, pending_.size());
  EXPECT_FALSE(future.IsReady());
  const auto* details = pending_[0].request.GetDict().FindDict("details");
  ASSERT_TRUE(details);
  EXPECT_EQ("POST", *details->FindString("method"));
  EXPECT_FALSE(details->contains("uploadData"));
  Reply(0);
  EXPECT_EQ(net::OK, std::get<0>(future.Take()));
}
TEST_F(ElectronWebRequestProxyTest,
       RequestHeadersUseTrustedSnapshotAfterDownstreamAndRetainNetLog) {
  Register("onBeforeSendHeaders");
  DownstreamHeaders downstream;
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> header_factory;
  auto factory = NetworkFactory(&header_factory, &downstream);
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client, "https://example.test/source");
  auto headers = NetworkHeaders(header_factory.get());
  net::HttpRequestHeaders original;
  original.SetHeader("Cookie", "session=trusted");
  original.SetHeader("Authorization", "removed-by-application");
  RequestHeaderFuture future;
  SendHeaders(headers.get(), "https://example.test/source", original, &future);
  ASSERT_EQ(1u, pending_.size());
  EXPECT_FALSE(future.IsReady());
  EXPECT_EQ(42, downstream.original_id);
  const auto* observed =
      pending_[0].request.GetDict().FindDict("details")->FindDict(
          "requestHeaders");
  ASSERT_TRUE(observed);
  EXPECT_EQ("session=trusted", *observed->FindString("Cookie"));
  EXPECT_EQ("retained", *observed->FindString("X-Existing-Interceptor"));
  auto replacement = observed->Clone();
  replacement.Remove("Authorization");
  replacement.Set("x-application", "added");
  Reply(0, base::DictValue().Set("requestHeaders", std::move(replacement)));
  auto [error, result, net_log] = future.Take();
  EXPECT_EQ(net::OK, error);
  ASSERT_TRUE(result);
  EXPECT_EQ("session=trusted", result->GetHeader("Cookie"));
  EXPECT_EQ("retained", result->GetHeader("X-Existing-Interceptor"));
  EXPECT_EQ("added", result->GetHeader("X-Application"));
  EXPECT_FALSE(result->HasHeader("Authorization"));
  ASSERT_TRUE(net_log);
  EXPECT_EQ(true, net_log->FindBool("downstream"));
}

TEST_F(ElectronWebRequestProxyTest,
       RequestHeadersOmittedEmptyCancellationAndDownstreamFailure) {
  Register("onBeforeSendHeaders");
  DownstreamHeaders downstream;
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> header_factory;
  auto factory = NetworkFactory(&header_factory, &downstream);
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client, "https://example.test/source");
  auto headers = NetworkHeaders(header_factory.get());
  RequestHeaderFuture unchanged;
  SendHeaders(headers.get(), "https://example.test/source", {}, &unchanged);
  Reply(0);
  auto unchanged_result = unchanged.Take();
  ASSERT_TRUE(std::get<1>(unchanged_result));
  EXPECT_EQ("retained",
            std::get<1>(unchanged_result)->GetHeader("X-Existing-Interceptor"));
  RequestHeaderFuture cleared;
  SendHeaders(headers.get(), "https://example.test/source", {}, &cleared);
  Reply(1, base::DictValue().Set("requestHeaders", base::DictValue()));
  auto cleared_result = cleared.Take();
  ASSERT_TRUE(std::get<1>(cleared_result));
  EXPECT_TRUE(std::get<1>(cleared_result)->IsEmpty());
  RequestHeaderFuture cancelled;
  SendHeaders(headers.get(), "https://example.test/source", {}, &cancelled);
  Reply(2, base::DictValue().Set("cancel", true));
  EXPECT_EQ(net::ERR_BLOCKED_BY_CLIENT, std::get<0>(cancelled.Take()));
  RequestHeaderFuture malformed;
  SendHeaders(headers.get(), "https://example.test/source", {}, &malformed);
  Reply(3, base::DictValue().Set("requestHeaders",
                                 base::DictValue().Set("x-test", "bad\r\n")));
  EXPECT_EQ(net::ERR_INVALID_ARGUMENT, std::get<0>(malformed.Take()));
  downstream.error = net::ERR_ACCESS_DENIED;
  RequestHeaderFuture failed;
  SendHeaders(headers.get(), "https://example.test/source", {}, &failed);
  auto failed_result = failed.Take();
  EXPECT_EQ(net::ERR_ACCESS_DENIED, std::get<0>(failed_result));
  ASSERT_TRUE(std::get<2>(failed_result));
  EXPECT_EQ(true, std::get<2>(failed_result)->FindBool("downstream"));
  EXPECT_EQ(4u, pending_.size());
}

TEST_F(ElectronWebRequestProxyTest,
       RequestHeadersMatchTransactionUrlAndNeverReplayPreviousCredentials) {
  Register("onBeforeSendHeaders", "send", true, "",
           base::DictValue().Set(
               "urls", base::ListValue().Append("https://second.test/*")));
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> header_factory;
  auto factory = NetworkFactory(&header_factory);
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client, "https://first.test/source");
  auto headers = NetworkHeaders(header_factory.get());
  net::HttpRequestHeaders authenticated;
  authenticated.SetHeader("Authorization", "first-origin-only");
  RequestHeaderFuture first;
  SendHeaders(headers.get(), "https://first.test/source", authenticated,
              &first);
  EXPECT_EQ(net::OK, std::get<0>(first.Take()));
  EXPECT_TRUE(pending_.empty());
  RequestHeaderFuture second;
  SendHeaders(headers.get(), "https://second.test/destination", {}, &second);
  ASSERT_EQ(1u, pending_.size());
  const auto* details = pending_[0].request.GetDict().FindDict("details");
  EXPECT_EQ("https://second.test/destination", *details->FindString("url"));
  EXPECT_TRUE(details->FindDict("requestHeaders")->empty());
  Reply(0,
        base::DictValue().Set("requestHeaders",
                              base::DictValue().Set("authorization", "new")));
  EXPECT_EQ("new", std::get<1>(second.Take())->GetHeader("Authorization"));
  RequestHeaderFuture retried;
  SendHeaders(headers.get(), "https://second.test/retry", {}, &retried);
  ASSERT_EQ(2u, pending_.size());
  EXPECT_TRUE(pending_[1]
                  .request.GetDict()
                  .FindDict("details")
                  ->FindDict("requestHeaders")
                  ->empty());
  Reply(1);
  EXPECT_FALSE(std::get<1>(retried.Take()));
}

TEST_F(ElectronWebRequestProxyTest,
       PreflightRunsBeforeRequestThenActualRequestHeadersAndCanCancel) {
  Register("onBeforeRequest", "before");
  Register("onBeforeSendHeaders", "send");
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> header_factory;
  auto factory = NetworkFactory(&header_factory);
  network::ResourceRequest request;
  request.url = GURL("https://example.test/preflight");
  request.method = "OPTIONS";
  mojo::Remote<network::mojom::TrustedHeaderClient> headers;
  header_factory->OnLoaderForCorsPreflightCreated(
      request, headers.BindNewPipeAndPassReceiver());
  net::HttpRequestHeaders actual;
  actual.SetHeader("Access-Control-Request-Method", "PUT");
  RequestHeaderFuture future;
  SendHeaders(headers.get(), "https://example.test/preflight", actual, &future);
  ASSERT_EQ(1u, pending_.size());
  EXPECT_EQ("onBeforeRequest",
            *pending_[0].request.GetDict().FindString("event"));
  Reply(0);
  ASSERT_EQ(2u, pending_.size());
  EXPECT_EQ("onBeforeSendHeaders",
            *pending_[1].request.GetDict().FindString("event"));
  const auto* details = pending_[1].request.GetDict().FindDict("details");
  EXPECT_EQ("OPTIONS", *details->FindString("method"));
  EXPECT_EQ("PUT", *details->FindDict("requestHeaders")
                        ->FindString("Access-Control-Request-Method"));
  Reply(1, base::DictValue().Set("cancel", true));
  EXPECT_EQ(net::ERR_BLOCKED_BY_CLIENT, std::get<0>(future.Take()));
  EXPECT_EQ(0, target_calls_);
}

TEST_F(ElectronWebRequestProxyTest,
       NonNetworkRequestHeadersReallyReplaceAndCancellationPreventsLoad) {
  Register("onBeforeSendHeaders");
  auto factory = Factory();
  network::ResourceRequest request;
  request.url = GURL("file:///fixture/private.txt");
  request.headers.SetHeader("X-Original", "remove");
  request.cors_exempt_headers.SetHeader("X-Exempt", "keep-without-exemption");
  network::TestURLLoaderClient client;
  auto loader = BeginRequest(factory.get(), &client, request);
  ASSERT_EQ(1u, pending_.size());
  EXPECT_EQ(0, target_calls_);
  const auto* observed =
      pending_[0].request.GetDict().FindDict("details")->FindDict(
          "requestHeaders");
  EXPECT_EQ("keep-without-exemption", *observed->FindString("X-Exempt"));
  Reply(0, base::DictValue().Set("requestHeaders",
                                 base::DictValue()
                                     .Set("x-exempt", "keep-without-exemption")
                                     .Set("x-application", "added")));
  ASSERT_EQ(1, target_.NumPending());
  const auto& actual = target_.GetPendingRequest(0)->request;
  EXPECT_FALSE(actual.headers.HasHeader("X-Original"));
  EXPECT_EQ("added", actual.headers.GetHeader("X-Application"));
  EXPECT_EQ("keep-without-exemption", actual.headers.GetHeader("X-Exempt"));
  EXPECT_TRUE(actual.cors_exempt_headers.IsEmpty());
  network::TestURLLoaderClient denied;
  auto denied_loader = Begin(factory.get(), &denied);
  Reply(1, base::DictValue().Set("cancel", true));
  denied.RunUntilComplete();
  EXPECT_EQ(net::ERR_BLOCKED_BY_CLIENT, denied.completion_status().error_code);
  EXPECT_EQ(1, target_calls_);
}

TEST_F(ElectronWebRequestProxyTest,
       NonNetworkCrossOriginRedirectDropsOldHeaderCredentials) {
  Register("onBeforeSendHeaders", "send");
  auto factory = Factory();
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client, "custom://first.test/source");
  Reply(0, base::DictValue().Set("requestHeaders",
                                 base::DictValue()
                                     .Set("Authorization", "old-authorization")
                                     .Set("Cookie", "old-cookie")
                                     .Set("X-Kept", "kept")));
  Register("onHeadersReceived", "response");
  auto* target = target_.GetPendingRequest(0);
  auto head = network::mojom::URLResponseHead::New();
  head->headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 302 Found");
  head->headers->AddHeader("Location", "custom://second.test/destination");
  // A pre-parsed head keeps this fixture independent of the network service.
  head->parsed_headers = network::mojom::ParsedHeaders::New();
  target->client->OnReceiveResponse(std::move(head), {}, std::nullopt);
  tasks_.RunUntilIdle();
  Reply(1);
  client.RunUntilRedirectReceived();
  loader->FollowRedirect(network::HttpRequestHeadersUpdateParams(),
                         std::nullopt);
  tasks_.RunUntilIdle();
  ASSERT_EQ(3u, pending_.size());
  const auto* details = pending_[2].request.GetDict().FindDict("details");
  EXPECT_EQ("custom://second.test/destination", *details->FindString("url"));
  const auto* observed = details->FindDict("requestHeaders");
  EXPECT_FALSE(observed->contains("Authorization"));
  EXPECT_FALSE(observed->contains("Cookie"));
  EXPECT_EQ("kept", *observed->FindString("X-Kept"));
  Reply(2,
        base::DictValue().Set("requestHeaders",
                              base::DictValue().Set("authorization", "new")));
  EXPECT_EQ(2, target_calls_);
  EXPECT_EQ("new", target_.GetPendingRequest(1)->request.headers.GetHeader(
                       "Authorization"));
}

TEST_F(ElectronWebRequestProxyTest,
       NonNetworkExistingLoaderReceivesCompleteRedirectHeaderReplacement) {
  Register("onBeforeSendHeaders", "send");
  auto factory = Factory();
  network::ResourceRequest request;
  request.url = GURL("custom://first.test/source");
  request.headers.SetHeader("X-Old", "remove");
  request.headers.SetHeader("Authorization", "old-authorization");
  request.cors_exempt_headers.SetHeader("X-Exempt", "remove-exemption");
  request.cors_exempt_headers.SetHeader("Cookie", "old-cookie");
  network::TestURLLoaderClient client;
  auto loader = BeginRequest(factory.get(), &client, request);
  Reply(0);
  ASSERT_EQ(1, target_.NumPending());
  auto* target = target_.GetPendingRequest(0);
  net::RedirectInfo redirect;
  redirect.status_code = 302;
  redirect.new_method = "GET";
  redirect.new_url = GURL("custom://second.test/destination");
  auto head = network::mojom::URLResponseHead::New();
  head->headers =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 302 Found");
  head->headers->AddHeader("Location", redirect.new_url.spec());
  target->client->OnReceiveRedirect(redirect, std::move(head));
  client.RunUntilRedirectReceived();
  network::HttpRequestHeadersUpdateParams renderer_headers;
  renderer_headers.modified_headers.SetHeader("X-Renderer", "updated");
  loader->FollowRedirect(std::move(renderer_headers), std::nullopt);
  tasks_.RunUntilIdle();
  ASSERT_EQ(2u, pending_.size());
  EXPECT_TRUE(target->test_url_loader->follow_redirect_params().empty());
  const auto* observed =
      pending_[1].request.GetDict().FindDict("details")->FindDict(
          "requestHeaders");
  EXPECT_FALSE(observed->contains("Authorization"));
  EXPECT_FALSE(observed->contains("Cookie"));
  EXPECT_EQ("updated", *observed->FindString("X-Renderer"));
  Reply(1, base::DictValue().Set("requestHeaders",
                                 base::DictValue().Set("X-New", "new-value")));
  ASSERT_EQ(1u, target->test_url_loader->follow_redirect_params().size());
  const auto& update = target->test_url_loader->follow_redirect_params()[0]
                           .headers_update_params;
  EXPECT_EQ("new-value", update.modified_headers.GetHeader("X-New"));
  EXPECT_EQ(1u, update.modified_headers.GetHeaderVector().size());
  EXPECT_TRUE(update.modified_cors_exempt_headers.IsEmpty());
  for (const char* removed :
       {"X-Old", "X-Exempt", "X-Renderer", "Authorization", "Cookie"}) {
    EXPECT_NE(std::ranges::find(update.removed_headers, removed),
              update.removed_headers.end());
  }
  EXPECT_EQ(1, target_calls_);
}

TEST_F(ElectronWebRequestProxyTest,
       SessionTeardownAbortsPendingRequestHeaderCallback) {
  Register("onBeforeSendHeaders");
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> header_factory;
  auto factory = NetworkFactory(&header_factory);
  network::TestURLLoaderClient client;
  auto loader = Begin(factory.get(), &client, "https://example.test/source");
  auto headers = NetworkHeaders(header_factory.get());
  RequestHeaderFuture future;
  SendHeaders(headers.get(), "https://example.test/source", {}, &future);
  ASSERT_EQ(1u, pending_.size());
  RemoveElectronWebRequestHandlers("app");
  EXPECT_EQ(net::ERR_ABORTED, std::get<0>(future.Take()));
  EXPECT_EQ(1u, cancelled_.size());
  client.RunUntilComplete();
  EXPECT_EQ(net::ERR_ABORTED, client.completion_status().error_code);
  Reply(0, base::DictValue().Set("requestHeaders", base::DictValue()));
}

}  // namespace
}  // namespace xenon::ipc
