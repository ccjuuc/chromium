// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_certificate_verifier.h"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "net/base/ip_address.h"
#include "net/cert/x509_certificate.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/pool.h"
#include "third_party/boringssl/src/pki/cert_errors.h"
#include "third_party/boringssl/src/pki/cert_issuer_source_static.h"
#include "third_party/boringssl/src/pki/certificate_policies.h"
#include "third_party/boringssl/src/pki/common_cert_errors.h"
#include "third_party/boringssl/src/pki/crl.h"
#include "third_party/boringssl/src/pki/encode_values.h"
#include "third_party/boringssl/src/pki/general_names.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"
#include "third_party/boringssl/src/pki/path_builder.h"
#include "third_party/boringssl/src/pki/signature_algorithm.h"
#include "third_party/boringssl/src/pki/simple_path_builder_delegate.h"
#include "third_party/boringssl/src/pki/trust_store_in_memory.h"

namespace net::gm {
namespace {

constexpr size_t kMaxCertificateBytes = 256 * 1024;
constexpr size_t kMaxTrustBytes = 64 * 1024 * 1024;
using bssl::CertErrorId;
DEFINE_CERT_ERROR_ID(kGmRevoked,
                     "Certificate revoked by explicit enterprise CRL");
DEFINE_CERT_ERROR_ID(kGmMissingCrl, "Required enterprise CRL unavailable");

// TrustStoreInMemory permits duplicate certificates and does not define which
// duplicate wins. Explicit distrust must take precedence over every anchor,
// including duplicate roots and an identical peer-supplied certificate.
class ExplicitTrustStore final : public bssl::TrustStoreInMemory {
 public:
  void Distrust(const bssl::ParsedCertificate& cert) {
    const auto der = cert.der_cert();
    disallowed_.emplace(reinterpret_cast<const char*>(der.data()), der.size());
  }

  bssl::CertificateTrust GetTrust(
      const bssl::ParsedCertificate* cert) override {
    const auto der = cert->der_cert();
    const std::string_view bytes(reinterpret_cast<const char*>(der.data()),
                                 der.size());
    return disallowed_.contains(bytes)
               ? bssl::CertificateTrust::ForDistrusted()
               : bssl::TrustStoreInMemory::GetTrust(cert);
  }

 private:
  std::set<std::string, std::less<>> disallowed_;
};

std::shared_ptr<const bssl::ParsedCertificate> Parse(GmBytes der) {
  if (!der.data || !der.size || der.size > kMaxCertificateBytes) {
    return nullptr;
  }
  return bssl::ParsedCertificate::Create(
      bssl::UniquePtr<CRYPTO_BUFFER>(
          CRYPTO_BUFFER_new(der.data, der.size, nullptr)),
      {}, nullptr);
}

std::shared_ptr<const bssl::ParsedCertificate> Parse(X509* cert) {
  if (!cert) {
    return nullptr;
  }
  const int size = i2d_X509(cert, nullptr);
  if (size <= 0 || static_cast<size_t>(size) > kMaxCertificateBytes) {
    return nullptr;
  }
  std::vector<uint8_t> der(size);
  uint8_t* cursor = der.data();
  return i2d_X509(cert, &cursor) == size ? Parse({der.data(), der.size()})
                                         : nullptr;
}

class Delegate final : public bssl::SimplePathBuilderDelegate {
 public:
  Delegate(const std::vector<std::string>& crls,
           uint32_t revocation_mode,
           int64_t now)
      : SimplePathBuilderDelegate(2048, DigestPolicy::kStrong),
        crls_(crls),
        revocation_mode_(revocation_mode),
        now_(now),
        deadline_(base::TimeTicks::Now() + base::Seconds(1)) {}

  bool IsDeadlineExpired() override {
    return base::TimeTicks::Now() >= deadline_;
  }

  void CheckPathAfterVerification(
      const bssl::CertPathBuilder&,
      bssl::CertPathBuilderResultPath* path) override {
    if (!path->IsValid() || revocation_mode_ == GM_REVOCATION_DISABLED) {
      return;
    }
    // A directly trusted leaf has no issuer in this path with which to
    // authenticate a CRL. Do not silently waive a required revocation check.
    if (path->certs.size() < 2) {
      path->errors.GetErrorsForCert(0)->AddError(kGmMissingCrl);
      return;
    }
    const size_t count = revocation_mode_ == GM_REVOCATION_REQUIRE_LEAF_CRL
                             ? 1
                             : path->certs.size() - 1;
    for (size_t i = 0; i < count; ++i) {
      std::vector<bssl::ParsedDistributionPoint> points;
      bssl::ParsedExtension extension;
      if (path->certs[i]->GetExtension(
              bssl::der::Input(bssl::kCrlDistributionPointsOid), &extension) &&
          !bssl::ParseCrlDistributionPoints(extension.value, &points)) {
        path->errors.GetErrorsForCert(i)->AddError(kGmMissingCrl);
        continue;
      }
      if (points.size() > 128 || IsDeadlineExpired()) {
        path->errors.GetErrorsForCert(i)->AddError(kGmMissingCrl);
        return;
      }
      // An explicitly configured full issuer CRL need not appear in a URI
      // distribution point. An empty DP accepts only a full, direct CRL;
      // CheckCRL still enforces issuer, scope, signature and validity times.
      points.emplace_back();
      bool good = false;
      bool revoked = false;
      for (const auto& crl : crls_) {
        for (const auto& point : points) {
          if (IsDeadlineExpired()) {
            path->errors.GetErrorsForCert(i)->AddError(kGmMissingCrl);
            return;
          }
          const auto status =
              bssl::CheckCRL(crl, path->certs, i, point, now_, std::nullopt);
          good |= status == bssl::CRLRevocationStatus::GOOD;
          revoked |= status == bssl::CRLRevocationStatus::REVOKED;
          if (status != bssl::CRLRevocationStatus::UNKNOWN) {
            break;
          }
        }
      }
      if (revoked || !good) {
        path->errors.GetErrorsForCert(i)->AddError(revoked ? kGmRevoked
                                                           : kGmMissingCrl);
      }
    }
  }

 private:
  const std::vector<std::string>& crls_;
  const uint32_t revocation_mode_;
  const int64_t now_;
  const base::TimeTicks deadline_;
};

bool MatchesName(const bssl::ParsedCertificate& leaf,
                 std::string_view hostname,
                 uint32_t mode) {
  if (!leaf.has_subject_alt_names() || hostname.empty() ||
      hostname.find('\0') != std::string_view::npos) {
    return false;
  }
  if (mode == GM_HOSTNAME_BROWSER_SAN) {
    auto cert =
        X509Certificate::CreateFromBuffer(bssl::UpRef(leaf.cert_buffer()), {});
    return cert && cert->VerifyNameMatch(hostname);
  }
  IPAddress address;
  if (address.AssignFromIPLiteral(hostname)) {
    const auto bytes = address.bytes().span();
    return std::ranges::any_of(
        leaf.subject_alt_names()->ip_addresses, [&](const auto& ip) {
          return ip.size() == bytes.size() && std::ranges::equal(ip, bytes);
        });
  }
  return std::ranges::any_of(
      leaf.subject_alt_names()->dns_names, [&](std::string_view name) {
        return name.find('*') == std::string_view::npos &&
               base::EqualsCaseInsensitiveASCII(name, hostname);
      });
}

}  // namespace

struct GmCertificateVerifier::Impl {
  ExplicitTrustStore trust;
  bssl::ParsedCertificateList intermediates;
  std::vector<std::string> crls;
  uint32_t revocation_mode = GM_REVOCATION_DISABLED;
  uint32_t protocol = 0;
};

GmCertificateVerifier::GmCertificateVerifier(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
GmCertificateVerifier::~GmCertificateVerifier() = default;

std::unique_ptr<GmCertificateVerifier> GmCertificateVerifier::Create(
    const GmContextConfig& config) {
  if (config.struct_size != sizeof(config) ||
      (config.protocol != GM_PROTOCOL_TLCP &&
       config.protocol != GM_PROTOCOL_RFC8998) ||
      config.trust_anchor_count > 4096 ||
      config.untrusted_intermediate_count > 4096 ||
      config.disallowed_certificate_count > 4096 || config.crl_count > 128 ||
      (config.trust_anchor_count && !config.trust_anchors) ||
      (config.untrusted_intermediate_count &&
       !config.untrusted_intermediates) ||
      (config.disallowed_certificate_count &&
       !config.disallowed_certificates) ||
      (config.crl_count && !config.crls) ||
      config.revocation_mode > GM_REVOCATION_REQUIRE_CHAIN_CRLS) {
    return nullptr;
  }
  auto impl = std::make_unique<Impl>();
  impl->revocation_mode = config.revocation_mode;
  impl->protocol = config.protocol;
  size_t total = 0;
  for (int category = 0; category < 3; ++category) {
    const auto* data = category == 0   ? config.trust_anchors
                       : category == 1 ? config.untrusted_intermediates
                                       : config.disallowed_certificates;
    const size_t count = category == 0   ? config.trust_anchor_count
                         : category == 1 ? config.untrusted_intermediate_count
                                         : config.disallowed_certificate_count;
    // The C facade supplies these arrays; pointer presence and bounded counts
    // were checked above. Keep all indexing inside the resulting span.
    auto certificates = UNSAFE_BUFFERS(base::span(data, count));
    for (const auto& der : certificates) {
      if (der.size > kMaxTrustBytes - total) {
        return nullptr;
      }
      total += der.size;
      auto cert = Parse(der);
      if (!cert) {
        return nullptr;
      }
      if (category == 0) {
        impl->trust.AddCertificate(std::move(cert),
                                   bssl::CertificateTrust::ForTrustAnchor()
                                       .WithEnforceAnchorExpiry()
                                       .WithEnforceAnchorConstraints());
      } else if (category == 1) {
        impl->intermediates.push_back(std::move(cert));
      } else {
        impl->trust.Distrust(*cert);
      }
    }
  }
  // The array is caller-owned and its presence/count were validated above.
  auto crls = UNSAFE_BUFFERS(base::span(config.crls, config.crl_count));
  bssl::SimplePathBuilderDelegate crl_algorithm_policy(
      2048, bssl::SimplePathBuilderDelegate::DigestPolicy::kStrong);
  for (const auto& crl : crls) {
    if (!crl.data || !crl.size || crl.size > 1024 * 1024 ||
        crl.size > kMaxTrustBytes - total) {
      return nullptr;
    }
    total += crl.size;
    bssl::der::Input tbs, algorithm;
    bssl::der::BitString signature;
    bssl::ParsedCrlTbsCertList parsed;
    if (!bssl::ParseCrlCertificateList(bssl::der::Input(crl.data, crl.size),
                                       &tbs, &algorithm, &signature) ||
        !bssl::ParseCrlTbsCertList(tbs, &parsed) || !parsed.next_update ||
        *parsed.next_update <= parsed.this_update) {
      return nullptr;
    }
    // CheckCRL verifies the signature but does not apply a digest policy.
    // Apply the same strong policy used for certificate chains, including
    // when an ordinary RSA or EC issuer signs an SM2 leaf and its CRL.
    const auto outer_algorithm = bssl::ParseSignatureAlgorithm(algorithm);
    const auto tbs_algorithm =
        bssl::ParseSignatureAlgorithm(parsed.signature_algorithm_tlv);
    bssl::CertErrors algorithm_errors;
    if (!outer_algorithm || !tbs_algorithm ||
        *outer_algorithm != *tbs_algorithm ||
        !crl_algorithm_policy.IsSignatureAlgorithmAcceptable(
            *outer_algorithm, &algorithm_errors)) {
      return nullptr;
    }
    if (parsed.crl_extensions_tlv) {
      std::map<bssl::der::Input, bssl::ParsedExtension> extensions;
      // Delta CRLs require combining a separately authenticated base CRL.
      // This policy supports full direct CRLs only, including when a broken
      // issuer incorrectly marks deltaCRLIndicator as non-critical.
      constexpr uint8_t kDeltaCrlIndicatorOid[] = {0x55, 0x1d, 0x1b};
      if (!bssl::ParseExtensions(*parsed.crl_extensions_tlv, &extensions) ||
          extensions.contains(bssl::der::Input(kDeltaCrlIndicatorOid))) {
        return nullptr;
      }
    }
    impl->crls.emplace_back(reinterpret_cast<const char*>(crl.data), crl.size);
  }
  return std::unique_ptr<GmCertificateVerifier>(
      new GmCertificateVerifier(std::move(impl)));
}

bool GmCertificateVerifier::Verify(X509* leaf,
                                   STACK_OF(X509) * untrusted,
                                   std::string_view hostname,
                                   uint32_t hostname_mode,
                                   uint32_t role,
                                   STACK_OF(X509) * *verified,
                                   long* verify_result,
                                   uint32_t* certificate_errors) const {
  if (!verify_result || !certificate_errors) {
    return false;
  }
  *verify_result = X509_V_ERR_APPLICATION_VERIFICATION;
  *certificate_errors = GM_CERT_ERROR_INVALID;
  if (!verified || hostname_mode > GM_HOSTNAME_BROWSER_SAN ||
      (role != GM_CERTIFICATE_SIGNING && role != GM_CERTIFICATE_ENCRYPTION) ||
      sk_X509_num(untrusted) > 32) {
    return false;
  }
  auto parsed = Parse(leaf);
  if (!parsed) {
    return false;
  }
  bssl::UniquePtr<EVP_PKEY> public_key(X509_get_pubkey(leaf));
  const EC_KEY* ec =
      public_key ? EVP_PKEY_get0_EC_KEY(public_key.get()) : nullptr;
  if (!ec || EC_GROUP_get_curve_name(EC_KEY_get0_group(ec)) != NID_sm2 ||
      (parsed->has_basic_constraints() && parsed->basic_constraints().is_ca) ||
      (impl_->protocol == GM_PROTOCOL_TLCP && !parsed->has_key_usage()) ||
      (parsed->has_key_usage() &&
       (role == GM_CERTIFICATE_SIGNING
            ? !parsed->key_usage().AssertsBit(
                  bssl::KEY_USAGE_BIT_DIGITAL_SIGNATURE)
            : !(parsed->key_usage().AssertsBit(
                    bssl::KEY_USAGE_BIT_KEY_ENCIPHERMENT) ||
                parsed->key_usage().AssertsBit(
                    bssl::KEY_USAGE_BIT_DATA_ENCIPHERMENT) ||
                parsed->key_usage().AssertsBit(
                    bssl::KEY_USAGE_BIT_KEY_AGREEMENT))))) {
    *verify_result = X509_V_ERR_INVALID_PURPOSE;
    return false;
  }
  if (!MatchesName(*parsed, hostname, hostname_mode)) {
    *verify_result = X509_V_ERR_HOSTNAME_MISMATCH;
    *certificate_errors = GM_CERT_ERROR_NAME;
    return false;
  }
  bssl::CertIssuerSourceStatic issuers;
  for (const auto& issuer : impl_->intermediates) {
    issuers.AddCert(issuer);
  }
  size_t total = 0;
  for (size_t i = 0; i < sk_X509_num(untrusted); ++i) {
    auto issuer = Parse(sk_X509_value(untrusted, i));
    if (!issuer || issuer->der_cert().size() > 1024 * 1024 - total) {
      return false;
    }
    total += issuer->der_cert().size();
    issuers.AddCert(std::move(issuer));
  }
  const base::Time now = base::Time::Now();
  const int64_t now_seconds = (now - base::Time::UnixEpoch()).InSeconds();
  bssl::der::GeneralizedTime time;
  if (!bssl::der::EncodePosixTimeAsGeneralizedTime(now_seconds, &time)) {
    return false;
  }
  Delegate delegate(impl_->crls, impl_->revocation_mode, now_seconds);
  bssl::CertPathBuilder builder(parsed, &impl_->trust, &delegate, time,
                                bssl::KeyPurpose::SERVER_AUTH,
                                bssl::InitialExplicitPolicy::kFalse,
                                {bssl::der::Input(bssl::kAnyPolicyOid)},
                                bssl::InitialPolicyMappingInhibit::kFalse,
                                bssl::InitialAnyPolicyInhibit::kFalse);
  builder.SetIterationLimit(1000);
  builder.SetDepthLimit(17);
  builder.AddCertIssuerSource(&issuers);
  auto result = builder.Run();
  const auto* path = result.GetBestValidPath();
  if (!path) {
    if (result.AnyPathContainsError(kGmRevoked)) {
      *verify_result = X509_V_ERR_CERT_REVOKED;
      *certificate_errors = GM_CERT_ERROR_REVOKED;
    } else if (result.AnyPathContainsError(kGmMissingCrl)) {
      *verify_result = X509_V_ERR_UNABLE_TO_GET_CRL;
      *certificate_errors = GM_CERT_ERROR_REVOCATION_UNAVAILABLE;
    } else if (result.AnyPathContainsError(
                   bssl::cert_errors::kDistrustedByTrustStore)) {
      *verify_result = X509_V_ERR_CERT_REJECTED;
      *certificate_errors = GM_CERT_ERROR_AUTHORITY;
    } else if (result.AnyPathContainsError(
                   bssl::cert_errors::kValidityFailedNotAfter) ||
               result.AnyPathContainsError(
                   bssl::cert_errors::kValidityFailedNotBefore)) {
      *verify_result = X509_V_ERR_CERT_HAS_EXPIRED;
      *certificate_errors = GM_CERT_ERROR_DATE;
    } else {
      using Code = bssl::VerifyError::StatusCode;
      switch (result.GetBestPathVerifyError().Code()) {
        case Code::CERTIFICATE_EXPIRED:
        case Code::CERTIFICATE_NOT_YET_VALID:
          *verify_result = X509_V_ERR_CERT_HAS_EXPIRED;
          *certificate_errors = GM_CERT_ERROR_DATE;
          break;
        case Code::PATH_NOT_FOUND:
          *verify_result = X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY;
          *certificate_errors = GM_CERT_ERROR_AUTHORITY;
          break;
        case Code::CERTIFICATE_UNSUPPORTED_KEY:
        case Code::CERTIFICATE_UNSUPPORTED_SIGNATURE_ALGORITHM:
          *certificate_errors = GM_CERT_ERROR_WEAK_SIGNATURE;
          break;
        default:
          break;
      }
    }
    return false;
  }
  bssl::UniquePtr<STACK_OF(X509)> chain(sk_X509_new_null());
  if (!chain) {
    return false;
  }
  for (const auto& cert : path->certs) {
    const auto bytes = cert->der_cert();
    const uint8_t* cursor = bytes.data();
    bssl::UniquePtr<X509> x509(d2i_X509(nullptr, &cursor, bytes.size()));
    if (!x509 || !sk_X509_push(chain.get(), x509.get())) {
      return false;
    }
    x509.release();
  }
  *verify_result = X509_V_OK;
  *certificate_errors = 0;
  sk_X509_pop_free(*verified, X509_free);
  *verified = chain.release();
  return true;
}

}  // namespace net::gm
