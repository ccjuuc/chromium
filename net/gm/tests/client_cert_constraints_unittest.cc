// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "base/containers/to_vector.h"
#include "base/time/time.h"
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"
#include "net/gm/gm_client_cert_constraints.h"
#include "net/gm/tests/client_cert_test_data.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"

namespace net::gm {
namespace {

constexpr uint16_t kSm2Sm3 = 0x0708;

ClientCertificateRequest Request(TlsProtocol protocol = TlsProtocol::kRfc8998) {
  ClientCertificateRequest request;
  request.request_id = 1;
  request.protocol = protocol;
  request.required_roles = kClientCertificateSigning;
  if (protocol == TlsProtocol::kTlcp) {
    request.required_roles |= kClientCertificateEncryption;
  }
  request.signature_algorithms = {kSm2Sm3};
  return request;
}

base::Time ValidTime() {
  // 2027, independent of the machine clock. Fixtures are valid 2020--2120.
  return base::Time::FromSecondsSinceUnixEpoch(1800000000);
}

std::vector<uint8_t> IssuerName(base::span<const uint8_t> certificate) {
  auto parsed = bssl::ParsedCertificate::Create(
      x509_util::CreateCryptoBuffer(certificate), {}, nullptr);
  CHECK(parsed);
  return base::ToVector(parsed->issuer_tlv());
}

TEST(GmClientCertConstraintsTest, AcceptsSm2SigningAndTlcpDualCertificates) {
  const auto signing = test::SigningCertificate();
  const auto encryption = test::EncryptionCertificate();
  EXPECT_TRUE(
      MatchesClientCertificateRequest(signing, {}, {}, Request(), ValidTime()));
  EXPECT_TRUE(MatchesClientCertificateRequest(
      signing, encryption, {}, Request(TlsProtocol::kTlcp), ValidTime()));
}

TEST(GmClientCertConstraintsTest, JointPolicyRequiresNegotiatedRfc8998Request) {
  const ConnectionProfile joint(TlsProtocol::kTls13Auto, "enterprise", 1,
                                "feitian", 1, 1);
  const ConnectionProfile strict(TlsProtocol::kRfc8998, "enterprise", 1,
                                 "feitian", 1, 1);
  const ConnectionProfile tlcp(TlsProtocol::kTlcp, "enterprise", 1, "feitian",
                               1, 1);
  EXPECT_TRUE(IsClientCertificateRequestForProfile(joint, Request()));
  EXPECT_TRUE(IsClientCertificateRequestForProfile(strict, Request()));
  EXPECT_FALSE(
      IsClientCertificateRequestForProfile(joint, Request(TlsProtocol::kTlcp)));
  EXPECT_FALSE(IsClientCertificateRequestForProfile(
      strict, Request(TlsProtocol::kTlcp)));
  EXPECT_FALSE(IsClientCertificateRequestForProfile(tlcp, Request()));
  EXPECT_TRUE(
      IsClientCertificateRequestForProfile(tlcp, Request(TlsProtocol::kTlcp)));
  for (const auto& profile : {joint, strict, tlcp, ConnectionProfile()}) {
    EXPECT_FALSE(IsClientCertificateRequestForProfile(
        profile, Request(TlsProtocol::kTls13Auto)));
    EXPECT_FALSE(IsClientCertificateRequestForProfile(
        profile, Request(TlsProtocol::kStandardTls)));
  }
  EXPECT_FALSE(IsClientCertificateRequestForProfile(
      ConnectionProfile(TlsProtocol::kTls13Auto, "", 0, "", 0, 1), Request()));
}

TEST(GmClientCertConstraintsTest, FiltersClientAuthAndKeyUsage) {
  EXPECT_FALSE(MatchesClientCertificateRequest(test::ServerCertificate(), {},
                                               {}, Request(), ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(test::EncryptionCertificate(),
                                               {}, {}, Request(), ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      test::SigningCertificate(), test::SigningCertificate(), {},
      Request(TlsProtocol::kTlcp), ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      test::SigningCertificate(), test::ServerCertificate(), {},
      Request(TlsProtocol::kTlcp), ValidTime()));
}

TEST(GmClientCertConstraintsTest, RejectsCaCertificateAsClientLeaf) {
  EXPECT_FALSE(MatchesClientCertificateRequest(test::CaSigningCertificate(), {},
                                               {}, Request(), ValidTime()));
}

TEST(GmClientCertConstraintsTest, TlcpRequiresExplicitRoleKeyUsage) {
  const auto no_usage = test::NoKeyUsageCertificate();
  // TLS 1.3 allows an absent keyUsage extension. The pinned NTLS peer requires
  // explicit role usages on both leaves; reject before asking for a PIN.
  EXPECT_TRUE(MatchesClientCertificateRequest(no_usage, {}, {}, Request(),
                                              ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      no_usage, test::EncryptionCertificate(), {}, Request(TlsProtocol::kTlcp),
      ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      test::SigningCertificate(), no_usage, {}, Request(TlsProtocol::kTlcp),
      ValidTime()));
}

TEST(GmClientCertConstraintsTest, RequiresTheSm2NamedCurve) {
  // A valid EC signing certificate with clientAuth is insufficient: its curve
  // must be SM2, even when the requested signature scheme is SM2/SM3.
  EXPECT_FALSE(MatchesClientCertificateRequest(test::P256Certificate(), {}, {},
                                               Request(), ValidTime()));
}

TEST(GmClientCertConstraintsTest, EnforcesCertificateValidityWindow) {
  const auto signing = test::SigningCertificate();
  auto certificate = X509Certificate::CreateFromBytes(signing);
  ASSERT_TRUE(certificate);
  EXPECT_TRUE(MatchesClientCertificateRequest(signing, {}, {}, Request(),
                                              certificate->valid_start()));
  EXPECT_TRUE(MatchesClientCertificateRequest(signing, {}, {}, Request(),
                                              certificate->valid_expiry()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      signing, {}, {}, Request(),
      certificate->valid_start() - base::Seconds(1)));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      signing, {}, {}, Request(),
      certificate->valid_expiry() + base::Seconds(1)));
}

TEST(GmClientCertConstraintsTest, MatchesAnyAcceptedCaAndRejectsOtherIssuers) {
  const auto signing = test::SigningCertificate();
  auto request = Request();
  const auto other_name = IssuerName(test::OtherEncryptionCertificate());
  request.certificate_authorities = {other_name};
  EXPECT_FALSE(
      MatchesClientCertificateRequest(signing, {}, {}, request, ValidTime()));
  request.certificate_authorities.push_back(IssuerName(signing));
  EXPECT_TRUE(
      MatchesClientCertificateRequest(signing, {}, {}, request, ValidTime()));
  request.certificate_authorities = {{0x30, 0x01, 0xff}};
  EXPECT_FALSE(
      MatchesClientCertificateRequest(signing, {}, {}, request, ValidTime()));
}

TEST(GmClientCertConstraintsTest, UnrelatedBundledCertificateCannotMatchCa) {
  auto request = Request();
  const auto unrelated = test::OtherEncryptionCertificate();
  request.certificate_authorities = {IssuerName(unrelated)};
  // The extra certificate is not an issuer of the signing leaf. Its issuer
  // name must not authorize this identity simply because it was bundled.
  EXPECT_FALSE(MatchesClientCertificateRequest(
      test::SigningCertificate(), {}, {unrelated}, request, ValidTime()));
}

TEST(GmClientCertConstraintsTest, MatchesCaAboveTheImmediateIssuer) {
  auto request = Request();
  const auto signing = test::ChainedSigningCertificate();
  const auto intermediate = test::ClientIntermediateCertificate();
  request.certificate_authorities = {IssuerName(intermediate)};
  EXPECT_FALSE(
      MatchesClientCertificateRequest(signing, {}, {}, request, ValidTime()));
  EXPECT_TRUE(MatchesClientCertificateRequest(signing, {}, {intermediate},
                                              request, ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      signing, {}, {test::OtherEncryptionCertificate()}, request, ValidTime()));
}

TEST(GmClientCertConstraintsTest,
     SignatureSchemesAndCertificateTypesMustMatch) {
  const auto signing = test::SigningCertificate();
  auto request = Request();
  request.signature_algorithms = {0x0403, 0x0804};
  EXPECT_FALSE(
      MatchesClientCertificateRequest(signing, {}, {}, request, ValidTime()));
  request.signature_algorithms.push_back(kSm2Sm3);
  request.certificate_types = {1, 64};
  EXPECT_TRUE(
      MatchesClientCertificateRequest(signing, {}, {}, request, ValidTime()));
  request.certificate_types = {1};
  EXPECT_FALSE(
      MatchesClientCertificateRequest(signing, {}, {}, request, ValidTime()));
  request.certificate_types.clear();
  request.signature_algorithms.clear();
  EXPECT_FALSE(
      MatchesClientCertificateRequest(signing, {}, {}, request, ValidTime()));
  request = Request(TlsProtocol::kTlcp);
  request.signature_algorithms.clear();
  EXPECT_TRUE(MatchesClientCertificateRequest(
      signing, test::EncryptionCertificate(), {}, request, ValidTime()));
}

TEST(GmClientCertConstraintsTest, TlcpRequiresMatchingDistinctRoleKeys) {
  const auto request = Request(TlsProtocol::kTlcp);
  const auto signing = test::SigningCertificate();
  EXPECT_FALSE(
      MatchesClientCertificateRequest(signing, {}, {}, request, ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      signing, test::OtherEncryptionCertificate(), {}, request, ValidTime()));
  // This valid SM2 clientAuth certificate permits both usages, so failure
  // specifically protects the requirement for distinct signing/encryption keys.
  const auto both_roles = test::BothUsageCertificate();
  EXPECT_TRUE(MatchesClientCertificateRequest(both_roles, {}, {}, Request(),
                                              ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(both_roles, both_roles, {},
                                               request, ValidTime()));
}

TEST(GmClientCertConstraintsTest, RejectsWrongProtocolAndRoleMasks) {
  auto request = Request();
  for (TlsProtocol protocol :
       {TlsProtocol::kStandardTls, TlsProtocol::kTls13Auto,
        static_cast<TlsProtocol>(99)}) {
    request.protocol = protocol;
    EXPECT_FALSE(IsValidClientCertificateRequest(request));
  }
  request = Request();
  for (uint32_t roles :
       {0u, kClientCertificateEncryption,
        kClientCertificateSigning | kClientCertificateEncryption,
        0x80000001u}) {
    request.required_roles = roles;
    EXPECT_FALSE(IsValidClientCertificateRequest(request));
  }
  request = Request(TlsProtocol::kTlcp);
  request.required_roles = kClientCertificateSigning;
  EXPECT_FALSE(IsValidClientCertificateRequest(request));
}

TEST(GmClientCertConstraintsTest, BoundsCertificateRequestBeforeParsing) {
  auto request = Request();
  request.certificate_authorities.assign(128, {0x30, 0});
  EXPECT_TRUE(IsValidClientCertificateRequest(request));
  request.certificate_authorities.push_back({0x30, 0});
  EXPECT_FALSE(IsValidClientCertificateRequest(request));
  request.certificate_authorities = {{}};
  EXPECT_FALSE(IsValidClientCertificateRequest(request));
  request.certificate_authorities = {std::vector<uint8_t>(16385, 0)};
  EXPECT_FALSE(IsValidClientCertificateRequest(request));
  request.certificate_authorities.assign(4, std::vector<uint8_t>(16384, 0));
  EXPECT_FALSE(IsValidClientCertificateRequest(request));
  request.certificate_authorities.back().pop_back();
  EXPECT_TRUE(IsValidClientCertificateRequest(request));

  request = Request();
  request.signature_algorithms.assign(128, kSm2Sm3);
  EXPECT_TRUE(IsValidClientCertificateRequest(request));
  request.signature_algorithms.push_back(kSm2Sm3);
  EXPECT_FALSE(IsValidClientCertificateRequest(request));
  request = Request();
  request.certificate_types.assign(256, 64);
  EXPECT_TRUE(IsValidClientCertificateRequest(request));
  request.certificate_types.push_back(64);
  EXPECT_FALSE(IsValidClientCertificateRequest(request));
}

TEST(GmClientCertConstraintsTest, RejectsMalformedAndOversizedCertificates) {
  const auto signing = test::SigningCertificate();
  const auto request = Request();
  EXPECT_FALSE(
      MatchesClientCertificateRequest({}, {}, {}, request, ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(std::vector<uint8_t>{0x30, 0},
                                               {}, {}, request, ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      std::vector<uint8_t>(256 * 1024 + 1, 0), {}, {}, request, ValidTime()));
  EXPECT_FALSE(
      MatchesClientCertificateRequest(signing, {}, {{}}, request, ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(signing, {}, {{0x30, 0}},
                                               request, ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      signing, {}, {std::vector<uint8_t>(256 * 1024 + 1, 0)}, request,
      ValidTime()));
  EXPECT_FALSE(MatchesClientCertificateRequest(
      signing, {}, std::vector<std::vector<uint8_t>>(17, signing), request,
      ValidTime()));
}

}  // namespace
}  // namespace net::gm
