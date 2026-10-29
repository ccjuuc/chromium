// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_SSL_PRIVATE_KEY_H_
#define NET_GM_GM_SSL_PRIVATE_KEY_H_

#include <array>
#include <map>
#include <vector>

#include "base/callback_list.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/task/single_thread_task_runner.h"
#include "net/base/net_export.h"
#include "net/gm/gm_client_identity.h"
#include "net/ssl/ssl_private_key.h"
#include "third_party/boringssl/src/include/openssl/evp.h"

namespace net::gm {

// Adapts an already authorized SM2 identity to Chromium's TLS 1.3 private-key
// interface. The socket must only install this credential after negotiating an
// RFC 8998 suite; TLCP uses a different transcript and identity convention.
// This class never enumerates devices or requests a PIN. All calls, including
// destruction, must occur on the identity's network thread.
class NET_EXPORT GmSSLPrivateKey final : public net::SSLPrivateKey {
 public:
  // Checks the signing certificate's structure, validity, client-auth usage
  // and SM2 public key. Server CertificateRequest constraints and browser
  // authorization must already have been checked by the identity provider.
  static scoped_refptr<GmSSLPrivateKey> Create(
      scoped_refptr<ClientIdentity> identity);

  std::string GetProviderName() override;
  std::vector<uint16_t> GetAlgorithmPreferences() override;
  void Sign(uint16_t algorithm,
            base::span<const uint8_t> input,
            SignCallback callback) override;

  // Cancels device requests and asynchronously completes pending callbacks
  // with ERR_ABORTED. This adapter cannot be used again. Destruction also
  // cancels requests, but discards callbacks, like ThreadedSSLPrivateKey.
  void Cancel();

 private:
  struct PendingSignature {
    std::array<uint8_t, 32> digest;
    SignCallback callback;
  };

  GmSSLPrivateKey(scoped_refptr<ClientIdentity> identity,
                  bssl::UniquePtr<EVP_PKEY> public_key);
  ~GmSSLPrivateKey() override;

  void OnSignature(uint64_t request_id,
                   int error,
                   std::vector<uint8_t> signature);
  void OnInvalidated();
  void CancelRequests(Error error, bool notify);
  void Reply(SignCallback callback,
             Error error,
             std::vector<uint8_t> signature = {});

  scoped_refptr<ClientIdentity> identity_;
  bssl::UniquePtr<EVP_PKEY> public_key_;
  const scoped_refptr<base::SingleThreadTaskRunner> task_runner_;
  std::map<uint64_t, PendingSignature> pending_;
  base::CallbackListSubscription invalidation_subscription_;
  bool cancelled_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<GmSSLPrivateKey> weak_factory_{this};
};

}  // namespace net::gm

#endif  // NET_GM_GM_SSL_PRIVATE_KEY_H_
