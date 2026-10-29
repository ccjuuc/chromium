// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <set>

#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "net/base/host_port_pair.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/network_handle.h"
#include "net/base/privacy_mode.h"
#include "net/base/proxy_chain.h"
#include "net/base/session_usage.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/gm/gm_client_context.h"
#include "net/gm/gm_tls_policy.h"
#include "net/http/http_stream_key.h"
#include "net/http/transport_security_state.h"
#include "net/socket/client_socket_pool.h"
#include "net/socket/next_proto.h"
#include "net/socket/ssl_client_socket.h"
#include "net/spdy/spdy_session_key.h"
#include "net/ssl/ssl_client_session_cache.h"
#include "net/ssl/ssl_config.h"
#include "net/ssl/test_ssl_config_service.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net::gm {
namespace {

TlsRule MakeRule(TlsProtocol protocol = TlsProtocol::kTlcp,
                 uint64_t revision = 1) {
  TlsRule rule;
  rule.host = "gm.example";
  rule.profile =
      ConnectionProfile(protocol, "enterprise", 1, "feitian", 1, revision);
  rule.trust_anchors = {{0x30, 0x00}};
  return rule;
}

class ConfigObserver : public SSLClientContext::Observer {
 public:
  void OnSSLConfigChanged(SSLClientContext::SSLConfigChangeType) override {
    ++global_changes;
  }
  void OnSSLConfigForServersChanged(
      const base::flat_set<HostPortPair>& changed) override {
    ++targeted_changes;
    servers = changed;
  }
  int global_changes = 0;
  int targeted_changes = 0;
  base::flat_set<HostPortPair> servers;
};

TEST(GmTlsPolicyTest, UpdatesPreserveUnrelatedOrdinarySessions) {
  base::test::TaskEnvironment task_environment;
  MockCertVerifier verifier;
  TransportSecurityState transport_security_state;
  SSLContextConfig config;
  TestSSLConfigService service(config);
  SSLClientSessionCache cache(SSLClientSessionCache::Config{});
  SSLClientContext context(&service, &verifier, &transport_security_state,
                           &cache, nullptr);
  ConfigObserver observer;
  context.AddObserver(&observer);

  bssl::UniquePtr<SSL_CTX> ssl_ctx(SSL_CTX_new(TLS_method()));
  const auto make_session = [&] {
    bssl::UniquePtr<SSL_SESSION> session(SSL_SESSION_new(ssl_ctx.get()));
    SSL_SESSION_set_protocol_version(session.get(), TLS1_2_VERSION);
    SSL_SESSION_set_time(session.get(), base::Time::Now().ToTimeT());
    SSL_SESSION_set_timeout(session.get(), 3600);
    return session;
  };
  SSLClientSessionCache::Key managed;
  managed.server = HostPortPair("gm.example", 443);
  SSLClientSessionCache::Key unrelated;
  unrelated.server = HostPortPair("ordinary.example", 443);
  cache.Insert(cache.generation_number(), managed, make_session());
  cache.Insert(cache.generation_number(), unrelated, make_session());
  ASSERT_EQ(cache.size(), 2u);

  config.gm_tls_policy =
      std::make_shared<const TlsPolicy>(std::vector<TlsRule>{MakeRule()});
  service.UpdateSSLConfigAndNotify(config);
  EXPECT_EQ(observer.global_changes, 0);
  EXPECT_EQ(observer.targeted_changes, 1);
  EXPECT_EQ(observer.servers,
            base::flat_set<HostPortPair>({HostPortPair("gm.example", 443),
                                          HostPortPair("gm.example.", 443)}));
  EXPECT_FALSE(cache.Lookup(managed));
  EXPECT_TRUE(cache.Lookup(unrelated));

  auto moved = MakeRule();
  moved.host = "other-gm.example";
  config.gm_tls_policy =
      std::make_shared<const TlsPolicy>(std::vector<TlsRule>{moved});
  service.UpdateSSLConfigAndNotify(config);
  EXPECT_EQ(observer.global_changes, 0);
  EXPECT_EQ(observer.targeted_changes, 2);
  EXPECT_TRUE(observer.servers.contains(HostPortPair("gm.example", 443)));
  EXPECT_TRUE(observer.servers.contains(HostPortPair("other-gm.example", 443)));
  EXPECT_FALSE(observer.servers.contains(unrelated.server));
  EXPECT_TRUE(cache.Lookup(unrelated));

  config.gm_tls_policy.reset();
  service.UpdateSSLConfigAndNotify(config);
  EXPECT_EQ(observer.global_changes, 0);
  EXPECT_EQ(observer.targeted_changes, 3);
  EXPECT_TRUE(cache.Lookup(unrelated));
  context.RemoveObserver(&observer);
}

TEST(GmTlsPolicyTest, InvalidPolicyAndOrdinarySslChangesInvalidateGlobally) {
  base::test::TaskEnvironment task_environment;
  MockCertVerifier verifier;
  TransportSecurityState transport_security_state;
  SSLContextConfig config;
  TestSSLConfigService service(config);
  SSLClientContext context(&service, &verifier, &transport_security_state,
                           nullptr, nullptr);
  ConfigObserver observer;
  context.AddObserver(&observer);

  config.gm_tls_policy =
      std::make_shared<const TlsPolicy>(std::vector<TlsRule>{TlsRule()});
  service.UpdateSSLConfigAndNotify(config);
  EXPECT_EQ(observer.global_changes, 1);
  EXPECT_EQ(observer.targeted_changes, 0);
  config.gm_tls_policy =
      std::make_shared<const TlsPolicy>(std::vector<TlsRule>{MakeRule()});
  service.UpdateSSLConfigAndNotify(config);
  EXPECT_EQ(observer.global_changes, 2);
  EXPECT_EQ(observer.targeted_changes, 0);

  config.gm_tls_policy.reset();
  config.ech_enabled = !config.ech_enabled;
  service.UpdateSSLConfigAndNotify(config);
  EXPECT_EQ(observer.global_changes, 3);
  EXPECT_EQ(observer.targeted_changes, 0);
  // A forced notification with identical config may represent a client-cert
  // pooling policy update, which must retain upstream global invalidation.
  service.UpdateSSLConfigAndNotify(config);
  EXPECT_EQ(observer.global_changes, 4);
  context.RemoveObserver(&observer);
}

TEST(GmTlsPolicyTest, ExactCanonicalSecureOrigin) {
  auto rule = MakeRule();
  rule.host = "GM.Example.";
  TlsPolicy policy({rule});
  ASSERT_TRUE(policy.IsValid());
  EXPECT_TRUE(policy.Match("https", "gm.example", 443));
  EXPECT_TRUE(policy.Match("https", "GM.EXAMPLE.", 443));
  EXPECT_TRUE(policy.Match("wss", "gm.example", 443));
  EXPECT_FALSE(policy.Match("http", "gm.example", 443));
  EXPECT_FALSE(policy.Match("ws", "gm.example", 443));
  EXPECT_FALSE(policy.Match("https", "gm.example", 8443));
  EXPECT_FALSE(policy.Match("https", "sub.gm.example", 443));
  EXPECT_FALSE(policy.Match("https", "other.example", 443));
}

TEST(GmAutoConfigTest, DiscoveryRulesUseSystemTrustWithoutSelectingAnIdentity) {
  AutoConfig config;
  config.enabled = true;
  config.epoch = 7;
  config.trust_anchors = {{0x30, 1}};
  config.untrusted_intermediates = {{0x30, 2}};
  config.disallowed_certificates = {{0x30, 3}};
  config.crls = {{0x30, 4}};
  const auto rule =
      config.CreateRule("https", "GM.Example.", 443, TlsProtocol::kTlcp);
  ASSERT_TRUE(rule);
  EXPECT_TRUE(rule->IsValid());
  EXPECT_TRUE(rule->automatic);
  EXPECT_EQ(rule->host, "gm.example");
  EXPECT_EQ(rule->trust_anchors, config.trust_anchors);
  EXPECT_EQ(rule->untrusted_intermediates, config.untrusted_intermediates);
  EXPECT_EQ(rule->disallowed_certificates, config.disallowed_certificates);
  EXPECT_EQ(rule->crls, config.crls);
  EXPECT_TRUE(rule->profile.client_identity_policy_id().empty());
  EXPECT_EQ(rule->profile.trust_generation(), 7u);
  EXPECT_EQ(rule->profile.effective_policy_revision(), 7u);
  EXPECT_FALSE(TlsPolicy({*rule}).IsValid());
}

TEST(GmAutoConfigTest, EmptyTrustDoesNotDisableDiscoveryOrRelaxManagedTrust) {
  AutoConfig config;
  config.enabled = true;
  config.epoch = 1;
  const auto rule =
      config.CreateRule("https", "gm.example", 443, TlsProtocol::kRfc8998);
  ASSERT_TRUE(rule);
  EXPECT_TRUE(rule->IsValid());
  EXPECT_TRUE(rule->trust_anchors.empty());
  auto managed = *rule;
  managed.automatic = false;
  EXPECT_FALSE(managed.IsValid());

  auto next = config.WithEpoch(2);
  EXPECT_NE(rule->profile,
            next->CreateRule("https", "gm.example", 443, TlsProtocol::kRfc8998)
                ->profile);
  EXPECT_FALSE(config.WithEpoch(0)->CreateRule("https", "gm.example", 443,
                                               TlsProtocol::kRfc8998));
}

TEST(GmAutoConfigTest, RejectsDisabledInvalidAndInsecureRequests) {
  AutoConfig config;
  config.epoch = 1;
  EXPECT_FALSE(
      config.CreateRule("https", "gm.example", 443, TlsProtocol::kTlcp));
  config.enabled = true;
  for (const char* scheme : {"http", "ws", "ftp"}) {
    EXPECT_FALSE(
        config.CreateRule(scheme, "gm.example", 443, TlsProtocol::kTlcp));
  }
  EXPECT_FALSE(config.CreateRule("https", "bad/host", 443, TlsProtocol::kTlcp));
  EXPECT_FALSE(config.CreateRule("https", "gm.example", 0, TlsProtocol::kTlcp));
  EXPECT_FALSE(
      config.CreateRule("https", "gm.example", 443, TlsProtocol::kStandardTls));
  config.disallowed_certificates = {{}};
  EXPECT_FALSE(config.IsValid());
  config.disallowed_certificates.assign(4097, {0x30, 0});
  EXPECT_FALSE(config.IsValid());
}

TEST(GmAutoConfigTest, JointEntryPreservesTrustSnapshotAndCannotBecomeManaged) {
  AutoConfig config;
  config.enabled = true;
  config.epoch = 7;
  config.trust_anchors = {{0x30, 1}};
  config.untrusted_intermediates = {{0x30, 2}};
  config.disallowed_certificates = {{0x30, 3}};
  config.crls = {{0x30, 4}};
  const auto rule =
      config.CreateRule("https", "gm.example", 443, TlsProtocol::kTls13Auto);
  ASSERT_TRUE(rule);
  ASSERT_TRUE(rule->IsValid());
  EXPECT_TRUE(config.MatchesRule("https", "gm.example", 443, *rule));
  EXPECT_FALSE(TlsPolicy({*rule}).IsValid());
  EXPECT_FALSE(
      config.WithEpoch(8)->MatchesRule("https", "gm.example", 443, *rule));
  for (int change = 0; change < 7; ++change) {
    SCOPED_TRACE(change);
    auto altered = *rule;
    switch (change) {
      case 0:
        altered.trust_anchors.clear();
        break;
      case 1:
        altered.untrusted_intermediates.clear();
        break;
      case 2:
        altered.disallowed_certificates.clear();
        break;
      case 3:
        altered.crls.clear();
        break;
      case 4:
        altered.automatic = false;
        break;
      case 5:
        altered.profile = ConnectionProfile(TlsProtocol::kTls13Auto,
                                            "enterprise", 7, "", 0, 7);
        break;
      case 6:
        altered.profile =
            ConnectionProfile(TlsProtocol::kTls13Auto, "windows-system", 7,
                              "unexpected-key", 1, 7);
        break;
    }
    EXPECT_FALSE(config.MatchesRule("https", "gm.example", 443, altered));
  }
  config.trust_anchors.clear();
  const auto empty =
      config.CreateRule("https", "gm.example", 443, TlsProtocol::kTls13Auto);
  ASSERT_TRUE(empty);
  EXPECT_TRUE(config.MatchesRule("https", "gm.example", 443, *empty));
  auto forged_managed = *empty;
  forged_managed.automatic = false;
  EXPECT_FALSE(forged_managed.IsValid());
  EXPECT_FALSE(TlsPolicy({forged_managed}).IsValid());
}

TEST(GmTlsPolicyTest, JointAndStrictPoliciesKeepSeparateEpochsAndPoolKeys) {
  auto strict = MakeRule(TlsProtocol::kRfc8998);
  auto joint = MakeRule(TlsProtocol::kTls13Auto);
  TlsPolicy strict_policy({strict});
  TlsPolicy joint_policy({joint});
  ASSERT_TRUE(strict_policy.IsValid());
  ASSERT_TRUE(joint_policy.IsValid());
  EXPECT_NE(strict_policy, joint_policy);
  EXPECT_NE(strict.profile, joint.profile);
  EXPECT_TRUE(joint.profile.AllowsHttp2());
  EXPECT_FALSE(joint.profile.AllowsQuic());
  EXPECT_FALSE(joint.profile.AllowsCrossOriginHttp2());
  const auto updated = joint_policy.WithPolicyEpoch(42);
  const auto selected = updated->Match("https", "gm.example", 443);
  ASSERT_TRUE(selected);
  EXPECT_EQ(selected->profile.protocol(), TlsProtocol::kTls13Auto);
  EXPECT_EQ(selected->profile.effective_policy_revision(), 42u);
  EXPECT_EQ(selected->trust_anchors, joint.trust_anchors);
  EXPECT_NE(selected->profile, joint.profile);
  SSLConfig config;
  config.alpn_protos = {NextProto::kProtoHTTP2, NextProto::kProtoHTTP11};
  ApplyTlsRule(selected, &config);
  EXPECT_EQ(config.gm_rule, selected);
  EXPECT_EQ(config.alpn_protos,
            NextProtoVector({NextProto::kProtoHTTP2, NextProto::kProtoHTTP11}));
}

TEST(GmAutoConfigTest, ExpiringOriginClearsOnlyItsSessionPartition) {
  ClientContext context;
  ClientContext::SessionKey first;
  first.server = HostPortPair("GM.Example.", 443);
  ClientContext::SessionKey other;
  other.server = HostPortPair("other.example", 443);
  const auto first_session = context.GetSession(first);
  const auto other_session = context.GetSession(other);
  context.StoreSession(first, first_session.binding, {1, 2, 3});
  context.StoreSession(other, other_session.binding, {4, 5, 6});
  context.ClearForServers({HostPortPair("gm.example", 443)});
  const auto replaced = context.GetSession(first);
  EXPECT_TRUE(replaced.blob.empty());
  EXPECT_NE(replaced.binding, first_session.binding);
  EXPECT_EQ(context.GetSession(other).blob, (std::vector<uint8_t>{4, 5, 6}));
  EXPECT_EQ(context.GetSession(other).binding, other_session.binding);
}

TEST(GmTlsPolicyTest, CanonicalIpOrigins) {
  auto rule = MakeRule();
  rule.host = "[2001:db8::1]";
  TlsPolicy policy({rule});
  ASSERT_TRUE(policy.IsValid());
  EXPECT_TRUE(policy.Match("https", "2001:db8::1", 443));
  EXPECT_FALSE(policy.Match("https", "2001:db8::2", 443));
}

TEST(GmTlsPolicyTest, DuplicateAndMalformedPolicyFailsClosed) {
  auto rule = MakeRule();
  auto alias = rule;
  alias.host = "GM.EXAMPLE.";
  TlsPolicy duplicate({rule, alias});
  ASSERT_FALSE(duplicate.IsValid());
  ASSERT_TRUE(duplicate.Match("https", "other.example", 443));
  EXPECT_FALSE(duplicate.Match("https", "other.example", 443)->IsValid());
  for (const char* host :
       {"", "*", "*.example", "bad host", "https://gm.example"}) {
    rule.host = host;
    TlsPolicy invalid({rule});
    EXPECT_FALSE(invalid.IsValid()) << host;
    EXPECT_TRUE(invalid.Match("https", "gm.example", 443)) << host;
  }
}

TEST(GmTlsPolicyTest, InvalidTrustAndRevocationFailClosed) {
  auto rule = MakeRule();
  rule.trust_anchors.clear();
  EXPECT_FALSE(TlsPolicy({rule}).IsValid());
  rule = MakeRule();
  rule.revocation_mode = 1;
  EXPECT_FALSE(TlsPolicy({rule}).IsValid());
  rule.crls = {{0x30, 0}};
  EXPECT_TRUE(TlsPolicy({rule}).IsValid());
  rule.revocation_mode = 3;
  EXPECT_FALSE(TlsPolicy({rule}).IsValid());
  rule = MakeRule();
  rule.port = 0;
  EXPECT_FALSE(TlsPolicy({rule}).IsValid());
  rule = MakeRule();
  rule.profile = ConnectionProfile();
  EXPECT_FALSE(TlsPolicy({rule}).IsValid());
}

TEST(GmTlsPolicyTest, PolicyResourceBounds) {
  auto rule = MakeRule();
  rule.trust_anchors[0].resize(256 * 1024 + 1);
  EXPECT_FALSE(TlsPolicy({rule}).IsValid());
  rule = MakeRule();
  rule.crls.resize(129, {0x30, 0});
  EXPECT_FALSE(TlsPolicy({rule}).IsValid());
  EXPECT_FALSE(TlsPolicy(std::vector<TlsRule>(257, MakeRule())).IsValid());
}

TEST(GmTlsPolicyTest, EpochSeparatesUpdatesAndPreservesInvalidPolicy) {
  TlsPolicy policy({MakeRule()});
  auto first = policy.WithPolicyEpoch(1);
  auto second = policy.WithPolicyEpoch(2);
  ASSERT_TRUE(first->IsValid());
  ASSERT_TRUE(second->IsValid());
  EXPECT_NE(first->Match("https", "gm.example", 443)->profile,
            second->Match("https", "gm.example", 443)->profile);
  EXPECT_EQ(*first, *policy.WithPolicyEpoch(1));
  EXPECT_FALSE(policy.WithPolicyEpoch(0)->IsValid());
  TlsPolicy invalid({TlsRule()});
  EXPECT_FALSE(invalid.WithPolicyEpoch(5)->IsValid());
  auto changed = MakeRule();
  changed.trust_anchors[0].push_back(1);
  EXPECT_NE(policy, TlsPolicy({changed}));
}

TEST(GmTlsPolicyTest, HardenSslOptionsAndRestrictTlcpAlpn) {
  SSLConfig config;
  config.early_data_enabled = true;
  config.ignore_certificate_errors = true;
  config.ech_config_list = {1, 2, 3};
  config.renego_allowed_default = true;
  config.alpn_protos = {NextProto::kProtoHTTP2, NextProto::kProtoHTTP11};
  config.application_settings = {{NextProto::kProtoHTTP2, {}}};
  auto rule = std::make_shared<const TlsRule>(MakeRule());
  ApplyTlsRule(rule, &config);
  EXPECT_EQ(config.gm_rule, rule);
  EXPECT_FALSE(config.early_data_enabled);
  EXPECT_FALSE(config.ignore_certificate_errors);
  EXPECT_TRUE(config.ech_config_list.empty());
  EXPECT_TRUE(config.application_settings.empty());
  EXPECT_FALSE(config.renego_allowed_default);
  EXPECT_EQ(config.alpn_protos, NextProtoVector({NextProto::kProtoHTTP11}));

  config.alpn_protos = {NextProto::kProtoHTTP2, NextProto::kProtoHTTP11};
  ApplyTlsRule(std::make_shared<const TlsRule>(MakeRule(TlsProtocol::kRfc8998)),
               &config);
  EXPECT_EQ(config.alpn_protos.size(), 2u);
}

TEST(GmTlsPolicyTest, StandardTlsOptionsUnchanged) {
  SSLConfig config;
  config.early_data_enabled = true;
  config.ech_config_list = {1, 2, 3};
  ApplyTlsRule(nullptr, &config);
  EXPECT_FALSE(config.gm_rule);
  EXPECT_TRUE(config.early_data_enabled);
  EXPECT_EQ(config.ech_config_list.size(), 3u);
}

TEST(GmTlsPolicyTest, NewAndLegacyPoolKeysSeparateSecurityProfiles) {
  const url::SchemeHostPort origin("https", "gm.example", 443);
  const auto key = [&](std::shared_ptr<const TlsRule> rule) {
    return HttpStreamKey(origin, PRIVACY_MODE_DISABLED, SocketTag(),
                         NetworkAnonymizationKey(), SecureDnsPolicy::kAllow,
                         false, handles::kInvalidNetworkHandle, std::nullopt,
                         std::move(rule));
  };
  const auto group = [&](std::shared_ptr<const TlsRule> rule) {
    return ClientSocketPool::GroupId(
        origin, PRIVACY_MODE_DISABLED, NetworkAnonymizationKey(),
        SecureDnsPolicy::kAllow, false, handles::kInvalidNetworkHandle,
        std::move(rule));
  };
  auto a = std::make_shared<const TlsRule>(MakeRule());
  auto equal = std::make_shared<const TlsRule>(*a);
  auto changed =
      std::make_shared<const TlsRule>(MakeRule(TlsProtocol::kTlcp, 2));
  EXPECT_EQ(key(a), key(equal));
  EXPECT_EQ(group(a), group(equal));
  EXPECT_NE(key(a), key(nullptr));
  EXPECT_NE(group(a), group(nullptr));
  EXPECT_NE(key(a), key(changed));
  EXPECT_NE(group(a), group(changed));
  EXPECT_EQ(
      std::set<HttpStreamKey>({key(nullptr), key(a), key(equal), key(changed)})
          .size(),
      3u);
  EXPECT_EQ(std::set<ClientSocketPool::GroupId>(
                {group(nullptr), group(a), group(equal), group(changed)})
                .size(),
            3u);
  EXPECT_EQ(key(a).CalculateSpdySessionKey().gm_profile(), a->profile);
  EXPECT_FALSE(key(a).CalculateQuicSessionAliasKey().destination().IsValid());
  EXPECT_TRUE(
      key(nullptr).CalculateQuicSessionAliasKey().destination().IsValid());
}

TEST(GmTlsPolicyTest, SpdyCannotAliasDifferentTrustOrClientIdentityOrigins) {
  auto rule = MakeRule(TlsProtocol::kRfc8998);
  const auto key = [&](const char* host, const ConnectionProfile& profile) {
    return SpdySessionKey(HostPortPair(host, 443), PRIVACY_MODE_DISABLED,
                          ProxyChain::Direct(), SessionUsage::kDestination,
                          SocketTag(), NetworkAnonymizationKey(),
                          SecureDnsPolicy::kAllow, false,
                          handles::kInvalidNetworkHandle, profile);
  };
  const auto a = key("gm.example", rule.profile);
  EXPECT_TRUE(a.CompareForAliasing(a).is_potentially_aliasable);
  EXPECT_FALSE(a.CompareForAliasing(key("other.example", rule.profile))
                   .is_potentially_aliasable);
  EXPECT_FALSE(a.CompareForAliasing(key("gm.example", ConnectionProfile()))
                   .is_potentially_aliasable);
  EXPECT_FALSE(
      a.CompareForAliasing(
           key("gm.example", MakeRule(TlsProtocol::kRfc8998, 2).profile))
          .is_potentially_aliasable);
  EXPECT_TRUE(key("a.example", ConnectionProfile())
                  .CompareForAliasing(key("b.example", ConnectionProfile()))
                  .is_potentially_aliasable);
}

}  // namespace
}  // namespace net::gm
