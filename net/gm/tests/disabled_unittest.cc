// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <memory>
#include <vector>

#include "base/test/scoped_feature_list.h"
#include "base/test/task_environment.h"
#include "net/base/features.h"
#include "net/base/net_error_details.h"
#include "net/base/test_completion_callback.h"
#include "net/gm/gm_buildflags.h"
#include "net/gm/gm_tls_policy.h"
#include "net/http/http_network_session.h"
#include "net/http/http_network_transaction.h"
#include "net/http/http_request_info.h"
#include "net/socket/socket_test_util.h"
#include "net/spdy/spdy_test_util_common.h"
#include "net/ssl/ssl_cipher_suite_names.h"
#include "net/ssl/test_ssl_config_service.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/scheme_host_port.h"

#if !BUILDFLAG(XENON_GM)
namespace net::gm {
namespace {

class RetryableProtocolErrorSocket : public MockSSLClientSocket {
 public:
  using MockSSLClientSocket::MockSSLClientSocket;

  void PopulateNetErrorDetails(NetErrorDetails* details) const override {
    // A socket-level retry hint must not activate a compiled-out feature.
    details->gm_protocol_retry_allowed = true;
  }
};

class DisabledGmSocketFactory : public MockClientSocketFactory {
 public:
  std::unique_ptr<SSLClientSocket> CreateSSLClientSocket(
      SSLClientContext*,
      std::unique_ptr<StreamSocket> transport,
      const HostPortPair& server,
      const SSLConfig& config) override {
    ++attempts;
    EXPECT_FALSE(config.gm_rule);
    return std::make_unique<RetryableProtocolErrorSocket>(
        std::move(transport), server, config, &ssl_data);
  }

  SSLSocketDataProvider ssl_data{ASYNC, ERR_SSL_PROTOCOL_ERROR};
  size_t attempts = 0;
};

class GmDisabledTest : public testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    feature_list_.InitWithFeatureState(features::kHappyEyeballsV3, GetParam());
    auto automatic = std::make_shared<AutoConfig>();
    automatic->enabled = true;
    automatic->epoch = 1;
    automatic->trust_anchors = {{0x30, 0}};
    rule_ =
        automatic->CreateRule("https", "gm.example", 443, TlsProtocol::kTlcp);
    ASSERT_TRUE(rule_);
    ASSERT_TRUE(rule_->IsValid());
    SSLContextConfig config;
    config.gm_auto_config = automatic;
    config.gm_tls_policy =
        std::make_shared<const TlsPolicy>(std::vector<TlsRule>{*rule_});
    deps_.ssl_config_service = std::make_unique<TestSSLConfigService>(config);
    session_ = SpdySessionDependencies::SpdyCreateSessionWithSocketFactory(
        &deps_, &factory_);
  }

  int Request(bool supplied_rule) {
    // The legacy pool retains Chromium's ordinary TLS retry after a protocol
    // error. It must remain ordinary TLS; GM does not add another attempt.
    for (auto& transport : transport_data_) {
      factory_.AddSocketDataProvider(&transport);
    }
    HttpRequestInfo request;
    request.url = GURL("https://gm.example/");
    request.method = "GET";
    request.traffic_annotation =
        MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS);
    if (supplied_rule) {
      request.gm_auto_rule = rule_;
      request.gm_auto_proxy_chain = ProxyChain::Direct();
    }
    HttpNetworkTransaction transaction(DEFAULT_PRIORITY, session_.get());
    TestCompletionCallback completion;
    return completion.GetResult(
        transaction.Start(&request, completion.callback(), NetLogWithSource()));
  }

  base::test::TaskEnvironment task_environment_;
  base::test::ScopedFeatureList feature_list_;
  SpdySessionDependencies deps_;
  std::array<StaticSocketDataProvider, 2> transport_data_;
  DisabledGmSocketFactory factory_;
  std::shared_ptr<const TlsRule> rule_;
  std::unique_ptr<HttpNetworkSession> session_;
};

TEST_P(GmDisabledTest, RulesAndCapabilityHintsCannotEnableGm) {
  const url::SchemeHostPort destination("https", "gm.example", 443);
  EXPECT_FALSE(session_->ssl_client_context()->config().gm_tls_policy);
  EXPECT_FALSE(session_->ssl_client_context()->config().gm_auto_config);
  EXPECT_FALSE(session_->GetGmTlsRule(destination));
  EXPECT_FALSE(session_->IsGmTlsRuleCurrent(destination, rule_));
  session_->RememberGmCapability(destination, {}, PRIVACY_MODE_DISABLED,
                                 ProxyChain::Direct(), rule_);
  EXPECT_FALSE(
      session_->HasGmCapabilityHint(destination, {}, PRIVACY_MODE_DISABLED));
  EXPECT_FALSE(session_->GetGmCapabilityHint(
      destination, {}, PRIVACY_MODE_DISABLED, ProxyChain::Direct()));
}

TEST_P(GmDisabledTest, ProtocolFailureDoesNotProbeGm) {
  EXPECT_EQ(Request(false), ERR_SSL_PROTOCOL_ERROR);
  EXPECT_EQ(factory_.attempts, GetParam() ? 1u : 2u);
}

TEST_P(GmDisabledTest, RequestMetadataCannotEnableGm) {
  EXPECT_EQ(Request(true), ERR_SSL_PROTOCOL_ERROR);
  EXPECT_EQ(factory_.attempts, GetParam() ? 1u : 2u);
}

INSTANTIATE_TEST_SUITE_P(BothPools, GmDisabledTest, testing::Bool());

TEST(GmDisabledMetadataTest, NationalCipherSuitesAreNotRecognized) {
  for (uint16_t suite : {0x00c6, 0x00c7, 0xe013, 0xe053}) {
    const char *key_exchange, *cipher, *mac;
    bool aead, tls13;
    SSLCipherSuiteToStrings(&key_exchange, &cipher, &mac, &aead, &tls13, suite);
    EXPECT_STREQ(cipher, "???");
    EXPECT_FALSE(aead);
    EXPECT_FALSE(tls13);
    EXPECT_FALSE(IsTLSCipherSuiteAllowedByHTTP2(suite));
  }
  EXPECT_EQ(SSLKeyExchangeGroupToName(41), nullptr);
  EXPECT_STREQ(SSLKeyExchangeGroupToName(29), "X25519");
}

}  // namespace
}  // namespace net::gm
#endif  // !BUILDFLAG(XENON_GM)
