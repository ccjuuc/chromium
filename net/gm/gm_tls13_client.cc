// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_tls13_client.h"

#include <utility>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/task/thread_pool.h"
#include "crypto/sha2.h"
#include "net/base/net_errors.h"
#include "net/cert/asn1_util.h"
#include "net/cert/cert_verify_result.h"
#include "net/cert/x509_certificate.h"
#include "net/gm/gm_certificate_verifier.h"
#include "net/gm/gm_client_cert_constraints.h"
#include "net/gm/gm_client_context.h"
#include "net/gm/gm_ssl_private_key.h"
#include "net/gm/gm_tls_policy.h"
#include "third_party/boringssl/src/include/openssl/pool.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "third_party/boringssl/src/include/openssl/x509.h"

namespace net::gm {
namespace {

CertStatus Status(uint32_t errors) {
  CertStatus status = 0;
  for (auto [gm_error, net_error] :
       {std::pair{GM_CERT_ERROR_AUTHORITY, CERT_STATUS_AUTHORITY_INVALID},
        {GM_CERT_ERROR_DATE, CERT_STATUS_DATE_INVALID},
        {GM_CERT_ERROR_NAME, CERT_STATUS_COMMON_NAME_INVALID},
        {GM_CERT_ERROR_REVOKED, CERT_STATUS_REVOKED},
        {GM_CERT_ERROR_REVOCATION_UNAVAILABLE,
         CERT_STATUS_UNABLE_TO_CHECK_REVOCATION},
        {GM_CERT_ERROR_INVALID, CERT_STATUS_INVALID},
        {GM_CERT_ERROR_WEAK_SIGNATURE, CERT_STATUS_WEAK_SIGNATURE_ALGORITHM}}) {
    if (errors & gm_error) {
      status |= net_error;
    }
  }
  return status;
}

// Everything passed to this function is independently owned by the worker.
// Neither SSL nor the socket's result object may be used off its sequence.
int VerifyCertificate(const GmCertificateVerifier* verifier,
                      const X509Certificate& peer,
                      std::string_view hostname,
                      CertVerifyResult* result) {
  const auto& buffers = peer.cert_buffers();
  bssl::UniquePtr<STACK_OF(X509)> chain(sk_X509_new_null());
  if (!verifier || !chain || buffers.empty() || buffers.size() > 32) {
    return ERR_CERT_INVALID;
  }
  size_t total = 0;
  for (const auto& buffer : buffers) {
    const size_t size = CRYPTO_BUFFER_len(buffer.get());
    if (size > 256 * 1024 || size > 1024 * 1024 - total) {
      return ERR_CERT_INVALID;
    }
    total += size;
    bssl::UniquePtr<X509> certificate(X509_parse_from_buffer(buffer.get()));
    if (!certificate || !sk_X509_push(chain.get(), certificate.get())) {
      return ERR_CERT_INVALID;
    }
    certificate.release();
  }
  STACK_OF(X509)* verified = nullptr;
  long verify_result = X509_V_ERR_APPLICATION_VERIFICATION;
  uint32_t errors = GM_CERT_ERROR_INVALID;
  const bool ok =
      verifier->Verify(sk_X509_value(chain.get(), 0), chain.get(), hostname,
                       GM_HOSTNAME_BROWSER_SAN, GM_CERTIFICATE_SIGNING,
                       &verified, &verify_result, &errors);
  bssl::UniquePtr<STACK_OF(X509)> verified_chain(verified);
  result->cert_status = Status(errors);
  if (!ok) {
    if (!IsCertStatusError(result->cert_status)) {
      result->cert_status |= CERT_STATUS_INVALID;
    }
    return MapCertStatusToNetError(result->cert_status);
  }
  std::vector<std::string> der_chain;
  for (size_t i = 0; i < sk_X509_num(verified); ++i) {
    X509* certificate = sk_X509_value(verified, i);
    const int size = i2d_X509(certificate, nullptr);
    if (size <= 0) {
      result->cert_status = CERT_STATUS_INVALID;
      return ERR_CERT_INVALID;
    }
    auto& der = der_chain.emplace_back(size, '\0');
    uint8_t* cursor = reinterpret_cast<uint8_t*>(der.data());
    if (i2d_X509(certificate, &cursor) != size) {
      result->cert_status = CERT_STATUS_INVALID;
      return ERR_CERT_INVALID;
    }
  }
  std::vector<std::string_view> der_views(der_chain.begin(), der_chain.end());
  result->verified_cert = X509Certificate::CreateFromDERCertChain(der_views);
  result->is_issued_by_known_root = false;
  result->public_key_hashes.clear();
  for (const auto& der : der_chain) {
    std::string_view spki;
    if (!asn1::ExtractSPKIFromDERCert(der, &spki)) {
      result->cert_status = CERT_STATUS_INVALID;
      return ERR_CERT_INVALID;
    }
    result->public_key_hashes.push_back(
        crypto::SHA256Hash(base::as_byte_span(spki)));
  }
  if (!result->verified_cert) {
    result->cert_status = CERT_STATUS_INVALID;
    return ERR_CERT_INVALID;
  }
  return OK;
}

struct VerificationResult {
  int error = ERR_CERT_INVALID;
  CertVerifyResult result;
};

std::unique_ptr<VerificationResult> VerifyOnWorker(
    std::shared_ptr<CachedCertificateVerifier> cache,
    scoped_refptr<X509Certificate> certificate,
    std::string hostname) {
  auto completed = std::make_unique<VerificationResult>();
  completed->result.verified_cert = certificate;
  completed->result.cert_status = CERT_STATUS_INVALID;
  completed->error = VerifyCertificate(cache->GetForWorker(), *certificate,
                                       hostname, &completed->result);
  return completed;
}

// Matches CertVerifier::Request: destroying the handle discards completion.
// An already running worker may finish, but only the reply on the originating
// sequence can access the caller. The worker never receives this object.
class VerificationRequest final : public CertVerifier::Request {
 public:
  using Callback = base::OnceCallback<void(int, const CertVerifyResult&)>;
  explicit VerificationRequest(Callback callback)
      : callback_(std::move(callback)) {}
  ~VerificationRequest() override = default;

  bool Start(std::shared_ptr<CachedCertificateVerifier> cache,
             scoped_refptr<X509Certificate> certificate,
             std::string hostname) {
    return base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&VerifyOnWorker, std::move(cache),
                       std::move(certificate), std::move(hostname)),
        base::BindOnce(&VerificationRequest::OnComplete,
                       weak_factory_.GetWeakPtr()));
  }

 private:
  void OnComplete(std::unique_ptr<VerificationResult> completed) {
    // Completion may synchronously delete this request.
    auto callback = std::move(callback_);
    std::move(callback).Run(completed->error, completed->result);
  }

  Callback callback_;
  base::WeakPtrFactory<VerificationRequest> weak_factory_{this};
};

}  // namespace

Tls13Client::Tls13Client(
    const HostPortPair& server,
    std::shared_ptr<const TlsRule> rule,
    ClientContext* context,
    ClientIdentityProvider* provider,
    PrivacyMode privacy_mode,
    base::RepeatingCallback<base::UnguessableToken()> resolver,
    base::RepeatingClosure resume,
    base::RepeatingClosure invalidated)
    : server_(server),
      rule_(std::move(rule)),
      context_(context),
      provider_(provider),
      privacy_mode_(privacy_mode),
      resolver_(std::move(resolver)),
      resume_(std::move(resume)),
      invalidated_(std::move(invalidated)) {}

Tls13Client::~Tls13Client() {
  Cancel();
}

bool Tls13Client::IsGmCipher(const SSL* ssl) {
  const SSL_CIPHER* cipher = SSL_get_current_cipher(ssl);
  if (!cipher) {
    return false;
  }
  const uint16_t id = SSL_CIPHER_get_protocol_id(cipher);
  return id == 0x00c6 || id == 0x00c7;
}

int Tls13Client::Verify(scoped_refptr<X509Certificate> certificate,
                        std::string_view hostname,
                        CertVerifyResult* result,
                        CompletionOnceCallback callback,
                        std::unique_ptr<CertVerifier::Request>* request) {
  request->reset();
  server_verified_ = false;
  result->cert_status = CERT_STATUS_INVALID;
  if (!certificate) {
    return ERR_CERT_INVALID;
  }
  auto cache = context_->GetCertificateVerifier(rule_);
  if (!cache) {
    return ERR_CERT_INVALID;
  }
  auto pending = std::make_unique<VerificationRequest>(
      base::BindOnce(&Tls13Client::OnVerifyComplete, weak_factory_.GetWeakPtr(),
                     base::Unretained(result), std::move(callback)));
  if (!pending->Start(std::move(cache), std::move(certificate),
                      std::string(hostname))) {
    return ERR_ABORTED;
  }
  *request = std::move(pending);
  return ERR_IO_PENDING;
}

void Tls13Client::OnVerifyComplete(CertVerifyResult* caller_result,
                                   CompletionOnceCallback callback,
                                   int error,
                                   const CertVerifyResult& result) {
  *caller_result = result;
  server_verified_ = error == OK;
  // The socket may destroy this helper when it resumes the handshake.
  std::move(callback).Run(error);
}

void Tls13Client::LoadCachedIdentity() {
  if (provider_ && privacy_mode_ != PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS &&
      rule_->enable_session_resumption) {
    auto identity = provider_->GetCachedIdentity(server_, rule_->profile);
    if (identity && identity->is_valid()) {
      SetIdentity(std::move(identity));
    }
  }
}

void Tls13Client::SetIdentity(scoped_refptr<ClientIdentity> identity) {
  invalidation_subscription_ = {};
  identity_ = std::move(identity);
  if (identity_) {
    invalidation_subscription_ =
        identity_->AddInvalidationCallback(base::BindRepeating(
            &Tls13Client::OnInvalidated, weak_factory_.GetWeakPtr()));
  }
}

int Tls13Client::SelectIdentity(SSL* ssl) {
  if (!server_verified_ || !IsGmCipher(ssl)) {
    identity_error_ = ERR_BAD_SSL_CLIENT_AUTH_CERT;
    return 0;
  }
  if (identity_error_ != OK) {
    return 0;
  }
  if (identity_selected_) {
    return 1;
  }
  if (identity_pending_) {
    return -1;
  }
  ClientCertificateRequest request;
  request.request_id = 1;  // TLS 1.3 does not renegotiate client identities.
  request.protocol = TlsProtocol::kRfc8998;
  request.required_roles = kClientCertificateSigning;
  request.gm_client_auth_context_resolver = resolver_;
  const uint16_t* algorithms = nullptr;
  const size_t count = SSL_get0_peer_verify_algorithms(ssl, &algorithms);
  if (count > 128) {
    identity_error_ = ERR_BAD_SSL_CLIENT_AUTH_CERT;
    return 0;
  }
  // The pointer/count are borrowed from the current SSL CertificateRequest.
  auto algorithms_span = UNSAFE_BUFFERS(base::span(algorithms, count));
  request.signature_algorithms.assign(algorithms_span.begin(),
                                      algorithms_span.end());
  const auto* names = SSL_get0_server_requested_CAs(ssl);
  if (sk_CRYPTO_BUFFER_num(names) > 128) {
    identity_error_ = ERR_BAD_SSL_CLIENT_AUTH_CERT;
    return 0;
  }
  size_t total = 0;
  for (size_t i = 0; i < sk_CRYPTO_BUFFER_num(names); ++i) {
    const auto* name = sk_CRYPTO_BUFFER_value(names, i);
    const size_t size = CRYPTO_BUFFER_len(name);
    if (size == 0 || size > 16384 || size > 65535 - total) {
      identity_error_ = ERR_BAD_SSL_CLIENT_AUTH_CERT;
      return 0;
    }
    total += size;
    auto bytes = bssl::Span(CRYPTO_BUFFER_data(name), size);
    request.certificate_authorities.emplace_back(bytes.begin(), bytes.end());
  }
  // The provider checks the current request constraints even for a cached
  // authorization; do not use a cached certificate solely because it exists.
  if (!provider_ ||
      privacy_mode_ == PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS) {
    SetIdentity(nullptr);
    identity_selected_ = true;
    identity_error_ = rule_->profile.client_identity_policy_id().empty()
                          ? OK
                          : ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
    return identity_error_ == OK ? 1 : 0;
  }
  identity_pending_ = true;
  request_ = request;
  identity_cancel_.ReplaceClosure(provider_->AcquireIdentity(
      server_, rule_->profile, request,
      base::BindOnce(&Tls13Client::OnIdentity, weak_factory_.GetWeakPtr())));
  return -1;
}

void Tls13Client::OnIdentity(int error,
                             scoped_refptr<ClientIdentity> identity) {
  identity_cancel_.ReplaceClosure({});
  identity_pending_ = false;
  if (error == OK &&
      ((identity && !identity->is_valid()) ||
       (identity && (!request_ || !MatchesClientCertificateRequest(
                                      identity->signing_certificate(),
                                      identity->encryption_certificate(),
                                      identity->intermediates(), *request_))) ||
       (!identity && !rule_->profile.client_identity_policy_id().empty()))) {
    error = ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
  }
  identity_error_ = error;
  identity_selected_ = true;
  SetIdentity(error == OK ? std::move(identity) : nullptr);
  resume_.Run();
}

bool Tls13Client::GetCredential(scoped_refptr<X509Certificate>* certificate,
                                scoped_refptr<SSLPrivateKey>* key) {
  *certificate = nullptr;
  *key = nullptr;
  if (!identity_) {
    return true;
  }
  if (!identity_->is_valid()) {
    return false;
  }
  std::vector<std::string_view> chain = {
      base::as_string_view(base::span(identity_->signing_certificate()))};
  for (const auto& issuer : identity_->intermediates()) {
    chain.push_back(base::as_string_view(base::span(issuer)));
  }
  *certificate = X509Certificate::CreateFromDERCertChain(chain);
  if (!private_key_) {
    private_key_ = GmSSLPrivateKey::Create(identity_);
  }
  *key = private_key_;
  return *certificate && *key;
}

void Tls13Client::ClearIdentity() {
  if (private_key_) {
    private_key_->Cancel();
    private_key_ = nullptr;
  }
  SetIdentity(nullptr);
}

void Tls13Client::Cancel() {
  weak_factory_.InvalidateWeakPtrs();
  server_verified_ = false;
  identity_cancel_.RunAndReset();
  identity_pending_ = false;
  if (private_key_) {
    private_key_->Cancel();
    private_key_ = nullptr;
  }
  invalidation_subscription_ = {};
  identity_ = nullptr;
}

void Tls13Client::OnInvalidated() {
  identity_error_ = ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
  // The socket owns us and may destroy us while handling this notification.
  auto callback = invalidated_;
  callback.Run();
}

}  // namespace net::gm
