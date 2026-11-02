// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/gm_identity_provider.h"

#include <map>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "net/base/net_errors.h"
#include "net/cert/x509_certificate.h"
#include "net/gm/tests/client_cert_test_data.h"
#include "services/gm_key/public/mojom/gm_key_service.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {

using AcquireFuture =
    base::test::TestFuture<int, scoped_refptr<net::gm::ClientIdentity>>;
using SignFuture = base::test::TestFuture<int, std::vector<uint8_t>>;

net::gm::ConnectionProfile Profile(uint64_t generation = 1) {
  return {
      net::gm::TlsProtocol::kTlcp, "enterprise", 1, "feitian", generation, 1};
}

net::gm::ClientCertificateRequest MakeCertificateRequest() {
  net::gm::ClientCertificateRequest request;
  request.protocol = net::gm::TlsProtocol::kTlcp;
  request.required_roles = net::gm::kClientCertificateSigning |
                           net::gm::kClientCertificateEncryption;
  request.certificate_types = {64};
  return request;
}

gm_key::mojom::KeyIdentityPtr TestIdentity() {
  auto identity = gm_key::mojom::KeyIdentity::New();
  identity->identity_id = "simulated-feitian";
  // Device generation is distinct from the managed-policy generation.
  identity->generation = 7;
  identity->signing_certificate = net::gm::test::SigningCertificate();
  identity->encryption_certificate = net::gm::test::EncryptionCertificate();
  return identity;
}

class FakeSigner : public gm_key::mojom::KeySigner {
 public:
  mojo::PendingRemote<gm_key::mojom::KeySigner> Bind() {
    return receiver.BindNewPipeAndPassRemote();
  }
  void Sign(uint64_t request,
            const std::vector<uint8_t>& digest,
            SignCallback callback) override {
    requests.push_back(request);
    EXPECT_EQ(digest.size(), 32u);
    callbacks.emplace(request, std::move(callback));
  }
  void Cancel(uint64_t request) override { cancellations.push_back(request); }
  void Complete(uint64_t request) {
    auto found = callbacks.find(request);
    ASSERT_NE(found, callbacks.end());
    auto callback = std::move(found->second);
    callbacks.erase(found);
    auto result = gm_key::mojom::KeyResult::New();
    result->error = gm_key::mojom::KeyError::kOk;
    std::move(callback).Run(std::move(result), {0x30, 0x00});
  }
  mojo::Receiver<gm_key::mojom::KeySigner> receiver{this};
  std::vector<uint64_t> requests;
  std::vector<uint64_t> cancellations;
  std::map<uint64_t, SignCallback> callbacks;
};

class FakeBroker : public mojom::GmIdentityBroker {
 public:
  mojo::PendingRemote<mojom::GmIdentityBroker> Bind() {
    return receiver.BindNewPipeAndPassRemote();
  }
  void AcquireIdentity(uint64_t request_id,
                       const url::Origin& origin,
                       const std::string& policy_id,
                       uint64_t generation,
                       mojom::GmClientCertificateRequestPtr constraints,
                       AcquireIdentityCallback callback) override {
    origins.push_back(origin);
    requests.push_back(request_id);
    policies.push_back(policy_id);
    certificate_requests.push_back(std::move(constraints));
    generations.push_back(generation);
    callbacks.push_back(std::move(callback));
  }
  void Cancel(uint64_t request_id) override {
    cancellations.push_back(request_id);
  }
  FakeSigner* Reply(size_t index,
                    bool valid = true,
                    gm_key::mojom::KeyIdentityPtr identity = nullptr) {
    auto signer = std::make_unique<FakeSigner>();
    auto grant = mojom::GmIdentityGrant::New();
    grant->identity = identity ? std::move(identity) : TestIdentity();
    if (!valid) {
      grant->identity->signing_certificate.clear();
    }
    grant->signer = signer->Bind();
    FakeSigner* result = signer.get();
    signers.push_back(std::move(signer));
    std::move(callbacks.at(index)).Run(net::OK, std::move(grant));
    return result;
  }
  mojo::Receiver<mojom::GmIdentityBroker> receiver{this};
  std::vector<url::Origin> origins;
  std::vector<uint64_t> requests;
  std::vector<uint64_t> cancellations;
  std::vector<uint64_t> generations;
  std::vector<std::string> policies;
  std::vector<mojom::GmClientCertificateRequestPtr> certificate_requests;
  std::vector<AcquireIdentityCallback> callbacks;
  std::vector<std::unique_ptr<FakeSigner>> signers;
};

class GmIdentityProviderTest : public testing::Test {
 protected:
  GmIdentityProviderTest() {
    // Public test certificates are valid 2020--2120. Keep cache-expiry tests
    // deterministic and independent of the machine clock.
    task_environment.AdvanceClock(
        base::Time::FromSecondsSinceUnixEpoch(1800000000) - base::Time::Now());
  }
  base::UnguessableToken RegisterContext() {
    return provider.RegisterRequest(base::BindRepeating(
        [](FakeBroker* broker, const url::Origin& origin,
           const std::string& policy_id, uint64_t generation,
           mojom::GmClientCertificateRequestPtr constraints,
           mojom::GmIdentityBroker::AcquireIdentityCallback callback) {
          const uint64_t id = broker->requests.size() + 1;
          broker->AcquireIdentity(id, origin, policy_id, generation,
                                  std::move(constraints), std::move(callback));
          return base::BindOnce(&FakeBroker::Cancel, base::Unretained(broker),
                                id);
        },
        base::Unretained(&broker)));
  }
  net::gm::ClientCertificateRequest CertificateRequest() {
    auto request = MakeCertificateRequest();
    request.gm_client_auth_context = context;
    return request;
  }
  base::test::TaskEnvironment task_environment{
      base::test::TaskEnvironment::MainThreadType::IO,
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  FakeBroker broker;
  GmIdentityProvider provider{broker.Bind()};
  const base::UnguessableToken context = RegisterContext();
  const net::HostPortPair server{"gm.example", 443};
};

TEST_F(GmIdentityProviderTest, NoInteractiveContextCannotPrompt) {
  AcquireFuture required;
  provider.AcquireIdentity(server, Profile(), MakeCertificateRequest(),
                           required.GetCallback());
  EXPECT_EQ(required.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(required.Get<1>());
  AcquireFuture optional;
  net::gm::ConnectionProfile optional_profile(net::gm::TlsProtocol::kTlcp,
                                              "enterprise", 1, "", 0, 1);
  provider.AcquireIdentity(server, optional_profile, MakeCertificateRequest(),
                           optional.GetCallback());
  EXPECT_EQ(optional.Get<0>(), net::OK);
  EXPECT_FALSE(optional.Get<1>());
  EXPECT_TRUE(broker.requests.empty());
}

TEST_F(GmIdentityProviderTest,
       UnregisterCancelsOnlyItsPageAndIgnoresLateReply) {
  AcquireFuture first;
  AcquireFuture second;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           first.GetCallback());
  auto second_request = CertificateRequest();
  const auto second_context = RegisterContext();
  second_request.gm_client_auth_context = second_context;
  provider.AcquireIdentity(server, Profile(), second_request,
                           second.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.requests.size(), 2u);
  provider.UnregisterRequest(context);
  EXPECT_EQ(first.Get<0>(), net::ERR_ABORTED);
  EXPECT_EQ(broker.cancellations, std::vector<uint64_t>({broker.requests[0]}));
  EXPECT_FALSE(second.IsReady());
  broker.Reply(0);
  task_environment.RunUntilIdle();
  EXPECT_FALSE(provider.GetCachedIdentity(server, Profile()));
  broker.Reply(1);
  EXPECT_EQ(second.Get<0>(), net::OK);
  EXPECT_TRUE(second.Get<1>());
}

TEST_F(GmIdentityProviderTest, UnregisterBeforeDispatchNeverShowsPrompt) {
  AcquireFuture future;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           future.GetCallback());
  provider.UnregisterRequest(context);
  EXPECT_EQ(future.Get<0>(), net::ERR_ABORTED);
  task_environment.RunUntilIdle();
  EXPECT_TRUE(broker.requests.empty());
  EXPECT_TRUE(broker.cancellations.empty());
}

TEST_F(GmIdentityProviderTest,
       CachedAuthorizationDoesNotRequireInteractiveContext) {
  AcquireFuture first;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           first.GetCallback());
  task_environment.RunUntilIdle();
  broker.Reply(0);
  ASSERT_EQ(first.Get<0>(), net::OK);
  provider.UnregisterRequest(context);
  AcquireFuture cached;
  int resolver_calls = 0;
  auto request = MakeCertificateRequest();
  request.gm_client_auth_context_resolver = base::BindRepeating(
      [](int* calls) {
        ++*calls;
        return base::UnguessableToken::Create();
      },
      base::Unretained(&resolver_calls));
  provider.AcquireIdentity(server, Profile(), request, cached.GetCallback());
  EXPECT_EQ(cached.Get<0>(), net::OK);
  ASSERT_TRUE(cached.Get<1>());
  EXPECT_EQ(cached.Get<1>()->signing_key_id(),
            first.Get<1>()->signing_key_id());
  EXPECT_EQ(broker.requests.size(), 1u);
  EXPECT_EQ(resolver_calls, 0);
}

TEST_F(GmIdentityProviderTest, ResolvesInteractiveContextOnlyOnCacheMiss) {
  int resolver_calls = 0;
  auto request = MakeCertificateRequest();
  request.gm_client_auth_context_resolver = base::BindRepeating(
      [](int* calls, base::UnguessableToken context) {
        ++*calls;
        return context;
      },
      base::Unretained(&resolver_calls), context);
  AcquireFuture first;
  provider.AcquireIdentity(server, Profile(), request, first.GetCallback());
  EXPECT_EQ(resolver_calls, 1);
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.requests.size(), 1u);
  broker.Reply(0);
  EXPECT_EQ(first.Get<0>(), net::OK);
  AcquireFuture cached;
  provider.AcquireIdentity(server, Profile(), request, cached.GetCallback());
  EXPECT_EQ(cached.Get<0>(), net::OK);
  EXPECT_EQ(resolver_calls, 1);
  EXPECT_EQ(broker.requests.size(), 1u);
}

TEST_F(GmIdentityProviderTest,
       CoalescesAuthorizationAndReturnsIndependentLeases) {
  AcquireFuture a;
  AcquireFuture b;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           a.GetCallback());
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           b.GetCallback());
  EXPECT_FALSE(a.IsReady());
  EXPECT_FALSE(b.IsReady());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.callbacks.size(), 1u);
  broker.Reply(0);
  ASSERT_EQ(a.Get<0>(), net::OK);
  ASSERT_EQ(b.Get<0>(), net::OK);
  EXPECT_NE(a.Get<1>(), b.Get<1>());
  EXPECT_EQ(a.Get<1>()->identity_id(), b.Get<1>()->identity_id());
  EXPECT_EQ(a.Get<1>()->signing_key_id(), b.Get<1>()->signing_key_id());
  EXPECT_EQ(a.Get<1>()->encryption_key_id(), b.Get<1>()->encryption_key_id());

  AcquireFuture cached;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           cached.GetCallback());
  EXPECT_FALSE(cached.IsReady());
  EXPECT_EQ(cached.Get<0>(), net::OK);
  EXPECT_EQ(broker.callbacks.size(), 1u);
}

TEST_F(GmIdentityProviderTest, OverlappingRequestIdsCancelOnlyTheirOwnLease) {
  AcquireFuture a;
  AcquireFuture b;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           a.GetCallback());
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           b.GetCallback());
  task_environment.RunUntilIdle();
  auto* signer = broker.Reply(0);
  ASSERT_EQ(a.Get<0>(), net::OK);
  ASSERT_EQ(b.Get<0>(), net::OK);
  SignFuture first;
  SignFuture second;
  a.Get<1>()->SignDigest(1, {}, first.GetCallback());
  b.Get<1>()->SignDigest(1, {}, second.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(signer->requests, std::vector<uint64_t>({1, 2}));
  a.Get<1>()->Cancel(1);
  task_environment.RunUntilIdle();
  EXPECT_EQ(signer->cancellations, std::vector<uint64_t>({1}));
  signer->Complete(1);  // A late reply after cancellation must be ignored.
  signer->Complete(2);
  EXPECT_EQ(second.Get<0>(), net::OK);
  EXPECT_FALSE(first.IsReady());

  SignFuture third;
  b.Get<1>()->SignDigest(1, {}, third.GetCallback());
  task_environment.RunUntilIdle();
  EXPECT_EQ(signer->requests, std::vector<uint64_t>({1, 2, 3}));
  signer->Complete(3);
  EXPECT_EQ(third.Get<0>(), net::OK);
}

TEST_F(GmIdentityProviderTest,
       SignerDisconnectInvalidatesAllLeasesAndPendingSigns) {
  AcquireFuture a;
  AcquireFuture b;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           a.GetCallback());
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           b.GetCallback());
  task_environment.RunUntilIdle();
  auto* signer = broker.Reply(0);
  ASSERT_EQ(a.Get<0>(), net::OK);
  ASSERT_EQ(b.Get<0>(), net::OK);
  SignFuture signing;
  a.Get<1>()->SignDigest(1, {}, signing.GetCallback());
  task_environment.RunUntilIdle();
  signer->receiver.reset();
  EXPECT_EQ(signing.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(a.Get<1>()->is_valid());
  EXPECT_FALSE(b.Get<1>()->is_valid());
  AcquireFuture reacquire;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           reacquire.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.callbacks.size(), 2u);
  broker.Reply(1);
  EXPECT_EQ(reacquire.Get<0>(), net::OK);
}

TEST_F(GmIdentityProviderTest,
       BrokerDisconnectRevokesGrantsAndFailsPendingAcquire) {
  AcquireFuture active;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           active.GetCallback());
  task_environment.RunUntilIdle();
  broker.Reply(0);
  ASSERT_EQ(active.Get<0>(), net::OK);
  AcquireFuture pending;
  provider.AcquireIdentity(server, Profile(2), CertificateRequest(),
                           pending.GetCallback());
  task_environment.RunUntilIdle();
  broker.receiver.reset();
  EXPECT_EQ(pending.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(active.Get<1>()->is_valid());
  AcquireFuture disconnected;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           disconnected.GetCallback());
  EXPECT_FALSE(disconnected.IsReady());
  EXPECT_EQ(disconnected.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
}

TEST_F(GmIdentityProviderTest, OriginPortAndPolicyGenerationPartitionGrants) {
  AcquireFuture a;
  AcquireFuture b;
  AcquireFuture c;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           a.GetCallback());
  provider.AcquireIdentity(net::HostPortPair("gm.example", 8443), Profile(),
                           CertificateRequest(), b.GetCallback());
  provider.AcquireIdentity(server, Profile(2), CertificateRequest(),
                           c.GetCallback());
  task_environment.RunUntilIdle();
  EXPECT_EQ(broker.callbacks.size(), 3u);
  EXPECT_NE(broker.origins[0], broker.origins[1]);
  EXPECT_NE(broker.generations[0], broker.generations[2]);
  broker.Reply(0);
  broker.Reply(1);
  broker.Reply(2);
  EXPECT_EQ(a.Get<0>(), net::OK);
  EXPECT_EQ(b.Get<0>(), net::OK);
  EXPECT_EQ(c.Get<0>(), net::OK);
  EXPECT_NE(a.Get<1>()->signing_key_id(), b.Get<1>()->signing_key_id());
}

TEST_F(GmIdentityProviderTest, MalformedGrantFailsClosedWithoutCaching) {
  AcquireFuture invalid;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           invalid.GetCallback());
  task_environment.RunUntilIdle();
  broker.Reply(0, false);
  EXPECT_EQ(invalid.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(invalid.Get<1>());
  AcquireFuture retry;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           retry.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.callbacks.size(), 2u);
  broker.Reply(1);
  EXPECT_EQ(retry.Get<0>(), net::OK);
}

TEST_F(GmIdentityProviderTest, CachedIdentityNeverRequestsNewAuthorization) {
  EXPECT_FALSE(provider.GetCachedIdentity(server, Profile()));
  task_environment.RunUntilIdle();
  EXPECT_TRUE(broker.requests.empty());
  AcquireFuture authorized;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           authorized.GetCallback());
  task_environment.RunUntilIdle();
  auto* signer = broker.Reply(0);
  ASSERT_EQ(authorized.Get<0>(), net::OK);
  auto cached = provider.GetCachedIdentity(server, Profile());
  ASSERT_TRUE(cached);
  EXPECT_EQ(cached->signing_key_id(), authorized.Get<1>()->signing_key_id());
  EXPECT_FALSE(provider.GetCachedIdentity(server, Profile(2)));
  signer->receiver.reset();
  task_environment.RunUntilIdle();
  EXPECT_FALSE(provider.GetCachedIdentity(server, Profile()));
  EXPECT_EQ(broker.requests.size(), 1u);
}

TEST_F(GmIdentityProviderTest,
       ChangedAcceptedConstraintsPreserveLiveIdentityAndSessionKeyIds) {
  AcquireFuture first, changed;
  auto request = CertificateRequest();
  provider.AcquireIdentity(server, Profile(), request, first.GetCallback());
  task_environment.RunUntilIdle();
  auto* original_signer = broker.Reply(0);
  ASSERT_EQ(first.Get<0>(), net::OK);
  SignFuture pending;
  first.Get<1>()->SignDigest(11, {}, pending.GetCallback());

  request.signature_algorithms = {0x0708};
  provider.AcquireIdentity(server, Profile(), request, changed.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.callbacks.size(), 2u);
  auto* redundant_signer = broker.Reply(1);
  bool redundant_closed = false;
  redundant_signer->receiver.set_disconnect_handler(
      base::BindOnce([](bool* closed) { *closed = true; }, &redundant_closed));
  ASSERT_EQ(changed.Get<0>(), net::OK);
  task_environment.RunUntilIdle();
  EXPECT_TRUE(redundant_closed);
  EXPECT_TRUE(first.Get<1>()->is_valid());
  EXPECT_TRUE(changed.Get<1>()->is_valid());
  EXPECT_EQ(first.Get<1>()->signing_key_id(),
            changed.Get<1>()->signing_key_id());
  EXPECT_EQ(first.Get<1>()->encryption_key_id(),
            changed.Get<1>()->encryption_key_id());
  auto cached = provider.GetCachedIdentity(server, Profile());
  ASSERT_TRUE(cached);
  EXPECT_EQ(cached->signing_key_id(), first.Get<1>()->signing_key_id());

  // Closing the redundant capability must not cancel the live grant's work.
  SignFuture second;
  changed.Get<1>()->SignDigest(11, {}, second.GetCallback());
  task_environment.RunUntilIdle();
  EXPECT_TRUE(redundant_signer->requests.empty());
  ASSERT_EQ(original_signer->requests, std::vector<uint64_t>({1, 2}));
  original_signer->Complete(1);
  original_signer->Complete(2);
  EXPECT_EQ(pending.Get<0>(), net::OK);
  EXPECT_EQ(second.Get<0>(), net::OK);
  original_signer->receiver.reset();
  task_environment.RunUntilIdle();
  EXPECT_FALSE(first.Get<1>()->is_valid());
  EXPECT_FALSE(changed.Get<1>()->is_valid());
  EXPECT_FALSE(cached->is_valid());
}

TEST_F(GmIdentityProviderTest,
       ChangedIdentityContentsNeverReuseAnExistingCapability) {
  AcquireFuture first, changed;
  auto request = CertificateRequest();
  provider.AcquireIdentity(server, Profile(), request, first.GetCallback());
  task_environment.RunUntilIdle();
  broker.Reply(0);
  ASSERT_EQ(first.Get<0>(), net::OK);
  request.signature_algorithms = {0x0708};
  provider.AcquireIdentity(server, Profile(), request, changed.GetCallback());
  task_environment.RunUntilIdle();
  auto identity = TestIdentity();
  // IDs and leaf certificates agree, but the complete public identity does
  // not. A newly returned chain must not borrow an earlier signing grant.
  identity->intermediates.push_back(
      net::gm::test::ClientIntermediateCertificate());
  auto* changed_signer = broker.Reply(1, true, std::move(identity));
  ASSERT_EQ(changed.Get<0>(), net::OK);
  EXPECT_NE(first.Get<1>()->signing_key_id(),
            changed.Get<1>()->signing_key_id());
  EXPECT_FALSE(provider.GetCachedIdentity(server, Profile()));
  SignFuture signing;
  changed.Get<1>()->SignDigest(1, {}, signing.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(changed_signer->requests, std::vector<uint64_t>({1}));
  changed_signer->Complete(1);
  EXPECT_EQ(signing.Get<0>(), net::OK);
}

TEST_F(GmIdentityProviderTest, NewConstraintsNeverReviveRevokedAuthorization) {
  AcquireFuture first, changed;
  auto request = CertificateRequest();
  provider.AcquireIdentity(server, Profile(), request, first.GetCallback());
  task_environment.RunUntilIdle();
  auto* revoked_signer = broker.Reply(0);
  ASSERT_EQ(first.Get<0>(), net::OK);
  revoked_signer->receiver.reset();
  task_environment.RunUntilIdle();
  ASSERT_FALSE(first.Get<1>()->is_valid());

  request.signature_algorithms = {0x0708};
  provider.AcquireIdentity(server, Profile(), request, changed.GetCallback());
  task_environment.RunUntilIdle();
  broker.Reply(1);
  ASSERT_EQ(changed.Get<0>(), net::OK);
  EXPECT_NE(first.Get<1>()->signing_key_id(),
            changed.Get<1>()->signing_key_id());
  EXPECT_TRUE(changed.Get<1>()->is_valid());
  auto cached = provider.GetCachedIdentity(server, Profile());
  ASSERT_TRUE(cached);
  EXPECT_EQ(cached->signing_key_id(), changed.Get<1>()->signing_key_id());
}

TEST_F(GmIdentityProviderTest, ExpiredIdentityCannotBeReusedForNewConstraints) {
  AcquireFuture first, expired;
  auto request = CertificateRequest();
  provider.AcquireIdentity(server, Profile(), request, first.GetCallback());
  task_environment.RunUntilIdle();
  broker.Reply(0);
  ASSERT_EQ(first.Get<0>(), net::OK);
  auto certificate = net::X509Certificate::CreateFromBytes(
      TestIdentity()->signing_certificate);
  ASSERT_TRUE(certificate);
  task_environment.AdvanceClock(certificate->valid_expiry() -
                                base::Time::Now() + base::Seconds(1));
  EXPECT_FALSE(provider.GetCachedIdentity(server, Profile()));
  request.signature_algorithms = {0x0708};
  provider.AcquireIdentity(server, Profile(), request, expired.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.callbacks.size(), 2u);
  broker.Reply(1);
  EXPECT_EQ(expired.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(expired.Get<1>());
}

TEST_F(GmIdentityProviderTest, DifferentServerConstraintsNeverCoalesce) {
  AcquireFuture first, different;
  auto request = CertificateRequest();
  provider.AcquireIdentity(server, Profile(), request, first.GetCallback());
  request.signature_algorithms = {0x0403};  // Only ECDSA with SHA-256.
  provider.AcquireIdentity(server, Profile(), request, different.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.requests.size(), 2u);
  EXPECT_NE(broker.certificate_requests[0]->signature_algorithms,
            broker.certificate_requests[1]->signature_algorithms);
  broker.Reply(0);
  broker.Reply(1);
  EXPECT_EQ(first.Get<0>(), net::OK);
  EXPECT_EQ(different.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
}

TEST_F(GmIdentityProviderTest,
       ConnectionRequestIdsDoNotPartitionAuthorization) {
  AcquireFuture first, second;
  auto request = CertificateRequest();
  request.request_id = 11;
  provider.AcquireIdentity(server, Profile(), request, first.GetCallback());
  request.request_id = 12;
  provider.AcquireIdentity(server, Profile(), request, second.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.requests.size(), 1u);
  broker.Reply(0);
  EXPECT_EQ(first.Get<0>(), net::OK);
  EXPECT_EQ(second.Get<0>(), net::OK);
}

TEST_F(GmIdentityProviderTest, TrustEpochAndProtocolPartitionAuthorization) {
  AcquireFuture first, epoch, protocol;
  auto profile = Profile();
  provider.AcquireIdentity(server, profile, CertificateRequest(),
                           first.GetCallback());
  net::gm::ConnectionProfile other_epoch(net::gm::TlsProtocol::kTlcp,
                                         "enterprise", 2, "feitian", 1, 2);
  provider.AcquireIdentity(server, other_epoch, CertificateRequest(),
                           epoch.GetCallback());
  net::gm::ConnectionProfile other_protocol(net::gm::TlsProtocol::kRfc8998,
                                            "enterprise", 1, "feitian", 1, 1);
  auto request = CertificateRequest();
  request.protocol = net::gm::TlsProtocol::kRfc8998;
  request.required_roles = net::gm::kClientCertificateSigning;
  request.signature_algorithms = {0x0708};
  request.certificate_types.clear();
  provider.AcquireIdentity(server, other_protocol, request,
                           protocol.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.requests.size(), 3u);
  for (size_t index = 0; index < 3; ++index) {
    broker.Reply(index);
  }
  EXPECT_EQ(first.Get<0>(), net::OK);
  EXPECT_EQ(epoch.Get<0>(), net::OK);
  EXPECT_EQ(protocol.Get<0>(), net::OK);
}

TEST_F(GmIdentityProviderTest, AutomaticIdentityStillUsesBrowserAuthorization) {
  const net::gm::ConnectionProfile profile(net::gm::TlsProtocol::kTlcp,
                                           "windows-system", 4, "", 0, 4);
  AcquireFuture result;
  provider.AcquireIdentity(server, profile, CertificateRequest(),
                           result.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.requests.size(), 1u);
  EXPECT_TRUE(broker.policies[0].empty());
  EXPECT_EQ(broker.generations[0], 0u);
  broker.Reply(0);
  EXPECT_EQ(result.Get<0>(), net::OK);
}

TEST_F(GmIdentityProviderTest, JointPolicyUsesConcreteRfcRequestAndOwnGrant) {
  using net::gm::TlsProtocol;
  const net::gm::ConnectionProfile joint(TlsProtocol::kTls13Auto, "enterprise",
                                         1, "feitian", 1, 1);
  const net::gm::ConnectionProfile strict(TlsProtocol::kRfc8998, "enterprise",
                                          1, "feitian", 1, 1);
  auto request = CertificateRequest();
  request.protocol = TlsProtocol::kRfc8998;
  request.required_roles = net::gm::kClientCertificateSigning;
  request.signature_algorithms = {0x0708};
  request.certificate_types.clear();
  AcquireFuture joint_result, strict_result;
  provider.AcquireIdentity(server, joint, request, joint_result.GetCallback());
  provider.AcquireIdentity(server, strict, request,
                           strict_result.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.requests.size(), 2u);
  for (size_t index = 0; index < 2; ++index) {
    EXPECT_EQ(broker.certificate_requests[index]->protocol,
              mojom::GmTlsProtocol::kRfc8998);
    EXPECT_EQ(broker.certificate_requests[index]->required_roles,
              net::gm::kClientCertificateSigning);
    broker.Reply(index);
  }
  ASSERT_EQ(joint_result.Get<0>(), net::OK);
  ASSERT_EQ(strict_result.Get<0>(), net::OK);
  const auto cached_joint = provider.GetCachedIdentity(server, joint);
  const auto cached_strict = provider.GetCachedIdentity(server, strict);
  ASSERT_TRUE(cached_joint);
  ASSERT_TRUE(cached_strict);
  EXPECT_NE(cached_joint->signing_key_id(), cached_strict->signing_key_id());
  AcquireFuture cached;
  request.gm_client_auth_context = {};
  provider.AcquireIdentity(server, joint, request, cached.GetCallback());
  EXPECT_EQ(cached.Get<0>(), net::OK);
  EXPECT_EQ(broker.requests.size(), 2u);
}

TEST_F(GmIdentityProviderTest, JointPolicyRejectsAmbiguousOrTlcpKeyRequests) {
  using net::gm::TlsProtocol;
  const net::gm::ConnectionProfile joint(TlsProtocol::kTls13Auto, "enterprise",
                                         1, "feitian", 1, 1);
  for (TlsProtocol protocol : {TlsProtocol::kTls13Auto, TlsProtocol::kTlcp,
                               TlsProtocol::kStandardTls}) {
    SCOPED_TRACE(static_cast<int>(protocol));
    auto request = CertificateRequest();
    request.protocol = protocol;
    if (protocol != TlsProtocol::kTlcp) {
      request.required_roles = net::gm::kClientCertificateSigning;
      request.signature_algorithms = {0x0708};
    }
    AcquireFuture result;
    provider.AcquireIdentity(server, joint, request, result.GetCallback());
    EXPECT_EQ(result.Get<0>(),
              net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    EXPECT_FALSE(result.Get<1>());
  }
  EXPECT_TRUE(broker.requests.empty());
}

TEST_F(GmIdentityProviderTest, OptionalNoCertificateDecisionIsNotCached) {
  const net::gm::ConnectionProfile profile(net::gm::TlsProtocol::kTlcp,
                                           "windows-system", 4, "", 0, 4);
  AcquireFuture first, second;
  provider.AcquireIdentity(server, profile, CertificateRequest(),
                           first.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.callbacks.size(), 1u);
  std::move(broker.callbacks[0]).Run(net::OK, nullptr);
  EXPECT_EQ(first.Get<0>(), net::OK);
  EXPECT_FALSE(first.Get<1>());
  EXPECT_FALSE(provider.GetCachedIdentity(server, profile));
  provider.AcquireIdentity(server, profile, CertificateRequest(),
                           second.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.callbacks.size(), 2u);
  broker.Reply(1);
  EXPECT_EQ(second.Get<0>(), net::OK);
  EXPECT_TRUE(second.Get<1>());
}

TEST_F(GmIdentityProviderTest,
       BrokerCertificateRequestCannotEnterStandardTlsRetry) {
  AcquireFuture result;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           result.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.callbacks.size(), 1u);
  std::move(broker.callbacks.front())
      .Run(net::ERR_SSL_CLIENT_AUTH_CERT_NEEDED, nullptr);
  EXPECT_EQ(result.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(result.Get<1>());
}

TEST_F(GmIdentityProviderTest, CancelsBrokerOnlyAfterLastWaiterWithdraws) {
  AcquireFuture first;
  AcquireFuture second;
  auto cancel_first = provider.AcquireIdentity(
      server, Profile(), CertificateRequest(), first.GetCallback());
  auto cancel_second = provider.AcquireIdentity(
      server, Profile(), CertificateRequest(), second.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.requests.size(), 1u);
  std::move(cancel_first).Run();
  task_environment.RunUntilIdle();
  EXPECT_TRUE(broker.cancellations.empty());
  std::move(cancel_second).Run();
  task_environment.RunUntilIdle();
  EXPECT_EQ(broker.cancellations, broker.requests);
  broker.Reply(0);
  task_environment.RunUntilIdle();
  EXPECT_FALSE(first.IsReady());
  EXPECT_FALSE(second.IsReady());
}

TEST_F(GmIdentityProviderTest,
       CancelledWaiterCannotCancelRemainingAuthorization) {
  AcquireFuture first;
  AcquireFuture second;
  auto cancel = provider.AcquireIdentity(
      server, Profile(), CertificateRequest(), first.GetCallback());
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           second.GetCallback());
  task_environment.RunUntilIdle();
  std::move(cancel).Run();
  broker.Reply(0);
  EXPECT_EQ(second.Get<0>(), net::OK);
  EXPECT_FALSE(first.IsReady());
  EXPECT_TRUE(broker.cancellations.empty());
}

TEST_F(GmIdentityProviderTest,
       CancelledOldReplyCannotCompleteNewRequestForSameOrigin) {
  AcquireFuture first;
  auto cancel = provider.AcquireIdentity(
      server, Profile(), CertificateRequest(), first.GetCallback());
  task_environment.RunUntilIdle();
  std::move(cancel).Run();
  AcquireFuture next;
  provider.AcquireIdentity(server, Profile(), CertificateRequest(),
                           next.GetCallback());
  task_environment.RunUntilIdle();
  ASSERT_EQ(broker.requests.size(), 2u);
  EXPECT_LT(broker.requests[0], broker.requests[1]);
  broker.Reply(0);
  task_environment.RunUntilIdle();
  EXPECT_FALSE(first.IsReady());
  EXPECT_FALSE(next.IsReady());
  broker.Reply(1);
  EXPECT_EQ(next.Get<0>(), net::OK);
}

}  // namespace
}  // namespace network
