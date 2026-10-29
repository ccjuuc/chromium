// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_CERTIFICATE_VERIFIER_H_
#define NET_GM_GM_CERTIFICATE_VERIFIER_H_

#include <memory>
#include <string_view>

#include "net/base/net_export.h"
#include "net/gm/gm_backend.h"
#include "third_party/boringssl/src/include/openssl/x509.h"

namespace net::gm {

// Validates explicit enterprise trust with Chromium's libpki. It does not
// consult ambient OpenSSL configuration or retrieve certificates/CRLs online.
// Each TLCP leaf is validated independently with its own key usage and name.
class NET_EXPORT GmCertificateVerifier {
 public:
  static std::unique_ptr<GmCertificateVerifier> Create(
      const GmContextConfig& config);
  ~GmCertificateVerifier();

  GmCertificateVerifier(const GmCertificateVerifier&) = delete;
  GmCertificateVerifier& operator=(const GmCertificateVerifier&) = delete;

  // On success, replaces *verified with a newly allocated, owned X509 chain.
  // On failure, leaves *verified unchanged. The caller frees either chain
  // with sk_X509_pop_free(chain, X509_free). Error outputs are always set.
  bool Verify(X509* leaf,
              STACK_OF(X509) * untrusted,
              std::string_view hostname,
              uint32_t hostname_mode,
              uint32_t role,
              STACK_OF(X509) * *verified,
              long* verify_result,
              uint32_t* certificate_errors) const;

 private:
  struct Impl;
  explicit GmCertificateVerifier(std::unique_ptr<Impl> impl);
  const std::unique_ptr<Impl> impl_;
};

}  // namespace net::gm

#endif  // NET_GM_GM_CERTIFICATE_VERIFIER_H_
