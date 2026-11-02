// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_client_cert_constraints.h"

#include <algorithm>

#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"

namespace net::gm {
namespace {
constexpr uint16_t kSm2Sm3 = 0x0708;
constexpr uint8_t kEcSign = 64;
constexpr size_t kMaxCertificate = 256 * 1024;

bool HasSm2PublicKey(bssl::der::Input spki) {
  // id-ecPublicKey + the named curve 1.2.156.10197.1.301. Merely being
  // EC, or having an SM2 issuer signature, does not make a client key SM2.
  static constexpr uint8_t kEcPublicKey[] = {0x2a, 0x86, 0x48, 0xce,
                                             0x3d, 0x02, 0x01};
  static constexpr uint8_t kSm2Curve[] = {0x2a, 0x81, 0x1c, 0xcf,
                                          0x55, 0x01, 0x82, 0x2d};
  CBS input, sequence, algorithm, oid, curve, key;
  uint8_t unused_bits = 0, encoding = 0;
  CBS_init(&input, spki.data(), spki.size());
  return CBS_get_asn1(&input, &sequence, CBS_ASN1_SEQUENCE) &&
         CBS_len(&input) == 0 &&
         CBS_get_asn1(&sequence, &algorithm, CBS_ASN1_SEQUENCE) &&
         CBS_get_asn1(&algorithm, &oid, CBS_ASN1_OBJECT) &&
         CBS_mem_equal(&oid, kEcPublicKey, sizeof(kEcPublicKey)) &&
         CBS_get_asn1(&algorithm, &curve, CBS_ASN1_OBJECT) &&
         CBS_mem_equal(&curve, kSm2Curve, sizeof(kSm2Curve)) &&
         CBS_len(&algorithm) == 0 &&
         CBS_get_asn1(&sequence, &key, CBS_ASN1_BITSTRING) &&
         CBS_len(&sequence) == 0 && CBS_len(&key) == 66 &&
         CBS_get_u8(&key, &unused_bits) && unused_bits == 0 &&
         CBS_get_u8(&key, &encoding) && encoding == 4;
}

std::shared_ptr<const bssl::ParsedCertificate> CheckLeaf(
    base::span<const uint8_t> der,
    bool encryption,
    const bssl::ParsedCertificateList& intermediates,
    const ClientCertificateRequest& request,
    base::Time now) {
  if (der.empty() || der.size() > kMaxCertificate) {
    return nullptr;
  }
  auto parsed = bssl::ParsedCertificate::Create(
      x509_util::CreateCryptoBuffer(der), {}, nullptr);
  if (!parsed || !HasSm2PublicKey(parsed->tbs().spki_tlv) ||
      (request.protocol == TlsProtocol::kTlcp && !parsed->has_key_usage()) ||
      (parsed->has_basic_constraints() && parsed->basic_constraints().is_ca)) {
    return nullptr;
  }
  if (parsed->has_key_usage()) {
    const auto& usage = parsed->key_usage();
    if (encryption
            ? !(usage.AssertsBit(bssl::KEY_USAGE_BIT_KEY_ENCIPHERMENT) ||
                usage.AssertsBit(bssl::KEY_USAGE_BIT_DATA_ENCIPHERMENT) ||
                usage.AssertsBit(bssl::KEY_USAGE_BIT_KEY_AGREEMENT))
            : !usage.AssertsBit(bssl::KEY_USAGE_BIT_DIGITAL_SIGNATURE)) {
      return nullptr;
    }
  }
  if (parsed->has_extended_key_usage()) {
    static constexpr uint8_t kClientAuth[] = {0x2b, 0x06, 0x01, 0x05,
                                              0x05, 0x07, 0x03, 0x02};
    static constexpr uint8_t kAnyUsage[] = {0x55, 0x1d, 0x25, 0x00};
    if (!std::ranges::any_of(parsed->extended_key_usage(), [](auto oid) {
          return oid == bssl::der::Input(kClientAuth) ||
                 oid == bssl::der::Input(kAnyUsage);
        })) {
      return nullptr;
    }
  }
  auto certificate = X509Certificate::CreateFromBytes(der);
  if (!certificate || now < certificate->valid_start() ||
      now > certificate->valid_expiry()) {
    return nullptr;
  }
  if (!request.certificate_authorities.empty()) {
    std::vector<std::string> issuers;
    for (const auto& ca : request.certificate_authorities) {
      issuers.emplace_back(reinterpret_cast<const char*>(ca.data()), ca.size());
    }
    // Only follow issuer candidates reachable from this leaf. Scanning the
    // entire supplied bundle would let an unrelated certificate's issuer DN
    // satisfy the request and prompt for an unsuitable identity.
    bssl::ParsedCertificateList reachable{parsed};
    std::vector<bool> visited(intermediates.size());
    bool matches = false;
    for (size_t next = 0; next < reachable.size() && !matches; ++next) {
      const auto current = reachable[next];
      auto current_cert = X509Certificate::CreateFromBytes(
          x509_util::CryptoBufferAsSpan(current->cert_buffer()));
      if (current_cert && current_cert->IsIssuedByEncoded(issuers)) {
        matches = true;
        break;
      }
      for (size_t i = 0; i < intermediates.size(); ++i) {
        const auto& candidate = intermediates[i];
        if (visited[i] ||
            candidate->normalized_subject() != current->normalized_issuer() ||
            !candidate->has_basic_constraints() ||
            !candidate->basic_constraints().is_ca ||
            (candidate->has_key_usage() &&
             !candidate->key_usage().AssertsBit(
                 bssl::KEY_USAGE_BIT_KEY_CERT_SIGN))) {
          continue;
        }
        visited[i] = true;
        auto issuer_cert = X509Certificate::CreateFromBytes(
            x509_util::CryptoBufferAsSpan(candidate->cert_buffer()));
        if (issuer_cert && now >= issuer_cert->valid_start() &&
            now <= issuer_cert->valid_expiry()) {
          reachable.push_back(candidate);
        }
      }
    }
    if (!matches) {
      return nullptr;
    }
  }
  return parsed;
}
}  // namespace

bool IsValidClientCertificateRequest(const ClientCertificateRequest& request) {
  const bool tlcp = request.protocol == TlsProtocol::kTlcp;
  if ((!tlcp && request.protocol != TlsProtocol::kRfc8998) ||
      request.required_roles != (kClientCertificateSigning |
                                 (tlcp ? kClientCertificateEncryption : 0)) ||
      request.certificate_authorities.size() > 128 ||
      request.signature_algorithms.size() > 128 ||
      request.certificate_types.size() > 256) {
    return false;
  }
  size_t bytes = 0;
  for (const auto& ca : request.certificate_authorities) {
    if (ca.empty() || ca.size() > 16384) {
      return false;
    }
    bytes += ca.size();
  }
  return bytes <= 65535;
}

bool IsClientCertificateRequestForProfile(
    const ConnectionProfile& profile,
    const ClientCertificateRequest& request) {
  return profile.IsValid() && IsValidClientCertificateRequest(request) &&
         (profile.protocol() == request.protocol ||
          (profile.protocol() == TlsProtocol::kTls13Auto &&
           request.protocol == TlsProtocol::kRfc8998));
}

bool MatchesClientCertificateRequest(
    base::span<const uint8_t> signing_certificate,
    base::span<const uint8_t> encryption_certificate,
    const std::vector<std::vector<uint8_t>>& intermediates,
    const ClientCertificateRequest& request,
    base::Time now) {
  // Match the backend's identity-chain limit before requesting Key access.
  if (!IsValidClientCertificateRequest(request) || intermediates.size() > 16 ||
      std::ranges::any_of(intermediates,
                          [](const auto& cert) {
                            return cert.empty() ||
                                   cert.size() > kMaxCertificate;
                          }) ||
      (!request.signature_algorithms.empty() &&
       !std::ranges::contains(request.signature_algorithms, kSm2Sm3)) ||
      (request.protocol == TlsProtocol::kRfc8998 &&
       request.signature_algorithms.empty()) ||
      (!request.certificate_types.empty() &&
       !std::ranges::contains(request.certificate_types, kEcSign))) {
    return false;
  }
  bssl::ParsedCertificateList parsed_intermediates;
  for (const auto& der : intermediates) {
    if (!bssl::ParsedCertificate::CreateAndAddToVector(
            x509_util::CreateCryptoBuffer(der), {}, &parsed_intermediates,
            nullptr)) {
      return false;
    }
  }
  auto signing =
      CheckLeaf(signing_certificate, false, parsed_intermediates, request, now);
  if (!signing) {
    return false;
  }
  if (request.required_roles & kClientCertificateEncryption) {
    auto encryption = CheckLeaf(encryption_certificate, true,
                                parsed_intermediates, request, now);
    // The pair must identify the same subject under the same issuer and
    // carry distinct role keys. Both certificates must satisfy the request.
    return encryption &&
           signing->normalized_subject() == encryption->normalized_subject() &&
           signing->normalized_issuer() == encryption->normalized_issuer() &&
           signing->tbs().spki_tlv != encryption->tbs().spki_tlv;
  }
  return true;
}

}  // namespace net::gm
