// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_client_cert_chain_win.h"

#include <algorithm>
#include <array>
#include <memory>
#include <vector>

#include "base/containers/to_vector.h"
#include "base/numerics/safe_conversions.h"
#include "base/time/time.h"
#include "crypto/scoped_capi_types.h"
#include "net/cert/x509_util.h"
#include "net/gm/gm_client_cert_constraints.h"
#include "net/gm/tests/client_cert_test_data.h"
#include "net/test/cert_builder.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"
#include "third_party/boringssl/src/pki/verify_certificate_chain.h"
#include "third_party/boringssl/src/pki/verify_signed_data.h"

namespace gm_browser {
namespace {

class GmClientCertChainWinTest : public testing::Test {
 protected:
  GmClientCertChainWinTest()
      : roots_(CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr)),
        intermediates_(
            CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr)) {}

  bool Add(HCERTSTORE store, base::span<const uint8_t> der) {
    return CertAddEncodedCertificateToStore(
        store, X509_ASN_ENCODING, der.data(),
        base::checked_cast<DWORD>(der.size()), CERT_STORE_ADD_ALWAYS, nullptr);
  }

  std::optional<std::vector<std::vector<uint8_t>>> Find(
      base::span<const uint8_t> signing,
      const std::vector<std::vector<uint8_t>>& existing = {}) {
    return FindGmClientCertificateIntermediatesFromStoresForTesting(
        signing, {}, existing, roots_.get(), intermediates_.get());
  }

  crypto::ScopedHCERTSTORE roots_;
  crypto::ScopedHCERTSTORE intermediates_;
};

TEST_F(GmClientCertChainWinTest,
       RootOnlyAuthorityMatchesAfterLocalCaDiscovery) {
  // These are real SM2 certificates: leaf -> intermediate -> root. Windows
  // need not support SM2 chain validation to discover the public issuer.
  const auto signing = net::gm::test::ChainedSigningCertificate();
  const auto issuer = net::gm::test::ClientIntermediateCertificate();
  const auto parsed = bssl::ParsedCertificate::Create(
      net::x509_util::CreateCryptoBuffer(issuer), {}, nullptr);
  ASSERT_TRUE(parsed);
  net::gm::ClientCertificateRequest request;
  request.protocol = net::gm::TlsProtocol::kRfc8998;
  request.required_roles = net::gm::kClientCertificateSigning;
  request.signature_algorithms = {0x0708};
  request.certificate_authorities = {base::ToVector(parsed->issuer_tlv())};
  EXPECT_FALSE(
      net::gm::MatchesClientCertificateRequest(signing, {}, {}, request));
  ASSERT_TRUE(Add(intermediates_.get(), issuer));
  // An unrelated public SM2 CA must not contaminate the client chain.
  ASSERT_TRUE(Add(roots_.get(), net::gm::test::CaSigningCertificate()));
  const auto chain = Find(signing);
  ASSERT_TRUE(chain);
  ASSERT_EQ(*chain, (std::vector<std::vector<uint8_t>>{issuer}));
  EXPECT_TRUE(
      net::gm::MatchesClientCertificateRequest(signing, {}, *chain, request));
}

TEST_F(GmClientCertChainWinTest,
       SearchesBothStoresAndDeduplicatesProvidedIssuer) {
  const auto signing = net::gm::test::ChainedSigningCertificate();
  const auto issuer = net::gm::test::ClientIntermediateCertificate();
  ASSERT_TRUE(Add(roots_.get(), issuer));
  ASSERT_TRUE(Add(intermediates_.get(), issuer));
  const auto chain = Find(signing, {issuer});
  ASSERT_TRUE(chain);
  EXPECT_EQ(*chain, (std::vector<std::vector<uint8_t>>{issuer}));
}

TEST_F(GmClientCertChainWinTest, OmitsVerifiedSelfSignedRootAndExpiredIssuer) {
  auto certificates = net::CertBuilder::CreateSimpleChain3();
  for (auto& cert : certificates) {
    cert->SetValidity(base::Time::Now() - base::Days(1),
                      base::Time::Now() + base::Days(30));
  }
  const auto leaf = certificates[0]->GetDER();
  const auto issuer = certificates[1]->GetDER();
  const auto root = certificates[2]->GetDER();
  ASSERT_TRUE(Add(intermediates_.get(), base::as_byte_span(issuer)));
  ASSERT_TRUE(Add(roots_.get(), base::as_byte_span(root)));
  auto chain = Find(base::as_byte_span(leaf));
  ASSERT_TRUE(chain);
  EXPECT_EQ(*chain, (std::vector<std::vector<uint8_t>>{
                        base::ToVector(base::as_byte_span(issuer))}));

  crypto::ScopedHCERTSTORE expired_store(
      CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr));
  certificates[1]->SetValidity(base::Time::Now() - base::Days(2),
                               base::Time::Now() - base::Days(1));
  ASSERT_TRUE(
      Add(expired_store.get(), base::as_byte_span(certificates[1]->GetDER())));
  chain = FindGmClientCertificateIntermediatesFromStoresForTesting(
      base::as_byte_span(leaf), {}, {}, roots_.get(), expired_store.get());
  ASSERT_TRUE(chain);
  EXPECT_TRUE(chain->empty());
}

TEST_F(GmClientCertChainWinTest,
       PreservesSameNameCaRolloverWithAndWithoutKeyIdentifiers) {
  for (bool omit_key_identifiers : {false, true}) {
    SCOPED_TRACE(omit_key_identifiers);
    auto certificates = net::CertBuilder::CreateSimpleChain3();
    certificates[1]->SetSubjectTLV(
        base::as_byte_span(certificates[2]->GetSubject()));
    if (omit_key_identifiers) {
      for (auto& certificate : certificates) {
        certificate->EraseExtension(
            bssl::der::Input(bssl::kSubjectKeyIdentifierOid));
        certificate->SetAuthorityKeyIdentifier({});
      }
    }
    const auto leaf = certificates[0]->GetDER();
    const auto rollover = certificates[1]->GetDER();
    const auto root = certificates[2]->GetDER();
    auto parse = [](const std::string& der) {
      return bssl::ParsedCertificate::Create(
          net::x509_util::CreateCryptoBuffer(base::as_byte_span(der)), {},
          nullptr);
    };
    const auto parsed_leaf = parse(leaf);
    const auto parsed_rollover = parse(rollover);
    const auto parsed_root = parse(root);
    ASSERT_TRUE(parsed_leaf);
    ASSERT_TRUE(parsed_rollover);
    ASSERT_TRUE(parsed_root);
    EXPECT_EQ(parsed_rollover->subject_key_identifier().has_value(),
              !omit_key_identifiers);
    EXPECT_EQ(parsed_rollover->authority_key_identifier().has_value(),
              !omit_key_identifiers);
    EXPECT_EQ(parsed_rollover->normalized_subject(),
              parsed_rollover->normalized_issuer());
    ASSERT_TRUE(parsed_leaf->signature_algorithm());
    ASSERT_TRUE(parsed_rollover->signature_algorithm());
    EXPECT_TRUE(bssl::VerifySignedData(
        *parsed_leaf->signature_algorithm(), parsed_leaf->tbs_certificate_tlv(),
        parsed_leaf->signature_value(), parsed_rollover->tbs().spki_tlv,
        nullptr));
    EXPECT_TRUE(bssl::VerifySignedData(*parsed_rollover->signature_algorithm(),
                                       parsed_rollover->tbs_certificate_tlv(),
                                       parsed_rollover->signature_value(),
                                       parsed_root->tbs().spki_tlv, nullptr));
    EXPECT_FALSE(bssl::VerifyCertificateIsSelfSigned(*parsed_rollover, nullptr,
                                                     nullptr));
    EXPECT_TRUE(
        bssl::VerifyCertificateIsSelfSigned(*parsed_root, nullptr, nullptr));

    ASSERT_TRUE(Add(intermediates_.get(), base::as_byte_span(rollover)));
    ASSERT_TRUE(Add(roots_.get(), base::as_byte_span(root)));
    const auto chain = Find(base::as_byte_span(leaf));
    ASSERT_TRUE(chain);
    EXPECT_EQ(*chain, (std::vector<std::vector<uint8_t>>{
                          base::ToVector(base::as_byte_span(rollover))}));
  }
}

TEST_F(GmClientCertChainWinTest, OmitsVerifiedSm2SelfSignedRoot) {
  const auto issuer = net::gm::test::CaSigningCertificate();
  const auto parsed = bssl::ParsedCertificate::Create(
      net::x509_util::CreateCryptoBuffer(issuer), {}, nullptr);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->normalized_subject(), parsed->normalized_issuer());
  ASSERT_TRUE(parsed->signature_algorithm());
  EXPECT_TRUE(bssl::VerifyCertificateIsSelfSigned(*parsed, nullptr, nullptr));
  ASSERT_TRUE(Add(roots_.get(), issuer));
  const auto chain = Find(issuer);
  ASSERT_TRUE(chain);
  EXPECT_TRUE(chain->empty());
}

TEST_F(GmClientCertChainWinTest,
       RetainsUnknownSignatureSelfIssuedCandidateAndTerminatesCycle) {
  // Unsupported signature verification must not turn equal DNs into evidence
  // that a candidate can safely be omitted. SM2 is supported now, so replace
  // both signature OIDs with an unrecognized OID without changing the names.
  auto issuer = net::gm::test::CaSigningCertificate();
  constexpr std::array<uint8_t, 10> kSm2Sm3Oid = {0x06, 0x08, 0x2a, 0x81, 0x1c,
                                                  0xcf, 0x55, 0x01, 0x83, 0x75};
  size_t replacements = 0;
  for (auto position = issuer.begin(); position != issuer.end();) {
    position = std::search(position, issuer.end(), kSm2Sm3Oid.begin(),
                           kSm2Sm3Oid.end());
    if (position == issuer.end()) {
      break;
    }
    position += kSm2Sm3Oid.size();
    *(position - 1) = 0x7f;
    ++replacements;
  }
  ASSERT_EQ(2u, replacements);
  const auto parsed = bssl::ParsedCertificate::Create(
      net::x509_util::CreateCryptoBuffer(issuer), {}, nullptr);
  ASSERT_TRUE(parsed);
  EXPECT_EQ(parsed->normalized_subject(), parsed->normalized_issuer());
  EXPECT_FALSE(parsed->signature_algorithm());
  ASSERT_TRUE(Add(roots_.get(), issuer));
  const auto chain = Find(issuer);
  ASSERT_TRUE(chain);
  EXPECT_EQ(*chain, (std::vector<std::vector<uint8_t>>{issuer}));
}

TEST_F(GmClientCertChainWinTest, RejectsIncompleteStoresAndMalformedInput) {
  const auto signing = net::gm::test::ChainedSigningCertificate();
  EXPECT_FALSE(Find({}));
  EXPECT_FALSE(Find(signing, {{0x30, 0}}));
  EXPECT_FALSE(FindGmClientCertificateIntermediatesFromStoresForTesting(
      signing, {}, {}, nullptr, intermediates_.get()));
  EXPECT_FALSE(FindGmClientCertificateIntermediatesFromStoresForTesting(
      signing, std::vector<uint8_t>{0x30, 0}, {}, roots_.get(),
      intermediates_.get()));
  EXPECT_FALSE(
      Find(signing, std::vector<std::vector<uint8_t>>(
                        17, net::gm::test::ClientIntermediateCertificate())));
}

}  // namespace
}  // namespace gm_browser
