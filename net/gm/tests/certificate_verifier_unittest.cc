// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <atomic>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/containers/span.h"
#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/time/time_override.h"
#include "net/base/net_errors.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/x509_certificate.h"
#include "net/gm/gm_certificate_verifier.h"
#include "net/gm/gm_client_context.h"
#include "net/gm/gm_tls13_client.h"
#include "net/gm/gm_tls_policy.h"
#include "net/gm/tests/certificate_verifier_test_data.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/asn1.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/ec_key.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/boringssl/src/include/openssl/obj.h"
#include "third_party/boringssl/src/include/openssl/rsa.h"
#include "third_party/boringssl/src/include/openssl/x509v3.h"

namespace net::gm {
namespace {

using Bytes = std::vector<uint8_t>;
using Certificate = bssl::UniquePtr<X509>;
using Key = bssl::UniquePtr<EVP_PKEY>;

Key NewKey(int curve = NID_sm2) {
  bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(curve));
  Key key(EVP_PKEY_new());
  if (!ec || !key || !EC_KEY_generate_key(ec.get()) ||
      !EVP_PKEY_set1_EC_KEY(key.get(), ec.get())) {
    return nullptr;
  }
  return key;
}

Key NewRsaKey() {
  bssl::UniquePtr<EVP_PKEY_CTX> context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
  EVP_PKEY* key = nullptr;
  if (!context || !EVP_PKEY_keygen_init(context.get()) ||
      !EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) ||
      !EVP_PKEY_keygen(context.get(), &key)) {
    return nullptr;
  }
  return Key(key);
}

const EVP_MD* SigningDigest(EVP_PKEY* key) {
  return EVP_PKEY_id(key) == EVP_PKEY_EC &&
                 EVP_PKEY_get_ec_curve_nid(key) == NID_sm2
             ? EVP_sm3()
             : EVP_sha256();
}

bool AddExtension(X509* certificate, X509* issuer, int nid, const char* value) {
  X509V3_CTX context;
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  bssl::UniquePtr<X509_EXTENSION> extension(
      X509V3_EXT_nconf_nid(nullptr, &context, nid, value));
  return extension && X509_add_ext(certificate, extension.get(), -1);
}

Certificate NewCertificate(EVP_PKEY* key,
                           X509* issuer,
                           EVP_PKEY* issuer_key,
                           const char* name,
                           int serial,
                           bool ca,
                           const char* usage,
                           const char* san = "DNS:gm.test,IP:127.0.0.1",
                           const char* eku = "serverAuth") {
  Certificate cert(X509_new());
  if (!cert || !key || !X509_set_version(cert.get(), 2) ||
      !ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial) ||
      !ASN1_TIME_set_string(X509_getm_notBefore(cert.get()),
                            "20200101000000Z") ||
      !ASN1_TIME_set_string(X509_getm_notAfter(cert.get()),
                            "21200101000000Z") ||
      !X509_set_pubkey(cert.get(), key) ||
      !X509_NAME_add_entry_by_txt(
          X509_get_subject_name(cert.get()), "CN", MBSTRING_ASC,
          reinterpret_cast<const uint8_t*>(name), -1, -1, 0) ||
      !X509_set_issuer_name(cert.get(),
                            issuer ? X509_get_subject_name(issuer)
                                   : X509_get_subject_name(cert.get())) ||
      !AddExtension(cert.get(), issuer ? issuer : cert.get(),
                    NID_basic_constraints,
                    ca ? "critical,CA:TRUE" : "critical,CA:FALSE") ||
      (usage && !AddExtension(cert.get(), issuer ? issuer : cert.get(),
                              NID_key_usage, usage)) ||
      (!ca && eku &&
       !AddExtension(cert.get(), issuer, NID_ext_key_usage, eku)) ||
      (!ca && san &&
       !AddExtension(cert.get(), issuer, NID_subject_alt_name, san)) ||
      X509_sign(cert.get(), issuer_key ? issuer_key : key,
                SigningDigest(issuer_key ? issuer_key : key)) <= 0) {
    return nullptr;
  }
  return cert;
}

Bytes Der(X509* certificate) {
  const int size = i2d_X509(certificate, nullptr);
  if (size <= 0) {
    return {};
  }
  Bytes result(size);
  uint8_t* cursor = result.data();
  return i2d_X509(certificate, &cursor) == size ? result : Bytes();
}

Certificate Parse(base::span<const uint8_t> der) {
  const uint8_t* cursor = der.data();
  Certificate cert(d2i_X509(nullptr, &cursor, der.size()));
  return cert && cursor == der.subspan(der.size()).data() ? std::move(cert)
                                                          : nullptr;
}

Certificate ExpiredCopy(X509* source, EVP_PKEY* signer) {
  Certificate cert(X509_dup(source));
  if (!cert ||
      !ASN1_TIME_set_string(X509_getm_notBefore(cert.get()),
                            "20100101000000Z") ||
      !ASN1_TIME_set_string(X509_getm_notAfter(cert.get()),
                            "20110101000000Z") ||
      X509_sign(cert.get(), signer, EVP_sm3()) <= 0) {
    return nullptr;
  }
  return cert;
}

enum class CrlValidity {
  kCurrent,
  kExpired,
  kFuture,
  kMissingNextUpdate,
  kReversed,
  kEmpty,
};

Bytes Crl(X509* issuer,
          EVP_PKEY* signer,
          X509* revoked = nullptr,
          CrlValidity validity = CrlValidity::kCurrent,
          std::optional<bool> delta_critical = std::nullopt,
          const EVP_MD* digest = nullptr) {
  bssl::UniquePtr<X509_CRL> crl(X509_CRL_new());
  const int64_t now = std::time(nullptr);
  bssl::UniquePtr<ASN1_TIME> last(ASN1_TIME_adj(
      nullptr, now, 0, validity == CrlValidity::kFuture ? 3600 : -300));
  const int next_offset = validity == CrlValidity::kExpired    ? -60
                          : validity == CrlValidity::kReversed ? -600
                          : validity == CrlValidity::kEmpty    ? -300
                                                               : 7200;
  bssl::UniquePtr<ASN1_TIME> next(ASN1_TIME_adj(nullptr, now, 0, next_offset));
  if (!crl || !last || !next || !X509_CRL_set_version(crl.get(), 1) ||
      !X509_CRL_set_issuer_name(crl.get(), X509_get_subject_name(issuer)) ||
      !X509_CRL_set1_lastUpdate(crl.get(), last.get()) ||
      (validity != CrlValidity::kMissingNextUpdate &&
       !X509_CRL_set1_nextUpdate(crl.get(), next.get()))) {
    return {};
  }
  if (revoked) {
    bssl::UniquePtr<X509_REVOKED> entry(X509_REVOKED_new());
    bssl::UniquePtr<ASN1_TIME> date(ASN1_TIME_adj(nullptr, now, 0, -120));
    if (!entry || !date ||
        !X509_REVOKED_set_serialNumber(entry.get(),
                                       X509_get_serialNumber(revoked)) ||
        !X509_REVOKED_set_revocationDate(entry.get(), date.get()) ||
        !X509_CRL_add0_revoked(crl.get(), entry.get())) {
      return {};
    }
    entry.release();
  }
  if (delta_critical.has_value()) {
    bssl::UniquePtr<ASN1_INTEGER> base_number(ASN1_INTEGER_new());
    if (!base_number || !ASN1_INTEGER_set(base_number.get(), 1) ||
        !X509_CRL_add1_ext_i2d(crl.get(), NID_delta_crl, base_number.get(),
                               *delta_critical, X509V3_ADD_DEFAULT)) {
      return {};
    }
  }
  if (!X509_CRL_sort(crl.get()) ||
      X509_CRL_sign(crl.get(), signer,
                    digest ? digest : SigningDigest(signer)) <= 0) {
    return {};
  }
  const int size = i2d_X509_CRL(crl.get(), nullptr);
  if (size <= 0) {
    return {};
  }
  Bytes result(size);
  uint8_t* cursor = result.data();
  return i2d_X509_CRL(crl.get(), &cursor) == size ? result : Bytes();
}

struct Policy {
  std::vector<X509*> roots;
  std::vector<X509*> intermediates;
  std::vector<X509*> disallowed;
  std::vector<Bytes> crls;
  uint32_t revocation = GM_REVOCATION_DISABLED;
  uint32_t protocol = GM_PROTOCOL_TLCP;
};

std::unique_ptr<GmCertificateVerifier> Create(const Policy& policy) {
  std::vector<Bytes> roots, intermediates, disallowed;
  for (auto* cert : policy.roots) {
    roots.push_back(Der(cert));
  }
  for (auto* cert : policy.intermediates) {
    intermediates.push_back(Der(cert));
  }
  for (auto* cert : policy.disallowed) {
    disallowed.push_back(Der(cert));
  }
  auto views = [](const std::vector<Bytes>& certs) {
    std::vector<GmBytes> result;
    for (const auto& cert : certs) {
      result.push_back({cert.data(), cert.size()});
    }
    return result;
  };
  const auto anchor_views = views(roots);
  const auto issuer_views = views(intermediates);
  const auto denied_views = views(disallowed);
  const auto crl_views = views(policy.crls);
  GmContextConfig config = {};
  config.struct_size = sizeof(config);
  config.protocol = policy.protocol;
  config.trust_anchors = anchor_views.data();
  config.trust_anchor_count = anchor_views.size();
  config.untrusted_intermediates = issuer_views.data();
  config.untrusted_intermediate_count = issuer_views.size();
  config.disallowed_certificates = denied_views.data();
  config.disallowed_certificate_count = denied_views.size();
  config.crls = crl_views.data();
  config.crl_count = crl_views.size();
  config.revocation_mode = policy.revocation;
  return GmCertificateVerifier::Create(config);
}

struct Verification {
  bool accepted;
  long verify_result;
  uint32_t errors;
  bssl::UniquePtr<STACK_OF(X509)> chain;
};

Verification Verify(const GmCertificateVerifier& verifier,
                    X509* leaf,
                    uint32_t role = GM_CERTIFICATE_SIGNING,
                    std::string_view name = "gm.test",
                    uint32_t name_mode = GM_HOSTNAME_EXACT_SAN,
                    STACK_OF(X509) * intermediates = nullptr) {
  STACK_OF(X509)* chain = nullptr;
  long verify_result = -1;
  uint32_t errors = 0;
  bool accepted = verifier.Verify(leaf, intermediates, name, name_mode, role,
                                  &chain, &verify_result, &errors);
  return {accepted, verify_result, errors,
          bssl::UniquePtr<STACK_OF(X509)>(chain)};
}

class GmCertificateVerifierTest : public testing::Test {
 protected:
  void SetUp() override {
    root_key_ = NewKey();
    intermediate_key_ = NewKey();
    signing_key_ = NewKey();
    encryption_key_ = NewKey();
    other_key_ = NewKey();
    ASSERT_TRUE(root_key_);
    ASSERT_TRUE(intermediate_key_);
    ASSERT_TRUE(signing_key_);
    ASSERT_TRUE(encryption_key_);
    ASSERT_TRUE(other_key_);
    root_ = NewCertificate(root_key_.get(), nullptr, nullptr, "GM root", 1,
                           true, "critical,keyCertSign,cRLSign");
    ASSERT_TRUE(root_);
    intermediate_ =
        NewCertificate(intermediate_key_.get(), root_.get(), root_key_.get(),
                       "GM issuer", 2, true, "critical,keyCertSign,cRLSign");
    ASSERT_TRUE(intermediate_);
    signing_ = NewLeaf(signing_key_.get(), 3, "critical,digitalSignature");
    encryption_ = NewLeaf(encryption_key_.get(), 4, "critical,keyEncipherment");
    ASSERT_TRUE(signing_);
    ASSERT_TRUE(encryption_);
  }

  Certificate NewLeaf(EVP_PKEY* key,
                      int serial,
                      const char* usage,
                      const char* san = "DNS:gm.test,IP:127.0.0.1",
                      const char* eku = "serverAuth") {
    return NewCertificate(key, intermediate_.get(), intermediate_key_.get(),
                          "gm.test", serial, false, usage, san, eku);
  }

  Policy DefaultPolicy() {
    return {.roots = {root_.get()}, .intermediates = {intermediate_.get()}};
  }

  Key root_key_, intermediate_key_, signing_key_, encryption_key_, other_key_;
  Certificate root_, intermediate_, signing_, encryption_;
};

TEST_F(GmCertificateVerifierTest, VerifiesBothTlcpRoleChainsIndependently) {
  auto verifier = Create(DefaultPolicy());
  ASSERT_TRUE(verifier);
  for (const auto [leaf, role] :
       {std::pair(signing_.get(), GM_CERTIFICATE_SIGNING),
        std::pair(encryption_.get(), GM_CERTIFICATE_ENCRYPTION)}) {
    auto result = Verify(*verifier, leaf, role);
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(X509_V_OK, result.verify_result);
    EXPECT_EQ(0u, result.errors);
    ASSERT_EQ(3u, sk_X509_num(result.chain.get()));
    EXPECT_EQ(0, X509_cmp(leaf, sk_X509_value(result.chain.get(), 0)));
    EXPECT_EQ(0, X509_cmp(root_.get(), sk_X509_value(result.chain.get(), 2)));
  }
}

TEST_F(GmCertificateVerifierTest, PeerIntermediateIsNotATrustAnchor) {
  Policy policy = DefaultPolicy();
  policy.intermediates.clear();
  auto verifier = Create(policy);
  ASSERT_TRUE(verifier);
  EXPECT_FALSE(Verify(*verifier, signing_.get()).accepted);
  bssl::UniquePtr<STACK_OF(X509)> peer(sk_X509_new_null());
  ASSERT_TRUE(peer);
  ASSERT_TRUE(X509_up_ref(intermediate_.get()));
  ASSERT_TRUE(sk_X509_push(peer.get(), intermediate_.get()));
  EXPECT_TRUE(Verify(*verifier, signing_.get(), GM_CERTIFICATE_SIGNING,
                     "gm.test", GM_HOSTNAME_EXACT_SAN, peer.get())
                  .accepted);
  policy.roots.clear();
  verifier = Create(policy);
  ASSERT_TRUE(verifier);
  auto rejected = Verify(*verifier, signing_.get(), GM_CERTIFICATE_SIGNING,
                         "gm.test", GM_HOSTNAME_EXACT_SAN, peer.get());
  EXPECT_FALSE(rejected.accepted);
  EXPECT_NE(0u, rejected.errors);
}

TEST_F(GmCertificateVerifierTest, RequiresMatchingSanForEachRole) {
  auto verifier = Create(DefaultPolicy());
  ASSERT_TRUE(verifier);
  EXPECT_TRUE(
      Verify(*verifier, signing_.get(), GM_CERTIFICATE_SIGNING, "GM.TEST")
          .accepted);
  EXPECT_TRUE(
      Verify(*verifier, signing_.get(), GM_CERTIFICATE_SIGNING, "127.0.0.1")
          .accepted);
  auto mismatch = Verify(*verifier, encryption_.get(),
                         GM_CERTIFICATE_ENCRYPTION, "different.test");
  EXPECT_FALSE(mismatch.accepted);
  EXPECT_EQ(GM_CERT_ERROR_NAME, mismatch.errors);
  EXPECT_FALSE(Verify(*verifier, signing_.get(), GM_CERTIFICATE_SIGNING,
                      std::string_view("gm.test\0.other.test", 19))
                   .accepted);
  auto no_san =
      NewLeaf(signing_key_.get(), 5, "critical,digitalSignature", nullptr);
  ASSERT_TRUE(no_san);
  // The CN matches, but both modes require a SAN.
  for (uint32_t mode : {GM_HOSTNAME_EXACT_SAN, GM_HOSTNAME_BROWSER_SAN}) {
    EXPECT_FALSE(
        Verify(*verifier, no_san.get(), GM_CERTIFICATE_SIGNING, "gm.test", mode)
            .accepted);
  }
}

TEST_F(GmCertificateVerifierTest,
       ExactModeRejectsWildcardAndBrowserModeChecksIt) {
  auto wildcard = NewLeaf(signing_key_.get(), 5, "critical,digitalSignature",
                          "DNS:*.gm.test");
  ASSERT_TRUE(wildcard);
  auto verifier = Create(DefaultPolicy());
  ASSERT_TRUE(verifier);
  EXPECT_FALSE(
      Verify(*verifier, wildcard.get(), GM_CERTIFICATE_SIGNING, "one.gm.test")
          .accepted);
  EXPECT_TRUE(Verify(*verifier, wildcard.get(), GM_CERTIFICATE_SIGNING,
                     "one.gm.test", GM_HOSTNAME_BROWSER_SAN)
                  .accepted);
  EXPECT_FALSE(Verify(*verifier, wildcard.get(), GM_CERTIFICATE_SIGNING,
                      "two.one.gm.test", GM_HOSTNAME_BROWSER_SAN)
                   .accepted);
}

TEST_F(GmCertificateVerifierTest,
       RejectsOtherCurveEvenWithValidSm2IssuerSignature) {
  auto key = NewKey(NID_X9_62_prime256v1);
  ASSERT_TRUE(key);
  auto cert = NewLeaf(key.get(), 5, "critical,digitalSignature");
  ASSERT_TRUE(cert);
  EXPECT_EQ(NID_SM2_with_SM3, X509_get_signature_nid(cert.get()));
  auto verifier = Create(DefaultPolicy());
  ASSERT_TRUE(verifier);
  auto rejected = Verify(*verifier, cert.get());
  EXPECT_FALSE(rejected.accepted);
  EXPECT_EQ(X509_V_ERR_INVALID_PURPOSE, rejected.verify_result);
}

TEST_F(GmCertificateVerifierTest, RejectsSwappedRolesAndClientOnlyEku) {
  auto verifier = Create(DefaultPolicy());
  ASSERT_TRUE(verifier);
  EXPECT_FALSE(
      Verify(*verifier, signing_.get(), GM_CERTIFICATE_ENCRYPTION).accepted);
  EXPECT_FALSE(
      Verify(*verifier, encryption_.get(), GM_CERTIFICATE_SIGNING).accepted);
  auto client = NewLeaf(signing_key_.get(), 5, "critical,digitalSignature",
                        "DNS:gm.test", "clientAuth");
  ASSERT_TRUE(client);
  EXPECT_FALSE(Verify(*verifier, client.get()).accepted);
}

TEST_F(GmCertificateVerifierTest, TlcpRequiresExplicitRoleUsage) {
  auto no_usage = NewLeaf(signing_key_.get(), 5, nullptr);
  ASSERT_TRUE(no_usage);
  Policy policy = DefaultPolicy();
  auto verifier = Create(policy);
  ASSERT_TRUE(verifier);
  EXPECT_FALSE(
      Verify(*verifier, no_usage.get(), GM_CERTIFICATE_SIGNING).accepted);
  EXPECT_FALSE(
      Verify(*verifier, no_usage.get(), GM_CERTIFICATE_ENCRYPTION).accepted);
  policy.protocol = GM_PROTOCOL_RFC8998;
  verifier = Create(policy);
  ASSERT_TRUE(verifier);
  EXPECT_TRUE(Verify(*verifier, no_usage.get()).accepted);
}

TEST_F(GmCertificateVerifierTest, RejectsCaCertificatesInBothServerLeafRoles) {
  for (uint32_t protocol : {GM_PROTOCOL_TLCP, GM_PROTOCOL_RFC8998}) {
    SCOPED_TRACE(protocol);
    Policy policy = DefaultPolicy();
    policy.protocol = protocol;
    auto verifier = Create(policy);
    ASSERT_TRUE(verifier);
    for (const auto [leaf, role] :
         {std::pair(signing_.get(), GM_CERTIFICATE_SIGNING),
          std::pair(encryption_.get(), GM_CERTIFICATE_ENCRYPTION)}) {
      SCOPED_TRACE(role);
      ASSERT_TRUE(Verify(*verifier, leaf, role).accepted);
      Certificate ca_leaf(X509_dup(leaf));
      ASSERT_TRUE(ca_leaf);
      const int index =
          X509_get_ext_by_NID(ca_leaf.get(), NID_basic_constraints, -1);
      ASSERT_GE(index, 0);
      bssl::UniquePtr<X509_EXTENSION> old_constraints(
          X509_delete_ext(ca_leaf.get(), index));
      ASSERT_TRUE(old_constraints);
      ASSERT_TRUE(AddExtension(ca_leaf.get(), intermediate_.get(),
                               NID_basic_constraints, "critical,CA:TRUE"));
      ASSERT_GT(X509_sign(ca_leaf.get(), intermediate_key_.get(), EVP_sm3()),
                0);
      auto rejected = Verify(*verifier, ca_leaf.get(), role);
      EXPECT_FALSE(rejected.accepted);
      EXPECT_EQ(X509_V_ERR_INVALID_PURPOSE, rejected.verify_result);
      EXPECT_EQ(GM_CERT_ERROR_INVALID, rejected.errors);
    }
  }
}

TEST_F(GmCertificateVerifierTest, RejectsWrongIssuerKeyAndTamperedSignature) {
  auto wrong =
      NewCertificate(signing_key_.get(), intermediate_.get(), other_key_.get(),
                     "gm.test", 5, false, "critical,digitalSignature");
  ASSERT_TRUE(wrong);
  auto verifier = Create(DefaultPolicy());
  ASSERT_TRUE(verifier);
  EXPECT_FALSE(Verify(*verifier, wrong.get()).accepted);
  auto bytes = Der(signing_.get());
  ASSERT_FALSE(bytes.empty());
  bytes.back() ^= 1;
  auto tampered = Parse(bytes);
  ASSERT_TRUE(tampered);
  EXPECT_FALSE(Verify(*verifier, tampered.get()).accepted);
}

TEST_F(GmCertificateVerifierTest,
       ExplicitDistrustAppliesToRootIntermediateAndLeaf) {
  for (X509* denied :
       {root_.get(), intermediate_.get(), signing_.get(), encryption_.get()}) {
    SCOPED_TRACE(X509_get_serialNumber(denied));
    Policy policy = DefaultPolicy();
    policy.disallowed = {denied};
    auto verifier = Create(policy);
    ASSERT_TRUE(verifier);
    EXPECT_EQ(denied == encryption_.get(),
              Verify(*verifier, signing_.get()).accepted);
    EXPECT_EQ(denied == signing_.get(),
              Verify(*verifier, encryption_.get(), GM_CERTIFICATE_ENCRYPTION)
                  .accepted);
  }
}

TEST_F(GmCertificateVerifierTest, DistrustOverridesDuplicateRootsInAnyOrder) {
  auto other_root =
      NewCertificate(other_key_.get(), nullptr, nullptr, "Unrelated root", 8,
                     true, "critical,keyCertSign,cRLSign");
  ASSERT_TRUE(other_root);
  const std::vector<std::vector<X509*>> roots = {
      {root_.get(), root_.get(), other_root.get()},
      {root_.get(), other_root.get(), root_.get()},
      {other_root.get(), root_.get(), root_.get()}};
  const std::vector<std::vector<X509*>> denied = {
      {root_.get(), root_.get(), other_root.get()},
      {root_.get(), other_root.get(), root_.get()},
      {other_root.get(), root_.get(), root_.get()}};
  for (size_t root_order = 0; root_order < roots.size(); ++root_order) {
    SCOPED_TRACE(root_order);
    Policy policy = DefaultPolicy();
    policy.roots = roots[root_order];
    auto verifier = Create(policy);
    ASSERT_TRUE(verifier);
    EXPECT_TRUE(Verify(*verifier, signing_.get()).accepted);
    for (size_t deny_order = 0; deny_order < denied.size(); ++deny_order) {
      SCOPED_TRACE(deny_order);
      policy.disallowed = denied[deny_order];
      verifier = Create(policy);
      ASSERT_TRUE(verifier);
      EXPECT_FALSE(Verify(*verifier, signing_.get()).accepted);
      EXPECT_FALSE(
          Verify(*verifier, encryption_.get(), GM_CERTIFICATE_ENCRYPTION)
              .accepted);
    }
  }
}

TEST_F(GmCertificateVerifierTest, RejectsExpiredLeafIntermediateAndAnchor) {
  auto expired_leaf = ExpiredCopy(signing_.get(), intermediate_key_.get());
  auto expired_issuer = ExpiredCopy(intermediate_.get(), root_key_.get());
  auto expired_root = ExpiredCopy(root_.get(), root_key_.get());
  ASSERT_TRUE(expired_leaf);
  ASSERT_TRUE(expired_issuer);
  ASSERT_TRUE(expired_root);
  for (int position = 0; position < 3; ++position) {
    SCOPED_TRACE(position);
    Policy policy = DefaultPolicy();
    if (position == 1) {
      policy.intermediates = {expired_issuer.get()};
    } else if (position == 2) {
      policy.roots = {expired_root.get()};
    }
    auto verifier = Create(policy);
    ASSERT_TRUE(verifier);
    auto rejected =
        Verify(*verifier, position == 0 ? expired_leaf.get() : signing_.get());
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(GM_CERT_ERROR_DATE, rejected.errors);
  }
}

TEST_F(GmCertificateVerifierTest, MissingCrlFailsRequiredPolicies) {
  for (uint32_t mode :
       {GM_REVOCATION_REQUIRE_LEAF_CRL, GM_REVOCATION_REQUIRE_CHAIN_CRLS}) {
    Policy policy = DefaultPolicy();
    policy.revocation = mode;
    auto verifier = Create(policy);
    ASSERT_TRUE(verifier);
    auto rejected = Verify(*verifier, signing_.get());
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(GM_CERT_ERROR_REVOCATION_UNAVAILABLE, rejected.errors);
    EXPECT_EQ(X509_V_ERR_UNABLE_TO_GET_CRL, rejected.verify_result);
  }
}

TEST_F(GmCertificateVerifierTest, ValidLeafCrlAndCompleteChainCrlPass) {
  Policy policy = DefaultPolicy();
  policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
  policy.crls = {Crl(intermediate_.get(), intermediate_key_.get())};
  ASSERT_FALSE(policy.crls.front().empty());
  auto verifier = Create(policy);
  ASSERT_TRUE(verifier);
  EXPECT_TRUE(Verify(*verifier, signing_.get()).accepted);
  EXPECT_TRUE(
      Verify(*verifier, encryption_.get(), GM_CERTIFICATE_ENCRYPTION).accepted);
  policy.revocation = GM_REVOCATION_REQUIRE_CHAIN_CRLS;
  verifier = Create(policy);
  ASSERT_TRUE(verifier);
  EXPECT_FALSE(Verify(*verifier, signing_.get()).accepted);
  policy.crls.push_back(Crl(root_.get(), root_key_.get()));
  ASSERT_FALSE(policy.crls.back().empty());
  verifier = Create(policy);
  ASSERT_TRUE(verifier);
  EXPECT_TRUE(Verify(*verifier, signing_.get()).accepted);
  EXPECT_TRUE(
      Verify(*verifier, encryption_.get(), GM_CERTIFICATE_ENCRYPTION).accepted);
}

TEST_F(GmCertificateVerifierTest, StaleFutureWrongSignerAndWrongIssuerCrlFail) {
  std::vector<Bytes> invalid_crls = {
      Crl(intermediate_.get(), intermediate_key_.get(), nullptr,
          CrlValidity::kExpired),
      Crl(intermediate_.get(), intermediate_key_.get(), nullptr,
          CrlValidity::kFuture),
      Crl(intermediate_.get(), other_key_.get()),
      Crl(root_.get(), root_key_.get())};
  for (const auto& crl : invalid_crls) {
    ASSERT_FALSE(crl.empty());
    Policy policy = DefaultPolicy();
    policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
    policy.crls = {crl};
    auto verifier = Create(policy);
    ASSERT_TRUE(verifier);
    auto rejected = Verify(*verifier, signing_.get());
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(GM_CERT_ERROR_REVOCATION_UNAVAILABLE, rejected.errors);
  }
}

TEST_F(GmCertificateVerifierTest, RevokedRoleCannotBeHiddenByAnUnrevokedCrl) {
  for (X509* revoked : {signing_.get(), encryption_.get()}) {
    Policy policy = DefaultPolicy();
    policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
    policy.crls = {Crl(intermediate_.get(), intermediate_key_.get()),
                   Crl(intermediate_.get(), intermediate_key_.get(), revoked)};
    ASSERT_FALSE(policy.crls.back().empty());
    auto verifier = Create(policy);
    ASSERT_TRUE(verifier);
    auto signing = Verify(*verifier, signing_.get());
    auto encryption =
        Verify(*verifier, encryption_.get(), GM_CERTIFICATE_ENCRYPTION);
    EXPECT_EQ(revoked != signing_.get(), signing.accepted);
    EXPECT_EQ(revoked != encryption_.get(), encryption.accepted);
    const auto& rejected = revoked == signing_.get() ? signing : encryption;
    EXPECT_EQ(X509_V_ERR_CERT_REVOKED, rejected.verify_result);
    EXPECT_EQ(GM_CERT_ERROR_REVOKED, rejected.errors);
  }
}

TEST_F(GmCertificateVerifierTest, RevokedIntermediateFailsBothRoleChains) {
  Policy policy = DefaultPolicy();
  policy.revocation = GM_REVOCATION_REQUIRE_CHAIN_CRLS;
  policy.crls = {Crl(intermediate_.get(), intermediate_key_.get()),
                 Crl(root_.get(), root_key_.get(), intermediate_.get())};
  ASSERT_FALSE(policy.crls.back().empty());
  auto verifier = Create(policy);
  ASSERT_TRUE(verifier);
  for (const auto [leaf, role] :
       {std::pair(signing_.get(), GM_CERTIFICATE_SIGNING),
        std::pair(encryption_.get(), GM_CERTIFICATE_ENCRYPTION)}) {
    auto rejected = Verify(*verifier, leaf, role);
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(GM_CERT_ERROR_REVOKED, rejected.errors);
  }
}

TEST_F(GmCertificateVerifierTest, RejectsMalformedCrlConfiguration) {
  Policy policy = DefaultPolicy();
  policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
  policy.crls = {{0x30, 0x00}};
  EXPECT_FALSE(Create(policy));
  policy.crls = {Crl(intermediate_.get(), intermediate_key_.get())};
  ASSERT_FALSE(policy.crls.front().empty());
  policy.crls.front().push_back(0);
  EXPECT_FALSE(Create(policy));
}

TEST_F(GmCertificateVerifierTest, RejectsUnboundedOrNonpositiveCrlValidity) {
  for (CrlValidity validity : {CrlValidity::kMissingNextUpdate,
                               CrlValidity::kReversed, CrlValidity::kEmpty}) {
    SCOPED_TRACE(static_cast<int>(validity));
    Policy policy = DefaultPolicy();
    policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
    policy.crls = {
        Crl(intermediate_.get(), intermediate_key_.get(), nullptr, validity)};
    ASSERT_FALSE(policy.crls.front().empty());
    EXPECT_FALSE(Create(policy));
  }
}

TEST_F(GmCertificateVerifierTest, RejectsCriticalAndNoncriticalDeltaCrls) {
  Policy policy = DefaultPolicy();
  policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
  policy.crls = {Crl(intermediate_.get(), intermediate_key_.get())};
  ASSERT_FALSE(policy.crls.front().empty());
  ASSERT_TRUE(Create(policy));
  for (bool critical : {false, true}) {
    SCOPED_TRACE(critical);
    policy.crls = {Crl(intermediate_.get(), intermediate_key_.get(), nullptr,
                       CrlValidity::kCurrent, critical)};
    ASSERT_FALSE(policy.crls.front().empty());
    EXPECT_FALSE(Create(policy));
  }
}

TEST_F(GmCertificateVerifierTest, MixedIssuersRequireStrongCrlSignatures) {
  for (bool rsa : {false, true}) {
    SCOPED_TRACE(rsa ? "RSA issuer" : "P256 issuer");
    Key issuer_key = rsa ? NewRsaKey() : NewKey(NID_X9_62_prime256v1);
    ASSERT_TRUE(issuer_key);
    auto issuer =
        NewCertificate(issuer_key.get(), nullptr, nullptr, "Ordinary issuer",
                       10, true, "critical,keyCertSign,cRLSign");
    ASSERT_TRUE(issuer);
    auto leaf =
        NewCertificate(signing_key_.get(), issuer.get(), issuer_key.get(),
                       "gm.test", 11, false, "critical,digitalSignature");
    ASSERT_TRUE(leaf);
    Policy policy;
    policy.roots = {issuer.get()};
    policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
    policy.crls = {Crl(issuer.get(), issuer_key.get())};
    ASSERT_FALSE(policy.crls.front().empty());
    auto verifier = Create(policy);
    ASSERT_TRUE(verifier);
    ASSERT_TRUE(Verify(*verifier, leaf.get()).accepted);

    // The same issuer and SM2 leaf are valid with a SHA-256 CRL, but an
    // otherwise valid SHA-1 CRL must not satisfy the strong trust policy.
    policy.crls = {Crl(issuer.get(), issuer_key.get(), nullptr,
                       CrlValidity::kCurrent, std::nullopt, EVP_sha1())};
    ASSERT_FALSE(policy.crls.front().empty());
    EXPECT_FALSE(Create(policy));
  }
}

TEST_F(GmCertificateVerifierTest, RequiresMatchingInnerAndOuterCrlAlgorithms) {
  auto issuer_key = NewKey(NID_X9_62_prime256v1);
  ASSERT_TRUE(issuer_key);
  auto issuer =
      NewCertificate(issuer_key.get(), nullptr, nullptr, "P256 issuer", 10,
                     true, "critical,keyCertSign,cRLSign");
  ASSERT_TRUE(issuer);
  Bytes original = Crl(issuer.get(), issuer_key.get());
  ASSERT_FALSE(original.empty());
  const uint8_t* cursor = original.data();
  bssl::UniquePtr<X509_CRL> crl(
      d2i_X509_CRL(nullptr, &cursor, original.size()));
  ASSERT_TRUE(crl);
  const X509_ALGOR* outer = nullptr;
  X509_CRL_get0_signature(crl.get(), nullptr, &outer);
  ASSERT_TRUE(outer);
  // Both algorithms are individually strong. Change only the unsigned outer
  // identifier; the signed tbsCertList still identifies ECDSA with SHA-256.
  ASSERT_TRUE(X509_ALGOR_set0(const_cast<X509_ALGOR*>(outer),
                              OBJ_nid2obj(NID_ecdsa_with_SHA384), V_ASN1_UNDEF,
                              nullptr));
  const int size = i2d_X509_CRL(crl.get(), nullptr);
  ASSERT_GT(size, 0);
  Bytes modified(size);
  uint8_t* out = modified.data();
  ASSERT_EQ(size, i2d_X509_CRL(crl.get(), &out));
  Policy policy;
  policy.roots = {issuer.get()};
  policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
  policy.crls = {std::move(modified)};
  EXPECT_FALSE(Create(policy));
}

TEST_F(GmCertificateVerifierTest, BoundsCrlDistributionPointWork) {
  Policy policy = DefaultPolicy();
  policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
  policy.crls = {Crl(intermediate_.get(), intermediate_key_.get())};
  ASSERT_FALSE(policy.crls.front().empty());
  auto verifier = Create(policy);
  ASSERT_TRUE(verifier);
  for (size_t count : {128u, 129u}) {
    SCOPED_TRACE(count);
    Certificate leaf(X509_dup(signing_.get()));
    ASSERT_TRUE(leaf);
    std::string points;
    for (size_t i = 0; i < count; ++i) {
      if (!points.empty()) {
        points += ',';
      }
      points += "URI:https://gm.test/crl/" + std::to_string(i);
    }
    ASSERT_TRUE(AddExtension(leaf.get(), intermediate_.get(),
                             NID_crl_distribution_points, points.c_str()));
    ASSERT_GT(X509_sign(leaf.get(), intermediate_key_.get(), EVP_sm3()), 0);
    auto result = Verify(*verifier, leaf.get());
    EXPECT_EQ(count == 128, result.accepted);
    if (count > 128) {
      EXPECT_EQ(X509_V_ERR_UNABLE_TO_GET_CRL, result.verify_result);
      EXPECT_EQ(GM_CERT_ERROR_REVOCATION_UNAVAILABLE, result.errors);
    }
  }
}

std::atomic<int64_t> g_deadline_tick_reads{0};

base::TimeTicks AdvancingDeadlineTicks() {
  return base::TimeTicks() +
         base::Seconds(2 * (1 + g_deadline_tick_reads.fetch_add(
                                    1, std::memory_order_relaxed)));
}

TEST_F(GmCertificateVerifierTest, ExpiredVerificationDeadlineFailsClosed) {
  Policy policy = DefaultPolicy();
  policy.revocation = GM_REVOCATION_REQUIRE_LEAF_CRL;
  policy.crls = {Crl(intermediate_.get(), intermediate_key_.get())};
  ASSERT_FALSE(policy.crls.front().empty());
  auto verifier = Create(policy);
  ASSERT_TRUE(verifier);
  ASSERT_TRUE(Verify(*verifier, signing_.get()).accepted);
  g_deadline_tick_reads.store(0, std::memory_order_relaxed);
  // Move only monotonic time. Every deadline check observes two additional
  // seconds, so this exercises expiry without sleeping or timing assumptions.
  // Certificate and CRL wall-clock validity remain unchanged.
  base::subtle::ScopedTimeClockOverrides clock(nullptr, &AdvancingDeadlineTicks,
                                               nullptr);
  const auto result = Verify(*verifier, signing_.get());
  EXPECT_FALSE(result.accepted);
  EXPECT_NE(X509_V_OK, result.verify_result);
  EXPECT_NE(0u, result.errors);
  EXPECT_FALSE(result.chain);
}

TEST_F(GmCertificateVerifierTest, FailurePreservesPreviouslyVerifiedChain) {
  auto verifier = Create(DefaultPolicy());
  ASSERT_TRUE(verifier);
  auto good = Verify(*verifier, signing_.get());
  ASSERT_TRUE(good.accepted);
  STACK_OF(X509)* raw = good.chain.release();
  auto* original = raw;
  long code = X509_V_OK;
  uint32_t errors = 0;
  EXPECT_FALSE(verifier->Verify(
      encryption_.get(), nullptr, "wrong.test", GM_HOSTNAME_EXACT_SAN,
      GM_CERTIFICATE_ENCRYPTION, &raw, &code, &errors));
  bssl::UniquePtr<STACK_OF(X509)> owned(raw);
  EXPECT_EQ(original, raw);
  EXPECT_EQ(GM_CERT_ERROR_NAME, errors);
  EXPECT_EQ(3u, sk_X509_num(raw));
}

// ThreadPool work remains queued until explicitly released. This exercises the
// actual asynchronous request contract without sleeping or mocking validation.
class GmTls13VerificationTest : public GmCertificateVerifierTest {
 protected:
  void SetUp() override {
    GmCertificateVerifierTest::SetUp();
    if (HasFatalFailure()) {
      return;
    }
    rule_ = std::make_shared<TlsRule>();
    rule_->host = "gm.test";
    rule_->profile = ConnectionProfile(TlsProtocol::kTls13Auto,
                                       "test-enterprise", 1, "", 0, 1);
    rule_->trust_anchors = {Der(root_.get())};
    rule_->untrusted_intermediates = {Der(intermediate_.get())};
    ASSERT_TRUE(rule_->IsValid());
    const auto der = Der(signing_.get());
    peer_ = X509Certificate::CreateFromBytes(der);
    ASSERT_TRUE(peer_);
    context_ = std::make_unique<ClientContext>();
  }

  std::unique_ptr<Tls13Client> Client(
      std::shared_ptr<const TlsRule> rule = nullptr) {
    return std::make_unique<Tls13Client>(
        HostPortPair("gm.test", 443), rule ? std::move(rule) : rule_,
        context_.get(), nullptr, PRIVACY_MODE_DISABLED,
        base::RepeatingCallback<base::UnguessableToken()>(), base::DoNothing(),
        base::DoNothing());
  }

  base::test::TaskEnvironment tasks_{
      base::test::TaskEnvironment::ThreadPoolExecutionMode::QUEUED};
  std::shared_ptr<TlsRule> rule_;
  scoped_refptr<X509Certificate> peer_;
  std::unique_ptr<ClientContext> context_;
};

TEST_F(GmTls13VerificationTest, CompletesAsynchronouslyWithVerifiedChain) {
  auto client = Client();
  CertVerifyResult result;
  std::unique_ptr<CertVerifier::Request> request;
  base::test::TestFuture<int> completed;
  EXPECT_EQ(client->Verify(peer_, "gm.test", &result, completed.GetCallback(),
                           &request),
            ERR_IO_PENDING);
  ASSERT_TRUE(request);
  base::RunLoop().RunUntilIdle();
  EXPECT_FALSE(completed.IsReady());
  EXPECT_EQ(result.cert_status, CERT_STATUS_INVALID);

  tasks_.RunUntilIdle();

  ASSERT_TRUE(completed.IsReady());
  EXPECT_EQ(completed.Get(), OK);
  EXPECT_EQ(result.cert_status, 0u);
  ASSERT_TRUE(result.verified_cert);
  EXPECT_EQ(result.verified_cert->intermediate_buffers().size(), 2u);
  EXPECT_EQ(result.public_key_hashes.size(), 3u);
  EXPECT_FALSE(result.is_issued_by_known_root);
}

TEST_F(GmTls13VerificationTest, RequestDestructionDiscardsResultAndCallback) {
  auto client = Client();
  CertVerifyResult result;
  std::unique_ptr<CertVerifier::Request> request;
  base::test::TestFuture<int> completed;
  ASSERT_EQ(client->Verify(peer_, "gm.test", &result, completed.GetCallback(),
                           &request),
            ERR_IO_PENDING);
  request.reset();
  result.cert_status = CERT_STATUS_DATE_INVALID;

  tasks_.RunUntilIdle();

  EXPECT_FALSE(completed.IsReady());
  EXPECT_EQ(result.cert_status, CERT_STATUS_DATE_INVALID);
}

TEST_F(GmTls13VerificationTest,
       HelperAndContextDestructionDiscardReplyToReleasedResult) {
  auto client = Client();
  auto result = std::make_unique<CertVerifyResult>();
  std::unique_ptr<CertVerifier::Request> request;
  base::test::TestFuture<int> completed;
  ASSERT_EQ(client->Verify(peer_, "gm.test", result.get(),
                           completed.GetCallback(), &request),
            ERR_IO_PENDING);
  // The worker must own everything it uses. In particular, it cannot access
  // this result, the helper, or the cache owner after cancellation.
  client.reset();
  context_.reset();
  result.reset();
  peer_.reset();
  rule_.reset();

  tasks_.RunUntilIdle();

  EXPECT_FALSE(completed.IsReady());
}

TEST_F(GmTls13VerificationTest, CompletionCanDestroyRequestAndHelper) {
  auto client = Client();
  CertVerifyResult result;
  std::unique_ptr<CertVerifier::Request> request;
  bool completed = false;
  ASSERT_EQ(client->Verify(peer_, "gm.test", &result,
                           base::BindLambdaForTesting([&](int error) {
                             EXPECT_EQ(error, OK);
                             request.reset();
                             client.reset();
                             completed = true;
                           }),
                           &request),
            ERR_IO_PENDING);

  tasks_.RunUntilIdle();

  EXPECT_TRUE(completed);
  EXPECT_FALSE(client);
  EXPECT_FALSE(request);
}

TEST_F(GmTls13VerificationTest, PendingJobsKeepTheirOwnTrustSnapshot) {
  auto trusted = Client();
  auto changed = std::make_shared<TlsRule>(*rule_);
  changed->disallowed_certificates = {Der(signing_.get())};
  changed->profile = ConnectionProfile(TlsProtocol::kTls13Auto,
                                       "test-enterprise", 2, "", 0, 1);
  CertVerifyResult trusted_result, changed_result;
  std::unique_ptr<CertVerifier::Request> trusted_request, changed_request;
  base::test::TestFuture<int> trusted_completed, changed_completed;
  ASSERT_EQ(trusted->Verify(peer_, "gm.test", &trusted_result,
                            trusted_completed.GetCallback(), &trusted_request),
            ERR_IO_PENDING);
  context_->ClearForServers({HostPortPair("gm.test", 443)});
  auto distrusted = Client(changed);
  ASSERT_EQ(
      distrusted->Verify(peer_, "gm.test", &changed_result,
                         changed_completed.GetCallback(), &changed_request),
      ERR_IO_PENDING);

  tasks_.RunUntilIdle();

  ASSERT_TRUE(trusted_completed.IsReady());
  ASSERT_TRUE(changed_completed.IsReady());
  EXPECT_EQ(trusted_completed.Get(), OK);
  EXPECT_EQ(changed_completed.Get(), ERR_CERT_AUTHORITY_INVALID);
  EXPECT_EQ(trusted_result.cert_status, 0u);
  EXPECT_TRUE(changed_result.cert_status & CERT_STATUS_AUTHORITY_INVALID);
}

base::Time TongsuoFixtureNow() {
  return base::Time::FromSecondsSinceUnixEpoch(test::kTongsuoFixtureUnixTime);
}

TEST(GmCertificateVerifierExternalTest, AcceptsTongsuoSignedDualCertsAndCrl) {
  // These immutable public fixtures were produced by the independent Tongsuo
  // implementation. Only wall-clock time is fixed; deadline clocks still run.
  base::subtle::ScopedTimeClockOverrides clock(&TongsuoFixtureNow, nullptr,
                                               nullptr);
  auto root = Parse(test::TongsuoRoot());
  auto signing = Parse(test::TongsuoSigning());
  auto encryption = Parse(test::TongsuoEncryption());
  ASSERT_TRUE(root);
  ASSERT_TRUE(signing);
  ASSERT_TRUE(encryption);
  Policy policy;
  policy.roots = {root.get()};
  policy.revocation = GM_REVOCATION_REQUIRE_CHAIN_CRLS;
  policy.crls = {test::TongsuoCrl()};
  auto verifier = Create(policy);
  ASSERT_TRUE(verifier);
  EXPECT_TRUE(Verify(*verifier, signing.get()).accepted);
  EXPECT_TRUE(
      Verify(*verifier, encryption.get(), GM_CERTIFICATE_ENCRYPTION).accepted);
  auto bytes = Der(encryption.get());
  ASSERT_FALSE(bytes.empty());
  bytes.back() ^= 1;
  auto corrupted = Parse(bytes);
  ASSERT_TRUE(corrupted);
  EXPECT_FALSE(
      Verify(*verifier, corrupted.get(), GM_CERTIFICATE_ENCRYPTION).accepted);
}

}  // namespace
}  // namespace net::gm
