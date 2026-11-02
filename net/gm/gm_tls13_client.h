// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_TLS13_CLIENT_H_
#define NET_GM_GM_TLS13_CLIENT_H_

#include <memory>
#include <optional>
#include <string_view>

#include "base/callback_list.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_export.h"
#include "net/base/privacy_mode.h"
#include "net/cert/cert_verifier.h"
#include "net/gm/gm_client_identity.h"
#include "third_party/boringssl/src/include/openssl/base.h"

namespace net {
class CertVerifyResult;
class SSLPrivateKey;
class X509Certificate;
namespace gm {
struct TlsRule;
class GmSSLPrivateKey;
class ClientContext;

// Per-connection bridge used only by the opt-in TLS 1.3 joint negotiation
// path. Ordinary TLS verification and client authentication remain in
// SSLClientSocketImpl; this object handles an actually negotiated RFC 8998
// suite. All operations belong to the socket's sequence.
class NET_EXPORT_PRIVATE Tls13Client {
 public:
  Tls13Client(const HostPortPair& server,
              std::shared_ptr<const TlsRule> rule,
              ClientContext* context,
              ClientIdentityProvider* provider,
              PrivacyMode privacy_mode,
              base::RepeatingCallback<base::UnguessableToken()> resolver,
              base::RepeatingClosure resume,
              base::RepeatingClosure invalidated);
  ~Tls13Client();

  static bool IsGmCipher(const SSL* ssl);
  // Called only after an RFC 8998 suite is selected. Parsing and verification
  // run on a worker using owned certificate and trust snapshots. The request
  // has CertVerifier's cancellation contract; Cancel() also discards replies.
  int Verify(scoped_refptr<X509Certificate> certificate,
             std::string_view hostname,
             CertVerifyResult* result,
             CompletionOnceCallback callback,
             std::unique_ptr<CertVerifier::Request>* request);

  // Does not prompt. The returned identity is only a live prior authorization.
  void LoadCachedIdentity();
  const ClientIdentity* identity() const { return identity_.get(); }
  bool identity_pending() const { return identity_pending_; }
  bool identity_selected() const { return identity_selected_; }
  int identity_error() const { return identity_error_; }

  // Return the cert-callback convention: 1 ready, -1 pending, 0 failed.
  int SelectIdentity(SSL* ssl);
  bool GetCredential(scoped_refptr<X509Certificate>* certificate,
                     scoped_refptr<SSLPrivateKey>* key);
  void ClearIdentity();
  void Cancel();

 private:
  void OnVerifyComplete(CertVerifyResult* caller_result,
                        CompletionOnceCallback callback,
                        int error,
                        const CertVerifyResult& result);
  void SetIdentity(scoped_refptr<ClientIdentity> identity);
  void OnIdentity(int error, scoped_refptr<ClientIdentity> identity);
  void OnInvalidated();

  const HostPortPair server_;
  const std::shared_ptr<const TlsRule> rule_;
  const raw_ptr<ClientContext> context_;
  const raw_ptr<ClientIdentityProvider> provider_;
  const PrivacyMode privacy_mode_;
  base::RepeatingCallback<base::UnguessableToken()> resolver_;
  base::RepeatingClosure resume_;
  base::RepeatingClosure invalidated_;
  scoped_refptr<ClientIdentity> identity_;
  scoped_refptr<GmSSLPrivateKey> private_key_;
  std::optional<ClientCertificateRequest> request_;
  base::CallbackListSubscription invalidation_subscription_;
  base::ScopedClosureRunner identity_cancel_;
  bool identity_pending_ = false;
  bool identity_selected_ = false;
  bool server_verified_ = false;
  int identity_error_ = 0;
  base::WeakPtrFactory<Tls13Client> weak_factory_{this};
};

}  // namespace gm
}  // namespace net

#endif  // NET_GM_GM_TLS13_CLIENT_H_
