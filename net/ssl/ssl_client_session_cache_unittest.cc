// Copyright 2015 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/ssl/ssl_client_session_cache.h"

#include <vector>

#include "base/memory_coordinator/memory_coordinator_features.h"
#include "base/memory_coordinator/test_memory_consumer_registry.h"
#include "base/memory_coordinator/utils.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/simple_test_clock.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/trace_event/memory_allocator_dump.h"
#include "base/trace_event/process_memory_dump.h"
#include "base/trace_event/trace_event.h"
#include "net/base/features.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/schemeful_site.h"
#include "net/gm/gm_connection_profile.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "url/gurl.h"

using testing::ByRef;
using testing::Contains;
using testing::Eq;
using testing::Field;

namespace net {

namespace {

std::unique_ptr<base::SimpleTestClock> MakeTestClock() {
  std::unique_ptr<base::SimpleTestClock> clock =
      std::make_unique<base::SimpleTestClock>();
  // SimpleTestClock starts at the null base::Time which converts to and from
  // time_t confusingly.
  clock->SetNow(base::Time::FromTimeT(1000000000));
  return clock;
}

SSLClientSessionCache::Key MakeTestKey(const std::string& str) {
  SSLClientSessionCache::Key key;
  key.server = HostPortPair(str, 443);
  return key;
}

SSLClientSessionCache::Key AddProxyChain(SSLClientSessionCache::Key key) {
  key.proxy_chain = ProxyChain::FromSchemeHostAndPort(ProxyServer::SCHEME_HTTPS,
                                                      "proxy", 999);
  key.proxy_chain_index = 0;
  return key;
}

SSLClientSessionCache::Key WithProxyUsage(SSLClientSessionCache::Key key) {
  key.session_usage = SessionUsage::kProxy;
  return key;
}

SSLClientSessionCache::Key MakeJointTestKey() {
  auto key = MakeTestKey("joint.test");
  key.gm_profile = gm::ConnectionProfile(gm::TlsProtocol::kTls13Auto, "corp-ca",
                                         1, "usbkey-policy", 1, 1);
  key.gm_identity_id = "authorized-key";
  key.gm_identity_generation = 1;
  return key;
}

class SSLClientSessionCacheTest : public testing::Test {
 public:
  SSLClientSessionCacheTest() : ssl_ctx_(SSL_CTX_new(TLS_method())) {}

 protected:
  bssl::UniquePtr<SSL_SESSION> NewSSLSession(
      uint16_t version = TLS1_2_VERSION) {
    SSL_SESSION* session = SSL_SESSION_new(ssl_ctx_.get());
    if (!SSL_SESSION_set_protocol_version(session, version)) {
      return nullptr;
    }
    return bssl::UniquePtr<SSL_SESSION>(session);
  }

  bssl::UniquePtr<SSL_SESSION> MakeTestSession(base::Time now,
                                               base::TimeDelta timeout) {
    bssl::UniquePtr<SSL_SESSION> session = NewSSLSession();
    SSL_SESSION_set_time(session.get(), now.ToTimeT());
    SSL_SESSION_set_timeout(session.get(), timeout.InSeconds());
    return session;
  }

  void SimulateMemoryLimitAndRelease(
      base::test::TaskEnvironment& task_environment,
      int percentage) {
    test_memory_consumer_registry_.NotifyUpdateMemoryLimitAsync(
        percentage, task_environment.QuitClosure());
    task_environment.RunUntilQuit();

    test_memory_consumer_registry_.NotifyReleaseMemoryAsync(
        task_environment.QuitClosure());
    task_environment.RunUntilQuit();
  }

  base::TestMemoryConsumerRegistry test_memory_consumer_registry_;

 private:
  bssl::UniquePtr<SSL_CTX> ssl_ctx_;
};

}  // namespace

// These tests rely on memory corruption detectors to verify that
// SSL_SESSION reference counts were correctly managed and no sessions
// leaked or were accessed after free.

// Test basic insertion and lookup operations.
TEST_F(SSLClientSessionCacheTest, Basic) {
  SSLClientSessionCache::Config config;
  SSLClientSessionCache cache(config);

  bssl::UniquePtr<SSL_SESSION> session1 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session2 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session3 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session4 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session5 = NewSSLSession();

  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(0u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session1));
  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(1u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session2));
  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session2.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(2u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session3));
  EXPECT_EQ(session3.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session2.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(2u, cache.size());

  cache.Insert(cache.generation_number(), AddProxyChain(MakeTestKey("key1")),
               bssl::UpRef(session4));
  EXPECT_EQ(session3.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session4.get(),
            cache.Lookup(AddProxyChain(MakeTestKey("key1"))).get());
  EXPECT_EQ(3u, cache.size());

  cache.Insert(cache.generation_number(), WithProxyUsage(MakeTestKey("key1")),
               bssl::UpRef(session5));
  EXPECT_EQ(session3.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session5.get(),
            cache.Lookup(WithProxyUsage(MakeTestKey("key1"))).get());
  EXPECT_EQ(4u, cache.size());

  cache.Flush();
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key3")).get());
  EXPECT_EQ(nullptr, cache.Lookup(AddProxyChain(MakeTestKey("key1"))).get());
  EXPECT_EQ(nullptr, cache.Lookup(WithProxyUsage(MakeTestKey("key1"))).get());
  EXPECT_EQ(0u, cache.size());
}

TEST_F(SSLClientSessionCacheTest,
       OrdinaryAndJointNegotiationCannotShareSessions) {
  SSLClientSessionCache cache({});
  const auto ordinary = MakeTestKey("joint.test");
  const auto joint = MakeJointTestKey();
  auto ordinary_session = NewSSLSession(TLS1_3_VERSION);
  auto joint_session = NewSSLSession(TLS1_3_VERSION);
  ASSERT_TRUE(ordinary_session);
  ASSERT_TRUE(joint_session);

  cache.Insert(cache.generation_number(), ordinary,
               bssl::UpRef(ordinary_session));
  EXPECT_FALSE(cache.Lookup(joint));
  cache.Insert(cache.gm_generation_number(), joint, bssl::UpRef(joint_session));
  EXPECT_EQ(cache.size(), 2u);
  EXPECT_EQ(cache.Lookup(ordinary).get(), ordinary_session.get());
  EXPECT_EQ(cache.Lookup(joint).get(), joint_session.get());
  EXPECT_EQ(cache.size(), 0u);

  cache.Insert(cache.gm_generation_number(), joint, bssl::UpRef(joint_session));
  EXPECT_FALSE(cache.Lookup(ordinary));
  EXPECT_EQ(cache.Lookup(joint).get(), joint_session.get());
}

TEST_F(SSLClientSessionCacheTest,
       JointTrustPolicyAndIdentityAreSeparatePartitions) {
  SSLClientSessionCache cache({});
  const auto original = MakeJointTestKey();
  std::vector<SSLClientSessionCache::Key> keys{original};
  for (const auto& profile : {
           gm::ConnectionProfile(gm::TlsProtocol::kRfc8998, "corp-ca", 1,
                                 "usbkey-policy", 1, 1),
           gm::ConnectionProfile(gm::TlsProtocol::kTls13Auto, "other-ca", 1,
                                 "usbkey-policy", 1, 1),
           gm::ConnectionProfile(gm::TlsProtocol::kTls13Auto, "corp-ca", 2,
                                 "usbkey-policy", 1, 1),
           gm::ConnectionProfile(gm::TlsProtocol::kTls13Auto, "corp-ca", 1,
                                 "other-policy", 1, 1),
           gm::ConnectionProfile(gm::TlsProtocol::kTls13Auto, "corp-ca", 1,
                                 "usbkey-policy", 2, 1),
           gm::ConnectionProfile(gm::TlsProtocol::kTls13Auto, "corp-ca", 1,
                                 "usbkey-policy", 1, 2),
       }) {
    auto changed = original;
    changed.gm_profile = profile;
    keys.push_back(std::move(changed));
  }
  auto other_device = original;
  other_device.gm_identity_id = "other-authorized-key";
  keys.push_back(std::move(other_device));
  auto renewed_authorization = original;
  renewed_authorization.gm_identity_generation = 2;
  keys.push_back(std::move(renewed_authorization));
  auto anonymous = original;
  anonymous.gm_identity_id.clear();
  anonymous.gm_identity_generation = 0;
  keys.push_back(std::move(anonymous));

  std::vector<bssl::UniquePtr<SSL_SESSION>> sessions;
  for (size_t i = 0; i < keys.size(); ++i) {
    SCOPED_TRACE(i);
    // A session from any previous policy or device must not be offered under
    // the new key, even when the host, NAK and TLS wire version are identical.
    EXPECT_FALSE(cache.Lookup(keys[i]));
    sessions.push_back(NewSSLSession(TLS1_3_VERSION));
    ASSERT_TRUE(sessions.back());
    cache.Insert(cache.gm_generation_number(), keys[i],
                 bssl::UpRef(sessions.back()));
  }
  EXPECT_EQ(cache.size(), keys.size());
  for (size_t i = 0; i < keys.size(); ++i) {
    SCOPED_TRACE(i);
    EXPECT_EQ(cache.Lookup(keys[i]).get(), sessions[i].get());
  }
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(SSLClientSessionCacheTest, FlushForServersClearsEveryJointPartition) {
  SSLClientSessionCache cache({});
  const auto joint = MakeJointTestKey();
  auto changed_identity = joint;
  changed_identity.gm_identity_generation = 2;
  auto other_port = joint;
  other_port.server = HostPortPair("joint.test", 444);
  auto other_host = joint;
  other_host.server = HostPortPair("unaffected.test", 443);
  const auto ordinary = MakeTestKey("joint.test");
  auto session = NewSSLSession(TLS1_3_VERSION);
  ASSERT_TRUE(session);
  const uint64_t generation = cache.generation_number();
  const uint64_t gm_generation = cache.gm_generation_number();
  for (const auto& key :
       {joint, changed_identity, other_port, other_host, ordinary}) {
    cache.Insert(key.gm_profile ? gm_generation : generation, key,
                 bssl::UpRef(session));
  }
  ASSERT_EQ(cache.size(), 5u);
  cache.FlushForServers({HostPortPair("joint.test", 443)});
  EXPECT_FALSE(cache.Lookup(joint));
  EXPECT_FALSE(cache.Lookup(changed_identity));
  EXPECT_FALSE(cache.Lookup(ordinary));
  EXPECT_EQ(cache.size(), 2u);
  // A late ticket callback from the invalidated connection cannot revive it.
  cache.Insert(gm_generation, joint, bssl::UpRef(session));
  EXPECT_FALSE(cache.Lookup(joint));
  EXPECT_EQ(cache.Lookup(other_port).get(), session.get());
  EXPECT_EQ(cache.Lookup(other_host).get(), session.get());
}

TEST_F(SSLClientSessionCacheTest,
       GmInvalidationPreservesOrdinaryAndProxyTickets) {
  SSLClientSessionCache cache({});
  const auto joint = MakeJointTestKey();
  auto changed_identity = joint;
  changed_identity.gm_identity_generation = 2;
  auto anonymous = joint;
  anonymous.gm_identity_id.clear();
  anonymous.gm_identity_generation = 0;
  const std::vector gm_keys = {joint, changed_identity, anonymous,
                               AddProxyChain(joint)};
  const auto ordinary = MakeTestKey("joint.test");
  const std::vector ordinary_keys = {ordinary, WithProxyUsage(ordinary),
                                     AddProxyChain(ordinary)};
  auto other_host = joint;
  other_host.server = HostPortPair("unaffected.test", 443);
  auto other_port = joint;
  other_port.server = HostPortPair("joint.test", 444);
  auto session = NewSSLSession(TLS1_3_VERSION);
  ASSERT_TRUE(session);

  // Deliberately separate the generations before insertion: GM tickets must
  // use their own generation even if the negotiated suite was ordinary TLS.
  cache.FlushGmForServers({HostPortPair("unrelated.test", 443)});
  const uint64_t generation = cache.generation_number();
  const uint64_t gm_generation = cache.gm_generation_number();
  ASSERT_NE(generation, gm_generation);
  for (const auto& key : gm_keys) {
    cache.Insert(gm_generation, key, bssl::UpRef(session));
  }
  for (const auto& key : ordinary_keys) {
    cache.Insert(generation, key, bssl::UpRef(session));
  }
  for (const auto& key : {other_host, other_port}) {
    cache.Insert(gm_generation, key, bssl::UpRef(session));
  }
  ASSERT_EQ(cache.size(), gm_keys.size() + ordinary_keys.size() + 2);

  // A Key removal or capability expiry must leave same-host ordinary HTTPS,
  // HTTPS proxy authentication, and unrelated GM origins reusable.
  cache.FlushGmForServers({joint.server});
  EXPECT_EQ(cache.generation_number(), generation);
  EXPECT_NE(cache.gm_generation_number(), gm_generation);
  for (const auto& key : gm_keys) {
    EXPECT_FALSE(cache.Lookup(key));
  }
  ASSERT_EQ(cache.size(), ordinary_keys.size() + 2);
  for (const auto& key : ordinary_keys) {
    EXPECT_EQ(cache.Lookup(key).get(), session.get());
  }
  EXPECT_EQ(cache.Lookup(other_host).get(), session.get());
  EXPECT_EQ(cache.Lookup(other_port).get(), session.get());
  EXPECT_EQ(cache.size(), 0u);
}

TEST_F(SSLClientSessionCacheTest,
       GmInvalidationRejectsStaleGmButAcceptsLateOrdinaryTickets) {
  SSLClientSessionCache cache({});
  const auto joint = MakeJointTestKey();
  auto other_gm = joint;
  other_gm.server = HostPortPair("unaffected.test", 443);
  const auto ordinary = MakeTestKey("joint.test");
  const uint64_t generation = cache.generation_number();
  const uint64_t gm_generation = cache.gm_generation_number();
  auto retained = NewSSLSession(TLS1_3_VERSION);
  auto late = NewSSLSession(TLS1_3_VERSION);
  auto current = NewSSLSession(TLS1_3_VERSION);
  ASSERT_TRUE(retained);
  ASSERT_TRUE(late);
  ASSERT_TRUE(current);
  cache.Insert(gm_generation, other_gm, bssl::UpRef(retained));

  cache.FlushGmForServers({joint.server});
  // These ordinary handshakes started before the GM invalidation. Their late
  // NewSessionTicket callbacks must still cache tickets without reconnecting.
  for (const auto& key : {ordinary, WithProxyUsage(ordinary),
                          MakeTestKey("ordinary-elsewhere.test")}) {
    cache.Insert(generation, key, bssl::UpRef(late));
    EXPECT_EQ(cache.Lookup(key).get(), late.get());
  }
  // Stale GM callbacks cannot revive the affected identity or overwrite an
  // unrelated, already cached ticket. Existing unrelated entries stay valid.
  cache.Insert(gm_generation, joint, bssl::UpRef(late));
  cache.Insert(gm_generation, other_gm, bssl::UpRef(late));
  EXPECT_FALSE(cache.Lookup(joint));
  EXPECT_EQ(cache.Lookup(other_gm).get(), retained.get());
  EXPECT_FALSE(cache.Lookup(other_gm));
  cache.Insert(cache.gm_generation_number(), joint, bssl::UpRef(current));
  EXPECT_EQ(cache.Lookup(joint).get(), current.get());
}

TEST_F(SSLClientSessionCacheTest,
       OrdinaryInvalidationRejectsLateTicketsFromBothGenerations) {
  for (const bool flush_all : {false, true}) {
    SCOPED_TRACE(flush_all);
    SSLClientSessionCache cache({});
    const auto joint = MakeJointTestKey();
    const auto ordinary = MakeTestKey("joint.test");
    auto other_joint = joint;
    other_joint.server = HostPortPair("unaffected.test", 443);
    const auto other_ordinary = MakeTestKey("unaffected.test");
    auto session = NewSSLSession(TLS1_3_VERSION);
    ASSERT_TRUE(session);
    cache.FlushGmForServers({HostPortPair("unrelated.test", 443)});
    const uint64_t generation = cache.generation_number();
    const uint64_t gm_generation = cache.gm_generation_number();
    ASSERT_NE(generation, gm_generation);
    for (const auto& key : {joint, other_joint}) {
      cache.Insert(gm_generation, key, bssl::UpRef(session));
    }
    for (const auto& key : {ordinary, other_ordinary}) {
      cache.Insert(generation, key, bssl::UpRef(session));
    }
    ASSERT_EQ(cache.size(), 4u);
    if (flush_all) {
      cache.Flush();
    } else {
      cache.FlushForServers({joint.server});
    }
    EXPECT_NE(cache.generation_number(), generation);
    EXPECT_NE(cache.gm_generation_number(), gm_generation);
    cache.Insert(generation, ordinary, bssl::UpRef(session));
    cache.Insert(gm_generation, joint, bssl::UpRef(session));
    EXPECT_FALSE(cache.Lookup(ordinary));
    EXPECT_FALSE(cache.Lookup(joint));
    EXPECT_EQ(cache.Lookup(other_ordinary).get(),
              flush_all ? nullptr : session.get());
    EXPECT_EQ(cache.Lookup(other_joint).get(),
              flush_all ? nullptr : session.get());
    cache.Insert(cache.generation_number(), ordinary, bssl::UpRef(session));
    cache.Insert(cache.gm_generation_number(), joint, bssl::UpRef(session));
    EXPECT_EQ(cache.Lookup(ordinary).get(), session.get());
    EXPECT_EQ(cache.Lookup(joint).get(), session.get());
  }
}

// Test basic insertion and lookup operations with single-use sessions.
TEST_F(SSLClientSessionCacheTest, BasicSingleUse) {
  SSLClientSessionCache::Config config;
  SSLClientSessionCache cache(config);

  bssl::UniquePtr<SSL_SESSION> session1 = NewSSLSession(TLS1_3_VERSION);
  bssl::UniquePtr<SSL_SESSION> session2 = NewSSLSession(TLS1_3_VERSION);
  bssl::UniquePtr<SSL_SESSION> session3 = NewSSLSession(TLS1_3_VERSION);

  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(0u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session1));
  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(0u, cache.size());

  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());

  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session1));
  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session1));
  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session2));

  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session2.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(1u, cache.size());

  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());

  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session1));
  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session3));
  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session2));
  EXPECT_EQ(session3.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session2.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(0u, cache.size());

  cache.Flush();
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key3")).get());
  EXPECT_EQ(0u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session1));
  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session2));
  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session3));
  EXPECT_EQ(session3.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session2.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
}

// Test insertion and lookup operations with both single-use and reusable
// sessions.
TEST_F(SSLClientSessionCacheTest, MixedUse) {
  SSLClientSessionCache::Config config;
  SSLClientSessionCache cache(config);

  bssl::UniquePtr<SSL_SESSION> session_single = NewSSLSession(TLS1_3_VERSION);
  bssl::UniquePtr<SSL_SESSION> session_reuse = NewSSLSession(TLS1_2_VERSION);

  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(0u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session_reuse));
  EXPECT_EQ(session_reuse.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(1u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session_single));
  EXPECT_EQ(session_single.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(0u, cache.size());

  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(0u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session_single));
  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session_single));
  EXPECT_EQ(1u, cache.size());

  EXPECT_EQ(session_single.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(session_single.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(0u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session_single));
  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session_reuse));
  EXPECT_EQ(session_reuse.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(session_reuse.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(1u, cache.size());
}

// Test that a session may be inserted at two different keys. This should never
// be necessary, but the API doesn't prohibit it.
TEST_F(SSLClientSessionCacheTest, DoubleInsert) {
  SSLClientSessionCache::Config config;
  SSLClientSessionCache cache(config);

  bssl::UniquePtr<SSL_SESSION> session = NewSSLSession();

  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(0u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session));
  EXPECT_EQ(session.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(1u, cache.size());

  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session));
  EXPECT_EQ(session.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(2u, cache.size());

  cache.Flush();
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(0u, cache.size());
}

// Tests that the session cache's size is correctly bounded.
TEST_F(SSLClientSessionCacheTest, MaxEntries) {
  SSLClientSessionCache::Config config;
  config.max_entries = 3;
  SSLClientSessionCache cache(config);

  bssl::UniquePtr<SSL_SESSION> session1 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session2 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session3 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session4 = NewSSLSession();

  // Insert three entries.
  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session1));
  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session2));
  cache.Insert(cache.generation_number(), MakeTestKey("key3"),
               bssl::UpRef(session3));
  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session2.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(session3.get(), cache.Lookup(MakeTestKey("key3")).get());
  EXPECT_EQ(3u, cache.size());

  // On insertion of a fourth, the first is removed.
  cache.Insert(cache.generation_number(), MakeTestKey("key4"),
               bssl::UpRef(session4));
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session4.get(), cache.Lookup(MakeTestKey("key4")).get());
  EXPECT_EQ(session3.get(), cache.Lookup(MakeTestKey("key3")).get());
  EXPECT_EQ(session2.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(3u, cache.size());

  // Despite being newest, the next to be removed is session4 as it was accessed
  // least. recently.
  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session1));
  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(session2.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(session3.get(), cache.Lookup(MakeTestKey("key3")).get());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key4")).get());
  EXPECT_EQ(3u, cache.size());
}

// Tests that session expiration works properly.
TEST_F(SSLClientSessionCacheTest, Expiration) {
  const size_t kNumEntries = 20;
  const size_t kExpirationCheckCount = 10;
  const base::TimeDelta kTimeout = base::Seconds(1000);

  SSLClientSessionCache::Config config;
  config.expiration_check_count = kExpirationCheckCount;
  std::unique_ptr<base::SimpleTestClock> clock = MakeTestClock();
  SSLClientSessionCache cache(config);
  cache.SetClockForTesting(clock.get());

  // Add |kNumEntries - 1| entries.
  for (size_t i = 0; i < kNumEntries - 1; i++) {
    bssl::UniquePtr<SSL_SESSION> session =
        MakeTestSession(clock->Now(), kTimeout);
    cache.Insert(cache.generation_number(),
                 MakeTestKey(base::NumberToString(i)), bssl::UpRef(session));
  }
  EXPECT_EQ(kNumEntries - 1, cache.size());

  // Expire all the previous entries and insert one more entry.
  clock->Advance(kTimeout * 2);
  bssl::UniquePtr<SSL_SESSION> session =
      MakeTestSession(clock->Now(), kTimeout);
  cache.Insert(cache.generation_number(), MakeTestKey("key"),
               bssl::UpRef(session));

  // All entries are still in the cache.
  EXPECT_EQ(kNumEntries, cache.size());

  // Perform one fewer lookup than needed to trigger the expiration check. This
  // shall not expire any session.
  for (size_t i = 0; i < kExpirationCheckCount - 1; i++) {
    cache.Lookup(MakeTestKey("key"));
  }

  // All entries are still in the cache.
  EXPECT_EQ(kNumEntries, cache.size());

  // Perform one more lookup. This will expire all sessions but the last one.
  cache.Lookup(MakeTestKey("key"));
  EXPECT_EQ(1u, cache.size());
  EXPECT_EQ(session.get(), cache.Lookup(MakeTestKey("key")).get());
  for (size_t i = 0; i < kNumEntries - 1; i++) {
    SCOPED_TRACE(i);
    EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey(base::NumberToString(i))));
  }
}

// Tests that Lookup performs an expiration check before returning a cached
// session.
TEST_F(SSLClientSessionCacheTest, LookupExpirationCheck) {
  // kExpirationCheckCount is set to a suitably large number so the automated
  // pruning never triggers.
  const size_t kExpirationCheckCount = 1000;
  const base::TimeDelta kTimeout = base::Seconds(1000);

  SSLClientSessionCache::Config config;
  config.expiration_check_count = kExpirationCheckCount;
  std::unique_ptr<base::SimpleTestClock> clock = MakeTestClock();
  SSLClientSessionCache cache(config);
  cache.SetClockForTesting(clock.get());

  // Insert an entry into the session cache.
  bssl::UniquePtr<SSL_SESSION> session =
      MakeTestSession(clock->Now(), kTimeout);
  cache.Insert(cache.generation_number(), MakeTestKey("key"),
               bssl::UpRef(session));
  EXPECT_EQ(session.get(), cache.Lookup(MakeTestKey("key")).get());
  EXPECT_EQ(1u, cache.size());

  // Expire the session.
  clock->Advance(kTimeout * 2);

  // The entry has not been removed yet.
  EXPECT_EQ(1u, cache.size());

  // But it will not be returned on lookup and gets pruned at that point.
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key")).get());
  EXPECT_EQ(0u, cache.size());

  // Re-inserting a session does not refresh the lifetime. The expiration
  // information in the session is used.
  cache.Insert(cache.generation_number(), MakeTestKey("key"),
               bssl::UpRef(session));
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key")).get());
  EXPECT_EQ(0u, cache.size());

  // Re-insert a fresh copy of the session.
  session = MakeTestSession(clock->Now(), kTimeout);
  cache.Insert(cache.generation_number(), MakeTestKey("key"),
               bssl::UpRef(session));
  EXPECT_EQ(session.get(), cache.Lookup(MakeTestKey("key")).get());
  EXPECT_EQ(1u, cache.size());

  // Sessions also are treated as expired if the clock rewinds.
  clock->Advance(base::Seconds(-2));
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key")).get());
  EXPECT_EQ(0u, cache.size());
}

// Memory pressure listeners are disabled on Windows and Mac, so this test
// is disabled on those platforms as it relies on receiving notifications.
//
// TODO(crbug.com/483018445): Check the kSuppressMemoryMonitor feature flag
// instead of buildflags once the feature is exposed publicly or moved to base.
// Currently, it is internal to components/memory_pressure.
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
#define MAYBE_TestFlushOnMemoryNotifications \
  DISABLED_TestFlushOnMemoryNotifications
#else
#define MAYBE_TestFlushOnMemoryNotifications TestFlushOnMemoryNotifications
#endif  // Test that SSL cache is flushed on low memory notifications
TEST_F(SSLClientSessionCacheTest, MAYBE_TestFlushOnMemoryNotifications) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitAndDisableFeature(base::kStatefulMemoryPressure);
  base::test::TaskEnvironment task_environment;

  // kExpirationCheckCount is set to a suitably large number so the automated
  // pruning never triggers.
  const size_t kExpirationCheckCount = 1000;
  const base::TimeDelta kTimeout = base::Seconds(1000);

  SSLClientSessionCache::Config config;
  config.expiration_check_count = kExpirationCheckCount;
  std::unique_ptr<base::SimpleTestClock> clock = MakeTestClock();
  SSLClientSessionCache cache(config);
  cache.SetClockForTesting(clock.get());

  // Insert an entry into the session cache.
  bssl::UniquePtr<SSL_SESSION> session1 =
      MakeTestSession(clock->Now(), kTimeout);
  cache.Insert(cache.generation_number(), MakeTestKey("key1"),
               bssl::UpRef(session1));
  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(1u, cache.size());

  // Expire the session.
  clock->Advance(kTimeout * 2);
  // Add one more session.
  bssl::UniquePtr<SSL_SESSION> session2 =
      MakeTestSession(clock->Now(), kTimeout);
  cache.Insert(cache.generation_number(), MakeTestKey("key2"),
               bssl::UpRef(session2));
  EXPECT_EQ(2u, cache.size());

  // Fire a notification that will flush expired sessions.
  SimulateMemoryLimitAndRelease(task_environment,
                                base::kModerateMemoryPressureThreshold);

  // Expired session's cache should be flushed.
  // Lookup returns nullptr, when cache entry not found.
  EXPECT_FALSE(cache.Lookup(MakeTestKey("key1")));
  EXPECT_TRUE(cache.Lookup(MakeTestKey("key2")));
  EXPECT_EQ(1u, cache.size());

  // Fire notification that will flush everything.
  SimulateMemoryLimitAndRelease(task_environment,
                                base::kCriticalMemoryPressureThreshold);
  EXPECT_EQ(0u, cache.size());
}

// Memory pressure listeners are disabled on Windows and Mac, so this test
// is disabled on those platforms as it relies on receiving notifications.
//
// TODO(crbug.com/483018445): Check the kSuppressMemoryMonitor feature flag
// instead of buildflags once the feature is exposed publicly or moved to base.
// Currently, it is internal to components/memory_pressure.
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
#define MAYBE_MemoryPressure DISABLED_MemoryPressure
#else
#define MAYBE_MemoryPressure MemoryPressure
#endif
// Tests that the session cache responds correctly to memory pressure events.
TEST_F(SSLClientSessionCacheTest, MAYBE_MemoryPressure) {
  base::test::ScopedFeatureList scoped_feature_list(
      base::kStatefulMemoryPressure);
  base::test::TaskEnvironment task_environment(
      base::test::TaskEnvironment::MainThreadType::IO);

  SSLClientSessionCache::Config config;
  config.max_entries = 10;
  // Use a large expiration check count so it doesn't interfere.
  config.expiration_check_count = 1000;
  SSLClientSessionCache cache(config);

  EXPECT_EQ(10u, cache.max_size());

  // Insert 10 entries.
  for (size_t i = 0; i < 10; i++) {
    bssl::UniquePtr<SSL_SESSION> session = NewSSLSession();
    cache.Insert(cache.generation_number(),
                 MakeTestKey(base::NumberToString(i)), bssl::UpRef(session));
  }
  EXPECT_EQ(10u, cache.size());

  // Memory pressure moderate should halve the cache size.
  SimulateMemoryLimitAndRelease(task_environment,
                                base::kModerateMemoryPressureThreshold);
  EXPECT_EQ(5u, cache.max_size());
  EXPECT_EQ(5u, cache.size());

  // Verify that the oldest entries (0-4) were removed and newer (5-9) remain.
  for (size_t i = 0; i < 5; i++) {
    EXPECT_EQ(nullptr,
              cache.Lookup(MakeTestKey(base::NumberToString(i))).get());
  }
  for (size_t i = 5; i < 10; i++) {
    EXPECT_NE(nullptr,
              cache.Lookup(MakeTestKey(base::NumberToString(i))).get());
  }

  // Memory pressure critical should clear the cache.
  SimulateMemoryLimitAndRelease(task_environment, 0);
  EXPECT_EQ(0u, cache.max_size());
  EXPECT_EQ(0u, cache.size());

  // Can't add an element under critical memory pressure.
  {
    bssl::UniquePtr<SSL_SESSION> session = NewSSLSession();
    cache.Insert(cache.generation_number(),
                 MakeTestKey(base::NumberToString(67)), bssl::UpRef(session));
    EXPECT_EQ(0u, cache.size());
  }

  // Memory pressure none should restore the original size limit.
  SimulateMemoryLimitAndRelease(task_environment, 100);
  EXPECT_EQ(10u, cache.max_size());
  EXPECT_EQ(0u, cache.size());

  // We should be able to insert 10 entries again.
  for (size_t i = 0; i < 10; i++) {
    bssl::UniquePtr<SSL_SESSION> session = NewSSLSession();
    cache.Insert(cache.generation_number(),
                 MakeTestKey(base::NumberToString(i)), bssl::UpRef(session));
  }
  EXPECT_EQ(10u, cache.size());
}

// Tests that when the kIgnoreMemoryPressureForSslClientSessionCache feature is
// enabled, memory pressure events do NOT flush or resize the SSL client session
// cache.
TEST_F(SSLClientSessionCacheTest, IgnoresMemoryPressureWhenFeatureEnabled) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatures(
      {features::kIgnoreMemoryPressureForSslClientSessionCache,
       base::kStatefulMemoryPressure},
      {});
  base::test::TaskEnvironment task_environment(
      base::test::TaskEnvironment::MainThreadType::IO);

  SSLClientSessionCache::Config config;
  config.max_entries = 10;
  SSLClientSessionCache cache(config);

  EXPECT_EQ(10u, cache.max_size());

  // Insert 10 entries.
  for (size_t i = 0; i < 10; i++) {
    bssl::UniquePtr<SSL_SESSION> session = NewSSLSession();
    cache.Insert(cache.generation_number(),
                 MakeTestKey(base::NumberToString(i)), bssl::UpRef(session));
  }
  EXPECT_EQ(10u, cache.size());

  // Simulate critical memory pressure.
  SimulateMemoryLimitAndRelease(task_environment,
                                base::kCriticalMemoryPressureThreshold);

  // The cache and its size limits should remain completely unaffected.
  EXPECT_EQ(10u, cache.max_size());
  EXPECT_EQ(10u, cache.size());
  EXPECT_NE(nullptr, cache.Lookup(MakeTestKey("0")).get());
}

// Tests that when the kIgnoreMemoryPressureForSslClientSessionCache feature is
// disabled (default behavior), memory pressure events flush and resize the SSL
// client session cache.
TEST_F(SSLClientSessionCacheTest,
       ClearsCacheOnMemoryPressureWhenFeatureDisabled) {
  base::test::ScopedFeatureList scoped_feature_list;
  scoped_feature_list.InitWithFeatures(
      {base::kStatefulMemoryPressure},
      {features::kIgnoreMemoryPressureForSslClientSessionCache});
  base::test::TaskEnvironment task_environment(
      base::test::TaskEnvironment::MainThreadType::IO);

  SSLClientSessionCache::Config config;
  config.max_entries = 10;
  SSLClientSessionCache cache(config);

  EXPECT_EQ(10u, cache.max_size());

  // Insert 10 entries.
  for (size_t i = 0; i < 10; i++) {
    bssl::UniquePtr<SSL_SESSION> session = NewSSLSession();
    cache.Insert(cache.generation_number(),
                 MakeTestKey(base::NumberToString(i)), bssl::UpRef(session));
  }
  EXPECT_EQ(10u, cache.size());

  // Simulate critical memory pressure.
  SimulateMemoryLimitAndRelease(task_environment,
                                base::kCriticalMemoryPressureThreshold);

  // Eviction should have run normally.
  EXPECT_EQ(0u, cache.max_size());
  EXPECT_EQ(0u, cache.size());
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("0")).get());
}

TEST_F(SSLClientSessionCacheTest, FlushForServer) {
  SSLClientSessionCache::Config config;
  SSLClientSessionCache cache(config);

  const SchemefulSite kSiteA(GURL("https://a.test"));
  const SchemefulSite kSiteB(GURL("https://b.test"));

  // Insert a number of cache entries.
  SSLClientSessionCache::Key key1;
  key1.server = HostPortPair("a.test", 443);
  auto session1 = NewSSLSession();
  cache.Insert(cache.generation_number(), key1, bssl::UpRef(session1));

  SSLClientSessionCache::Key key2;
  key2.server = HostPortPair("a.test", 443);
  key2.dest_ip_addr = IPAddress::IPv4Localhost();
  key2.network_anonymization_key =
      NetworkAnonymizationKey::CreateSameSite(kSiteB);
  key2.privacy_mode = PRIVACY_MODE_ENABLED;
  auto session2 = NewSSLSession();
  cache.Insert(cache.generation_number(), key2, bssl::UpRef(session2));

  SSLClientSessionCache::Key key3;
  key3.server = HostPortPair("a.test", 444);
  auto session3 = NewSSLSession();
  cache.Insert(cache.generation_number(), key3, bssl::UpRef(session3));

  SSLClientSessionCache::Key key4;
  key4.server = HostPortPair("b.test", 443);
  auto session4 = NewSSLSession();
  cache.Insert(cache.generation_number(), key4, bssl::UpRef(session4));

  SSLClientSessionCache::Key key5;
  key5.server = HostPortPair("b.test", 443);
  key5.network_anonymization_key =
      NetworkAnonymizationKey::CreateSameSite(kSiteA);
  auto session5 = NewSSLSession();
  cache.Insert(cache.generation_number(), key5, bssl::UpRef(session5));

  // Flush an unrelated server. The cache should be unaffected.
  cache.FlushForServers({HostPortPair("c.test", 443)});
  EXPECT_EQ(5u, cache.size());
  EXPECT_EQ(session1.get(), cache.Lookup(key1).get());
  EXPECT_EQ(session2.get(), cache.Lookup(key2).get());
  EXPECT_EQ(session3.get(), cache.Lookup(key3).get());
  EXPECT_EQ(session4.get(), cache.Lookup(key4).get());
  EXPECT_EQ(session5.get(), cache.Lookup(key5).get());

  // Flush a.test:443. |key1| and |key2| should match, but not the others.
  cache.FlushForServers({HostPortPair("a.test", 443)});
  EXPECT_EQ(3u, cache.size());
  EXPECT_EQ(nullptr, cache.Lookup(key1).get());
  EXPECT_EQ(nullptr, cache.Lookup(key2).get());
  EXPECT_EQ(session3.get(), cache.Lookup(key3).get());
  EXPECT_EQ(session4.get(), cache.Lookup(key4).get());
  EXPECT_EQ(session5.get(), cache.Lookup(key5).get());

  // Flush b.test:443. |key4| and |key5| match, but not |key3|.
  cache.FlushForServers({HostPortPair("b.test", 443)});
  EXPECT_EQ(1u, cache.size());
  EXPECT_EQ(nullptr, cache.Lookup(key1).get());
  EXPECT_EQ(nullptr, cache.Lookup(key2).get());
  EXPECT_EQ(session3.get(), cache.Lookup(key3).get());
  EXPECT_EQ(nullptr, cache.Lookup(key4).get());
  EXPECT_EQ(nullptr, cache.Lookup(key5).get());

  // Flush the last host, a.test:444.
  cache.FlushForServers({HostPortPair("a.test", 444)});
  EXPECT_EQ(0u, cache.size());
  EXPECT_EQ(nullptr, cache.Lookup(key1).get());
  EXPECT_EQ(nullptr, cache.Lookup(key2).get());
  EXPECT_EQ(nullptr, cache.Lookup(key3).get());
  EXPECT_EQ(nullptr, cache.Lookup(key4).get());
  EXPECT_EQ(nullptr, cache.Lookup(key5).get());
}

TEST_F(SSLClientSessionCacheTest, FlushForServers) {
  SSLClientSessionCache::Config config;
  SSLClientSessionCache cache(config);

  const SchemefulSite kSiteA(GURL("https://a.test"));
  const SchemefulSite kSiteB(GURL("https://b.test"));

  // Insert a number of cache entries.
  SSLClientSessionCache::Key key1;
  key1.server = HostPortPair("a.test", 443);
  auto session1 = NewSSLSession();
  cache.Insert(cache.generation_number(), key1, bssl::UpRef(session1));

  SSLClientSessionCache::Key key2;
  key2.server = HostPortPair("a.test", 443);
  key2.dest_ip_addr = IPAddress::IPv4Localhost();
  key2.network_anonymization_key =
      NetworkAnonymizationKey::CreateSameSite(kSiteB);
  key2.privacy_mode = PRIVACY_MODE_ENABLED;
  auto session2 = NewSSLSession();
  cache.Insert(cache.generation_number(), key2, bssl::UpRef(session2));

  SSLClientSessionCache::Key key3;
  key3.server = HostPortPair("a.test", 444);
  auto session3 = NewSSLSession();
  cache.Insert(cache.generation_number(), key3, bssl::UpRef(session3));

  SSLClientSessionCache::Key key4;
  key4.server = HostPortPair("b.test", 443);
  auto session4 = NewSSLSession();
  cache.Insert(cache.generation_number(), key4, bssl::UpRef(session4));

  SSLClientSessionCache::Key key5;
  key5.server = HostPortPair("b.test", 443);
  key5.network_anonymization_key =
      NetworkAnonymizationKey::CreateSameSite(kSiteA);
  auto session5 = NewSSLSession();
  cache.Insert(cache.generation_number(), key5, bssl::UpRef(session5));

  cache.FlushForServers({
      // Unrelated server. Should have no effect.
      HostPortPair("c.test", 443),
      // Flush a.test:443. |key1| and |key2| should match, but not the others.
      HostPortPair("a.test", 443),
      // Flush b.test:443. |key4| and |key5| match, but not |key3|.
      HostPortPair("b.test", 443),
  });
  EXPECT_EQ(1u, cache.size());
  EXPECT_EQ(nullptr, cache.Lookup(key1).get());
  EXPECT_EQ(nullptr, cache.Lookup(key2).get());
  EXPECT_EQ(session3.get(), cache.Lookup(key3).get());
  EXPECT_EQ(nullptr, cache.Lookup(key4).get());
  EXPECT_EQ(nullptr, cache.Lookup(key5).get());
}

TEST_F(SSLClientSessionCacheTest, InsertStaleConfigId) {
  SSLClientSessionCache::Config config;
  SSLClientSessionCache cache(config);

  bssl::UniquePtr<SSL_SESSION> session1 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session2 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session3 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session4 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session5 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session6 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session7 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session8 = NewSSLSession();
  bssl::UniquePtr<SSL_SESSION> session9 = NewSSLSession();

  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(0u, cache.size());

  const uint64_t generation_number_0 = cache.generation_number();

  cache.Insert(generation_number_0, MakeTestKey("key1"), bssl::UpRef(session1));
  EXPECT_EQ(session1.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(1u, cache.size());

  cache.Flush();
  EXPECT_EQ(0u, cache.size());

  cache.Insert(generation_number_0, MakeTestKey("key1"), bssl::UpRef(session2));
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(0u, cache.size());

  const uint64_t generation_number_1 = cache.generation_number();

  cache.Insert(generation_number_1, MakeTestKey("key1"), bssl::UpRef(session3));
  EXPECT_EQ(session3.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(1u, cache.size());

  cache.Flush();
  EXPECT_EQ(0u, cache.size());

  cache.Insert(generation_number_0, MakeTestKey("key1"), bssl::UpRef(session4));
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(0u, cache.size());

  cache.Insert(generation_number_1, MakeTestKey("key1"), bssl::UpRef(session5));
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(0u, cache.size());

  const uint64_t generation_number_2 = cache.generation_number();

  cache.Insert(generation_number_2, MakeTestKey("key1"), bssl::UpRef(session6));
  EXPECT_EQ(session6.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(1u, cache.size());

  cache.Insert(generation_number_2, MakeTestKey("key2"), bssl::UpRef(session7));
  EXPECT_EQ(session7.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(2u, cache.size());

  // FlushForServers also increments the generation_number, but only clears
  // entries for the specified HostPortPair.
  cache.FlushForServers({HostPortPair("key2", 443)});
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  // The other cached entries that don't match the flushed server should
  // still exist.
  EXPECT_EQ(session6.get(), cache.Lookup(MakeTestKey("key1")).get());
  EXPECT_EQ(1u, cache.size());

  cache.Insert(generation_number_2, MakeTestKey("key2"), bssl::UpRef(session8));
  EXPECT_EQ(nullptr, cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(1u, cache.size());

  const uint64_t generation_number_3 = cache.generation_number();

  cache.Insert(generation_number_3, MakeTestKey("key2"), bssl::UpRef(session9));
  EXPECT_EQ(session9.get(), cache.Lookup(MakeTestKey("key2")).get());
  EXPECT_EQ(2u, cache.size());
}

}  // namespace net
