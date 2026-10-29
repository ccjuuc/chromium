// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include "base/test/bind.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "net/base/features.h"
#include "net/base/net_error_details.h"
#include "net/base/privacy_mode.h"
#include "net/base/schemeful_site.h"
#include "net/base/test_completion_callback.h"
#include "net/gm/gm_tls_policy.h"
#include "net/http/http_network_session.h"
#include "net/http/http_network_transaction.h"
#include "net/http/http_request_info.h"
#include "net/http/http_stream_pool.h"
#include "net/http/http_transaction_test_util.h"
#include "net/proxy_resolution/configured_proxy_resolution_service.h"
#include "net/socket/client_socket_pool.h"
#include "net/socket/socket_test_util.h"
#include "net/spdy/spdy_test_util_common.h"
#include "net/ssl/test_ssl_config_service.h"
#include "net/ssl/test_static_ech_mode_getter.h"
#include "net/test/cert_test_util.h"
#include "net/test/ssl_test_util.h"
#include "net/test/test_data_directory.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net::gm {
namespace {

class RoutingSocket : public MockSSLClientSocket {
 public:
  RoutingSocket(std::unique_ptr<StreamSocket> transport,
                const HostPortPair& server,
                const SSLConfig& config,
                SSLSocketDataProvider* data,
                bool retry)
      : MockSSLClientSocket(std::move(transport), server, config, data),
        retry_(retry) {}
  void PopulateNetErrorDetails(NetErrorDetails* details) const override {
    details->gm_protocol_retry_allowed = retry_;
  }

 private:
  const bool retry_;
};

class RoutingSocketFactory : public MockClientSocketFactory {
 public:
  void AddAttempt(int result, bool retry, bool gm) {
    auto data = std::make_unique<SSLSocketDataProvider>(ASYNC, result);
    data->ssl_info.cert =
        ImportCertFromFile(GetTestCertsDirectory(), "ok_cert.pem");
    data->ssl_info.gm_tls = gm;
    data_.push_back(std::move(data));
    retry_.push_back(retry);
  }
  void SetLastConnectCallback(base::RepeatingClosure callback) {
    data_.back()->connect_callback = std::move(callback);
  }
  std::unique_ptr<SSLClientSocket> CreateSSLClientSocket(
      SSLClientContext*,
      std::unique_ptr<StreamSocket> transport,
      const HostPortPair& server,
      const SSLConfig& config) override {
    const size_t index = protocols.size();
    protocols.push_back(ProfileForRule(config.gm_rule).protocol());
    ech_configs.push_back(config.ech_config_list);
    alpns.push_back(config.alpn_protos);
    if (index >= data_.size()) {
      ADD_FAILURE() << "Unexpected additional TLS handshake";
      AddAttempt(ERR_UNEXPECTED, false, false);
    }
    return std::make_unique<RoutingSocket>(std::move(transport), server, config,
                                           data_[index].get(), retry_[index]);
  }
  std::vector<TlsProtocol> protocols;
  std::vector<std::vector<uint8_t>> ech_configs;
  std::vector<NextProtoVector> alpns;

 private:
  std::vector<std::unique_ptr<SSLSocketDataProvider>> data_;
  std::vector<bool> retry_;
};

class GmAutoRoutingTest : public testing::Test,
                          public testing::WithParamInterface<bool> {
 protected:
  void SetUp() override {
    feature_list_.InitWithFeatureState(features::kHappyEyeballsV3, GetParam());
    // The caching mock cannot represent different service/authority address
    // lists. Keep them distinct so ECH tests detect unsafe authority fallback.
    deps_.host_resolver = std::make_unique<MockHostResolver>(
        MockHostResolverBase::RuleResolver::GetLocalhostResult());
    AutoConfig automatic;
    automatic.enabled = true;
    automatic.epoch = 1;
    config_.gm_auto_config = std::make_shared<const AutoConfig>(automatic);
    auto service = std::make_unique<TestSSLConfigService>(config_);
    service_ = service.get();
    deps_.ssl_config_service = std::move(service);
    session_ = SpdySessionDependencies::SpdyCreateSessionWithSocketFactory(
        &deps_, &factory_);
  }
  void AddFailure(int error, bool retry, bool gm = false) {
    auto data = std::make_unique<StaticSocketDataProvider>();
    factory_.AddSocketDataProvider(data.get());
    transport_data_.push_back(std::move(data));
    factory_.AddAttempt(error, retry, gm);
  }
  void AddSuccess(bool gm) {
    // StaticSocketDataProvider borrows both arrays for the later asynchronous
    // transaction; they must outlive this setup helper.
    static const MockRead reads[] = {MockRead(
        ASYNC,
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")};
    static const MockWrite writes[] = {
        MockWrite(SYNCHRONOUS,
                  "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: "
                  "keep-alive\r\n\r\n")};
    auto transport = std::make_unique<StaticSocketDataProvider>(reads, writes);
    factory_.AddSocketDataProvider(transport.get());
    transport_data_.push_back(std::move(transport));
    factory_.AddAttempt(OK, false, gm);
  }
  int Start(HttpRequestInfo* request) {
    HttpNetworkTransaction transaction(DEFAULT_PRIORITY, session_.get());
    TestCompletionCallback completion;
    return completion.GetResult(
        transaction.Start(request, completion.callback(), NetLogWithSource()));
  }
  HttpRequestInfo Request() {
    HttpRequestInfo request;
    request.url = GURL("https://gm.example/");
    request.method = "GET";
    request.traffic_annotation =
        MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS);
    return request;
  }
  std::shared_ptr<const TlsRule> Rule(
      TlsProtocol protocol = TlsProtocol::kTlcp) {
    return config_.gm_auto_config->CreateRule("https", "gm.example", 443,
                                              protocol);
  }
  std::vector<uint8_t> SetEchEndpoints(bool h2_only = true,
                                       bool advertise_ech = true) {
    std::vector<uint8_t> ech_config;
    EXPECT_TRUE(MakeTestEchKeys("public.example", 128, &ech_config));
    HostResolverEndpointResult ech_endpoint, authority;
    ech_endpoint.ip_endpoints = {IPEndPoint(IPAddress(192, 0, 2, 1), 443)};
    ech_endpoint.metadata.supported_protocol_alpns =
        h2_only ? std::vector<std::string>{"h2"}
                : std::vector<std::string>{"http/1.1"};
    if (advertise_ech) {
      ech_endpoint.metadata.ech_config_list = ech_config;
    }
    authority.ip_endpoints = {IPEndPoint(IPAddress(192, 0, 2, 2), 443)};
    deps_.host_resolver->rules()->ClearRules();
    deps_.host_resolver->rules()->AddRule(
        "gm.example", MockHostResolverBase::RuleResolver::RuleResult(
                          std::vector{ech_endpoint, authority}));
    return ech_config;
  }
  void AddEchTransport(int result = OK) {
    auto transport = std::make_unique<StaticSocketDataProvider>();
    transport->set_connect_data(MockConnect(ASYNC, result));
    transport->set_expected_addresses(
        AddressList(IPEndPoint(IPAddress(192, 0, 2, 1), 443)));
    factory_.AddSocketDataProvider(transport.get());
    transport_data_.push_back(std::move(transport));
  }
  NetErrorDetails ConnectWithRealBoringSsl(base::span<const MockRead> reads) {
    StaticSocketDataProvider data(reads, base::span<const MockWrite>());
    data.set_connect_data(MockConnect(SYNCHRONOUS, OK));
    auto transport =
        std::make_unique<MockTCPClientSocket>(AddressList(), nullptr, &data);
    TestCompletionCallback completion;
    EXPECT_EQ(completion.GetResult(transport->Connect(completion.callback())),
              OK);
    auto socket = session_->ssl_client_context()->CreateSSLClientSocket(
        std::move(transport), HostPortPair("gm.example", 443), SSLConfig());
    const int result =
        completion.GetResult(socket->Connect(completion.callback()));
    EXPECT_TRUE(result == ERR_SSL_PROTOCOL_ERROR ||
                result == ERR_SSL_VERSION_OR_CIPHER_MISMATCH)
        << result;
    NetErrorDetails details;
    socket->PopulateNetErrorDetails(&details);
    return details;
  }
  void BenchmarkOrdinaryRequests(bool automatic_enabled, bool full_hint_table) {
    auto automatic = std::make_shared<AutoConfig>(*config_.gm_auto_config);
    automatic->enabled = automatic_enabled;
    config_.gm_auto_config = automatic;
    service_->UpdateSSLConfigAndNotify(config_);
    if (full_hint_table) {
      for (int i = 0; i < 256; ++i) {
        const std::string host = "hint-" + std::to_string(i) + ".example";
        session_->RememberGmCapability(
            url::SchemeHostPort("https", host, 443), {}, PRIVACY_MODE_DISABLED,
            ProxyChain::Direct(),
            automatic->CreateRule("https", host, 443, TlsProtocol::kRfc8998));
      }
      EXPECT_TRUE(session_->HasGmCapabilityHint(
          url::SchemeHostPort("https", "hint-255.example", 443), {},
          PRIVACY_MODE_DISABLED));
    }
    constexpr size_t kRequests = 200;
    MockRead reads[] = {MockRead(
        ASYNC,
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")};
    MockWrite writes[] = {
        MockWrite(SYNCHRONOUS,
                  "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: "
                  "keep-alive\r\n\r\n")};
    for (size_t i = 0; i < kRequests; ++i) {
      auto transport =
          std::make_unique<StaticSocketDataProvider>(reads, writes);
      factory_.AddSocketDataProvider(transport.get());
      transport_data_.push_back(std::move(transport));
      factory_.AddAttempt(OK, false, false);
    }
    auto request = Request();
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < kRequests; ++i) {
      EXPECT_EQ(Start(&request), OK);
      EXPECT_FALSE(session_->HasGmCapabilityHint(
          url::SchemeHostPort(request.url), {}, PRIVACY_MODE_DISABLED));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - begin)
                             .count();
    EXPECT_EQ(factory_.protocols.size(), kRequests);
    for (auto protocol : factory_.protocols) {
      EXPECT_EQ(protocol, TlsProtocol::kStandardTls);
    }
    // Simulated sockets measure routing/transaction overhead, not cryptographic
    // CPU or public-network latency. Timing is informational, never a pass bar.
    std::printf(
        "GM_AUTO_BENCH auto=%d hints=%d pool=%s requests=%zu "
        "handshakes=%zu elapsed_us=%lld\n",
        automatic_enabled, full_hint_table ? 256 : 0,
        GetParam() ? "stream" : "legacy", kRequests, factory_.protocols.size(),
        static_cast<long long>(elapsed));
  }
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::test::ScopedFeatureList feature_list_;
  SpdySessionDependencies deps_;
  RoutingSocketFactory factory_;
  std::vector<std::unique_ptr<StaticSocketDataProvider>> transport_data_;
  SSLContextConfig config_;
  raw_ptr<TestSSLConfigService> service_ = nullptr;
  std::unique_ptr<HttpNetworkSession> session_;
};

TEST_P(GmAutoRoutingTest, OrdinarySuccessUsesOneHandshake) {
  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
  MockWrite writes[] = {MockWrite(
      SYNCHRONOUS,
      "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: keep-alive\r\n\r\n")};
  StaticSocketDataProvider transport(reads, writes);
  factory_.AddSocketDataProvider(&transport);
  factory_.AddAttempt(OK, false, false);
  auto request = Request();
  EXPECT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kStandardTls});
  EXPECT_FALSE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                             {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, ExactMismatchTriesEachConcreteProtocolOnce) {
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true);
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true, true);
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true, true);
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_SSL_VERSION_OR_CIPHER_MISMATCH);
  EXPECT_EQ(
      factory_.protocols,
      (std::vector<TlsProtocol>{TlsProtocol::kStandardTls, TlsProtocol::kTlcp,
                                TlsProtocol::kRfc8998}));
  EXPECT_FALSE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                             {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, OrdinaryHandshakeSurvivesAutoConfigRefresh) {
  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
  MockWrite writes[] = {MockWrite(
      SYNCHRONOUS,
      "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: keep-alive\r\n\r\n")};
  StaticSocketDataProvider transport(reads, writes);
  factory_.AddSocketDataProvider(&transport);
  factory_.AddAttempt(OK, false, false);
  factory_.SetLastConnectCallback(base::BindLambdaForTesting([&] {
    config_.gm_auto_config = config_.gm_auto_config->WithEpoch(2);
    service_->UpdateSSLConfigAndNotify(config_);
  }));
  auto request = Request();
  EXPECT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols.size(), 1u);
}

TEST_P(GmAutoRoutingTest,
       SuccessfulDiscoveryRestoresCallerHeadersAndLearnsRoute) {
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true);
  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
  MockWrite writes[] = {MockWrite(
      SYNCHRONOUS,
      "GET /original HTTP/1.1\r\nHost: gm.example\r\nConnection: keep-alive\r\n"
      "X-Caller: preserved\r\n\r\n")};
  StaticSocketDataProvider transport(reads, writes);
  factory_.AddSocketDataProvider(&transport);
  factory_.AddAttempt(OK, false, true);
  auto request = Request();
  request.extra_headers.SetHeader("If-None-Match", "ordinary-cache-validator");
  request.gm_cache_original_headers.emplace();
  request.gm_cache_original_headers->SetHeader("X-Caller", "preserved");
  request.gm_cache_original_url = GURL("https://gm.example/original");
  EXPECT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols,
            (std::vector<TlsProtocol>{TlsProtocol::kStandardTls,
                                      TlsProtocol::kTlcp}));
  const auto hint = session_->GetGmCapabilityHint(
      url::SchemeHostPort(request.url), {}, PRIVACY_MODE_DISABLED,
      ProxyChain::Direct());
  ASSERT_TRUE(hint);
  EXPECT_EQ(hint->profile.protocol(), TlsProtocol::kTlcp);
  EXPECT_TRUE(transport.AllWriteDataConsumed());
}

TEST_P(GmAutoRoutingTest, LearnedRfcRouteUsesJointHandshake) {
  const auto request = Request();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule(TlsProtocol::kRfc8998));
  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
  MockWrite writes[] = {MockWrite(
      SYNCHRONOUS,
      "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: keep-alive\r\n\r\n")};
  StaticSocketDataProvider transport(reads, writes);
  factory_.AddSocketDataProvider(&transport);
  factory_.AddAttempt(OK, false, true);
  auto second_request = request;
  EXPECT_EQ(Start(&second_request), OK);
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kTls13Auto});
}

TEST_P(GmAutoRoutingTest,
       VerifiedRfcDiscoveryPromotesOnlySubsequentConnections) {
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true);
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true, true);
  AddSuccess(true);
  auto request = Request();
  ASSERT_EQ(Start(&request), OK);
  EXPECT_EQ(
      factory_.protocols,
      (std::vector<TlsProtocol>{TlsProtocol::kStandardTls, TlsProtocol::kTlcp,
                                TlsProtocol::kRfc8998}));
  const auto hint = session_->GetGmCapabilityHint(
      url::SchemeHostPort(request.url), {}, PRIVACY_MODE_DISABLED,
      ProxyChain::Direct());
  ASSERT_TRUE(hint);
  EXPECT_EQ(hint->profile.protocol(), TlsProtocol::kTls13Auto);
  EXPECT_TRUE(
      config_.gm_auto_config->MatchesRule("https", "gm.example", 443, *hint));

  task_environment_.FastForwardBy(base::Minutes(5));
  AddSuccess(true);
  ASSERT_EQ(Start(&request), OK);
  ASSERT_EQ(factory_.protocols.size(), 4u);
  EXPECT_EQ(factory_.protocols.back(), TlsProtocol::kTls13Auto);
  task_environment_.FastForwardBy(base::Minutes(5));
  EXPECT_TRUE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                            {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, RfcCertificateFailureCannotPromoteRoute) {
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true);
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true, true);
  AddFailure(ERR_CERT_AUTHORITY_INVALID, true, true);
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_CERT_AUTHORITY_INVALID);
  EXPECT_EQ(
      factory_.protocols,
      (std::vector<TlsProtocol>{TlsProtocol::kStandardTls, TlsProtocol::kTlcp,
                                TlsProtocol::kRfc8998}));
  EXPECT_FALSE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                             {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, JointOrdinaryResultDoesNotRefreshRfcEvidence) {
  auto request = Request();
  const url::SchemeHostPort origin(request.url);
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(),
                                 Rule(TlsProtocol::kRfc8998));
  task_environment_.FastForwardBy(base::Minutes(5));
  AddSuccess(false);
  ASSERT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kTls13Auto});
  // Successful ordinary negotiation must neither reconnect nor refresh the
  // ten-minute lifetime of the earlier verified RFC 8998 observation.
  EXPECT_TRUE(session_->HasGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED));
  task_environment_.FastForwardBy(base::Minutes(5));
  EXPECT_FALSE(
      session_->HasGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED));
  AddSuccess(false);
  ASSERT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols,
            (std::vector<TlsProtocol>{TlsProtocol::kTls13Auto,
                                      TlsProtocol::kStandardTls}));
  EXPECT_FALSE(
      session_->HasGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, JointOrdinaryResultCannotCreateRfcEvidence) {
  auto request = Request();
  request.gm_auto_rule = Rule(TlsProtocol::kTls13Auto);
  request.gm_auto_proxy_chain = ProxyChain::Direct();
  AddSuccess(false);
  ASSERT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kTls13Auto});
  EXPECT_FALSE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                             {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, JointMismatchDoesNotRepeatStrictRfcHandshake) {
  auto request = Request();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule(TlsProtocol::kRfc8998));
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true);
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true);
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true, true);
  EXPECT_EQ(Start(&request), ERR_SSL_VERSION_OR_CIPHER_MISMATCH);
  EXPECT_EQ(factory_.protocols,
            (std::vector<TlsProtocol>{TlsProtocol::kTls13Auto,
                                      TlsProtocol::kStandardTls,
                                      TlsProtocol::kTlcp}));
  EXPECT_FALSE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                             {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, PromotionRejectsStaleOrAlteredSystemTrust) {
  const url::SchemeHostPort origin("https", "gm.example", 443);
  const auto stale = Rule(TlsProtocol::kRfc8998);
  auto automatic = std::make_shared<AutoConfig>(*config_.gm_auto_config);
  automatic->epoch = 2;
  automatic->trust_anchors = {{0x30, 1}};
  automatic->untrusted_intermediates = {{0x30, 2}};
  automatic->disallowed_certificates = {{0x30, 3}};
  automatic->crls = {{0x30, 4}};
  config_.gm_auto_config = automatic;
  service_->UpdateSSLConfigAndNotify(config_);
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), stale);
  EXPECT_FALSE(
      session_->HasGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED));
  auto altered = std::make_shared<TlsRule>(*Rule(TlsProtocol::kRfc8998));
  altered->disallowed_certificates.clear();
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), altered);
  EXPECT_FALSE(
      session_->HasGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED));
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(),
                                 Rule(TlsProtocol::kRfc8998));
  const auto hint = session_->GetGmCapabilityHint(
      origin, {}, PRIVACY_MODE_DISABLED, ProxyChain::Direct());
  ASSERT_TRUE(hint);
  EXPECT_EQ(hint->profile.protocol(), TlsProtocol::kTls13Auto);
  EXPECT_TRUE(automatic->MatchesRule("https", "gm.example", 443, *hint));
}

TEST_P(GmAutoRoutingTest, LearnedRfcRouteKeepsEchInOneHandshake) {
  config_.ech_enabled = true;
  service_->UpdateSSLConfigAndNotify(config_);
  const auto ech_config = SetEchEndpoints(/*h2_only=*/false);
  auto request = Request();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule(TlsProtocol::kRfc8998));
  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
  MockWrite writes[] = {MockWrite(
      SYNCHRONOUS,
      "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: keep-alive\r\n\r\n")};
  StaticSocketDataProvider transport(reads, writes);
  transport.set_expected_addresses(
      AddressList(IPEndPoint(IPAddress(192, 0, 2, 1), 443)));
  factory_.AddSocketDataProvider(&transport);
  factory_.AddAttempt(OK, false, false);
  ASSERT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kTls13Auto});
  ASSERT_EQ(factory_.ech_configs.size(), 1u);
  EXPECT_EQ(factory_.ech_configs[0], ech_config);
}

TEST_P(GmAutoRoutingTest, StaleHintCanRecoverToOrdinaryTlsOnce) {
  auto request = Request();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule());
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true, true);
  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
  MockWrite writes[] = {MockWrite(
      SYNCHRONOUS,
      "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: keep-alive\r\n\r\n")};
  StaticSocketDataProvider transport(reads, writes);
  factory_.AddSocketDataProvider(&transport);
  factory_.AddAttempt(OK, false, false);
  EXPECT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols,
            (std::vector<TlsProtocol>{TlsProtocol::kTlcp,
                                      TlsProtocol::kStandardTls}));
  EXPECT_FALSE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                             {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, ExplicitStandardRetryIgnoresConcurrentRelearnedHint) {
  auto request = Request();
  // Another request repopulated the hint after this request selected its
  // standard retry. The concrete standard candidate must stay selected.
  request.gm_auto_proxy_chain = ProxyChain::Direct();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule());
  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
  MockWrite writes[] = {MockWrite(
      SYNCHRONOUS,
      "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: keep-alive\r\n\r\n")};
  StaticSocketDataProvider transport(reads, writes);
  factory_.AddSocketDataProvider(&transport);
  factory_.AddAttempt(OK, false, false);
  EXPECT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kStandardTls});
}

TEST_P(GmAutoRoutingTest, LearnedHintFreshEchReselectsStandardBeforeGmSocket) {
  config_.ech_enabled = true;
  service_->UpdateSSLConfigAndNotify(config_);
  const auto ech_config = SetEchEndpoints();
  auto request = Request();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule());
  // TCP for the learned GM route completes, but no GM SSL socket is created.
  // The h2-only ECH endpoint must remain reachable despite TLCP using h1.
  AddEchTransport();
  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")};
  MockWrite writes[] = {MockWrite(
      SYNCHRONOUS,
      "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: keep-alive\r\n\r\n")};
  StaticSocketDataProvider standard_transport(reads, writes);
  standard_transport.set_expected_addresses(
      AddressList(IPEndPoint(IPAddress(192, 0, 2, 1), 443)));
  factory_.AddSocketDataProvider(&standard_transport);
  factory_.AddAttempt(OK, false, false);
  EXPECT_EQ(Start(&request), OK);
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kStandardTls});
  ASSERT_EQ(factory_.ech_configs.size(), 1u);
  EXPECT_EQ(factory_.ech_configs[0], ech_config);
  EXPECT_TRUE(standard_transport.AllWriteDataConsumed());
  EXPECT_FALSE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                             {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, LearnedHintEchCannotFallBackToAuthorityAddress) {
  config_.ech_enabled = true;
  service_->UpdateSSLConfigAndNotify(config_);
  SetEchEndpoints();
  auto request = Request();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule());
  AddEchTransport(ERR_CONNECTION_REFUSED);
  EXPECT_EQ(Start(&request), ERR_CONNECTION_REFUSED);
  EXPECT_TRUE(factory_.protocols.empty());
  EXPECT_FALSE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                             {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, FreshEchCannotRetryStandardTwiceOrProbeAnotherGm) {
  config_.ech_enabled = true;
  service_->UpdateSSLConfigAndNotify(config_);
  // DNS changes after the first ordinary protocol mismatch. The resulting GM
  // route must stop before creating its socket, and cannot try Standard twice.
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true);
  std::vector<uint8_t> fresh_ech;
  factory_.SetLastConnectCallback(
      base::BindLambdaForTesting([&] { fresh_ech = SetEchEndpoints(); }));
  AddEchTransport();
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_ABORTED);
  EXPECT_FALSE(fresh_ech.empty());
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kStandardTls});
  EXPECT_FALSE(session_->HasGmCapabilityHint(url::SchemeHostPort(request.url),
                                             {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest, StandardReselectionEchCertificateFailureIsFinal) {
  config_.ech_enabled = true;
  service_->UpdateSSLConfigAndNotify(config_);
  const auto ech_config = SetEchEndpoints();
  auto request = Request();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule());
  AddEchTransport();
  AddEchTransport();
  factory_.AddAttempt(ERR_CERT_AUTHORITY_INVALID, false, false);
  EXPECT_EQ(Start(&request), ERR_CERT_AUTHORITY_INVALID);
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kStandardTls});
  ASSERT_EQ(factory_.ech_configs.size(), 1u);
  EXPECT_EQ(factory_.ech_configs[0], ech_config);
}

TEST_P(GmAutoRoutingTest, GenericAbortCannotReselectTls) {
  AddFailure(ERR_ABORTED, false);
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_ABORTED);
  EXPECT_EQ(factory_.protocols,
            std::vector<TlsProtocol>{TlsProtocol::kStandardTls});
}

TEST_P(GmAutoRoutingTest, ManagedGmKeepsExplicitEchAndAuthorityBehavior) {
  TlsRule explicit_rule = *Rule();
  explicit_rule.automatic = false;
  explicit_rule.trust_anchors = {{0x30, 0x00}};
  config_.gm_tls_policy =
      std::make_shared<const TlsPolicy>(std::vector<TlsRule>{explicit_rule});
  config_.ech_enabled = true;
  service_->UpdateSSLConfigAndNotify(config_);
  SetEchEndpoints();
  // Managed TLCP keeps h1-only DNS selection and its explicit ECH override.
  // Thus it uses the authority endpoint when the HTTPS endpoint is h2-only.
  auto transport = std::make_unique<StaticSocketDataProvider>();
  transport->set_expected_addresses(
      AddressList(IPEndPoint(IPAddress(192, 0, 2, 2), 443)));
  factory_.AddSocketDataProvider(transport.get());
  transport_data_.push_back(std::move(transport));
  factory_.AddAttempt(ERR_CERT_AUTHORITY_INVALID, false, true);
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_CERT_AUTHORITY_INVALID);
  EXPECT_EQ(factory_.protocols, std::vector<TlsProtocol>{TlsProtocol::kTlcp});
  ASSERT_EQ(factory_.ech_configs.size(), 1u);
  EXPECT_TRUE(factory_.ech_configs[0].empty());
  EXPECT_EQ(factory_.alpns[0], NextProtoVector{NextProto::kProtoHTTP11});
}

TEST_P(GmAutoRoutingTest, AutomaticGmRespectsHostEchDisabledSetting) {
  config_.ech_enabled = true;
  service_->UpdateSSLConfigAndNotify(config_);
  service_->SetEchModeGetter(std::make_unique<TestStaticEchModeGetter>(
      EchMode::kDisabled, "gm.example"));
  SetEchEndpoints();
  auto request = Request();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule());
  auto transport = std::make_unique<StaticSocketDataProvider>();
  transport->set_expected_addresses(
      AddressList(IPEndPoint(IPAddress(192, 0, 2, 2), 443)));
  factory_.AddSocketDataProvider(transport.get());
  transport_data_.push_back(std::move(transport));
  factory_.AddAttempt(ERR_CERT_AUTHORITY_INVALID, false, true);
  EXPECT_EQ(Start(&request), ERR_CERT_AUTHORITY_INVALID);
  EXPECT_EQ(factory_.protocols, std::vector<TlsProtocol>{TlsProtocol::kTlcp});
  ASSERT_EQ(factory_.ech_configs.size(), 1u);
  EXPECT_TRUE(factory_.ech_configs[0].empty());
  EXPECT_EQ(factory_.alpns[0], NextProtoVector{NextProto::kProtoHTTP11});
}

TEST_P(GmAutoRoutingTest, AutomaticGmNonEchH2OnlyEndpointUsesAuthority) {
  config_.ech_enabled = true;
  service_->UpdateSSLConfigAndNotify(config_);
  SetEchEndpoints(/*h2_only=*/true, /*advertise_ech=*/false);
  auto request = Request();
  session_->RememberGmCapability(url::SchemeHostPort(request.url), {},
                                 PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                 Rule());
  // Without effective ECH there is no reason to reselect Standard TLS. Keep
  // the concrete TLCP h1 filter, skip the h2-only service, and use authority.
  auto transport = std::make_unique<StaticSocketDataProvider>();
  transport->set_expected_addresses(
      AddressList(IPEndPoint(IPAddress(192, 0, 2, 2), 443)));
  factory_.AddSocketDataProvider(transport.get());
  transport_data_.push_back(std::move(transport));
  factory_.AddAttempt(ERR_CERT_AUTHORITY_INVALID, false, true);
  EXPECT_EQ(Start(&request), ERR_CERT_AUTHORITY_INVALID);
  EXPECT_EQ(factory_.protocols, std::vector<TlsProtocol>{TlsProtocol::kTlcp});
  ASSERT_EQ(factory_.ech_configs.size(), 1u);
  EXPECT_TRUE(factory_.ech_configs[0].empty());
  EXPECT_EQ(factory_.alpns[0], NextProtoVector{NextProto::kProtoHTTP11});
}

TEST_P(GmAutoRoutingTest,
       WrongVersionRequiresCompleteTlcpProtocolVersionAlert) {
  struct Case {
    std::vector<uint8_t> bytes;
    bool retry;
  };
  const Case cases[] = {
      {{0x15, 0x01, 0x01, 0x00, 0x02, 0x02, 0x46}, true},
      {{0x15, 0x03, 0x03, 0x00, 0x02, 0x02, 0x46}, true},
      {{'g', 'a', 'r', 'b', 'a', 'g', 'e'}, false},
      {{0x15, 0x01, 0x01, 0x00, 0x03, 0x02, 0x46}, false},
      {{0x15, 0x01, 0x01, 0x00, 0x02, 0x01, 0x46}, false},
      {{0x15, 0x01, 0x01, 0x00, 0x02, 0x02, 0x28}, false},
      {{0x16, 0x01, 0x01, 0x00, 0x02, 0x02, 0x46}, false},
      {{0x15, 0x01, 0x01, 0x00, 0x02}, false},
  };
  size_t case_index = 0;
  for (const auto& test_case : cases) {
    SCOPED_TRACE(case_index++);
    MockRead reads[] = {MockRead(SYNCHRONOUS, base::span(test_case.bytes)),
                        MockRead(SYNCHRONOUS, OK)};
    EXPECT_EQ(ConnectWithRealBoringSsl(reads).gm_protocol_retry_allowed,
              test_case.retry);
  }
}

TEST_P(GmAutoRoutingTest,
       SplitTlcpHeaderDoesNotAuthorizeRetryWithoutAlertBody) {
  static constexpr uint8_t kHeader[] = {0x15, 0x01, 0x01, 0x00, 0x02};
  static constexpr uint8_t kBody[] = {0x02, 0x46};
  MockRead reads[] = {MockRead(SYNCHRONOUS, base::span(kHeader)),
                      MockRead(SYNCHRONOUS, base::span(kBody))};
  EXPECT_FALSE(ConnectWithRealBoringSsl(reads).gm_protocol_retry_allowed);
}

TEST_P(GmAutoRoutingTest, TlcpPrefixCaptureHandlesPartialInitialRead) {
  static constexpr uint8_t kPrefix[] = {0x15, 0x01};
  static constexpr uint8_t kRemainder[] = {0x01, 0x00, 0x02, 0x02, 0x46};
  MockRead reads[] = {MockRead(ASYNC, base::span(kPrefix)),
                      MockRead(ASYNC, base::span(kRemainder))};
  EXPECT_TRUE(ConnectWithRealBoringSsl(reads).gm_protocol_retry_allowed);
}

TEST_P(GmAutoRoutingTest, DisabledAutoDoesNotCaptureTlcpRecord) {
  auto automatic = std::make_shared<AutoConfig>(*config_.gm_auto_config);
  automatic->enabled = false;
  config_.gm_auto_config = automatic;
  service_->UpdateSSLConfigAndNotify(config_);
  static constexpr uint8_t kAlert[] = {0x15, 0x01, 0x01, 0x00,
                                       0x02, 0x02, 0x46};
  MockRead reads[] = {MockRead(SYNCHRONOUS, base::span(kAlert))};
  EXPECT_FALSE(ConnectWithRealBoringSsl(reads).gm_protocol_retry_allowed);
}

TEST_P(GmAutoRoutingTest, GenericGmFailureNeverTriesAnotherProtocol) {
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true);
  AddFailure(ERR_SSL_PROTOCOL_ERROR, false, true);
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_SSL_PROTOCOL_ERROR);
  EXPECT_EQ(factory_.protocols,
            (std::vector<TlsProtocol>{TlsProtocol::kStandardTls,
                                      TlsProtocol::kTlcp}));
}

TEST_P(GmAutoRoutingTest, TcpFailureNeverProbesGm) {
  StaticSocketDataProvider transport;
  transport.set_connect_data(MockConnect(ASYNC, ERR_CONNECTION_REFUSED));
  factory_.AddSocketDataProvider(&transport);
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_CONNECTION_REFUSED);
  EXPECT_TRUE(factory_.protocols.empty());
}

TEST_P(GmAutoRoutingTest, GenericStandardTlsFailureNeverProbesGm) {
  AddFailure(ERR_SSL_PROTOCOL_ERROR, false);
  if (!GetParam()) {
    // Preserve the upstream legacy socket pool's one same-protocol retry.
    AddFailure(ERR_SSL_PROTOCOL_ERROR, false);
  }
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_SSL_PROTOCOL_ERROR);
  EXPECT_EQ(factory_.protocols.size(), GetParam() ? 1u : 2u);
  for (auto protocol : factory_.protocols) {
    EXPECT_EQ(protocol, TlsProtocol::kStandardTls);
  }
}

TEST_P(GmAutoRoutingTest, CertificateErrorCannotTriggerProtocolRetry) {
  AddFailure(ERR_CERT_AUTHORITY_INVALID, true);
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_CERT_AUTHORITY_INVALID);
  EXPECT_EQ(factory_.protocols.size(), 1u);
}

TEST_P(GmAutoRoutingTest,
       LearnedGmCertificateErrorClearsHintWithoutProxyCallback) {
  auto request = Request();
  const url::SchemeHostPort origin(request.url);
  const auto other_proxy = ProxyChain::FromSchemeHostAndPort(
      ProxyServer::SCHEME_HTTP, "other-proxy.example", 8080);
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), Rule());
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED, other_proxy,
                                 Rule());
  AddFailure(ERR_CERT_AUTHORITY_INVALID, true, true);
  EXPECT_EQ(Start(&request), ERR_CERT_AUTHORITY_INVALID);
  EXPECT_EQ(factory_.protocols, std::vector<TlsProtocol>{TlsProtocol::kTlcp});
  EXPECT_FALSE(session_->GetGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED,
                                             ProxyChain::Direct()));
  EXPECT_TRUE(session_->GetGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED,
                                            other_proxy));
}

TEST_P(GmAutoRoutingTest,
       LearnedGmClientAuthRequestClearsHintWithoutProtocolRetry) {
  auto request = Request();
  const url::SchemeHostPort origin(request.url);
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), Rule());
  AddFailure(ERR_SSL_CLIENT_AUTH_CERT_NEEDED, true, true);
  EXPECT_EQ(Start(&request), ERR_SSL_CLIENT_AUTH_CERT_NEEDED);
  EXPECT_EQ(factory_.protocols, std::vector<TlsProtocol>{TlsProtocol::kTlcp});
  EXPECT_FALSE(session_->GetGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED,
                                             ProxyChain::Direct()));
}

TEST_P(GmAutoRoutingTest, ExplicitRuleNeverFallsBack) {
  TlsRule explicit_rule = *Rule();
  explicit_rule.automatic = false;
  explicit_rule.trust_anchors = {{0x30, 0x00}};
  config_.gm_tls_policy =
      std::make_shared<const TlsPolicy>(std::vector<TlsRule>{explicit_rule});
  service_->UpdateSSLConfigAndNotify(config_);
  AddFailure(ERR_SSL_VERSION_OR_CIPHER_MISMATCH, true, true);
  auto request = Request();
  EXPECT_EQ(Start(&request), ERR_SSL_VERSION_OR_CIPHER_MISMATCH);
  EXPECT_EQ(factory_.protocols, std::vector<TlsProtocol>{TlsProtocol::kTlcp});
}

TEST_P(GmAutoRoutingTest, HintsArePartitionedAndExpire) {
  const url::SchemeHostPort origin("https", "gm.example", 443);
  const auto key = NetworkAnonymizationKey::CreateSameSite(
      SchemefulSite(GURL("https://top.example")));
  const auto other_key = NetworkAnonymizationKey::CreateSameSite(
      SchemefulSite(GURL("https://other.example")));
  const auto direct = ProxyChain::Direct();
  const auto proxy = ProxyChain::FromSchemeHostAndPort(ProxyServer::SCHEME_HTTP,
                                                       "proxy.example", 8080);
  session_->RememberGmCapability(origin, key, PRIVACY_MODE_DISABLED, direct,
                                 Rule());
  EXPECT_TRUE(session_->GetGmCapabilityHint(origin, key, PRIVACY_MODE_DISABLED,
                                            direct));
  EXPECT_FALSE(
      session_->GetGmCapabilityHint(origin, key, PRIVACY_MODE_ENABLED, direct));
  EXPECT_FALSE(session_->GetGmCapabilityHint(origin, other_key,
                                             PRIVACY_MODE_DISABLED, direct));
  EXPECT_FALSE(
      session_->GetGmCapabilityHint(origin, key, PRIVACY_MODE_DISABLED, proxy));
  EXPECT_TRUE(
      session_->HasGmCapabilityHint(origin, key, PRIVACY_MODE_DISABLED));
  task_environment_.FastForwardBy(base::Minutes(10));
  EXPECT_FALSE(session_->GetGmCapabilityHint(origin, key, PRIVACY_MODE_DISABLED,
                                             direct));
}

TEST_P(GmAutoRoutingTest, ConfigEpochInvalidatesHintsAndRules) {
  const url::SchemeHostPort origin("https", "gm.example", 443);
  const auto rule = Rule();
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), rule);
  config_.gm_auto_config = config_.gm_auto_config->WithEpoch(2);
  service_->UpdateSSLConfigAndNotify(config_);
  EXPECT_FALSE(
      session_->HasGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED));
  EXPECT_FALSE(session_->IsGmTlsRuleCurrent(origin, rule));
}

TEST_P(GmAutoRoutingTest, ExpiringHintPreservesUnexpiredProxyPartition) {
  const url::SchemeHostPort origin("https", "gm.example", 443);
  const auto proxy = ProxyChain::FromSchemeHostAndPort(
      ProxyServer::SCHEME_HTTP, "other-proxy.example", 8080);
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), Rule());
  task_environment_.FastForwardBy(base::Minutes(5));
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED, proxy,
                                 Rule());
  task_environment_.FastForwardBy(base::Minutes(5));
  EXPECT_FALSE(session_->GetGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED,
                                             ProxyChain::Direct()));
  EXPECT_TRUE(
      session_->GetGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED, proxy));
  task_environment_.FastForwardBy(base::Minutes(5));
  EXPECT_FALSE(
      session_->GetGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED, proxy));
}

TEST_P(GmAutoRoutingTest, ForgettingHintPreservesOtherNakAndPrivacyPartitions) {
  const url::SchemeHostPort origin("https", "gm.example", 443);
  const auto other_key = NetworkAnonymizationKey::CreateSameSite(
      SchemefulSite(GURL("https://other-top.example")));
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), Rule());
  session_->RememberGmCapability(origin, other_key, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), Rule());
  session_->RememberGmCapability(origin, {}, PRIVACY_MODE_ENABLED,
                                 ProxyChain::Direct(), Rule());
  session_->ForgetGmCapability(origin, {}, PRIVACY_MODE_DISABLED,
                               ProxyChain::Direct());
  EXPECT_FALSE(session_->GetGmCapabilityHint(origin, {}, PRIVACY_MODE_DISABLED,
                                             ProxyChain::Direct()));
  EXPECT_TRUE(session_->GetGmCapabilityHint(
      origin, other_key, PRIVACY_MODE_DISABLED, ProxyChain::Direct()));
  EXPECT_TRUE(session_->GetGmCapabilityHint(origin, {}, PRIVACY_MODE_ENABLED,
                                            ProxyChain::Direct()));
}

TEST_P(GmAutoRoutingTest, HintCountAndTrustBytesAreBounded) {
  for (int i = 0; i < 257; ++i) {
    const std::string host = "count-" + std::to_string(i) + ".example";
    session_->RememberGmCapability(url::SchemeHostPort("https", host, 443), {},
                                   PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
                                   config_.gm_auto_config->CreateRule(
                                       "https", host, 443, TlsProtocol::kTlcp));
  }
  EXPECT_FALSE(session_->HasGmCapabilityHint(
      url::SchemeHostPort("https", "count-0.example", 443), {},
      PRIVACY_MODE_DISABLED));
  EXPECT_TRUE(session_->HasGmCapabilityHint(
      url::SchemeHostPort("https", "count-256.example", 443), {},
      PRIVACY_MODE_DISABLED));

  auto automatic = std::make_shared<AutoConfig>(*config_.gm_auto_config);
  automatic->epoch = 2;
  // Structural validation deliberately does not parse these test DER blobs.
  // Each rule owns 2 MiB, so the byte budget permits only 32 of these hints.
  automatic->trust_anchors.assign(8, std::vector<uint8_t>(256 * 1024, 0x30));
  config_.gm_auto_config = automatic;
  service_->UpdateSSLConfigAndNotify(config_);
  for (int i = 0; i < 33; ++i) {
    const std::string host = "budget-" + std::to_string(i) + ".example";
    session_->RememberGmCapability(
        url::SchemeHostPort("https", host, 443), {}, PRIVACY_MODE_DISABLED,
        ProxyChain::Direct(),
        automatic->CreateRule("https", host, 443, TlsProtocol::kTlcp));
  }
  const url::SchemeHostPort first("https", "budget-0.example", 443);
  const url::SchemeHostPort last("https", "budget-32.example", 443);
  EXPECT_FALSE(session_->HasGmCapabilityHint(first, {}, PRIVACY_MODE_DISABLED));
  EXPECT_TRUE(session_->HasGmCapabilityHint(last, {}, PRIVACY_MODE_DISABLED));
  session_->ForgetGmCapability(last, {}, PRIVACY_MODE_DISABLED,
                               ProxyChain::Direct());
  session_->RememberGmCapability(
      first, {}, PRIVACY_MODE_DISABLED, ProxyChain::Direct(),
      automatic->CreateRule("https", first.host(), 443, TlsProtocol::kTlcp));
  EXPECT_TRUE(session_->HasGmCapabilityHint(first, {}, PRIVACY_MODE_DISABLED));
}

TEST_P(GmAutoRoutingTest,
       AutoTrustRefreshPreservesOrdinaryTicketsAndIdleSockets) {
  auto* cache = session_->ssl_client_context()->ssl_client_session_cache();
  ASSERT_TRUE(cache);
  bssl::UniquePtr<SSL_CTX> ssl_context(SSL_CTX_new(TLS_method()));
  bssl::UniquePtr<SSL_SESSION> ticket(SSL_SESSION_new(ssl_context.get()));
  SSL_SESSION_set_protocol_version(ticket.get(), TLS1_2_VERSION);
  SSL_SESSION_set_time(ticket.get(), base::Time::Now().ToTimeT());
  SSL_SESSION_set_timeout(ticket.get(), 3600);
  SSLClientSessionCache::Key ticket_key;
  ticket_key.server = HostPortPair("ordinary.example", 443);
  const uint64_t ordinary_generation = cache->generation_number();
  cache->Insert(ordinary_generation, ticket_key, bssl::UpRef(ticket));

  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"),
      MockRead(ASYNC, ERR_IO_PENDING)};
  MockWrite writes[] = {MockWrite(
      SYNCHRONOUS,
      "GET / HTTP/1.1\r\nHost: gm.example\r\nConnection: keep-alive\r\n\r\n")};
  StaticSocketDataProvider transport(reads, writes);
  factory_.AddSocketDataProvider(&transport);
  factory_.AddAttempt(OK, false, false);
  auto request = Request();
  {
    HttpNetworkTransaction transaction(DEFAULT_PRIORITY, session_.get());
    TestCompletionCallback complete;
    ASSERT_EQ(complete.GetResult(transaction.Start(
                  &request, complete.callback(), NetLogWithSource())),
              OK);
    std::string body;
    ASSERT_EQ(ReadTransaction(&transaction, &body), OK);
  }
  const auto idle_count = [&] {
    return GetParam() ? session_->http_stream_pool()->TotalIdleStreamCount()
                      : session_
                            ->GetSocketPool(
                                HttpNetworkSession::SocketPoolType::kNormal,
                                ProxyChain::Direct())
                            ->IdleSocketCount();
  };
  ASSERT_EQ(idle_count(), 1u);
  config_.gm_auto_config = config_.gm_auto_config->WithEpoch(2);
  service_->UpdateSSLConfigAndNotify(config_);
  EXPECT_EQ(idle_count(), 1u);
  EXPECT_EQ(cache->generation_number(), ordinary_generation);
  EXPECT_TRUE(cache->Lookup(ticket_key));
  // An ordinary handshake started before the system-trust refresh may deliver
  // its ticket afterward. An empty managed-server flush must not reject it.
  SSLClientSessionCache::Key late_key;
  late_key.server = HostPortPair("ordinary-late.example", 443);
  cache->Insert(ordinary_generation, late_key, bssl::UpRef(ticket));
  EXPECT_EQ(cache->Lookup(late_key).get(), ticket.get());
  const auto other_partition = NetworkAnonymizationKey::CreateSameSite(
      SchemefulSite(GURL("https://other-top.example")));
  const url::SchemeHostPort origin(request.url);
  session_->RememberGmCapability(origin, other_partition, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), Rule());
  session_->ForgetGmCapability(origin, other_partition, PRIVACY_MODE_DISABLED,
                               ProxyChain::Direct());
  EXPECT_EQ(idle_count(), 1u);
  EXPECT_EQ(cache->generation_number(), ordinary_generation);
  // A GM hint for a destination must not invalidate an in-flight ordinary TLS
  // handshake to an HTTPS proxy at the same host and port either.
  late_key.server = HostPortPair("gm.example", 443);
  late_key.session_usage = SessionUsage::kProxy;
  cache->Insert(ordinary_generation, late_key, bssl::UpRef(ticket));
  EXPECT_EQ(cache->Lookup(late_key).get(), ticket.get());
}

TEST_P(GmAutoRoutingTest,
       GmHintInvalidationDoesNotInvalidateOrdinaryHttpsProxy) {
  session_.reset();
  deps_.proxy_resolution_service =
      ConfiguredProxyResolutionService::CreateFixedFromPacResultForTest(
          "HTTPS gm.example:443", TRAFFIC_ANNOTATION_FOR_TESTS);
  session_ = SpdySessionDependencies::SpdyCreateSessionWithSocketFactory(
      &deps_, &factory_);
  MockRead reads[] = {
      MockRead(ASYNC, "HTTP/1.1 200 Connection Established\r\n\r\n"),
      MockRead(ASYNC, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"),
      MockRead(ASYNC, ERR_IO_PENDING)};
  MockWrite writes[] = {
      MockWrite(SYNCHRONOUS,
                "CONNECT target.example:443 HTTP/1.1\r\n"
                "Host: target.example:443\r\n"
                "Proxy-Connection: keep-alive\r\nUser-Agent: test-ua\r\n\r\n"),
      MockWrite(SYNCHRONOUS,
                "GET / HTTP/1.1\r\nHost: target.example\r\n"
                "Connection: keep-alive\r\n\r\n")};
  StaticSocketDataProvider transport(reads, writes);
  factory_.AddSocketDataProvider(&transport);
  factory_.AddAttempt(OK, false, false);
  factory_.AddAttempt(OK, false, false);
  auto request = Request();
  request.url = GURL("https://target.example/");
  {
    HttpNetworkTransaction transaction(DEFAULT_PRIORITY, session_.get());
    TestCompletionCallback complete;
    ASSERT_EQ(complete.GetResult(transaction.Start(
                  &request, complete.callback(), NetLogWithSource())),
              OK);
    std::string body;
    ASSERT_EQ(ReadTransaction(&transaction, &body), OK);
  }
  const auto proxy = ProxyChain::FromSchemeHostAndPort(
      ProxyServer::SCHEME_HTTPS, "gm.example", 443);
  auto* pool = session_->GetSocketPool(
      HttpNetworkSession::SocketPoolType::kNormal, proxy);
  ASSERT_EQ(pool->IdleSocketCount(), 1u);
  const url::SchemeHostPort gm_origin("https", "gm.example", 443);
  session_->RememberGmCapability(gm_origin, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), Rule());
  session_->ForgetGmCapability(gm_origin, {}, PRIVACY_MODE_DISABLED,
                               ProxyChain::Direct());
  EXPECT_EQ(pool->IdleSocketCount(), 1u);
  EXPECT_EQ(factory_.protocols,
            (std::vector<TlsProtocol>{TlsProtocol::kStandardTls,
                                      TlsProtocol::kStandardTls}));
}

TEST_P(GmAutoRoutingTest, OrdinaryPerformanceAutoDisabled) {
  BenchmarkOrdinaryRequests(false, false);
}

TEST_P(GmAutoRoutingTest, OrdinaryPerformanceAutoEnabled) {
  BenchmarkOrdinaryRequests(true, false);
}

TEST_P(GmAutoRoutingTest, OrdinaryPerformanceWith256Hints) {
  BenchmarkOrdinaryRequests(true, true);
}

INSTANTIATE_TEST_SUITE_P(BothConnectionPools,
                         GmAutoRoutingTest,
                         testing::Bool());
}  // namespace
}  // namespace net::gm
