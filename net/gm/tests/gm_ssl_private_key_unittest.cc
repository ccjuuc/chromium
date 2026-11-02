// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_ssl_private_key.h"

#include <algorithm>
#include <array>
#include <map>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/functional/callback_helpers.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "net/gm/tests/client_cert_test_data.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/asn1.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/ec_key.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/boringssl/src/include/openssl/sm2.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "third_party/boringssl/src/include/openssl/x509v3.h"

namespace net::gm {
namespace {

using Bytes = std::vector<uint8_t>;
using Completion = base::test::TestFuture<Error, Bytes>;

net::SSLPrivateKey::SignCallback Callback(Completion& completion) {
  return completion.GetCallback<Error, const Bytes&>();
}

bssl::UniquePtr<EC_KEY> MakeKey() {
  bssl::UniquePtr<EC_KEY> key(EC_KEY_new_by_curve_name(NID_sm2));
  CHECK(key && EC_KEY_generate_key(key.get()));
  return key;
}

Bytes MakeCertificate(EC_KEY* key) {
  bssl::UniquePtr<EVP_PKEY> public_key(EVP_PKEY_new());
  CHECK(public_key && EVP_PKEY_set1_EC_KEY(public_key.get(), key));
  bssl::UniquePtr<X509> certificate(X509_new());
  CHECK(certificate && X509_set_version(certificate.get(), 2));
  CHECK(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1));
  CHECK(ASN1_TIME_set_string(X509_getm_notBefore(certificate.get()),
                             "20200101000000Z"));
  CHECK(ASN1_TIME_set_string(X509_getm_notAfter(certificate.get()),
                             "21200101000000Z"));
  CHECK(X509_set_pubkey(certificate.get(), public_key.get()));
  constexpr uint8_t kName[] = "SM2 private key adapter test";
  CHECK(X509_NAME_add_entry_by_txt(X509_get_subject_name(certificate.get()),
                                   "CN", MBSTRING_ASC, kName, sizeof(kName) - 1,
                                   -1, 0));
  CHECK(X509_set_issuer_name(certificate.get(),
                             X509_get_subject_name(certificate.get())));
  for (const auto& [nid, value] : std::array<std::pair<int, const char*>, 3>{
           {{NID_basic_constraints, "critical,CA:FALSE"},
            {NID_key_usage, "critical,digitalSignature"},
            {NID_ext_key_usage, "clientAuth"}}}) {
    bssl::UniquePtr<X509_EXTENSION> extension(
        X509V3_EXT_nconf_nid(nullptr, nullptr, nid, value));
    CHECK(extension && X509_add_ext(certificate.get(), extension.get(), -1));
  }
  CHECK_GT(X509_sign(certificate.get(), public_key.get(), EVP_sm3()), 0);
  const int size = i2d_X509(certificate.get(), nullptr);
  CHECK_GT(size, 0);
  Bytes der(size);
  uint8_t* cursor = der.data();
  CHECK_EQ(i2d_X509(certificate.get(), &cursor), size);
  return der;
}

class SoftwareIdentity final : public ClientIdentity {
 public:
  struct Pending {
    std::array<uint8_t, 32> digest;
    SignCallback callback;
  };

  SoftwareIdentity(Bytes certificate, bssl::UniquePtr<EC_KEY> key)
      : ClientIdentity("software-test-identity",
                       1,
                       std::move(certificate),
                       {},
                       {}),
        key_(std::move(key)) {}

  void SignDigest(uint64_t request_id,
                  const std::array<uint8_t, 32>& digest,
                  SignCallback callback) override {
    request_ids.push_back(request_id);
    CHECK(pending.emplace(request_id, Pending{digest, std::move(callback)})
              .second);
  }

  void Cancel(uint64_t request_id) override {
    cancellations.push_back(request_id);
    // Deliberately keep the callback to model a device operation racing with
    // cancellation. Tests can deliver the late result explicitly.
  }

  Bytes Signature(uint64_t request_id, EC_KEY* override_key = nullptr) {
    Bytes signature(72);
    size_t size = 0;
    CHECK(SM2_sign_digest(pending.at(request_id).digest.data(),
                          signature.data(), &size, signature.size(),
                          override_key ? override_key : key_.get()));
    signature.resize(size);
    return signature;
  }

  void Deliver(uint64_t request_id, int error, Bytes signature) {
    auto found = pending.find(request_id);
    CHECK(found != pending.end());
    auto callback = std::move(found->second.callback);
    pending.erase(found);
    // Deliberately synchronous; the adapter must enforce its asynchronous
    // delivery boundary even when a provider violates that convention.
    std::move(callback).Run(error, std::move(signature));
  }

  void Complete(uint64_t request_id) {
    Deliver(request_id, OK, Signature(request_id));
  }

  const EC_KEY* key() const { return key_.get(); }
  std::vector<uint64_t> request_ids;
  std::vector<uint64_t> cancellations;
  std::map<uint64_t, Pending> pending;

 private:
  ~SoftwareIdentity() override = default;
  bssl::UniquePtr<EC_KEY> key_;
};

class GmSSLPrivateKeyTest : public testing::Test {
 protected:
  void SetUp() override {
    auto key = MakeKey();
    auto certificate = MakeCertificate(key.get());
    identity_ = base::MakeRefCounted<SoftwareIdentity>(std::move(certificate),
                                                       std::move(key));
    adapter_ = GmSSLPrivateKey::Create(identity_);
    ASSERT_TRUE(adapter_);
  }

  static constexpr std::array<uint8_t, 5> kInput = {1, 2, 3, 4, 5};
  base::test::TaskEnvironment task_environment_;
  scoped_refptr<SoftwareIdentity> identity_;
  scoped_refptr<GmSSLPrivateKey> adapter_;
};

TEST(GmClientIdentityTest, InvalidationRetainsIdentityThroughCallbacks) {
  auto identity = base::MakeRefCounted<SoftwareIdentity>(Bytes{}, nullptr);
  auto* raw_identity = identity.get();
  auto release_owner =
      identity->AddInvalidationCallback(base::BindLambdaForTesting([&] {
        EXPECT_FALSE(identity->is_valid());
        identity.reset();
      }));
  bool notified_after_release = false;
  auto observe_release = identity->AddInvalidationCallback(
      base::BindLambdaForTesting([&] { notified_after_release = true; }));

  raw_identity->Invalidate();

  EXPECT_FALSE(identity);
  EXPECT_TRUE(notified_after_release);
}

TEST_F(GmSSLPrivateKeyTest, AdvertisesOnlySm2) {
  EXPECT_EQ(adapter_->GetAlgorithmPreferences(),
            std::vector<uint16_t>({SSL_SIGN_SM2_SM3}));
  EXPECT_FALSE(adapter_->GetProviderName().empty());
}

TEST_F(GmSSLPrivateKeyTest, RejectsWrongCurveUsageAndMalformedCertificate) {
  for (const Bytes& certificate :
       {test::P256Certificate(), test::EncryptionCertificate(),
        test::ServerCertificate(), test::CaSigningCertificate(), Bytes{1, 2}}) {
    EXPECT_FALSE(GmSSLPrivateKey::Create(
        base::MakeRefCounted<SoftwareIdentity>(certificate, nullptr)));
  }
  Bytes trailing = identity_->signing_certificate();
  trailing.push_back(0);
  EXPECT_FALSE(GmSSLPrivateKey::Create(
      base::MakeRefCounted<SoftwareIdentity>(std::move(trailing), nullptr)));
  identity_->Invalidate();
  EXPECT_FALSE(GmSSLPrivateKey::Create(identity_));
  EXPECT_FALSE(GmSSLPrivateKey::Create(nullptr));
}

TEST_F(GmSSLPrivateKeyTest, RejectsAnInvalidSm2PublicPoint) {
  std::array<uint8_t, 65> point;
  ASSERT_EQ(EC_POINT_point2oct(EC_KEY_get0_group(identity_->key()),
                               EC_KEY_get0_public_key(identity_->key()),
                               POINT_CONVERSION_UNCOMPRESSED, point.data(),
                               point.size(), nullptr),
            point.size());
  Bytes certificate = identity_->signing_certificate();
  const auto encoded_point = std::ranges::search(certificate, point);
  ASSERT_FALSE(encoded_point.empty());
  std::ranges::fill(encoded_point, 0);
  *encoded_point.begin() = 4;  // Uncompressed (0, 0), which is not on SM2.
  EXPECT_FALSE(GmSSLPrivateKey::Create(
      base::MakeRefCounted<SoftwareIdentity>(std::move(certificate), nullptr)));
}

TEST_F(GmSSLPrivateKeyTest, BindsRfc8998IdentityAndVerifiesDeviceSignature) {
  Completion completion;
  adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(completion));
  ASSERT_EQ(identity_->request_ids.size(), 1u);
  const uint64_t request = identity_->request_ids.front();
  std::array<uint8_t, 32> expected;
  constexpr uint8_t kRfcIdentity[] = "TLSv1.3+GM+Cipher+Suite";
  ASSERT_TRUE(SM2_compute_digest(expected.data(), identity_->key(),
                                 kRfcIdentity, sizeof(kRfcIdentity) - 1,
                                 kInput.data(), kInput.size()));
  EXPECT_EQ(identity_->pending.at(request).digest, expected);
  std::array<uint8_t, 32> wrong_identity;
  constexpr uint8_t kTlcpIdentity[] = SM2_DEFAULT_USERID;
  ASSERT_TRUE(SM2_compute_digest(wrong_identity.data(), identity_->key(),
                                 kTlcpIdentity, sizeof(kTlcpIdentity) - 1,
                                 kInput.data(), kInput.size()));
  EXPECT_NE(expected, wrong_identity);
  identity_->Complete(request);
  EXPECT_FALSE(completion.IsReady());
  ASSERT_TRUE(completion.Wait());
  EXPECT_EQ(completion.Get<0>(), OK);
  EXPECT_TRUE(SM2_verify_digest(expected.data(), completion.Get<1>().data(),
                                completion.Get<1>().size(), identity_->key()));
  EXPECT_FALSE(SM2_verify_digest(wrong_identity.data(),
                                 completion.Get<1>().data(),
                                 completion.Get<1>().size(), identity_->key()));
}

TEST_F(GmSSLPrivateKeyTest, RejectsOtherAlgorithmsWithoutDeviceAccess) {
  Completion completion;
  adapter_->Sign(SSL_SIGN_ECDSA_SECP256R1_SHA256, kInput, Callback(completion));
  EXPECT_FALSE(completion.IsReady());
  EXPECT_EQ(completion.Get<0>(), ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED);
  EXPECT_TRUE(completion.Get<1>().empty());
  EXPECT_TRUE(identity_->request_ids.empty());
}

TEST_F(GmSSLPrivateKeyTest, RejectsUnboundedAndEmptyInputsWithoutDeviceAccess) {
  for (const Bytes& input : {Bytes(), Bytes(1025)}) {
    Completion completion;
    adapter_->Sign(SSL_SIGN_SM2_SM3, input, Callback(completion));
    EXPECT_EQ(completion.Get<0>(), ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED);
    EXPECT_TRUE(completion.Get<1>().empty());
  }
  EXPECT_TRUE(identity_->request_ids.empty());
}

TEST_F(GmSSLPrivateKeyTest, RejectsMalformedAndWrongKeySignatures) {
  auto other_key = MakeKey();
  for (int kind = 0; kind < 3; ++kind) {
    Completion completion;
    adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(completion));
    const uint64_t request = identity_->request_ids.back();
    Bytes signature = kind == 0 ? Bytes{0x30, 0}
                      : kind == 1
                          ? Bytes(73)
                          : identity_->Signature(request, other_key.get());
    identity_->Deliver(request, OK, std::move(signature));
    EXPECT_EQ(completion.Get<0>(), ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED);
    EXPECT_TRUE(completion.Get<1>().empty());
  }
}

TEST_F(GmSSLPrivateKeyTest, DeviceFailureNeverReturnsSignature) {
  Completion completion;
  adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(completion));
  const uint64_t request = identity_->request_ids.back();
  identity_->Deliver(request, ERR_ABORTED, identity_->Signature(request));
  EXPECT_EQ(completion.Get<0>(), ERR_ABORTED);
  EXPECT_TRUE(completion.Get<1>().empty());
}

TEST_F(GmSSLPrivateKeyTest, ConcurrentRequestsCompleteOutOfOrder) {
  Completion first, second;
  adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(first));
  constexpr std::array<uint8_t, 3> kOtherInput = {8, 6, 4};
  adapter_->Sign(SSL_SIGN_SM2_SM3, kOtherInput, Callback(second));
  ASSERT_EQ(identity_->request_ids.size(), 2u);
  const auto first_id = identity_->request_ids[0];
  const auto second_id = identity_->request_ids[1];
  EXPECT_NE(first_id, second_id);
  EXPECT_NE(identity_->pending.at(first_id).digest,
            identity_->pending.at(second_id).digest);
  identity_->Complete(second_id);
  EXPECT_EQ(second.Get<0>(), OK);
  EXPECT_FALSE(first.IsReady());
  identity_->Complete(first_id);
  EXPECT_EQ(first.Get<0>(), OK);
}

TEST_F(GmSSLPrivateKeyTest, CancelDiscardsQueuedSuccessAndRejectsFurtherUse) {
  Completion completion;
  adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(completion));
  const uint64_t request = identity_->request_ids.back();
  identity_->Complete(request);
  adapter_->Cancel();
  adapter_
      ->Cancel();  // Repeated cancellation must not drop the error callback.
  EXPECT_EQ(completion.Get<0>(), ERR_ABORTED);
  EXPECT_TRUE(completion.Get<1>().empty());
  EXPECT_EQ(identity_->cancellations, std::vector<uint64_t>({request}));
  task_environment_.RunUntilIdle();
  Completion after_cancel;
  adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(after_cancel));
  EXPECT_EQ(after_cancel.Get<0>(), ERR_ABORTED);
  EXPECT_EQ(identity_->request_ids.size(), 1u);
}

TEST_F(GmSSLPrivateKeyTest, InvalidationCancelsRequestAndIgnoresLateSuccess) {
  Completion completion;
  adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(completion));
  const uint64_t request = identity_->request_ids.back();
  identity_->Invalidate();
  EXPECT_EQ(completion.Get<0>(), ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_TRUE(completion.Get<1>().empty());
  EXPECT_EQ(identity_->cancellations, std::vector<uint64_t>({request}));
  identity_->Complete(request);
  task_environment_.RunUntilIdle();
  Completion after_invalidation;
  adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(after_invalidation));
  EXPECT_EQ(after_invalidation.Get<0>(),
            ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_EQ(identity_->request_ids.size(), 1u);
}

TEST_F(GmSSLPrivateKeyTest, DestructionCancelsAndDropsLateCallbacks) {
  Completion completion;
  adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(completion));
  const uint64_t request = identity_->request_ids.back();
  adapter_.reset();
  EXPECT_EQ(identity_->cancellations, std::vector<uint64_t>({request}));
  identity_->Complete(request);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(completion.IsReady());
}

TEST_F(GmSSLPrivateKeyTest, DestructionDropsQueuedArgumentFailure) {
  Completion completion;
  adapter_->Sign(SSL_SIGN_RSA_PSS_RSAE_SHA256, kInput, Callback(completion));
  adapter_.reset();
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(completion.IsReady());
  EXPECT_TRUE(identity_->request_ids.empty());
}

TEST_F(GmSSLPrivateKeyTest, CancelPreservesQueuedArgumentFailure) {
  Completion completion;
  adapter_->Sign(SSL_SIGN_RSA_PSS_RSAE_SHA256, kInput, Callback(completion));
  EXPECT_FALSE(completion.IsReady());
  adapter_->Cancel();
  task_environment_.RunUntilIdle();
  ASSERT_TRUE(completion.IsReady());
  EXPECT_EQ(completion.Get<0>(), ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED);
  EXPECT_TRUE(completion.Get<1>().empty());
  EXPECT_TRUE(identity_->request_ids.empty());
}

TEST_F(GmSSLPrivateKeyTest, BoundsOutstandingDeviceRequests) {
  for (size_t i = 0; i < 16; ++i) {
    adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, base::DoNothing());
  }
  Completion overflow;
  adapter_->Sign(SSL_SIGN_SM2_SM3, kInput, Callback(overflow));
  EXPECT_EQ(overflow.Get<0>(), ERR_INSUFFICIENT_RESOURCES);
  EXPECT_EQ(identity_->request_ids.size(), 16u);
  adapter_->Cancel();
  EXPECT_EQ(identity_->cancellations.size(), 16u);
  task_environment_.RunUntilIdle();
}

}  // namespace
}  // namespace net::gm
