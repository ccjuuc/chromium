// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_merger.h"

#include "services/network/public/mojom/cert_verifier_service_updater.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_types.h"

namespace xenon::certificates {
namespace {

Certificate MakeCertificate(uint8_t id) {
  Certificate certificate;
  certificate.der = {id};
  certificate.spki = {id, id};
  return certificate;
}

TEST(ManagedCertificateMergerTest, KeepsManagedAnchorsSeparateWithConstraints) {
  Snapshot snapshot;
  auto cert = MakeCertificate(1);
  cert.permitted_dns_names = {"example.com"};
  cert.permitted_cidrs.push_back({net::IPAddress(10, 0, 0, 0), 8});
  snapshot.certificates.push_back(cert);
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  MergeManagedCertificates(&snapshot, result.get());
  ASSERT_EQ(result->xenon_managed_trust_anchors.size(), 1u);
  const auto& managed = result->xenon_managed_trust_anchors[0];
  EXPECT_EQ(managed->certificate, cert.der);
  EXPECT_EQ(managed->permitted_dns_names, cert.permitted_dns_names);
  ASSERT_EQ(managed->permitted_cidrs.size(), 1u);
  EXPECT_EQ(managed->permitted_cidrs[0]->mask, net::IPAddress(255, 0, 0, 0));
  EXPECT_TRUE(result->trust_anchors.empty());
  EXPECT_TRUE(result->trust_anchors_with_enforced_constraints.empty());
}

TEST(ManagedCertificateMergerTest, PreservesEveryNativeTrustCategory) {
  Snapshot snapshot;
  for (uint8_t id = 1; id <= 5; ++id) {
    snapshot.certificates.push_back(MakeCertificate(id));
  }
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  result->trust_anchors.push_back({1});
  result->trust_anchors_with_enforced_constraints.push_back({2});
  auto constrained = cert_verifier::mojom::CertWithConstraints::New();
  constrained->certificate = {3};
  constrained->permitted_dns_names = {"native.example"};
  result->trust_anchors_with_additional_constraints.push_back(
      std::move(constrained));
  auto anchor_leaf = cert_verifier::mojom::CertWithConstraints::New();
  anchor_leaf->certificate = {4};
  result->trust_anchors_and_leafs.push_back(std::move(anchor_leaf));
  auto leaf = cert_verifier::mojom::CertWithConstraints::New();
  leaf->certificate = {5};
  result->trust_leafs.push_back(std::move(leaf));
  auto before = result.Clone();
  MergeManagedCertificates(&snapshot, result.get());
  EXPECT_EQ(before, result);
}

TEST(ManagedCertificateMergerTest, DoesNotOverrideSpkiDistrust) {
  Snapshot snapshot;
  snapshot.certificates.push_back(MakeCertificate(1));
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  result->distrusted_spkis.push_back(snapshot.certificates[0].spki);
  MergeManagedCertificates(&snapshot, result.get());
  EXPECT_TRUE(result->xenon_managed_trust_anchors.empty());
  EXPECT_EQ(result->distrusted_spkis.size(), 1u);
}

TEST(ManagedCertificateMergerTest, IntermediatesAreOnlyHintsAndDeduplicated) {
  Snapshot snapshot;
  auto intermediate = MakeCertificate(1);
  intermediate.trust_anchor = false;
  snapshot.certificates = {intermediate, intermediate};
  auto result = cert_verifier::mojom::AdditionalCertificates::New();
  MergeManagedCertificates(&snapshot, result.get());
  EXPECT_TRUE(result->xenon_managed_trust_anchors.empty());
  ASSERT_EQ(result->all_certificates.size(), 1u);
  EXPECT_EQ(result->all_certificates[0], intermediate.der);
  MergeManagedCertificates(nullptr, result.get());
  EXPECT_EQ(result->all_certificates.size(), 1u);
}

}  // namespace
}  // namespace xenon::certificates
