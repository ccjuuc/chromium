// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_key_manager.h"

#include <deque>
#include <map>
#include <set>

#include "base/functional/bind.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "chrome/browser/gm/gm_policy.h"
#include "chrome/grit/generated_resources.h"
#include "components/prefs/testing_pref_service.h"
#include "net/base/net_errors.h"
#include "net/gm/tests/client_cert_test_data.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/views/widget/widget.h"

namespace gm_browser {
namespace {
using gm_key::mojom::KeyError;
using gm_key::mojom::KeyIdentity;
using gm_key::mojom::KeyIdentityPtr;
using gm_key::mojom::KeyResult;
using gm_key::mojom::KeyResultPtr;
using gm_key::mojom::KeyService;
using gm_key::mojom::KeySigner;

KeyResultPtr Result(KeyError error = KeyError::kOk) {
  auto result = KeyResult::New();
  result->error = error;
  return result;
}
KeyIdentityPtr Identity() {
  auto identity = KeyIdentity::New();
  identity->identity_id = "test-device-instance";
  identity->generation = 1;
  identity->signing_certificate = net::gm::test::SigningCertificate();
  identity->encryption_certificate = net::gm::test::EncryptionCertificate();
  return identity;
}

network::mojom::GmClientCertificateRequestPtr CertificateRequest() {
  auto request = network::mojom::GmClientCertificateRequest::New();
  request->protocol = network::mojom::GmTlsProtocol::kTlcp;
  request->required_roles = net::gm::kClientCertificateSigning |
                            net::gm::kClientCertificateEncryption;
  request->certificate_types = {64};
  return request;
}

class FakeService : public KeyService, public KeySigner {
 public:
  mojo::PendingRemote<KeyService> Bind() {
    ++launches;
    receiver.reset();
    return receiver.BindNewPipeAndPassRemote();
  }
  void Configure(gm_key::mojom::DriverConfigPtr,
                 mojo::PendingRemote<gm_key::mojom::KeyObserver> remote,
                 ConfigureCallback callback) override {
    ++configurations;
    observer.reset();
    observer.Bind(std::move(remote));
    if (hold_configuration) {
      pending_configuration = std::move(callback);
      return;
    }
    std::move(callback).Run(Result());
  }
  void Enumerate(EnumerateCallback callback) override {
    if (hold_enumeration) {
      pending_enumerations.push_back(std::move(callback));
      return;
    }
    ReplyEnumeration(std::move(callback));
  }
  void ReplyEnumeration(EnumerateCallback callback) {
    std::vector<KeyIdentityPtr> identities;
    identities.push_back(enumerated_identity ? enumerated_identity->Clone()
                                             : Identity());
    std::move(callback).Run(Result(), std::move(identities));
  }
  void Authorize(uint64_t request_id,
                 const std::string&,
                 uint64_t,
                 gm_key::SecurePin pin,
                 mojo::PendingReceiver<KeySigner> signer,
                 AuthorizeCallback callback) override {
    ++authorizations;
    authorization_ids.push_back(request_id);
    EXPECT_EQ(pin.value(), (std::vector<uint8_t>{'1', '2', '3', '4'}));
    if (hold_authorization) {
      pending_authorizations.push_back(
          {request_id, std::move(signer), std::move(callback)});
      return;
    }
    if (fail_pin_once) {
      fail_pin_once = false;
      auto result = Result(KeyError::kPinIncorrect);
      result->pin_retries = 2;
      std::move(callback).Run(std::move(result), nullptr);
      return;
    }
    prepared.emplace(request_id, std::move(signer));
    std::move(callback).Run(
        Result(), returned_identity ? returned_identity->Clone() : Identity());
  }
  void CommitAuthorization(uint64_t request_id) override {
    commits.push_back(request_id);
    auto found = prepared.find(request_id);
    ASSERT_NE(found, prepared.end());
    EXPECT_FALSE(cancelled.contains(request_id));
    signers.Add(this, std::move(found->second));
    prepared.erase(found);
    authorized = true;
  }
  void CancelAuthorization(uint64_t request_id) override {
    cancellations.push_back(request_id);
    cancelled.insert(request_id);
    prepared.erase(request_id);
  }
  void GrantAuthorized(const std::string&,
                       uint64_t,
                       mojo::PendingReceiver<KeySigner> signer,
                       GrantAuthorizedCallback callback) override {
    ++grants;
    if (!authorized) {
      std::move(callback).Run(Result(KeyError::kPinRequired), nullptr);
      return;
    }
    signers.Add(this, std::move(signer));
    std::move(callback).Run(Result(), Identity());
  }
  void Lock(const std::string&, LockCallback callback) override {
    signers.Clear();
    authorized = false;
    std::move(callback).Run(Result());
  }
  void Sign(uint64_t,
            const std::vector<uint8_t>&,
            SignCallback callback) override {
    std::move(callback).Run(Result(), {1, 2});
  }
  void Cancel(uint64_t) override {}
  void CompleteConfiguration() {
    std::move(pending_configuration).Run(Result());
  }
  void CompleteEnumeration() {
    ASSERT_FALSE(pending_enumerations.empty());
    auto callback = std::move(pending_enumerations.front());
    pending_enumerations.pop_front();
    ReplyEnumeration(std::move(callback));
  }
  void CompleteAuthorization() {
    ASSERT_FALSE(pending_authorizations.empty());
    auto pending = std::move(pending_authorizations.front());
    pending_authorizations.pop_front();
    if (!cancelled.contains(pending.request_id)) {
      prepared.emplace(pending.request_id, std::move(pending.signer));
    }
    // Deliberately deliver a late successful reply even after cancellation.
    // The browser must not commit it or grant the next queued request.
    std::move(pending.callback).Run(Result(), Identity());
  }
  struct PendingAuthorization {
    uint64_t request_id;
    mojo::PendingReceiver<KeySigner> signer;
    AuthorizeCallback callback;
  };
  int launches = 0;
  int configurations = 0;
  int authorizations = 0;
  int grants = 0;
  bool authorized = false;
  std::vector<uint64_t> authorization_ids;
  std::vector<uint64_t> commits;
  std::vector<uint64_t> cancellations;
  std::set<uint64_t> cancelled;
  std::map<uint64_t, mojo::PendingReceiver<KeySigner>> prepared;
  bool fail_pin_once = false;
  bool hold_configuration = false;
  bool hold_enumeration = false;
  bool hold_authorization = false;
  KeyIdentityPtr enumerated_identity;
  KeyIdentityPtr returned_identity;
  ConfigureCallback pending_configuration;
  std::deque<EnumerateCallback> pending_enumerations;
  std::deque<PendingAuthorization> pending_authorizations;
  mojo::Receiver<KeyService> receiver{this};
  mojo::Remote<gm_key::mojom::KeyObserver> observer;
  mojo::ReceiverSet<KeySigner> signers;
};

class GmKeyManagerTest : public testing::Test {
 protected:
  void SetUp() override {
    RegisterPrefs(prefs_.registry());
    auto rule = base::DictValue()
                    .Set("host", "gm.example")
                    .Set("protocol", "tlcp")
                    .Set("trust_profile_id", "root")
                    .Set("trust_generation", 1)
                    .Set("identity_policy_id", "corporate-key")
                    .Set("identity_generation", 2)
                    .Set("policy_revision", 3)
                    .Set("revocation", "disabled")
                    .Set("trust_anchors", base::ListValue().Append("AQID"));
    prefs_.SetManagedPref(
        kSiteRulesPref, base::Value(base::ListValue().Append(std::move(rule))));
    prefs_.SetManagedPref(
        kDriverPref, base::Value(base::DictValue()
                                     .Set("path", "C:\\Fake\\SKF.dll")
                                     .Set("sha256", std::string(64, '1'))));
    manager_ = std::make_unique<KeyManager>(&prefs_);
    manager_->SetTestHooks(
        base::BindRepeating(&FakeService::Bind, base::Unretained(&service_)),
        base::BindRepeating(&GmKeyManagerTest::PromptForTest,
                            base::Unretained(this)));
    broker_.Bind(manager_->Bind());
  }
  base::WeakPtr<views::Widget> PromptForTest(
      const std::string& origin,
      const std::vector<std::u16string>& labels,
      const std::u16string& message,
      PinCallback callback) {
    EXPECT_EQ(origin, "https://gm.example");
    EXPECT_EQ(labels.size(), 1u);
    ++prompts_;
    last_message_ = message;
    if (hold_prompt_) {
      held_prompt_ = std::move(callback);
      return {};
    }
    auto pin = std::make_unique<gm_key::SecurePin>(
        std::vector<uint8_t>{'1', '2', '3', '4'});
    std::move(callback).Run(0, std::move(pin));
    return {};
  }
  uint64_t Acquire(
      base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr>* future) {
    const uint64_t request_id = next_request_id_++;
    broker_->AcquireIdentity(
        request_id, url::Origin::Create(GURL("https://gm.example")),
        "corporate-key", 2, CertificateRequest(), future->GetCallback());
    return request_id;
  }
  void SetSiteProtocol(const char* protocol) {
    auto rules = prefs_.GetList(kSiteRulesPref).Clone();
    rules[0].GetDict().Set("protocol", protocol);
    prefs_.SetManagedPref(kSiteRulesPref, base::Value(std::move(rules)));
  }
  void CompletePrompt(PinCallback callback) {
    std::move(callback).Run(0, std::make_unique<gm_key::SecurePin>(
                                   std::vector<uint8_t>{'1', '2', '3', '4'}));
  }
  base::test::TaskEnvironment tasks_;
  base::test::ScopedRunLoopTimeout timeout_{FROM_HERE, base::Seconds(10)};
  TestingPrefServiceSimple prefs_;
  FakeService service_;
  std::unique_ptr<KeyManager> manager_;
  mojo::Remote<network::mojom::GmIdentityBroker> broker_;
  int prompts_ = 0;
  bool hold_prompt_ = false;
  PinCallback held_prompt_;
  std::u16string last_message_;
  uint64_t next_request_id_ = 1;
};

TEST_F(GmKeyManagerTest, ConcurrentRequestsPromptOnceAndIssueSeparateGrants) {
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> first, second;
  Acquire(&first);
  Acquire(&second);
  EXPECT_EQ(first.Get<0>(), net::OK);
  EXPECT_EQ(second.Get<0>(), net::OK);
  ASSERT_TRUE(first.Get<1>());
  ASSERT_TRUE(second.Get<1>());
  EXPECT_EQ(prompts_, 1);
  EXPECT_EQ(service_.authorizations, 1);
  EXPECT_EQ(service_.grants, 1);
  tasks_.RunUntilIdle();
  EXPECT_EQ(service_.commits, service_.authorization_ids);
  EXPECT_TRUE(service_.cancellations.empty());
}

TEST_F(GmKeyManagerTest,
       RejectsUnmanagedOriginAndMismatchedIdentityGeneration) {
  for (const char* site : {"https://other.example", "http://gm.example"}) {
    base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> future;
    broker_->AcquireIdentity(next_request_id_++,
                             url::Origin::Create(GURL(site)), "corporate-key",
                             2, CertificateRequest(), future.GetCallback());
    EXPECT_NE(future.Get<0>(), net::OK);
    EXPECT_FALSE(future.Get<1>());
  }
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> future;
  broker_->AcquireIdentity(
      next_request_id_++, url::Origin::Create(GURL("https://gm.example")),
      "corporate-key", 1, CertificateRequest(), future.GetCallback());
  EXPECT_NE(future.Get<0>(), net::OK);
  EXPECT_EQ(prompts_, 0);
}

TEST_F(GmKeyManagerTest, IncorrectPinRequiresAnotherExplicitPrompt) {
  service_.fail_pin_once = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> future;
  Acquire(&future);
  EXPECT_EQ(future.Get<0>(), net::OK);
  EXPECT_EQ(prompts_, 2);
  EXPECT_EQ(service_.authorizations, 2);
  EXPECT_EQ(last_message_,
            l10n_util::GetStringUTF16(IDS_GM_PIN_INCORRECT) + u" " +
                l10n_util::GetStringFUTF16(IDS_GM_PIN_RETRIES, u"2"));
  tasks_.RunUntilIdle();
  ASSERT_EQ(service_.authorization_ids.size(), 2u);
  EXPECT_EQ(service_.cancellations,
            std::vector<uint64_t>({service_.authorization_ids[0]}));
  EXPECT_EQ(service_.commits,
            std::vector<uint64_t>({service_.authorization_ids[1]}));
}

TEST_F(GmKeyManagerTest, ServerConstraintsFilterBeforePinPrompt) {
  for (int kind = 0; kind < 3; ++kind) {
    auto request = CertificateRequest();
    if (kind == 0) {
      request->signature_algorithms = {0x0403};
    } else if (kind == 1) {
      request->certificate_types = {1};  // RSA signing only.
    } else {
      request->certificate_authorities = {{0x30, 0x00}};
    }
    base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> result;
    broker_->AcquireIdentity(
        next_request_id_++, url::Origin::Create(GURL("https://gm.example")),
        "corporate-key", 2, std::move(request), result.GetCallback());
    EXPECT_NE(result.Get<0>(), net::OK);
  }
  EXPECT_EQ(prompts_, 0);
  EXPECT_EQ(service_.authorizations, 0);
}

TEST_F(GmKeyManagerTest, JointPolicyAuthorizesOnlyConcreteRfc8998Request) {
  SetSiteProtocol("tls13_auto");
  for (auto protocol : {network::mojom::GmTlsProtocol::kTlcp,
                        network::mojom::GmTlsProtocol::kTls13Auto}) {
    auto request = CertificateRequest();
    request->protocol = protocol;
    if (protocol == network::mojom::GmTlsProtocol::kTls13Auto) {
      request->required_roles = net::gm::kClientCertificateSigning;
      request->signature_algorithms = {0x0708};
    }
    base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> result;
    broker_->AcquireIdentity(
        next_request_id_++, url::Origin::Create(GURL("https://gm.example")),
        "corporate-key", 2, std::move(request), result.GetCallback());
    EXPECT_EQ(result.Get<0>(),
              net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    EXPECT_FALSE(result.Get<1>());
  }
  EXPECT_EQ(prompts_, 0);
  EXPECT_EQ(service_.authorizations, 0);
  auto request = CertificateRequest();
  request->protocol = network::mojom::GmTlsProtocol::kRfc8998;
  request->required_roles = net::gm::kClientCertificateSigning;
  request->signature_algorithms = {0x0708};
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> result;
  broker_->AcquireIdentity(
      next_request_id_++, url::Origin::Create(GURL("https://gm.example")),
      "corporate-key", 2, std::move(request), result.GetCallback());
  EXPECT_EQ(result.Get<0>(), net::OK);
  EXPECT_TRUE(result.Get<1>());
  EXPECT_EQ(prompts_, 1);
  EXPECT_EQ(service_.authorizations, 1);
}

TEST_F(GmKeyManagerTest, StrictRfcPolicyDoesNotAcceptJointRequestMarker) {
  SetSiteProtocol("rfc8998");
  auto request = CertificateRequest();
  request->protocol = network::mojom::GmTlsProtocol::kTls13Auto;
  request->required_roles = net::gm::kClientCertificateSigning;
  request->signature_algorithms = {0x0708};
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> result;
  broker_->AcquireIdentity(
      next_request_id_++, url::Origin::Create(GURL("https://gm.example")),
      "corporate-key", 2, std::move(request), result.GetCallback());
  EXPECT_EQ(result.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(result.Get<1>());
  EXPECT_EQ(prompts_, 0);
  EXPECT_EQ(service_.launches, 0);
}

TEST_F(GmKeyManagerTest, RejectsWrongPurposeAndCurveBeforePin) {
  for (auto certificate :
       {net::gm::test::ServerCertificate(), net::gm::test::P256Certificate()}) {
    service_.enumerated_identity = Identity();
    service_.enumerated_identity->signing_certificate = std::move(certificate);
    base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> result;
    Acquire(&result);
    EXPECT_NE(result.Get<0>(), net::OK);
  }
  EXPECT_EQ(prompts_, 0);
}

TEST_F(GmKeyManagerTest, RevalidatesIdentityReturnedByAuthorization) {
  service_.returned_identity = Identity();
  service_.returned_identity->signing_certificate =
      net::gm::test::ServerCertificate();
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> result;
  Acquire(&result);
  EXPECT_NE(result.Get<0>(), net::OK);
  EXPECT_FALSE(result.Get<1>());
  EXPECT_EQ(prompts_, 1);
  tasks_.RunUntilIdle();
  EXPECT_TRUE(service_.commits.empty());
  EXPECT_EQ(service_.cancellations, service_.authorization_ids);
  EXPECT_TRUE(service_.prepared.empty());
}

TEST_F(GmKeyManagerTest, CachedSelectionMustMatchNewCertificateRequest) {
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> first, next;
  Acquire(&first);
  ASSERT_EQ(first.Get<0>(), net::OK);
  auto request = CertificateRequest();
  request->signature_algorithms = {0x0403};
  broker_->AcquireIdentity(
      next_request_id_++, url::Origin::Create(GURL("https://gm.example")),
      "corporate-key", 2, std::move(request), next.GetCallback());
  EXPECT_NE(next.Get<0>(), net::OK);
  EXPECT_EQ(prompts_, 1);
  EXPECT_EQ(service_.grants, 0);
}

TEST_F(GmKeyManagerTest,
       AutomaticIdentityRequiresDriverAndCannotOverridePolicy) {
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> conflict;
  broker_->AcquireIdentity(next_request_id_++,
                           url::Origin::Create(GURL("https://gm.example")), "",
                           0, CertificateRequest(), conflict.GetCallback());
  EXPECT_NE(conflict.Get<0>(), net::OK);
  EXPECT_EQ(service_.launches, 0);
  prefs_.SetManagedPref(kSiteRulesPref, base::Value(base::ListValue()));
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> automatic;
  broker_->AcquireIdentity(next_request_id_++,
                           url::Origin::Create(GURL("https://gm.example")), "",
                           0, CertificateRequest(), automatic.GetCallback());
  EXPECT_EQ(automatic.Get<0>(), net::OK);
  EXPECT_EQ(prompts_, 1);
  prefs_.RemoveManagedPref(kDriverPref);
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> no_driver;
  broker_->AcquireIdentity(next_request_id_++,
                           url::Origin::Create(GURL("https://gm.example")), "",
                           0, CertificateRequest(), no_driver.GetCallback());
  EXPECT_EQ(no_driver.Get<0>(), net::OK);
  EXPECT_FALSE(no_driver.Get<1>());
  EXPECT_EQ(prompts_, 1);
}

TEST_F(GmKeyManagerTest, PolicyChangeCancelsPromptAndDiscardsLatePin) {
  hold_prompt_ = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> future;
  Acquire(&future);
  tasks_.RunUntilIdle();
  ASSERT_TRUE(held_prompt_);
  prefs_.SetManagedPref(kSiteRulesPref, base::Value(base::ListValue()));
  EXPECT_EQ(future.Get<0>(), net::ERR_NETWORK_CHANGED);
  auto pin = std::make_unique<gm_key::SecurePin>(
      std::vector<uint8_t>{'1', '2', '3', '4'});
  std::move(held_prompt_).Run(0, std::move(pin));
  tasks_.RunUntilIdle();
  EXPECT_EQ(service_.authorizations, 0);
}

TEST_F(GmKeyManagerTest, DeviceRemovalCancelsPendingAuthorization) {
  hold_prompt_ = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> future;
  Acquire(&future);
  tasks_.RunUntilIdle();
  ASSERT_TRUE(held_prompt_);
  service_.observer->OnIdentitiesChanged();
  EXPECT_EQ(future.Get<0>(),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(future.Get<1>());
}

TEST_F(GmKeyManagerTest, ServiceCrashCompletesQueuedRequests) {
  hold_prompt_ = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> first, second;
  Acquire(&first);
  Acquire(&second);
  tasks_.RunUntilIdle();
  service_.receiver.reset();
  EXPECT_NE(first.Get<0>(), net::OK);
  EXPECT_NE(second.Get<0>(), net::OK);
}

TEST_F(GmKeyManagerTest, CancelPromptDiscardsLatePinAndContinuesTheQueue) {
  hold_prompt_ = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> first, second;
  const auto first_id = Acquire(&first);
  tasks_.RunUntilIdle();
  ASSERT_TRUE(held_prompt_);
  auto stale_prompt = std::move(held_prompt_);
  Acquire(&second);
  broker_->Cancel(first_id);
  EXPECT_EQ(first.Get<0>(), net::ERR_ABORTED);
  tasks_.RunUntilIdle();
  ASSERT_TRUE(held_prompt_);
  EXPECT_EQ(prompts_, 2);
  CompletePrompt(std::move(stale_prompt));
  tasks_.RunUntilIdle();
  EXPECT_EQ(service_.authorizations, 0);
  EXPECT_FALSE(second.IsReady());
  CompletePrompt(std::move(held_prompt_));
  EXPECT_EQ(second.Get<0>(), net::OK);
  EXPECT_EQ(service_.authorizations, 1);
  EXPECT_EQ(service_.launches, 1);
}

TEST_F(GmKeyManagerTest, CancelWhileConfiguringKeepsStartupForNextRequest) {
  service_.hold_configuration = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> first, second;
  const auto first_id = Acquire(&first);
  Acquire(&second);
  tasks_.RunUntilIdle();
  ASSERT_TRUE(service_.pending_configuration);
  broker_->Cancel(first_id);
  EXPECT_EQ(first.Get<0>(), net::ERR_ABORTED);
  EXPECT_FALSE(second.IsReady());
  service_.CompleteConfiguration();
  EXPECT_EQ(second.Get<0>(), net::OK);
  EXPECT_EQ(service_.launches, 1);
  EXPECT_EQ(service_.configurations, 1);
  EXPECT_EQ(prompts_, 1);
}

TEST_F(GmKeyManagerTest, CancelledEnumerationCannotPromptForTheNextRequest) {
  service_.hold_enumeration = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> first, second;
  const auto first_id = Acquire(&first);
  Acquire(&second);
  tasks_.RunUntilIdle();
  ASSERT_EQ(service_.pending_enumerations.size(), 1u);
  broker_->Cancel(first_id);
  EXPECT_EQ(first.Get<0>(), net::ERR_ABORTED);
  tasks_.RunUntilIdle();
  ASSERT_EQ(service_.pending_enumerations.size(), 2u);
  service_.CompleteEnumeration();
  tasks_.RunUntilIdle();
  EXPECT_EQ(prompts_, 0);
  EXPECT_FALSE(second.IsReady());
  service_.CompleteEnumeration();
  EXPECT_EQ(second.Get<0>(), net::OK);
  EXPECT_EQ(prompts_, 1);
}

TEST_F(GmKeyManagerTest, CancelledAuthorizationCannotGrantTheNextRequest) {
  service_.hold_authorization = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> first, second;
  const auto first_id = Acquire(&first);
  Acquire(&second);
  tasks_.RunUntilIdle();
  ASSERT_EQ(service_.pending_authorizations.size(), 1u);
  broker_->Cancel(first_id);
  EXPECT_EQ(first.Get<0>(), net::ERR_ABORTED);
  tasks_.RunUntilIdle();
  ASSERT_EQ(service_.pending_authorizations.size(), 2u);
  EXPECT_EQ(service_.cancellations,
            std::vector<uint64_t>({service_.authorization_ids[0]}));
  service_.CompleteAuthorization();
  tasks_.RunUntilIdle();
  EXPECT_FALSE(second.IsReady());
  EXPECT_EQ(prompts_, 2);
  EXPECT_EQ(service_.signers.size(), 0u);
  EXPECT_TRUE(service_.commits.empty());
  EXPECT_TRUE(service_.prepared.empty());
  service_.CompleteAuthorization();
  EXPECT_EQ(second.Get<0>(), net::OK);
  tasks_.RunUntilIdle();
  EXPECT_EQ(service_.commits,
            std::vector<uint64_t>({service_.authorization_ids[1]}));
  EXPECT_EQ(service_.signers.size(), 1u);
}

TEST_F(GmKeyManagerTest, RequestIdsAreMonotonicWithinEachBrokerPipe) {
  hold_prompt_ = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> pending;
  const auto id = Acquire(&pending);
  tasks_.RunUntilIdle();
  for (uint64_t invalid_id : {uint64_t{0}, id}) {
    base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> rejected;
    broker_->AcquireIdentity(
        invalid_id, url::Origin::Create(GURL("https://gm.example")),
        "corporate-key", 2, CertificateRequest(), rejected.GetCallback());
    EXPECT_EQ(rejected.Get<0>(), net::ERR_INVALID_ARGUMENT);
    EXPECT_FALSE(rejected.Get<1>());
  }
  broker_->Cancel(id);
  EXPECT_EQ(pending.Get<0>(), net::ERR_ABORTED);
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> reused;
  broker_->AcquireIdentity(id, url::Origin::Create(GURL("https://gm.example")),
                           "corporate-key", 2, CertificateRequest(),
                           reused.GetCallback());
  EXPECT_EQ(reused.Get<0>(), net::ERR_INVALID_ARGUMENT);
  EXPECT_EQ(prompts_, 1);
}

TEST_F(GmKeyManagerTest, CancelIsScopedToTheCallingBroker) {
  hold_prompt_ = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> first, other;
  const auto first_id = Acquire(&first);
  tasks_.RunUntilIdle();
  mojo::Remote<network::mojom::GmIdentityBroker> second_broker(
      manager_->Bind());
  second_broker->Cancel(first_id);
  tasks_.RunUntilIdle();
  EXPECT_FALSE(first.IsReady());
  second_broker->AcquireIdentity(
      first_id, url::Origin::Create(GURL("https://gm.example")),
      "corporate-key", 2, CertificateRequest(), other.GetCallback());
  second_broker->Cancel(first_id);
  EXPECT_EQ(other.Get<0>(), net::ERR_ABORTED);
  EXPECT_FALSE(first.IsReady());
  EXPECT_EQ(prompts_, 1);
  CompletePrompt(std::move(held_prompt_));
  EXPECT_EQ(first.Get<0>(), net::OK);
}

TEST_F(GmKeyManagerTest, BrokerDisconnectDropsItsRequestsAndLatePin) {
  hold_prompt_ = true;
  base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr> first, queued,
      other;
  Acquire(&first);
  Acquire(&queued);
  tasks_.RunUntilIdle();
  ASSERT_TRUE(held_prompt_);
  auto stale_prompt = std::move(held_prompt_);
  mojo::Remote<network::mojom::GmIdentityBroker> second_broker(
      manager_->Bind());
  second_broker->AcquireIdentity(
      1, url::Origin::Create(GURL("https://gm.example")), "corporate-key", 2,
      CertificateRequest(), other.GetCallback());
  broker_.reset();
  tasks_.RunUntilIdle();
  EXPECT_EQ(prompts_, 2);
  ASSERT_TRUE(held_prompt_);
  CompletePrompt(std::move(stale_prompt));
  tasks_.RunUntilIdle();
  EXPECT_EQ(service_.authorizations, 0);
  EXPECT_FALSE(other.IsReady());
  CompletePrompt(std::move(held_prompt_));
  EXPECT_EQ(other.Get<0>(), net::OK);
  EXPECT_EQ(service_.launches, 1);
}

TEST_F(GmKeyManagerTest, CancelReleasesAllSlotsInTheBoundedRequestQueue) {
  hold_prompt_ = true;
  using Future =
      base::test::TestFuture<int, network::mojom::GmIdentityGrantPtr>;
  std::vector<std::unique_ptr<Future>> requests;
  std::vector<uint64_t> ids;
  for (int i = 0; i < 32; ++i) {
    requests.push_back(std::make_unique<Future>());
    ids.push_back(Acquire(requests.back().get()));
  }
  Future overflow;
  Acquire(&overflow);
  EXPECT_NE(overflow.Get<0>(), net::OK);
  for (auto id : ids) {
    broker_->Cancel(id);
  }
  for (const auto& request : requests) {
    EXPECT_EQ(request->Get<0>(), net::ERR_ABORTED);
  }
  hold_prompt_ = false;
  Future recovered;
  Acquire(&recovered);
  EXPECT_EQ(recovered.Get<0>(), net::OK);
  EXPECT_EQ(service_.launches, 1);
}

}  // namespace
}  // namespace gm_browser
