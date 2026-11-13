// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_net.h"

#include <vector>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/test/task_environment.h"
#include "mojo/core/embedder/embedder.h"
#include "net/base/auth.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_status_code.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/url_loader_network_service_observer.mojom.h"
#include "services/network/test/test_url_loader_factory.h"
#include "services/network/test/test_utils.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {
namespace {
class ElectronNetEngineTest : public testing::Test {
 public:
  static void SetUpTestSuite() { mojo::core::Init(); }

 protected:
  ElectronNetEngineTest()
      : engine_(base::BindRepeating(&ElectronNetEngineTest::Factory,
                                    base::Unretained(this)),
                base::BindRepeating(&ElectronNetEngineTest::Event,
                                    base::Unretained(this))) {}
  void Factory(
      const std::string& partition,
      mojo::PendingRemote<mojom::ElectronNetworkHeadersObserver> headers,
      ElectronNetEngine::FactoryCallback callback) {
    partitions_.push_back(partition);
    headers_.emplace_back(std::move(headers));
    mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
    target_.Clone(remote.InitWithNewPipeAndPassReceiver());
    std::move(callback).Run(std::move(remote), "");
  }
  void Event(base::Value event) { events_.push_back(std::move(event)); }
  uint64_t Start(base::DictValue options = {}) {
    if (!options.contains("url")) {
      options.Set("url", "https://net.test/resource");
    }
    std::string error;
    const uint64_t id = engine_.Start(options, &error);
    EXPECT_NE(0u, id) << error;
    tasks_.RunUntilIdle();
    return id;
  }
  void Control(uint64_t id, const std::string& command) {
    std::string error;
    EXPECT_TRUE(engine_.Control(id, command, &error)) << error;
    tasks_.RunUntilIdle();
  }
  std::string Type(size_t index) {
    return *events_.at(index).GetDict().FindString("type");
  }
  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  network::TestURLLoaderFactory target_{true};
  std::vector<std::string> partitions_;
  std::vector<mojo::Remote<mojom::ElectronNetworkHeadersObserver>> headers_;
  std::vector<base::Value> events_;
  ElectronNetEngine engine_;
};

TEST_F(ElectronNetEngineTest,
       ResponseStreamsWithOneChunkCreditAndNoFiveMiBLimit) {
  const uint64_t id = Start();
  const std::string expected(6 * 1024 * 1024 + 17, 'x');
  ASSERT_TRUE(target_.SimulateResponseForPendingRequest(
      "https://net.test/resource", expected));
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, events_.size());
  EXPECT_EQ("response", Type(0));
  std::string received;
  while (received.size() < expected.size()) {
    const size_t count = events_.size();
    Control(id, "resume");
    ASSERT_EQ(count + 1, events_.size());
    ASSERT_EQ("data", Type(count));
    std::string chunk;
    ASSERT_TRUE(base::Base64Decode(
        *events_.back().GetDict().FindString("bodyBase64"), &chunk));
    EXPECT_LE(chunk.size(), 64u * 1024u);
    received += chunk;
    tasks_.RunUntilIdle();
    EXPECT_EQ(count + 1, events_.size());
  }
  EXPECT_EQ(expected, received);
  Control(id, "resume");
  EXPECT_EQ("end", Type(events_.size() - 1));
}

TEST_F(ElectronNetEngineTest, SessionCredentialsCacheAndUploadReachFactory) {
  Start(base::DictValue()
            .Set("partition", "persist:app-two")
            .Set("method", "POST")
            .Set("credentials", "include")
            .Set("cache", "no-store")
            .Set("bodyBase64", "AP8=")
            .Set("headers", base::DictValue().Set("X-App", "yes")));
  ASSERT_EQ(1u, partitions_.size());
  EXPECT_EQ("persist:app-two", partitions_[0]);
  ASSERT_EQ(1, target_.NumPending());
  const auto& request = target_.GetPendingRequest(0)->request;
  EXPECT_EQ(network::mojom::CredentialsMode::kInclude,
            request.credentials_mode);
  EXPECT_EQ(std::string("\0\xff", 2), network::GetUploadData(request));
  EXPECT_TRUE(request.load_flags & net::LOAD_DISABLE_CACHE);
  EXPECT_FALSE(request.load_flags & net::LOAD_BYPASS_PROXY);
  EXPECT_EQ("yes", request.headers.GetHeader("X-App"));
}

TEST_F(ElectronNetEngineTest, DefaultAuthenticationIsSeparateFromCookiePolicy) {
  Start();
  auto* pending = target_.GetPendingRequest(0);
  EXPECT_EQ(network::mojom::CredentialsMode::kInclude,
            pending->request.credentials_mode);
  EXPECT_TRUE(pending->options & network::mojom::kURLLoadOptionBlockAllCookies);
  Start(base::DictValue().Set("useSessionCookies", true));
  pending = target_.GetPendingRequest(1);
  EXPECT_EQ(network::mojom::CredentialsMode::kInclude,
            pending->request.credentials_mode);
  EXPECT_FALSE(pending->options &
               network::mojom::kURLLoadOptionBlockAllCookies);
  Start(base::DictValue()
            .Set("credentials", "omit")
            .Set("useSessionCookies", true));
  pending = target_.GetPendingRequest(2);
  EXPECT_EQ(network::mojom::CredentialsMode::kOmit,
            pending->request.credentials_mode);
  EXPECT_TRUE(pending->options & network::mojom::kURLLoadOptionBlockAllCookies);
}

TEST_F(ElectronNetEngineTest, EmptyBodyAndHeadCompleteWithoutDataCredit) {
  Start(base::DictValue().Set("method", "HEAD"));
  auto* pending = target_.GetPendingRequest(0);
  pending->client->OnReceiveResponse(
      network::CreateURLResponseHead(net::HTTP_OK), {}, std::nullopt);
  pending->client->OnComplete(network::URLLoaderCompletionStatus(net::OK));
  tasks_.RunUntilIdle();
  ASSERT_EQ(2u, events_.size());
  EXPECT_EQ("response", Type(0));
  EXPECT_EQ("end", Type(1));
}

TEST_F(ElectronNetEngineTest, HeaderDuplicatesAndHttpFailureAreRealResponses) {
  const auto id = Start();
  auto head = network::CreateURLResponseHead(net::HTTP_NOT_FOUND);
  head->headers->AddHeader("Set-Cookie", "one=1");
  head->headers->AddHeader("Set-Cookie", "two=2");
  // Ordinary URLResponseHead serialization removes cookies; the trusted
  // metadata channel must deliver them before its acknowledgement is sent.
  headers_[0]->OnResponseHeaders(head->headers->raw_headers(),
                                 base::DoNothing());
  tasks_.RunUntilIdle();
  target_.SimulateResponseWithoutRemovingFromPendingList(
      target_.GetPendingRequest(0), std::move(head), "missing",
      network::URLLoaderCompletionStatus(net::OK));
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, events_.size());
  EXPECT_EQ(404, events_[0].GetDict().FindInt("statusCode"));
  const auto* cookies =
      events_[0].GetDict().FindDict("headers")->FindList("set-cookie");
  ASSERT_TRUE(cookies);
  ASSERT_EQ(2u, cookies->size());
  EXPECT_EQ("one=1", (*cookies)[0].GetString());
  EXPECT_EQ("two=2", (*cookies)[1].GetString());
  Control(id, "abort");
}

class AuthResponder final : public network::mojom::AuthChallengeResponder {
 public:
  mojo::PendingRemote<network::mojom::AuthChallengeResponder> Bind() {
    return receiver_.BindNewPipeAndPassRemote();
  }
  void OnAuthCredentials(
      const std::optional<net::AuthCredentials>& value) override {
    ++calls;
    credentials = value;
  }
  int calls = 0;
  std::optional<net::AuthCredentials> credentials;

 private:
  mojo::Receiver<network::mojom::AuthChallengeResponder> receiver_{this};
};
TEST_F(ElectronNetEngineTest,
       AuthWaitsForMatchingReplyAndRejectsStaleCallbacks) {
  auto id = Start();
  auto* pending = target_.GetPendingRequest(0);
  ASSERT_TRUE(pending->request.trusted_params);
  mojo::Remote<network::mojom::URLLoaderNetworkServiceObserver> observer(
      std::move(pending->request.trusted_params->url_loader_network_observer));
  net::AuthChallengeInfo info;
  info.scheme = "basic";
  info.realm = "fixture";
  info.challenger = url::SchemeHostPort(GURL("https://net.test:8443"));
  AuthResponder first, second;
  observer->OnAuthRequired(std::nullopt, 1, GURL("https://net.test/"), true,
                           info, nullptr, first.Bind());
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, events_.size());
  EXPECT_EQ("login", Type(0));
  EXPECT_EQ(0, first.calls);
  EXPECT_TRUE(engine_.Auth(id, 1, "name", "password"));
  tasks_.RunUntilIdle();
  ASSERT_TRUE(first.credentials);
  EXPECT_EQ(u"name", first.credentials->username());
  EXPECT_FALSE(engine_.Auth(id, 1, "stale", "stale"));
  observer->OnAuthRequired(std::nullopt, 1, GURL("https://net.test/"), false,
                           info, nullptr, second.Bind());
  tasks_.RunUntilIdle();
  EXPECT_FALSE(engine_.Auth(id, 1, "stale", "stale"));
  EXPECT_TRUE(engine_.Auth(id, 2, std::nullopt, std::nullopt));
  tasks_.RunUntilIdle();
  EXPECT_EQ(1, second.calls);
  EXPECT_FALSE(second.credentials);
  Control(id, "abort");
}
TEST_F(ElectronNetEngineTest, AbortingPendingAuthenticationCancelsResponder) {
  auto id = Start();
  auto* pending = target_.GetPendingRequest(0);
  mojo::Remote<network::mojom::URLLoaderNetworkServiceObserver> observer(
      std::move(pending->request.trusted_params->url_loader_network_observer));
  net::AuthChallengeInfo info;
  info.scheme = "basic";
  AuthResponder responder;
  observer->OnAuthRequired(std::nullopt, 1, GURL("https://net.test/"), true,
                           info, nullptr, responder.Bind());
  tasks_.RunUntilIdle();
  Control(id, "abort");
  EXPECT_EQ(1, responder.calls);
  EXPECT_FALSE(responder.credentials);
  EXPECT_FALSE(engine_.Auth(id, 1, "late", "late"));
}

TEST_F(ElectronNetEngineTest, AbortBeforeFactoryAndShutdownNeverIssueRequests) {
  std::string error;
  auto id =
      engine_.Start(base::DictValue().Set("url", "https://net.test/"), &error);
  ASSERT_NE(0u, id);
  EXPECT_TRUE(engine_.Control(id, "abort", &error));
  tasks_.RunUntilIdle();
  EXPECT_TRUE(partitions_.empty());
  EXPECT_TRUE(events_.empty());
  engine_.Start(base::DictValue().Set("url", "https://net.test/"), &error);
  engine_.Shutdown();
  tasks_.RunUntilIdle();
  EXPECT_TRUE(partitions_.empty());
}

TEST_F(ElectronNetEngineTest, AbortStopsActualLoaderAndDoesNotEmitSuccess) {
  const auto id = Start();
  ASSERT_TRUE(target_.IsPending("https://net.test/resource"));
  Control(id, "abort");
  EXPECT_FALSE(target_.IsPending("https://net.test/resource"));
  EXPECT_TRUE(events_.empty());
}

TEST_F(ElectronNetEngineTest,
       RedirectWaitsForExplicitFollowAndRemainsHttpOnly) {
  const auto id = Start();
  auto* pending = target_.GetPendingRequest(0);
  net::RedirectInfo redirect;
  redirect.status_code = 302;
  redirect.new_url = GURL("http://net.test/destination");
  redirect.new_method = "GET";
  pending->client->OnReceiveRedirect(
      redirect, network::CreateURLResponseHead(net::HTTP_FOUND));
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, events_.size());
  EXPECT_EQ("redirect", Type(0));
  EXPECT_TRUE(pending->test_url_loader->follow_redirect_params().empty());
  Control(id, "follow-redirect");
  EXPECT_EQ(1u, pending->test_url_loader->follow_redirect_params().size());
  redirect.new_url = GURL("file:///private");
  pending->client->OnReceiveRedirect(
      redirect, network::CreateURLResponseHead(net::HTTP_FOUND));
  tasks_.RunUntilIdle();
  ASSERT_EQ(2u, events_.size());
  EXPECT_EQ("ERR_NOT_IMPLEMENTED", *events_[1].GetDict().FindString("code"));
}

TEST_F(ElectronNetEngineTest, NetworkFailureAndExplicitTimeoutAreTerminal) {
  Start();
  target_.GetPendingRequest(0)->client->OnComplete(
      network::URLLoaderCompletionStatus(net::ERR_CONNECTION_RESET));
  tasks_.RunUntilIdle();
  ASSERT_EQ(1u, events_.size());
  EXPECT_EQ("ERR_CONNECTION_RESET", *events_[0].GetDict().FindString("code"));
  Start(base::DictValue().Set("timeoutMs", 50));
  tasks_.FastForwardBy(base::Milliseconds(51));
  ASSERT_EQ(2u, events_.size());
  EXPECT_EQ("ERR_TIMED_OUT", *events_[1].GetDict().FindString("code"));
}

TEST_F(ElectronNetEngineTest, UnsupportedAndInvalidInputNeverReachesFactory) {
  for (const auto& options :
       {base::DictValue().Set("url", "file:///private"),
        base::DictValue()
            .Set("url", "https://net.test/")
            .Set("cache", "invented"),
        base::DictValue()
            .Set("url", "https://net.test/")
            .Set("headers", base::DictValue().Set("X-Bad", "a\r\nb")),
        base::DictValue()
            .Set("url", "https://net.test/")
            .Set("bodyBase64",
                 std::string(((32 * 1024 * 1024 + 2) / 3) * 4 + 1, 'A'))}) {
    std::string error;
    EXPECT_EQ(0u, engine_.Start(options, &error));
    EXPECT_FALSE(error.empty());
  }
  tasks_.RunUntilIdle();
  EXPECT_TRUE(partitions_.empty());
}
}  // namespace
}  // namespace xenon::ipc
