// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/managed_certificates/builtin_store_certificate.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "base/base_paths.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/string_view_util.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "net/base/net_errors.h"
#include "net/cert/cert_status_flags.h"
#include "net/cert/cert_verify_proc_builtin.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/crl_set.h"
#include "net/cert/ct_policy_enforcer.h"
#include "net/cert/do_nothing_ct_verifier.h"
#include "net/cert/internal/system_trust_store.h"
#include "net/cert/internal/trust_store_chrome.h"
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"
#include "net/log/net_log_with_source.h"
#include "net/net_buildflags.h"
#include "services/cert_verifier/cert_verifier_creation.h"
#include "services/network/public/mojom/cert_verifier_service_updater.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"
#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_merger.h"
#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_types.h"

namespace xenon::certificates {
namespace {

class BuiltinStoreCertificateTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root_));
    base::Time verification_time;
    ASSERT_TRUE(base::Time::FromUTCString("2027-01-01T00:00:00Z",
                                         &verification_time));
    task_environment_.AdvanceClock(verification_time - base::Time::Now());
  }

  scoped_refptr<net::X509Certificate> ReadCertificate(
      const base::FilePath& path) {
    std::string pem;
    EXPECT_TRUE(base::ReadFileToString(path, &pem));
    auto certificates = net::X509Certificate::CreateCertificateListFromBytes(
        base::as_byte_span(pem), net::X509Certificate::FORMAT_PEM_CERT_SEQUENCE);
    EXPECT_EQ(certificates.size(), 1u);
    return certificates.empty() ? nullptr : certificates[0];
  }

  scoped_refptr<net::X509Certificate> Fixture(std::string_view name) {
    return ReadCertificate(
        root_
            .AppendASCII("xenon_overlay/chrome/browser/managed_certificates/"
                         "testdata")
            .AppendASCII(name));
  }

  Certificate ReissuedRoot() {
    Certificate result = GetBuiltinStoreCertificate();
    const auto reissued = Fixture("store-constrained-root.pem");
    if (reissued) {
      result.der.assign(reissued->cert_span().begin(),
                        reissued->cert_span().end());
    }
    return result;
  }

#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
  int Verify(const cert_verifier::mojom::AdditionalCertificatesPtr& certificates,
             std::string_view fixture,
             std::string hostname,
             net::CertStatus* cert_status = nullptr) {
    auto leaf = Fixture(fixture);
    if (!leaf) {
      return net::ERR_FAILED;
    }
    net::CertVerifyProc::InstanceParams params;
    cert_verifier::UpdateCertVerifierInstanceParams(certificates, &params);
    auto verifier = net::CreateCertVerifyProcBuiltin(
        nullptr, net::CRLSet::EmptyCRLSetForTesting(),
        std::make_unique<net::DoNothingCTVerifier>(),
        base::MakeRefCounted<net::DefaultCTPolicyEnforcer>(),
        net::CreateChromeOnlySystemTrustStore(
            net::TrustStoreChrome::CreateTrustStoreForTesting({}, {}, 1)),
        params, std::nullopt);
    base::test::TestFuture<std::pair<int, net::CertStatus>> result;
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            [](scoped_refptr<net::CertVerifyProc> proc,
               scoped_refptr<net::X509Certificate> cert, std::string host) {
              net::CertVerifyResult verification_result;
              const int error = proc->Verify(
                  cert.get(), host, {}, {},
                  net::CertVerifyProc::VERIFY_DISABLE_NETWORK_FETCHES,
                  &verification_result, net::NetLogWithSource());
              return std::make_pair(error, verification_result.cert_status);
            },
            std::move(verifier), std::move(leaf), std::move(hostname)),
        result.GetCallback());
    const auto [error, status] = result.Get();
    if (cert_status) {
      *cert_status = status;
    }
    return error;
  }

  void ExpectNameConstraintRejection(
      const cert_verifier::mojom::AdditionalCertificatesPtr& certificates,
      std::string_view fixture,
      std::string hostname) {
    // MapPathBuilderErrorsToCertStatus maps BoringSSL's
    // kNotPermittedByNameConstraints to CERT_STATUS_INVALID. The dedicated
    // NAME_CONSTRAINT_VIOLATION status belongs to CertVerifyProc's separate
    // hardcoded public-key constraints, not these X.509/additional constraints.
    net::CertStatus status = 0;
    EXPECT_EQ(net::ERR_CERT_INVALID,
              Verify(certificates, fixture, std::move(hostname), &status));
    EXPECT_EQ(net::CERT_STATUS_INVALID, status);
  }
#endif  // BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::FilePath root_;
};

TEST_F(BuiltinStoreCertificateTest, MatchesPublicToolCertificateAndPinnedKey) {
  const auto tool_certificate = ReadCertificate(root_.AppendASCII(
      "tools/xl_extension_store/xl_store_root_ca.pem"));
  ASSERT_TRUE(tool_certificate);
  const auto& builtin = GetBuiltinStoreCertificate();
  EXPECT_EQ(base::as_string_view(builtin.der),
            base::as_string_view(tool_certificate->cert_span()));
  EXPECT_EQ(Sha256Hex(base::as_string_view(builtin.spki)),
            "e685858a1f8beb98eeb4cc7b5a310d7aed0881d2f4385be1a1d9d6c501bacb51");
  EXPECT_EQ(&builtin, &GetBuiltinStoreCertificate());
}

TEST_F(BuiltinStoreCertificateTest, UsesOrdinaryCaWithoutAdditionalNameLimits) {
  const auto& builtin = GetBuiltinStoreCertificate();
  const auto parsed = bssl::ParsedCertificate::Create(
      net::x509_util::CreateCryptoBuffer(builtin.der), {}, nullptr);
  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed->has_basic_constraints());
  EXPECT_TRUE(parsed->basic_constraints().is_ca);
  EXPECT_FALSE(parsed->has_name_constraints());
  EXPECT_TRUE(builtin.permitted_dns_names.empty());
  EXPECT_TRUE(builtin.permitted_cidrs.empty());
}

TEST_F(BuiltinStoreCertificateTest, ReissuedSnapshotKeepsBuiltinIdentity) {
  const auto reissued = ReissuedRoot();
  ASSERT_NE(reissued.der, GetBuiltinStoreCertificate().der);
  const auto parsed = bssl::ParsedCertificate::Create(
      net::x509_util::CreateCryptoBuffer(reissued.der), {}, nullptr);
  ASSERT_TRUE(parsed);
  ASSERT_TRUE(parsed->has_name_constraints());
  EXPECT_EQ(base::as_string_view(reissued.spki),
            base::as_string_view(parsed->tbs().spki_tlv));

  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  MergeBuiltinStoreCertificate(result.get());
  ASSERT_EQ(result->xenon_managed_trust_anchors.size(), 1u);
  auto initial = result.Clone();
  Snapshot downloaded;
  downloaded.certificates = {GetBuiltinStoreCertificate(), reissued};
  MergeManagedCertificates(&downloaded, result.get());
  MergeBuiltinStoreCertificate(result.get());
  EXPECT_EQ(initial, result);

  // A full remote-snapshot replacement must include the built-in root even
  // when the download is absent or no longer contains the store certificate.
  auto replacement = cert_verifier::mojom::AdditionalCertificates::New();
  MergeBuiltinStoreCertificate(replacement.get());
  MergeManagedCertificates(nullptr, replacement.get());
  EXPECT_EQ(initial, replacement);
}

TEST_F(BuiltinStoreCertificateTest, EveryNativeTrustCategoryRetainsPrecedence) {
  const auto reissued = ReissuedRoot();
  for (int category = 0; category < 5; ++category) {
    SCOPED_TRACE(category);
    auto result = cert_verifier::mojom::AdditionalCertificates::New();
    auto constrained = cert_verifier::mojom::CertWithConstraints::New();
    constrained->certificate = reissued.der;
    constrained->permitted_dns_names = {"child.xl.extensions.test.com"};
    switch (category) {
      case 0:
        result->trust_anchors.push_back(reissued.der);
        break;
      case 1:
        result->trust_anchors_with_enforced_constraints.push_back(reissued.der);
        break;
      case 2:
        result->trust_anchors_with_additional_constraints.push_back(
            std::move(constrained));
        break;
      case 3:
        result->trust_anchors_and_leafs.push_back(std::move(constrained));
        break;
      case 4:
        result->trust_leafs.push_back(std::move(constrained));
        break;
    }
    auto native = result.Clone();
    MergeBuiltinStoreCertificate(result.get());
    EXPECT_EQ(native, result);
  }
}

TEST_F(BuiltinStoreCertificateTest, CertificateHintsDoNotCountAsTrust) {
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  result->all_certificates.push_back(ReissuedRoot().der);
  MergeBuiltinStoreCertificate(result.get());
  ASSERT_EQ(result->xenon_managed_trust_anchors.size(), 1u);
  EXPECT_EQ(result->xenon_managed_trust_anchors[0]->certificate,
            GetBuiltinStoreCertificate().der);
}

TEST_F(BuiltinStoreCertificateTest, LocalSpkiDistrustSuppressesBuiltinTrust) {
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  result->distrusted_spkis.push_back(GetBuiltinStoreCertificate().spki);
  MergeBuiltinStoreCertificate(result.get());
  EXPECT_TRUE(result->xenon_managed_trust_anchors.empty());
  EXPECT_EQ(result->distrusted_spkis.size(), 1u);
}

#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
TEST_F(BuiltinStoreCertificateTest, InitialConfigurationVerifiesStoreAndChild) {
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  EXPECT_EQ(net::ERR_CERT_AUTHORITY_INVALID,
            Verify(result, "store-leaf.pem", "xl.extensions.test.com"));
  MergeBuiltinStoreCertificate(result.get());
  EXPECT_EQ(net::OK,
            Verify(result, "store-leaf.pem", "xl.extensions.test.com"));
  EXPECT_EQ(net::OK,
            Verify(result, "store-child.pem", "child.xl.extensions.test.com"));
}

TEST_F(BuiltinStoreCertificateTest, VerifiesValidOtherDnsAndIpCertificates) {
  auto builtin = cert_verifier::mojom::AdditionalCertificates::New();
  MergeBuiltinStoreCertificate(builtin.get());
  EXPECT_EQ(net::OK, Verify(builtin, "store-outside.pem", "outside.test.com"));
  EXPECT_EQ(net::OK, Verify(builtin, "store-ip.pem", "127.0.0.1"));
}

TEST_F(BuiltinStoreCertificateTest, ReissuedSnapshotKeepsBuiltinVerification) {
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  MergeBuiltinStoreCertificate(result.get());
  Snapshot downloaded;
  downloaded.certificates.push_back(ReissuedRoot());
  MergeManagedCertificates(&downloaded, result.get());
  EXPECT_EQ(net::OK, Verify(result, "store-outside.pem", "outside.test.com"));
  EXPECT_EQ(net::OK, Verify(result, "store-ip.pem", "127.0.0.1"));
}

TEST_F(BuiltinStoreCertificateTest, StillRejectsSubjectAlternativeNameMismatch) {
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  MergeBuiltinStoreCertificate(result.get());
  net::CertStatus status = 0;
  EXPECT_EQ(net::ERR_CERT_COMMON_NAME_INVALID,
            Verify(result, "store-leaf.pem", "outside.test.com", &status));
  EXPECT_EQ(net::CERT_STATUS_COMMON_NAME_INVALID, status);
  EXPECT_EQ(net::ERR_CERT_COMMON_NAME_INVALID,
            Verify(result, "store-ip.pem", "127.0.0.2", &status));
  EXPECT_EQ(net::CERT_STATUS_COMMON_NAME_INVALID, status);
}

TEST_F(BuiltinStoreCertificateTest, StillRejectsExpiredServerCertificate) {
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  MergeBuiltinStoreCertificate(result.get());
  EXPECT_EQ(net::OK,
            Verify(result, "store-leaf.pem", "xl.extensions.test.com"));
  task_environment_.AdvanceClock(base::Days(366));
  net::CertStatus status = 0;
  EXPECT_EQ(net::ERR_CERT_DATE_INVALID,
            Verify(result, "store-leaf.pem", "xl.extensions.test.com", &status));
  EXPECT_EQ(net::CERT_STATUS_DATE_INVALID, status);
}

TEST_F(BuiltinStoreCertificateTest, NativeConstraintsWinForSameReissuedKey) {
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  auto native = cert_verifier::mojom::CertWithConstraints::New();
  native->certificate = ReissuedRoot().der;
  native->permitted_dns_names = {"child.xl.extensions.test.com"};
  result->trust_anchors_with_additional_constraints.push_back(std::move(native));
  MergeBuiltinStoreCertificate(result.get());
  EXPECT_EQ(net::OK,
            Verify(result, "store-child.pem", "child.xl.extensions.test.com"));
  ExpectNameConstraintRejection(result, "store-leaf.pem", "xl.extensions.test.com");
}

TEST_F(BuiltinStoreCertificateTest, DistrustPreventsStoreVerification) {
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  result->distrusted_spkis.push_back(GetBuiltinStoreCertificate().spki);
  MergeBuiltinStoreCertificate(result.get());
  EXPECT_EQ(net::ERR_CERT_AUTHORITY_INVALID,
            Verify(result, "store-leaf.pem", "xl.extensions.test.com"));
}
#endif  // BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)

}  // namespace
}  // namespace xenon::certificates
