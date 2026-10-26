// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_merger.h"

#include <algorithm>

#include "services/network/public/mojom/cert_verifier_service_updater.mojom.h"
#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_types.h"

namespace xenon::certificates {
namespace {
bool HasExplicitTrust(const cert_verifier::mojom::AdditionalCertificates& certs,
                      const std::vector<uint8_t>& der) {
  const auto contains = [&](const auto& list) {
    return std::ranges::any_of(
        list, [&](const auto& cert) { return cert->certificate == der; });
  };
  return std::ranges::contains(certs.trust_anchors, der) ||
         std::ranges::contains(certs.trust_anchors_with_enforced_constraints,
                               der) ||
         contains(certs.trust_anchors_with_additional_constraints) ||
         contains(certs.trust_anchors_and_leafs) ||
         contains(certs.trust_leafs) ||
         contains(certs.xenon_managed_trust_anchors);
}
}  // namespace

void MergeManagedCertificates(
    const Snapshot* snapshot,
    cert_verifier::mojom::AdditionalCertificates* certificates) {
  if (!snapshot) {
    return;
  }
  for (const auto& cert : snapshot->certificates) {
    if (HasExplicitTrust(*certificates, cert.der) ||
        std::ranges::contains(certificates->distrusted_spkis, cert.spki)) {
      continue;
    }
    if (!cert.trust_anchor) {
      if (!std::ranges::contains(certificates->all_certificates, cert.der)) {
        certificates->all_certificates.push_back(cert.der);
      }
      continue;
    }
    auto managed = cert_verifier::mojom::CertWithConstraints::New();
    managed->certificate = cert.der;
    managed->permitted_dns_names = cert.permitted_dns_names;
    bool valid = true;
    for (const auto& cidr : cert.permitted_cidrs) {
      net::IPAddress mask;
      if (!(cidr.address.IsIPv4()
                ? net::IPAddress::CreateIPv4Mask(&mask, cidr.prefix_length)
                : net::IPAddress::CreateIPv6Mask(&mask, cidr.prefix_length))) {
        valid = false;
        break;
      }
      managed->permitted_cidrs.push_back(
          cert_verifier::mojom::CIDR::New(cidr.address, mask));
    }
    if (valid) {
      certificates->xenon_managed_trust_anchors.push_back(std::move(managed));
    }
  }
}
}  // namespace xenon::certificates
