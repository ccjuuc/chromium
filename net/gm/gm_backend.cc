// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Uses Chromium's BoringSSL instance. The socket facade contains no private
// keys.
#include "net/gm/gm_backend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "net/gm/gm_backend_loader.h"
#include "net/gm/gm_certificate_verifier.h"
#include "third_party/boringssl/src/include/openssl/bn.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/ecdsa.h"
#include "third_party/boringssl/src/include/openssl/err.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/boringssl/src/include/openssl/pool.h"
#include "third_party/boringssl/src/include/openssl/sm2.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"
#include "third_party/boringssl/src/include/openssl/x509v3.h"

static_assert(sizeof(void*) == 8, "The national TLS backend is 64-bit only");

struct GmContext {
  std::thread::id thread = std::this_thread::get_id();
  std::unique_ptr<net::gm::GmCertificateVerifier> verifier;
  SSL_CTX* ssl = nullptr;
  uint32_t protocol = 0;
  size_t connections = 0;
  unsigned char trust_binding[32] = {};
};

struct GmConnection {
  GmContext* context = nullptr;
  SSL* ssl = nullptr;
  BIO* input = nullptr;
  BIO* output = nullptr;
  GmStatus terminal = GM_OK;
  bool established = false;
  bool cancelled = false;
  bool pending = false;
  bool completed = false;
  GmStatus key_result = GM_OK;
  GmKeyRequest request = {};
  unsigned char signature[72] = {};
  size_t signature_size = 0;
  bool transport_eof = false;
  bool identity_authenticated = false;
  bool identity_installed = false;
  bool defer_identity = false;
  bool require_client_auth = false;
  bool identity_pending = false;
  bool identity_completed = false;
  bool authentication_started = false;
  // Chain validation and TLS proof of possession are separate milestones.
  // Imported chains are freshly validated for this connection before offering
  // the session, but cannot authenticate a full handshake if it is rejected.
  enum class PeerChainSource { kUnverified, kImportedSession, kHandshake };
  PeerChainSource verified_peer_chains = PeerChainSource::kUnverified;
  // Only exposes peer metadata once the handshake signature was also checked.
  bool peer_verified = false;
  bool protocol_retry_allowed = false;
  GmIdentityRequest identity_request = {};
  std::vector<std::vector<unsigned char>> ca_names;
  unsigned char server_name[254] = {};
  size_t server_name_size = 0;
  unsigned char alpn[32] = {};
  size_t alpn_size = 0;
  uint32_t hostname_mode = 0;
  long verify_result = X509_V_OK;
  uint32_t certificate_errors = 0;
  EVP_PKEY* signing_public_key = nullptr;
  uint64_t signing_key_id = 0;
  SSL_SESSION* latest_session = nullptr;
  bool resumption_enabled = false;
  uint64_t session_generation = 0;
  unsigned char session_binding[32] = {};
  STACK_OF(X509) * signing_chain = nullptr;
  STACK_OF(X509) * encryption_chain = nullptr;
  STACK_OF(X509) * diagnostic_signing_chain = nullptr;
  STACK_OF(X509) * diagnostic_encryption_chain = nullptr;
};

namespace {

constexpr size_t kBufferLimit = 1024 * 1024;
constexpr size_t kCertificateLimit = 256 * 1024;
constexpr size_t kSessionLimit = 2 * 1024 * 1024;
std::atomic<uint64_t> g_next_request{1};

// The C ABI and BoringSSL callbacks provide pointer/extent pairs. Entry points
// validate nullable pointers and impose limits before constructing these spans.
// As with all C buffers, their allocation extent is the caller's contract.
// Keep this adaptation at the boundary; indexing and copies below use spans.
template <typename T>
base::span<T> AbiSpan(T* data, size_t size) {
  return UNSAFE_BUFFERS(base::span<T>(data, size));
}

bool OnThread(const GmContext* context) {
  return context && context->thread == std::this_thread::get_id();
}

GmStatus Check(const GmConnection* connection) {
  if (!connection) {
    return GM_ERROR_ARGUMENT;
  }
  if (!OnThread(connection->context)) {
    return GM_ERROR_THREAD;
  }
  return connection->terminal;
}

bool ValidBytes(GmBytes bytes, size_t maximum = kCertificateLimit) {
  return bytes.data && bytes.size && bytes.size <= maximum;
}

bool HashU64(EVP_MD_CTX* hash, uint64_t value) {
  std::array<unsigned char, 8> encoded;
  for (size_t i = 0; i < encoded.size(); ++i) {
    encoded[encoded.size() - 1 - i] = static_cast<unsigned char>(value);
    value >>= 8;
  }
  return EVP_DigestUpdate(hash, encoded.data(), encoded.size()) == 1;
}

bool HashBytes(EVP_MD_CTX* hash, GmBytes bytes) {
  return HashU64(hash, bytes.size) &&
         (!bytes.size ||
          (bytes.data && EVP_DigestUpdate(hash, bytes.data, bytes.size) == 1));
}

bool HashTrust(GmContext* context, const GmContextConfig* config) {
  EVP_MD_CTX* hash = EVP_MD_CTX_new();
  unsigned int size = 0;
  bool ok = hash && EVP_DigestInit_ex(hash, EVP_sha256(), nullptr) &&
            HashU64(hash, config->protocol) &&
            HashU64(hash, config->revocation_mode) &&
            HashU64(hash, config->trust_anchor_count);
  for (size_t i = 0; ok && i < config->trust_anchor_count; ++i) {
    ok = HashBytes(
        hash, AbiSpan(config->trust_anchors, config->trust_anchor_count)[i]);
  }
  ok = ok && HashU64(hash, config->crl_count);
  for (size_t i = 0; ok && i < config->crl_count; ++i) {
    ok = HashBytes(hash, AbiSpan(config->crls, config->crl_count)[i]);
  }
  ok = ok && HashU64(hash, config->untrusted_intermediate_count);
  for (size_t i = 0; ok && i < config->untrusted_intermediate_count; ++i) {
    ok = HashBytes(hash, AbiSpan(config->untrusted_intermediates,
                                 config->untrusted_intermediate_count)[i]);
  }
  ok = ok && HashU64(hash, config->disallowed_certificate_count);
  for (size_t i = 0; ok && i < config->disallowed_certificate_count; ++i) {
    ok = HashBytes(hash, AbiSpan(config->disallowed_certificates,
                                 config->disallowed_certificate_count)[i]);
  }
  ok = ok && EVP_DigestFinal_ex(hash, context->trust_binding, &size) &&
       size == 32;
  EVP_MD_CTX_free(hash);
  return ok;
}

bool HashSessionBinding(GmConnection* connection,
                        const GmConnectionConfig* config,
                        GmBytes alpn) {
  EVP_MD_CTX* hash = EVP_MD_CTX_new();
  unsigned int size = 0;
  bool ok = hash && EVP_DigestInit_ex(hash, EVP_sha256(), nullptr) &&
            EVP_DigestUpdate(hash, connection->context->trust_binding, 32) &&
            HashU64(hash, config->hostname_mode) &&
            HashU64(hash, config->require_client_auth) &&
            HashBytes(hash, config->server_name) && HashBytes(hash, alpn) &&
            HashBytes(hash, config->session_binding) &&
            HashU64(hash, config->identity ? 1 : 0);
  if (config->identity) {
    const GmIdentity& identity = *config->identity;
    ok = ok && HashBytes(hash, identity.signing_certificate) &&
         HashBytes(hash, identity.encryption_certificate) &&
         HashU64(hash, identity.signing_key_id) &&
         HashU64(hash, identity.encryption_key_id) &&
         HashU64(hash, identity.intermediate_count);
    for (size_t i = 0; ok && i < identity.intermediate_count; ++i) {
      ok = HashBytes(hash, AbiSpan(identity.intermediates,
                                   identity.intermediate_count)[i]);
    }
  }
  ok = ok && EVP_DigestFinal_ex(hash, connection->session_binding, &size) &&
       size == 32;
  EVP_MD_CTX_free(hash);
  return ok;
}

bool ValidateAlpn(GmBytes protocols, uint32_t protocol) {
  if (!ValidBytes(protocols, 32)) {
    return false;
  }
  auto bytes = AbiSpan(protocols.data, protocols.size);
  bool h1 = false;
  bool h2 = false;
  size_t offset = 0;
  while (offset < protocols.size) {
    const size_t length = bytes[offset++];
    if (!length || length > protocols.size - offset) {
      return false;
    }
    if (std::ranges::equal(bytes.subspan(offset, length),
                           std::string_view("http/1.1"))) {
      if (h1) {
        return false;
      }
      h1 = true;
    } else if (std::ranges::equal(bytes.subspan(offset, length),
                                  std::string_view("h2"))) {
      if (h2 || protocol == GM_PROTOCOL_TLCP) {
        return false;
      }
      h2 = true;
    } else {
      return false;
    }
    offset += length;
  }
  return h1 || h2;
}

X509* ParseCertificate(GmContext*, GmBytes bytes) {
  if (!ValidBytes(bytes)) {
    return nullptr;
  }
  bssl::UniquePtr<CRYPTO_BUFFER> buffer(
      CRYPTO_BUFFER_new(bytes.data, bytes.size, nullptr));
  return buffer ? X509_parse_from_buffer(buffer.get()) : nullptr;
}

// The key service receives SM3(ZA || message), ready for SKF_ECCSignData.
// BoringSSL retains the handshake state while the device operation is pending;
// no fiber, provider-owned stack, or private key material crosses this
// boundary.
ssl_private_key_result_t Sign(SSL* ssl,
                              uint8_t*,
                              size_t*,
                              size_t,
                              uint16_t algorithm,
                              const uint8_t* message,
                              size_t message_size) {
  auto* connection = static_cast<GmConnection*>(SSL_get_app_data(ssl));
  if (!connection || !OnThread(connection->context) || connection->cancelled ||
      connection->terminal != GM_OK || connection->pending ||
      !connection->signing_public_key || !connection->signing_key_id ||
      algorithm != SSL_SIGN_SM2_SM3) {
    return ssl_private_key_failure;
  }
  const EC_KEY* key = EVP_PKEY_get0_EC_KEY(connection->signing_public_key);
  static constexpr char kRFC8998Identity[] = "TLSv1.3+GM+Cipher+Suite";
  const bool tlcp = connection->context->protocol == GM_PROTOCOL_TLCP;
  const char* id = tlcp ? SM2_DEFAULT_USERID : kRFC8998Identity;
  const size_t id_size =
      tlcp ? SM2_DEFAULT_USERID_LENGTH : sizeof(kRFC8998Identity) - 1;
  // TLCP's callback message is already SM3(transcript), as specified by the
  // protocol. RFC 8998 passes the raw TLS 1.3 CertificateVerify message.
  if (!key || (tlcp && message_size != 32)) {
    return ssl_private_key_failure;
  }
  connection->request = {};
  connection->request.struct_size = sizeof(GmKeyRequest);
  connection->request.request_id = g_next_request.fetch_add(1);
  connection->request.key_id = connection->signing_key_id;
  if (!SM2_compute_digest(connection->request.digest, key,
                          reinterpret_cast<const uint8_t*>(id), id_size,
                          message, message_size)) {
    return ssl_private_key_failure;
  }
  connection->pending = true;
  connection->completed = false;
  connection->key_result = GM_OK;
  return ssl_private_key_retry;
}

ssl_private_key_result_t Decrypt(SSL*,
                                 uint8_t*,
                                 size_t*,
                                 size_t,
                                 const uint8_t*,
                                 size_t) {
  // Static ECC TLCP uses the server encryption certificate. Client-side
  // encryption-key operations are not required or silently emulated.
  return ssl_private_key_failure;
}

ssl_private_key_result_t FinishSign(SSL* ssl,
                                    uint8_t* out,
                                    size_t* out_len,
                                    size_t max_out) {
  auto* connection = static_cast<GmConnection*>(SSL_get_app_data(ssl));
  if (!connection || !OnThread(connection->context) || !connection->pending ||
      connection->cancelled || connection->terminal != GM_OK) {
    return ssl_private_key_failure;
  }
  if (!connection->completed) {
    return ssl_private_key_retry;
  }
  connection->pending = false;
  bool valid =
      connection->key_result == GM_OK &&
      connection->signature_size <= max_out &&
      SM2_verify_digest(connection->request.digest, connection->signature,
                        connection->signature_size,
                        EVP_PKEY_get0_EC_KEY(connection->signing_public_key));
  if (valid) {
    AbiSpan(out, max_out)
        .first(connection->signature_size)
        .copy_from(base::span(connection->signature)
                       .first(connection->signature_size));
    *out_len = connection->signature_size;
    connection->identity_authenticated = true;
  } else {
    connection->terminal = connection->key_result < 0 ? connection->key_result
                                                      : GM_ERROR_CLIENT_AUTH;
  }
  OPENSSL_cleanse(connection->signature, sizeof(connection->signature));
  OPENSSL_cleanse(connection->request.digest,
                  sizeof(connection->request.digest));
  return valid ? ssl_private_key_success : ssl_private_key_failure;
}

const SSL_PRIVATE_KEY_METHOD kPrivateKeyMethod = {Sign, Decrypt, FinishSign};

bool IsSM2(EVP_PKEY* key) {
  const EC_KEY* ec = key ? EVP_PKEY_get0_EC_KEY(key) : nullptr;
  return ec && EC_GROUP_get_curve_name(EC_KEY_get0_group(ec)) == NID_sm2;
}

bool InstallIdentity(GmConnection* connection, const GmIdentity* identity) {
  if (!identity) {
    return true;
  }
  if (identity->struct_size != sizeof(*identity) || !identity->signing_key_id ||
      identity->intermediate_count > 16 ||
      (identity->intermediate_count && !identity->intermediates)) {
    return false;
  }
  bssl::UniquePtr<X509> sign(
      ParseCertificate(connection->context, identity->signing_certificate));
  bssl::UniquePtr<EVP_PKEY> key(sign ? X509_get_pubkey(sign.get()) : nullptr);
  if (!IsSM2(key.get())) {
    return false;
  }
  std::vector<bssl::UniquePtr<CRYPTO_BUFFER>> certificates;
  std::vector<CRYPTO_BUFFER*> chain;
  auto add = [&](GmBytes bytes) {
    bssl::UniquePtr<X509> parsed(ParseCertificate(connection->context, bytes));
    if (!parsed) {
      return false;
    }
    bssl::UniquePtr<CRYPTO_BUFFER> buffer(
        CRYPTO_BUFFER_new(bytes.data, bytes.size, nullptr));
    if (!buffer) {
      return false;
    }
    chain.push_back(buffer.get());
    certificates.push_back(std::move(buffer));
    return true;
  };
  if (!add(identity->signing_certificate)) {
    return false;
  }
  for (size_t i = 0; i < identity->intermediate_count; ++i) {
    if (!add(AbiSpan(identity->intermediates,
                     identity->intermediate_count)[i])) {
      return false;
    }
  }
  bssl::UniquePtr<CRYPTO_BUFFER> encryption;
  if (connection->context->protocol == GM_PROTOCOL_TLCP) {
    bssl::UniquePtr<X509> enc(ParseCertificate(
        connection->context, identity->encryption_certificate));
    bssl::UniquePtr<EVP_PKEY> enc_key(enc ? X509_get_pubkey(enc.get())
                                          : nullptr);
    if (!identity->encryption_key_id || !IsSM2(enc_key.get())) {
      return false;
    }
    encryption.reset(CRYPTO_BUFFER_new(identity->encryption_certificate.data,
                                       identity->encryption_certificate.size,
                                       nullptr));
    if (!encryption) {
      return false;
    }
  }
  const uint16_t algorithm = SSL_SIGN_SM2_SM3;
  if (!SSL_set_signing_algorithm_prefs(connection->ssl, &algorithm, 1) ||
      !SSL_set_chain_and_key(connection->ssl, chain.data(), chain.size(),
                             nullptr, &kPrivateKeyMethod) ||
      (connection->context->protocol == GM_PROTOCOL_TLCP &&
       !SSL_set1_tlcp_encryption_certificate(connection->ssl,
                                             encryption.get()))) {
    return false;
  }
  EVP_PKEY_free(connection->signing_public_key);
  connection->signing_public_key = key.release();
  connection->signing_key_id = identity->signing_key_id;
  connection->identity_installed = true;
  return true;
}

bool VerifyRoleChain(GmConnection* connection,
                     X509* leaf,
                     STACK_OF(X509) * untrusted,
                     uint32_t role,
                     STACK_OF(X509) * *verified) {
  STACK_OF(X509)* result = nullptr;
  if (!connection->context->verifier->Verify(
          leaf, untrusted,
          std::string_view(
              reinterpret_cast<const char*>(connection->server_name),
              connection->server_name_size),
          connection->hostname_mode, role, &result, &connection->verify_result,
          &connection->certificate_errors)) {
    return false;
  }
  sk_X509_pop_free(*verified, X509_free);
  *verified = result;
  return true;
}

STACK_OF(X509) * ParsePeerRole(GmConnection* connection,
                               const STACK_OF(CRYPTO_BUFFER) * buffers,
                               bool encryption) {
  const bool tlcp = connection->context->protocol == GM_PROTOCOL_TLCP;
  const size_t count = buffers ? sk_CRYPTO_BUFFER_num(buffers) : 0;
  if (count < (encryption ? 2u : 1u) || count > 18) {
    return nullptr;
  }
  bssl::UniquePtr<STACK_OF(X509)> chain(sk_X509_new_null());
  if (!chain) {
    return nullptr;
  }
  for (size_t i = encryption ? 1u : 0u; i < count;
       i = i < (tlcp ? 2u : 1u) ? (tlcp ? 2u : 1u) : i + 1) {
    const CRYPTO_BUFFER* buffer = sk_CRYPTO_BUFFER_value(buffers, i);
    bssl::UniquePtr<X509> cert(ParseCertificate(
        connection->context,
        {CRYPTO_BUFFER_data(buffer), CRYPTO_BUFFER_len(buffer)}));
    if (!cert || !sk_X509_push(chain.get(), cert.get())) {
      return nullptr;
    }
    cert.release();
  }
  return chain.release();
}

void CaptureDiagnosticPeers(GmConnection* connection) {
  const auto* peers = SSL_get0_peer_certificates(connection->ssl);
  bssl::UniquePtr<STACK_OF(X509)> sign(ParsePeerRole(connection, peers, false));
  bssl::UniquePtr<STACK_OF(X509)> enc;
  if (connection->context->protocol == GM_PROTOCOL_TLCP) {
    enc.reset(ParsePeerRole(connection, peers, true));
  }
  sk_X509_pop_free(connection->diagnostic_signing_chain, X509_free);
  sk_X509_pop_free(connection->diagnostic_encryption_chain, X509_free);
  connection->diagnostic_signing_chain = sign.release();
  connection->diagnostic_encryption_chain = enc.release();
}

bool CaptureVerifiedPeers(GmConnection* connection) {
  if (connection->cancelled || connection->terminal != GM_OK) {
    return false;
  }
  const bool resumed = SSL_session_reused(connection->ssl);
  const auto source = resumed ? GmConnection::PeerChainSource::kImportedSession
                              : GmConnection::PeerChainSource::kHandshake;
  if (connection->verified_peer_chains == source) {
    return true;
  }
  if (!resumed) {
    CaptureDiagnosticPeers(connection);
  }
  STACK_OF(X509)* sign = resumed ? connection->signing_chain
                                 : connection->diagnostic_signing_chain;
  STACK_OF(X509)* enc = resumed ? connection->encryption_chain
                                : connection->diagnostic_encryption_chain;
  if (!sign || !sk_X509_num(sign) ||
      !VerifyRoleChain(connection, sk_X509_value(sign, 0), sign,
                       GM_CERTIFICATE_SIGNING, &connection->signing_chain)) {
    if (connection->verify_result == X509_V_OK) {
      connection->verify_result = X509_V_ERR_APPLICATION_VERIFICATION;
      connection->certificate_errors = GM_CERT_ERROR_INVALID;
    }
    return false;
  }
  const bool valid = connection->context->protocol != GM_PROTOCOL_TLCP ||
                     (enc && sk_X509_num(enc) &&
                      VerifyRoleChain(connection, sk_X509_value(enc, 0), enc,
                                      GM_CERTIFICATE_ENCRYPTION,
                                      &connection->encryption_chain));
  if (!valid && connection->verify_result == X509_V_OK) {
    connection->verify_result = X509_V_ERR_APPLICATION_VERIFICATION;
    connection->certificate_errors = GM_CERT_ERROR_INVALID;
  }
  if (valid) {
    connection->verified_peer_chains = source;
  }
  return valid;
}

ssl_verify_result_t VerifyPeer(SSL* ssl, uint8_t* alert) {
  auto* connection = static_cast<GmConnection*>(SSL_get_app_data(ssl));
  if (!connection || connection->cancelled ||
      !CaptureVerifiedPeers(connection)) {
    if (connection) {
      connection->terminal = GM_ERROR_CERTIFICATE;
    }
    *alert = SSL_AD_BAD_CERTIFICATE;
    return ssl_verify_invalid;
  }
  return ssl_verify_ok;
}

int SelectIdentityImpl(SSL* ssl, void* argument) {
  auto* connection = static_cast<GmConnection*>(argument);
  connection->authentication_started = true;
  if (connection->cancelled || connection->terminal != GM_OK) {
    return 0;
  }
  if (connection->identity_completed) {
    return 1;
  }
  if (connection->identity_pending) {
    return -1;
  }
  // BoringSSL invokes this after server chain and handshake-signature
  // validation. Reuse that chain result, then expose the authenticated peer so
  // Chromium checks PSL/name policy before authorizing a device identity.
  if (!CaptureVerifiedPeers(connection)) {
    connection->terminal = GM_ERROR_CERTIFICATE;
    return 0;
  }
  connection->peer_verified = true;
  auto& request = connection->identity_request;
  request = {};
  request.struct_size = sizeof(request);
  request.request_id = g_next_request.fetch_add(1);
  request.protocol = connection->context->protocol;
  request.required_roles =
      GM_IDENTITY_ROLE_SIGNING |
      (request.protocol == GM_PROTOCOL_TLCP ? GM_IDENTITY_ROLE_ENCRYPTION : 0);
  const auto* names = SSL_get0_server_requested_CAs(ssl);
  const size_t count = names ? sk_CRYPTO_BUFFER_num(names) : 0;
  size_t total = 0;
  if (count > 128) {
    connection->terminal = GM_ERROR_LIMIT;
    return 0;
  }
  for (size_t i = 0; i < count; ++i) {
    const CRYPTO_BUFFER* name = sk_CRYPTO_BUFFER_value(names, i);
    const size_t size = CRYPTO_BUFFER_len(name);
    if (!size || size > 16384 || size > 65535 - total) {
      connection->terminal = GM_ERROR_LIMIT;
      return 0;
    }
    total += size;
    const auto name_bytes = AbiSpan(CRYPTO_BUFFER_data(name), size);
    connection->ca_names.emplace_back(name_bytes.begin(), name_bytes.end());
  }
  request.ca_name_count = static_cast<uint32_t>(count);
  const uint16_t* algorithms = nullptr;
  const size_t algorithm_count =
      SSL_get0_peer_verify_algorithms(ssl, &algorithms);
  if (algorithm_count > 128) {
    connection->terminal = GM_ERROR_LIMIT;
    return 0;
  }
  request.signature_algorithm_count = static_cast<uint32_t>(algorithm_count);
  if (algorithm_count) {
    base::span(request.signature_algorithms)
        .first(algorithm_count)
        .copy_from(AbiSpan(algorithms, algorithm_count));
  }
  const unsigned char* types = nullptr;
  const size_t type_count = SSL_get0_certificate_types(ssl, &types);
  if (type_count > 256 || (type_count && !types)) {
    connection->terminal = GM_ERROR_LIMIT;
    return 0;
  }
  request.certificate_type_count = static_cast<uint32_t>(type_count);
  if (type_count) {
    base::span(request.certificate_types)
        .first(type_count)
        .copy_from(AbiSpan(types, type_count));
  }
  connection->identity_pending = true;
  return -1;
}

int SelectIdentity(SSL* ssl, void* argument) {
  return SelectIdentityImpl(ssl, argument);
}

void ObserveHandshake(int write,
                      int,
                      int type,
                      const void* bytes,
                      size_t size,
                      SSL*,
                      void* argument) {
  auto* connection = static_cast<GmConnection*>(argument);
  if (write || type != SSL3_RT_HANDSHAKE || !size) {
    return;
  }
  const auto message = *static_cast<const unsigned char*>(bytes);
  if (message == SSL3_MT_CERTIFICATE ||
      message == SSL3_MT_COMPRESSED_CERTIFICATE) {
    // A new server certificate replaces any imported session's chains. In
    // particular, a failed full handshake must report its rejected peer, not
    // the previously authenticated session's leaf. CertificateRequest alone
    // must not reset validation already completed in the TLCP state machine.
    connection->verified_peer_chains =
        GmConnection::PeerChainSource::kUnverified;
    connection->peer_verified = false;
    sk_X509_pop_free(connection->signing_chain, X509_free);
    sk_X509_pop_free(connection->encryption_chain, X509_free);
    connection->signing_chain = nullptr;
    connection->encryption_chain = nullptr;
  }
  if (message == SSL3_MT_CERTIFICATE ||
      message == SSL3_MT_COMPRESSED_CERTIFICATE ||
      message == SSL3_MT_CERTIFICATE_REQUEST) {
    connection->authentication_started = true;
    // A rejected ticket followed by a full handshake must not retain the
    // previous session's client-authentication result.
    connection->identity_authenticated = false;
  }
}

constexpr unsigned char kSessionMagic[] = {'G', 'M', 'S', '3', 0, 0, 0, 0};
constexpr size_t kSessionHeaderSize = sizeof(kSessionMagic) + 32 + 4 * 4;

STACK_OF(X509) *
    DecodeChain(GmConnection* connection, CBS* input, uint32_t count) {
  STACK_OF(X509)* chain = sk_X509_new_null();
  if (!chain) {
    return nullptr;
  }
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t size;
    CBS certificate_bytes;
    if (!CBS_get_u32(input, &size) || !size || size > kCertificateLimit ||
        !CBS_get_bytes(input, &certificate_bytes, size)) {
      sk_X509_pop_free(chain, X509_free);
      return nullptr;
    }
    X509* certificate = ParseCertificate(
        connection->context,
        {CBS_data(&certificate_bytes), CBS_len(&certificate_bytes)});
    if (!certificate || !sk_X509_push(chain, certificate)) {
      X509_free(certificate);
      sk_X509_pop_free(chain, X509_free);
      return nullptr;
    }
  }
  return chain;
}

GmStatus ImportSession(GmConnection* connection,
                       GmBytes bytes,
                       base::span<const uint16_t> cipher_suites) {
  if (!connection->resumption_enabled || !ValidBytes(bytes, kSessionLimit) ||
      bytes.size < kSessionHeaderSize) {
    return GM_ERROR_ARGUMENT;
  }
  const auto blob = AbiSpan(bytes.data, bytes.size);
  if (blob.first(sizeof(kSessionMagic)) != base::span(kSessionMagic) ||
      CRYPTO_memcmp(blob.subspan(sizeof(kSessionMagic), 32u).data(),
                    connection->session_binding, 32)) {
    return GM_ERROR_ARGUMENT;
  }
  CBS input;
  CBS_init(&input, blob.data(), blob.size());
  if (!CBS_skip(&input, sizeof(kSessionMagic) + 32)) {
    return GM_ERROR_ARGUMENT;
  }
  uint32_t flags, session_size, sign_count, enc_count;
  if (!CBS_get_u32(&input, &flags) || (flags & ~GM_INFO_CLIENT_CERTIFICATE) ||
      ((flags & GM_INFO_CLIENT_CERTIFICATE) &&
       !connection->identity_installed) ||
      !CBS_get_u32(&input, &session_size) || !session_size ||
      session_size > kCertificateLimit || !CBS_get_u32(&input, &sign_count) ||
      !sign_count || sign_count > 17 || !CBS_get_u32(&input, &enc_count) ||
      enc_count > 17 ||
      (connection->context->protocol == GM_PROTOCOL_TLCP) != (enc_count != 0) ||
      session_size > CBS_len(&input)) {
    return GM_ERROR_ARGUMENT;
  }
  CBS session_bytes;
  if (!CBS_get_bytes(&input, &session_bytes, session_size)) {
    return GM_ERROR_ARGUMENT;
  }
  SSL_SESSION* session =
      SSL_SESSION_from_bytes(CBS_data(&session_bytes), CBS_len(&session_bytes),
                             connection->context->ssl);
  bool valid =
      session && SSL_SESSION_is_resumable(session) &&
      SSL_SESSION_get_protocol_version(session) ==
          (connection->context->protocol == GM_PROTOCOL_TLCP ? TLCP1_1_VERSION
                                                             : TLS1_3_VERSION);
  if (valid && !cipher_suites.empty()) {
    const SSL_CIPHER* cipher = SSL_SESSION_get0_cipher(session);
    valid = cipher && std::ranges::contains(cipher_suites,
                                            SSL_CIPHER_get_protocol_id(cipher));
  }
  if (valid) {
    const time_t now = std::time(nullptr);
    const uint64_t created = SSL_SESSION_get_time(session);
    const uint32_t lifetime = SSL_SESSION_get_timeout(session);
    valid = now >= 0 && static_cast<uint64_t>(now) >= created && lifetime > 0 &&
            static_cast<uint64_t>(now) - created < lifetime;
  }
  if (!valid) {
    SSL_SESSION_free(session);
    return GM_ERROR_ARGUMENT;
  }
  STACK_OF(X509)* sign = DecodeChain(connection, &input, sign_count);
  STACK_OF(X509)* enc =
      enc_count ? DecodeChain(connection, &input, enc_count) : nullptr;
  GmStatus status = GM_ERROR_ARGUMENT;
  if (sign && (!enc_count || enc) && CBS_len(&input) == 0) {
    const auto* session_peers = SSL_SESSION_get0_peer_certificates(session);
    const CRYPTO_BUFFER* peer_buffer =
        session_peers && sk_CRYPTO_BUFFER_num(session_peers)
            ? sk_CRYPTO_BUFFER_value(session_peers, 0)
            : nullptr;
    bssl::UniquePtr<X509> peer(
        peer_buffer ? ParseCertificate(connection->context,
                                       {CRYPTO_BUFFER_data(peer_buffer),
                                        CRYPTO_BUFFER_len(peer_buffer)})
                    : nullptr);
    bool peer_matches =
        peer && X509_cmp(peer.get(), sk_X509_value(sign, 0)) == 0;
    if (enc) {
      const CRYPTO_BUFFER* enc_buffer =
          session_peers && sk_CRYPTO_BUFFER_num(session_peers) > 1
              ? sk_CRYPTO_BUFFER_value(session_peers, 1)
              : nullptr;
      bssl::UniquePtr<X509> enc_peer(
          enc_buffer ? ParseCertificate(connection->context,
                                        {CRYPTO_BUFFER_data(enc_buffer),
                                         CRYPTO_BUFFER_len(enc_buffer)})
                     : nullptr);
      peer_matches = peer_matches && enc_peer &&
                     X509_cmp(enc_peer.get(), sk_X509_value(enc, 0)) == 0;
    }
    // Bind both authenticated TLCP leaves, or the RFC 8998 leaf, to the
    // separately serialized chains before revalidating them.
    if (peer_matches) {
      status = GM_ERROR_CERTIFICATE;
      if (VerifyRoleChain(connection, sk_X509_value(sign, 0), sign,
                          GM_CERTIFICATE_SIGNING, &connection->signing_chain) &&
          (!enc || VerifyRoleChain(connection, sk_X509_value(enc, 0), enc,
                                   GM_CERTIFICATE_ENCRYPTION,
                                   &connection->encryption_chain))) {
        status = SSL_set_session(connection->ssl, session) == 1 ? GM_OK
                                                                : GM_ERROR_TLS;
        if (status == GM_OK) {
          connection->verified_peer_chains =
              GmConnection::PeerChainSource::kImportedSession;
          connection->identity_authenticated =
              flags & GM_INFO_CLIENT_CERTIFICATE;
        }
      }
    }
  }
  sk_X509_pop_free(sign, X509_free);
  sk_X509_pop_free(enc, X509_free);
  SSL_SESSION_free(session);
  return status;
}

void FreeContext(GmContext* context) {
  SSL_CTX_free(context->ssl);
  delete context;
}

int NewSessionCallback(SSL* ssl, SSL_SESSION* session) {
  auto* connection = static_cast<GmConnection*>(SSL_get_app_data(ssl));
  if (!connection || !connection->resumption_enabled) {
    return 0;
  }
  SSL_SESSION_free(connection->latest_session);
  connection->latest_session = session;
  if (++connection->session_generation == 0) {
    ++connection->session_generation;
  }
  // Retain this callback reference. TLS 1.3 tickets may arrive after Finished.
  return 1;
}

SSL_SESSION* CurrentSession(GmConnection* connection) {
  return connection->latest_session ? connection->latest_session
                                    : SSL_get_session(connection->ssl);
}

GmStatus CreateContext(const GmContextConfig* config, GmContext** out) {
  if (!out) {
    return GM_ERROR_ARGUMENT;
  }
  *out = nullptr;
  if (!config || config->struct_size != sizeof(*config) ||
      (config->trust_anchor_count && !config->trust_anchors) ||
      config->trust_anchor_count > 4096 || config->crl_count > 128 ||
      config->untrusted_intermediate_count > 4096 ||
      config->disallowed_certificate_count > 4096 ||
      (config->untrusted_intermediate_count &&
       !config->untrusted_intermediates) ||
      (config->disallowed_certificate_count &&
       !config->disallowed_certificates) ||
      (config->crl_count && !config->crls) ||
      config->revocation_mode > GM_REVOCATION_REQUIRE_CHAIN_CRLS) {
    return GM_ERROR_ARGUMENT;
  }
  if (config->protocol != GM_PROTOCOL_TLCP &&
      config->protocol != GM_PROTOCOL_RFC8998) {
    return GM_ERROR_UNSUPPORTED;
  }
  auto* context = new (std::nothrow) GmContext;
  if (!context) {
    return GM_ERROR_LIMIT;
  }
  context->protocol = config->protocol;
  context->verifier = net::gm::GmCertificateVerifier::Create(*config);
  context->ssl = SSL_CTX_new(TLS_with_buffers_method());
  if (!context->verifier || !context->ssl || !HashTrust(context, config)) {
    FreeContext(context);
    return GM_ERROR_TLS;
  }
  SSL_CTX_set_custom_verify(context->ssl, SSL_VERIFY_PEER, VerifyPeer);
  SSL_CTX_set_max_cert_list(context->ssl, kCertificateLimit);
  SSL_CTX_set_session_cache_mode(
      context->ssl, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL);
  SSL_CTX_sess_set_new_cb(context->ssl, NewSessionCallback);
  *out = context;
  return GM_OK;
}

GmStatus DestroyContext(GmContext* context) {
  if (!context) {
    return GM_ERROR_ARGUMENT;
  }
  if (!OnThread(context)) {
    return GM_ERROR_THREAD;
  }
  if (context->connections) {
    return GM_ERROR_STATE;
  }
  FreeContext(context);
  return GM_OK;
}

GmStatus CreateConnectionImpl(GmContext* context,
                              const GmConnectionConfig* config,
                              base::span<const uint16_t> cipher_suites,
                              GmConnection** out) {
  if (!out) {
    return GM_ERROR_ARGUMENT;
  }
  *out = nullptr;
  if (!context || !config || config->struct_size != sizeof(*config) ||
      !ValidBytes(config->server_name, 253) ||
      config->hostname_mode > GM_HOSTNAME_BROWSER_SAN ||
      config->session_binding.size > 4096 ||
      (config->session_binding.size && !config->session_binding.data) ||
      config->session_blob.size > kSessionLimit || config->defer_identity > 1 ||
      config->require_client_auth > 1 ||
      (config->session_blob.size && !config->session_blob.data)) {
    return GM_ERROR_ARGUMENT;
  }
  if (!OnThread(context)) {
    return GM_ERROR_THREAD;
  }
  for (size_t i = 0; i < config->server_name.size; ++i) {
    const unsigned char ch =
        AbiSpan(config->server_name.data, config->server_name.size)[i];
    if (ch <= 32 || ch >= 127 || ch == '/' || ch == '\\' || ch == '*') {
      return GM_ERROR_ARGUMENT;
    }
  }
  const auto host_bytes =
      AbiSpan(config->server_name.data, config->server_name.size);
  const std::string host(host_bytes.begin(), host_bytes.end());
  auto* connection = new (std::nothrow) GmConnection;
  if (!connection) {
    return GM_ERROR_LIMIT;
  }
  connection->context = context;
  connection->defer_identity = config->defer_identity != 0;
  connection->require_client_auth = config->require_client_auth != 0;
  connection->hostname_mode = config->hostname_mode;
  base::span(connection->server_name)
      .first(host_bytes.size())
      .copy_from(host_bytes);
  connection->server_name_size = config->server_name.size;
  connection->ssl = SSL_new(context->ssl);
  connection->input = BIO_new(BIO_s_mem());
  connection->output = BIO_new(BIO_s_mem());
  if (!connection->ssl || !connection->input || !connection->output) {
    SSL_free(connection->ssl);
    BIO_free(connection->input);
    BIO_free(connection->output);
    delete connection;
    return GM_ERROR_TLS;
  }
  BIO_set_mem_eof_return(connection->input, -1);
  SSL_set_bio(connection->ssl, connection->input, connection->output);
  SSL_set_app_data(connection->ssl, connection);
  SSL_set_msg_callback(connection->ssl, ObserveHandshake);
  SSL_set_msg_callback_arg(connection->ssl, connection);
  if (connection->defer_identity) {
    SSL_set_cert_cb(connection->ssl, SelectIdentity, connection);
  }
  SSL_set_connect_state(connection->ssl);
  SSL_set_renegotiate_mode(connection->ssl, ssl_renegotiate_never);
  bool configured =
      SSL_set_gm_mode(connection->ssl, context->protocol == GM_PROTOCOL_TLCP
                                           ? SSL_GM_MODE_TLCP
                                           : SSL_GM_MODE_RFC8998_ONLY) == 1;
  if (!cipher_suites.empty()) {
    configured = configured && SSL_set1_gm_cipher_suites(connection->ssl,
                                                         cipher_suites.data(),
                                                         cipher_suites.size());
  }
  if (context->protocol == GM_PROTOCOL_RFC8998) {
    configured = configured &&
                 SSL_set_min_proto_version(connection->ssl, TLS1_3_VERSION) &&
                 SSL_set_max_proto_version(connection->ssl, TLS1_3_VERSION);
  }
  ASN1_OCTET_STRING* ip = a2i_IPADDRESS(host.c_str());
  if (ip) {
    ASN1_OCTET_STRING_free(ip);
  } else {
    // The verifier separately checks SAN names. Never send SNI for IP literals.
    configured = configured && host.find(':') == std::string::npos &&
                 SSL_set_tlsext_host_name(connection->ssl, host.c_str());
  }
  const unsigned char default_alpn[] = {8,   'h', 't', 't', 'p',
                                        '/', '1', '.', '1'};
  GmBytes alpn = config->alpn.size
                     ? config->alpn
                     : GmBytes{default_alpn, sizeof(default_alpn)};
  if (alpn.data && alpn.size <= 32) {
    base::span(connection->alpn)
        .first(alpn.size)
        .copy_from(AbiSpan(alpn.data, alpn.size));
    connection->alpn_size = alpn.size;
  }
  configured = configured && ValidateAlpn(alpn, context->protocol) &&
               SSL_set_alpn_protos(connection->ssl, alpn.data,
                                   static_cast<unsigned int>(alpn.size)) == 0 &&
               InstallIdentity(connection, config->identity);
  connection->resumption_enabled = config->session_binding.size != 0;
  configured = configured && HashSessionBinding(connection, config, alpn);
  if (!connection->resumption_enabled) {
    SSL_set_options(connection->ssl, SSL_OP_NO_TICKET);
  }
  GmStatus failure = GM_ERROR_ARGUMENT;
  if (configured && config->session_blob.size) {
    failure = ImportSession(connection, config->session_blob, cipher_suites);
    configured = failure == GM_OK;
  }
  if (!configured) {
    SSL_free(connection->ssl);
    EVP_PKEY_free(connection->signing_public_key);
    SSL_SESSION_free(connection->latest_session);
    sk_X509_pop_free(connection->signing_chain, X509_free);
    sk_X509_pop_free(connection->encryption_chain, X509_free);
    sk_X509_pop_free(connection->diagnostic_signing_chain, X509_free);
    sk_X509_pop_free(connection->diagnostic_encryption_chain, X509_free);
    delete connection;
    return failure;
  }
  ++context->connections;
  *out = connection;
  return GM_OK;
}

GmStatus CreateConnection(GmContext* context,
                          const GmConnectionConfig* config,
                          GmConnection** out) {
  return CreateConnectionImpl(context, config, {}, out);
}

bool IsLocalPeerCertificateError(uint32_t error) {
  if (ERR_GET_LIB(error) != ERR_LIB_SSL) {
    return false;
  }
  switch (ERR_GET_REASON(error)) {
    case SSL_R_CERTIFICATE_VERIFY_FAILED:
    case SSL_R_WRONG_CERTIFICATE_TYPE:
    case SSL_R_BAD_ECC_CERT:
    case SSL_R_CANNOT_PARSE_LEAF_CERT:
    case SSL_R_KEY_USAGE_BIT_INCORRECT:
      return true;
    default:
      return false;
  }
}

GmStatus MapResult(GmConnection* connection, int result) {
  if (connection->terminal != GM_OK) {
    return connection->terminal;
  }
  if (result > 0) {
    return GM_OK;
  }
  const int error = SSL_get_error(connection->ssl, result);
  if (connection->cancelled) {
    return connection->terminal = GM_ERROR_CANCELLED;
  }
  if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) {
    return GM_WANT_INPUT;
  }
  if (error == SSL_ERROR_WANT_PRIVATE_KEY_OPERATION && connection->pending) {
    return GM_WANT_KEY;
  }
  if (error == SSL_ERROR_WANT_X509_LOOKUP && connection->identity_pending) {
    return GM_WANT_IDENTITY;
  }
  if (error == SSL_ERROR_ZERO_RETURN) {
    return connection->terminal = GM_CLOSED;
  }
  if (connection->verify_result != X509_V_OK) {
    return connection->terminal = GM_ERROR_CERTIFICATE;
  }
  const uint32_t reason = ERR_peek_last_error();
  const uint32_t first_reason = ERR_peek_error();
  if (error == SSL_ERROR_SSL && (IsLocalPeerCertificateError(reason) ||
                                 IsLocalPeerCertificateError(first_reason))) {
    // BoringSSL checks certificate structure, key types and TLCP role usage
    // before calling the custom verifier. Preserve the rejected peer's chain
    // for diagnostics without marking it verified or attempting another
    // transport. Peer alerts about our client certificate are not local
    // server-certificate validation failures.
    CaptureDiagnosticPeers(connection);
    const bool bad_usage =
        (ERR_GET_LIB(reason) == ERR_LIB_SSL &&
         ERR_GET_REASON(reason) == SSL_R_KEY_USAGE_BIT_INCORRECT) ||
        (ERR_GET_LIB(first_reason) == ERR_LIB_SSL &&
         ERR_GET_REASON(first_reason) == SSL_R_KEY_USAGE_BIT_INCORRECT);
    connection->verify_result = bad_usage ? X509_V_ERR_INVALID_PURPOSE
                                          : X509_V_ERR_APPLICATION_VERIFICATION;
    connection->certificate_errors = GM_CERT_ERROR_INVALID;
    connection->authentication_started = true;
    connection->protocol_retry_allowed = false;
    return connection->terminal = GM_ERROR_CERTIFICATE;
  }
  // Only explicit protocol/cipher incompatibility before any certificate or
  // authentication activity permits another transport attempt. In particular,
  // generic handshake_failure, EOF, malformed records and TLS errors do not.
  if (!connection->authentication_started && !connection->request.request_id &&
      connection->verify_result == X509_V_OK &&
      ERR_GET_LIB(reason) == ERR_LIB_SSL) {
    const int code = ERR_GET_REASON(reason);
    connection->protocol_retry_allowed =
        code == SSL_R_UNSUPPORTED_PROTOCOL ||
        code == SSL_R_TLSV1_ALERT_PROTOCOL_VERSION ||
        code == SSL_R_NO_SHARED_CIPHER;
  }
  return connection->terminal = GM_ERROR_TLS;
}

GmStatus Handshake(GmConnection* connection) {
  const GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (connection->established) {
    return GM_OK;
  }
  if (connection->pending && !connection->completed) {
    return GM_WANT_KEY;
  }
  if (connection->identity_pending) {
    return GM_WANT_IDENTITY;
  }
  ERR_clear_error();
  const int result = SSL_do_handshake(connection->ssl);
  const GmStatus mapped = MapResult(connection, result);
  if (mapped == GM_OK) {
    if (!CaptureVerifiedPeers(connection)) {
      return connection->terminal = GM_ERROR_CERTIFICATE;
    }
    if (connection->require_client_auth &&
        !connection->identity_authenticated) {
      return connection->terminal = GM_ERROR_CLIENT_AUTH;
    }
    connection->established = true;
    // A resumed TLCP session does not invoke the new-session callback. It is
    // nevertheless eligible for export after fresh certificate revalidation.
    if (connection->resumption_enabled && !connection->session_generation &&
        SSL_SESSION_is_resumable(CurrentSession(connection))) {
      connection->session_generation = 1;
    }
  }
  return mapped;
}

GmStatus FeedCiphertext(GmConnection* connection,
                        const uint8_t* data,
                        size_t size,
                        size_t* consumed) {
  if (!consumed || (size && !data)) {
    return GM_ERROR_ARGUMENT;
  }
  *consumed = 0;
  const GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (connection->transport_eof) {
    return GM_ERROR_STATE;
  }
  if (size > kBufferLimit ||
      BIO_ctrl_pending(connection->input) > kBufferLimit - size) {
    return GM_ERROR_LIMIT;
  }
  if (!size) {
    return GM_OK;
  }
  return BIO_write_ex(connection->input, data, size, consumed) ? GM_OK
                                                               : GM_ERROR_TLS;
}

GmStatus DrainCiphertext(GmConnection* connection,
                         uint8_t* data,
                         size_t capacity,
                         size_t* written) {
  if (!connection || !written || (capacity && !data)) {
    return GM_ERROR_ARGUMENT;
  }
  *written = 0;
  if (!OnThread(connection->context)) {
    return GM_ERROR_THREAD;
  }
  // Fatal alerts remain drainable after a handshake failure.
  if (!capacity || !BIO_ctrl_pending(connection->output)) {
    return GM_OK;
  }
  const int result =
      BIO_read(connection->output, data,
               static_cast<int>(std::min(capacity, size_t{INT_MAX})));
  if (result <= 0) {
    return GM_ERROR_TLS;
  }
  *written = static_cast<size_t>(result);
  return GM_OK;
}

GmStatus ReadPlaintext(GmConnection* connection,
                       uint8_t* data,
                       size_t capacity,
                       size_t* written) {
  if (!written || !data || !capacity) {
    return GM_ERROR_ARGUMENT;
  }
  *written = 0;
  const GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (!connection->established) {
    return GM_ERROR_STATE;
  }
  ERR_clear_error();
  const int result =
      SSL_read(connection->ssl, data,
               static_cast<int>(std::min(capacity, size_t{INT_MAX})));
  if (result > 0) {
    *written = static_cast<size_t>(result);
  }
  return MapResult(connection, result);
}

GmStatus WritePlaintext(GmConnection* connection,
                        const uint8_t* data,
                        size_t size,
                        size_t* written) {
  if (!written || !data || !size) {
    return GM_ERROR_ARGUMENT;
  }
  *written = 0;
  const GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (!connection->established) {
    return GM_ERROR_STATE;
  }
  if (size > 16384 ||
      BIO_ctrl_pending(connection->output) > kBufferLimit - 32768) {
    return GM_ERROR_LIMIT;
  }
  ERR_clear_error();
  const int result = SSL_write(connection->ssl, data, static_cast<int>(size));
  if (result > 0) {
    *written = static_cast<size_t>(result);
  }
  return MapResult(connection, result);
}

GmStatus GetKeyRequest(GmConnection* connection, GmKeyRequest* request) {
  if (!request || request->struct_size != sizeof(*request)) {
    return GM_ERROR_ARGUMENT;
  }
  const GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (!connection->pending || connection->completed) {
    return GM_ERROR_STATE;
  }
  *request = connection->request;
  return GM_OK;
}

bool CanonicalSignature(const uint8_t* bytes, size_t size) {
  if (!bytes || size < 8 || size > 72) {
    return false;
  }
  ECDSA_SIG* sig = ECDSA_SIG_from_bytes(bytes, size);
  if (!sig) {
    return false;
  }
  unsigned char encoded[72];
  unsigned char* out = encoded;
  const BIGNUM* r;
  const BIGNUM* s;
  ECDSA_SIG_get0(sig, &r, &s);
  EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_sm2);
  const BIGNUM* order = group ? EC_GROUP_get0_order(group) : nullptr;
  bool valid = order && !BN_is_zero(r) && !BN_is_zero(s) &&
               !BN_is_negative(r) && !BN_is_negative(s) &&
               BN_cmp(r, order) < 0 && BN_cmp(s, order) < 0 &&
               i2d_ECDSA_SIG(sig, nullptr) == static_cast<int>(size);
  if (valid) {
    valid = i2d_ECDSA_SIG(sig, &out) == static_cast<int>(size) &&
            !CRYPTO_memcmp(bytes, encoded, size);
  }
  ECDSA_SIG_free(sig);
  EC_GROUP_free(group);
  return valid;
}

GmStatus CompleteKeyRequest(GmConnection* connection,
                            uint64_t id,
                            GmStatus result,
                            const uint8_t* signature,
                            size_t size) {
  const GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (!connection->pending || connection->completed ||
      connection->request.request_id != id) {
    return GM_ERROR_STATE;
  }
  if (result == GM_OK && !CanonicalSignature(signature, size)) {
    return GM_ERROR_ARGUMENT;
  }
  if (result != GM_OK && result >= 0) {
    return GM_ERROR_ARGUMENT;
  }
  connection->key_result = result;
  connection->signature_size = result == GM_OK ? size : 0;
  if (result == GM_OK) {
    base::span(connection->signature)
        .first(size)
        .copy_from(AbiSpan(signature, size));
  }
  connection->completed = true;
  return GM_OK;
}

GmStatus GetIdentityRequest(GmConnection* connection,
                            GmIdentityRequest* request) {
  const GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (!request || request->struct_size != sizeof(*request)) {
    return GM_ERROR_ARGUMENT;
  }
  if (!connection->identity_pending) {
    return GM_ERROR_STATE;
  }
  *request = connection->identity_request;
  return GM_OK;
}

GmStatus GetIdentityRequestCa(GmConnection* connection,
                              size_t index,
                              uint8_t* data,
                              size_t capacity,
                              size_t* written) {
  const GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (!written || (capacity && !data)) {
    return GM_ERROR_ARGUMENT;
  }
  *written = 0;
  if (!connection->identity_pending || index >= connection->ca_names.size()) {
    return GM_ERROR_STATE;
  }
  const auto& name = connection->ca_names[index];
  *written = name.size();
  if (!data && !capacity) {
    return GM_OK;
  }
  if (capacity < name.size()) {
    return GM_ERROR_LIMIT;
  }
  AbiSpan(data, capacity).first(name.size()).copy_from(name);
  return GM_OK;
}

GmStatus CompleteIdentityRequest(GmConnection* connection,
                                 uint64_t id,
                                 GmStatus result,
                                 const GmIdentity* identity,
                                 GmBytes binding) {
  const GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (!connection->identity_pending ||
      connection->identity_request.request_id != id) {
    return GM_ERROR_STATE;
  }
  if ((result != GM_OK && result >= 0) || binding.size > 4096 ||
      (binding.size && !binding.data)) {
    return GM_ERROR_ARGUMENT;
  }
  if (result != GM_OK) {
    connection->identity_pending = false;
    return connection->terminal = result;
  }
  if (!identity && connection->require_client_auth) {
    connection->identity_pending = false;
    return connection->terminal = GM_ERROR_CLIENT_AUTH;
  }
  SSL_certs_clear(connection->ssl);
  if (connection->context->protocol == GM_PROTOCOL_TLCP) {
    SSL_set1_tlcp_encryption_certificate(connection->ssl, nullptr);
  }
  EVP_PKEY_free(connection->signing_public_key);
  connection->signing_public_key = nullptr;
  connection->signing_key_id = 0;
  connection->identity_installed = false;
  connection->identity_authenticated = false;
  if (!InstallIdentity(connection, identity)) {
    return connection->terminal = GM_ERROR_CLIENT_AUTH;
  }
  GmConnectionConfig config = {};
  config.server_name = {connection->server_name, connection->server_name_size};
  config.hostname_mode = connection->hostname_mode;
  config.require_client_auth = connection->require_client_auth;
  config.identity = identity;
  config.session_binding = binding;
  if (!HashSessionBinding(connection, &config,
                          {connection->alpn, connection->alpn_size})) {
    return connection->terminal = GM_ERROR_TLS;
  }
  connection->resumption_enabled = binding.size != 0;
  connection->identity_pending = false;
  connection->identity_completed = true;
  return GM_OK;
}

GmStatus Cancel(GmConnection* connection) {
  if (!connection) {
    return GM_ERROR_ARGUMENT;
  }
  if (!OnThread(connection->context)) {
    return GM_ERROR_THREAD;
  }
  if (connection->cancelled && !connection->pending) {
    return GM_OK;
  }
  connection->cancelled = true;
  connection->protocol_retry_allowed = false;
  connection->identity_pending = false;
  // No suspended C stack exists. Discard the pending result and let SSL_free
  // release its handshake state; late completions are rejected by Check().
  connection->pending = false;
  connection->completed = false;
  OPENSSL_cleanse(connection->signature, sizeof(connection->signature));
  OPENSSL_cleanse(connection->request.digest,
                  sizeof(connection->request.digest));
  connection->terminal = GM_ERROR_CANCELLED;
  return GM_OK;
}

GmStatus DestroyConnection(GmConnection* connection) {
  const GmStatus status = Cancel(connection);
  if (status != GM_OK) {
    return status;
  }
  SSL_free(connection->ssl);
  EVP_PKEY_free(connection->signing_public_key);
  SSL_SESSION_free(connection->latest_session);
  sk_X509_pop_free(connection->signing_chain, X509_free);
  sk_X509_pop_free(connection->encryption_chain, X509_free);
  sk_X509_pop_free(connection->diagnostic_signing_chain, X509_free);
  sk_X509_pop_free(connection->diagnostic_encryption_chain, X509_free);
  --connection->context->connections;
  delete connection;
  return GM_OK;
}

uint32_t CertificateErrors(GmConnection* connection) {
  return connection->certificate_errors ? connection->certificate_errors
         : connection->terminal == GM_ERROR_CERTIFICATE ? GM_CERT_ERROR_INVALID
                                                        : 0;
}

GmStatus GetConnectionInfo(GmConnection* connection, GmConnectionInfo* info) {
  if (!connection || !info || info->struct_size != sizeof(*info)) {
    return GM_ERROR_ARGUMENT;
  }
  if (!OnThread(connection->context)) {
    return GM_ERROR_THREAD;
  }
  if (!connection->established && !connection->peer_verified &&
      connection->terminal == GM_OK) {
    return GM_ERROR_STATE;
  }
  *info = {};
  info->struct_size = sizeof(*info);
  info->protocol = connection->context->protocol;
  info->wire_version = static_cast<uint16_t>(SSL_version(connection->ssl));
  const SSL_CIPHER* cipher = SSL_get_current_cipher(connection->ssl);
  info->cipher_suite = cipher ? SSL_CIPHER_get_protocol_id(cipher) : 0;
  info->session_reused = SSL_session_reused(connection->ssl);
  SSL_SESSION* session =
      connection->established ? CurrentSession(connection) : nullptr;
  info->session_generation =
      connection->established && connection->resumption_enabled && session &&
              SSL_SESSION_is_resumable(session)
          ? connection->session_generation
          : 0;
  info->verify_result = connection->verify_result;
  info->certificate_errors = CertificateErrors(connection);
  info->flags =
      (connection->established ? GM_INFO_ESTABLISHED : 0) |
      (connection->identity_authenticated ? GM_INFO_CLIENT_CERTIFICATE : 0) |
      (connection->protocol_retry_allowed ? GM_INFO_PROTOCOL_RETRY_ALLOWED : 0);
  info->peer_signature_algorithm =
      SSL_get_peer_signature_algorithm(connection->ssl);
  info->key_exchange_group = SSL_get_group_id(connection->ssl);
  if (connection->context->protocol == GM_PROTOCOL_RFC8998 &&
      info->key_exchange_group == SSL_GROUP_SM2) {
    info->flags |= GM_INFO_FORWARD_SECRET;
  }
  const unsigned char* alpn = nullptr;
  unsigned int alpn_size = 0;
  SSL_get0_alpn_selected(connection->ssl, &alpn, &alpn_size);
  if (alpn_size > sizeof(info->alpn)) {
    return GM_ERROR_TLS;
  }
  info->alpn_size = alpn_size;
  if (alpn_size) {
    base::span(info->alpn).first(alpn_size).copy_from(AbiSpan(alpn, alpn_size));
  }
  return GM_OK;
}

STACK_OF(X509) * RoleChain(GmConnection* connection, uint32_t role) {
  if (role == GM_CERTIFICATE_SIGNING) {
    return connection->signing_chain ? connection->signing_chain
                                     : connection->diagnostic_signing_chain;
  }
  if (role == GM_CERTIFICATE_ENCRYPTION) {
    return connection->encryption_chain
               ? connection->encryption_chain
               : connection->diagnostic_encryption_chain;
  }
  return nullptr;
}

GmStatus GetPeerCertificateCount(GmConnection* connection,
                                 uint32_t role,
                                 size_t* count) {
  if (!connection || !count ||
      (role != GM_CERTIFICATE_SIGNING && role != GM_CERTIFICATE_ENCRYPTION)) {
    return GM_ERROR_ARGUMENT;
  }
  *count = 0;
  if (!OnThread(connection->context)) {
    return GM_ERROR_THREAD;
  }
  if (!connection->established && !connection->peer_verified &&
      connection->terminal == GM_OK) {
    return GM_ERROR_STATE;
  }
  STACK_OF(X509)* chain = RoleChain(connection, role);
  *count = chain ? static_cast<size_t>(sk_X509_num(chain)) : 0;
  return GM_OK;
}

GmStatus GetPeerCertificate(GmConnection* connection,
                            uint32_t role,
                            size_t index,
                            uint8_t* data,
                            size_t capacity,
                            size_t* written) {
  if (!written || (capacity && !data)) {
    return GM_ERROR_ARGUMENT;
  }
  *written = 0;
  size_t count = 0;
  GmStatus status = GetPeerCertificateCount(connection, role, &count);
  if (status != GM_OK) {
    return status;
  }
  if (index >= count) {
    return GM_ERROR_ARGUMENT;
  }
  X509* certificate =
      sk_X509_value(RoleChain(connection, role), static_cast<int>(index));
  const int length = i2d_X509(certificate, nullptr);
  if (length <= 0) {
    return GM_ERROR_TLS;
  }
  *written = static_cast<size_t>(length);
  if (!data && !capacity) {
    return GM_OK;
  }
  if (capacity < *written) {
    return GM_ERROR_LIMIT;
  }
  unsigned char* cursor = data;
  return i2d_X509(certificate, &cursor) == length ? GM_OK : GM_ERROR_TLS;
}

GmStatus ExportKeyingMaterial(GmConnection* connection,
                              GmBytes label,
                              GmBytes context,
                              uint32_t use_context,
                              uint8_t* out,
                              size_t size) {
  GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (!connection->established) {
    return GM_ERROR_STATE;
  }
  // The facade does not define a TLCP exporter. Preserve that ABI contract.
  if (connection->context->protocol == GM_PROTOCOL_TLCP) {
    return GM_ERROR_UNSUPPORTED;
  }
  if (!ValidBytes(label, 1024) || context.size > kBufferLimit ||
      (context.size && !context.data) || use_context > 1 || (size && !out) ||
      size > kBufferLimit) {
    return GM_ERROR_ARGUMENT;
  }
  for (size_t i = 0; i < label.size; ++i) {
    if (!AbiSpan(label.data, label.size)[i] ||
        AbiSpan(label.data, label.size)[i] > 127) {
      return GM_ERROR_ARGUMENT;
    }
  }
  ERR_clear_error();
  return SSL_export_keying_material(connection->ssl, out, size,
                                    reinterpret_cast<const char*>(label.data),
                                    label.size, context.data, context.size,
                                    use_context) == 1
             ? GM_OK
             : GM_ERROR_TLS;
}

GmStatus NotifyTransportEof(GmConnection* connection) {
  GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  connection->transport_eof = true;
  BIO_set_mem_eof_return(connection->input, 0);
  return GM_OK;
}

GmStatus GetPendingPlaintext(GmConnection* connection, size_t* bytes) {
  if (!connection || !bytes) {
    return GM_ERROR_ARGUMENT;
  }
  *bytes = 0;
  if (!OnThread(connection->context)) {
    return GM_ERROR_THREAD;
  }
  *bytes = static_cast<size_t>(SSL_pending(connection->ssl));
  return GM_OK;
}

GmStatus GetPendingCiphertext(GmConnection* connection, size_t* bytes) {
  if (!connection || !bytes) {
    return GM_ERROR_ARGUMENT;
  }
  *bytes = 0;
  if (!OnThread(connection->context)) {
    return GM_ERROR_THREAD;
  }
  // SSL_has_pending also covers a record already read from BIO but not yet
  // processed, e.g. a post-handshake ticket. Its exact byte count is private.
  *bytes = BIO_ctrl_pending(connection->input);
  if (!*bytes && SSL_has_pending(connection->ssl)) {
    *bytes = 1;
  }
  return GM_OK;
}

GmStatus ExportSession(GmConnection* connection,
                       uint8_t* data,
                       size_t capacity,
                       size_t* written) {
  if (!written || (capacity && !data)) {
    return GM_ERROR_ARGUMENT;
  }
  *written = 0;
  GmStatus status = Check(connection);
  if (status != GM_OK) {
    return status;
  }
  if (!connection->established || !connection->resumption_enabled ||
      !connection->signing_chain) {
    return GM_ERROR_STATE;
  }
  SSL_SESSION* session = CurrentSession(connection);
  if (!session || !SSL_SESSION_is_resumable(session)) {
    return GM_ERROR_STATE;
  }
  struct SerializedSession {
    uint8_t* data = nullptr;
    size_t size = 0;
    ~SerializedSession() {
      OPENSSL_cleanse(data, size);
      OPENSSL_free(data);
    }
  } serialized;
  if (!SSL_SESSION_to_bytes(session, &serialized.data, &serialized.size) ||
      !serialized.size || serialized.size > kCertificateLimit) {
    return GM_ERROR_LIMIT;
  }
  const uint32_t session_size = static_cast<uint32_t>(serialized.size);
  size_t size = kSessionHeaderSize + session_size;
  for (STACK_OF(X509)* chain :
       {connection->signing_chain, connection->encryption_chain}) {
    const int count = chain ? sk_X509_num(chain) : 0;
    if (count > 17) {
      return GM_ERROR_LIMIT;
    }
    for (int i = 0; i < count; ++i) {
      const int encoded = i2d_X509(sk_X509_value(chain, i), nullptr);
      if (encoded <= 0 || static_cast<size_t>(encoded) > kCertificateLimit) {
        return GM_ERROR_LIMIT;
      }
      size += 4 + encoded;
    }
  }
  if (size > kSessionLimit) {
    return GM_ERROR_LIMIT;
  }
  *written = size;
  if (!data && !capacity) {
    return GM_OK;
  }
  if (capacity < size) {
    return GM_ERROR_LIMIT;
  }
  bssl::ScopedCBB output;
  if (!CBB_init_fixed(output.get(), data, capacity) ||
      !CBB_add_bytes(output.get(), kSessionMagic, sizeof(kSessionMagic)) ||
      !CBB_add_bytes(output.get(), connection->session_binding, 32) ||
      !CBB_add_u32(output.get(), connection->identity_authenticated
                                     ? GM_INFO_CLIENT_CERTIFICATE
                                     : 0) ||
      !CBB_add_u32(output.get(), session_size) ||
      !CBB_add_u32(output.get(), sk_X509_num(connection->signing_chain)) ||
      !CBB_add_u32(output.get(), connection->encryption_chain
                                     ? sk_X509_num(connection->encryption_chain)
                                     : 0) ||
      !CBB_add_bytes(output.get(), serialized.data, serialized.size)) {
    return GM_ERROR_TLS;
  }
  for (STACK_OF(X509)* chain :
       {connection->signing_chain, connection->encryption_chain}) {
    const size_t count = chain ? sk_X509_num(chain) : 0;
    for (size_t i = 0; i < count; ++i) {
      X509* certificate = sk_X509_value(chain, i);
      const int encoded = i2d_X509(certificate, nullptr);
      uint8_t* certificate_bytes = nullptr;
      if (!CBB_add_u32(output.get(), static_cast<uint32_t>(encoded)) ||
          !CBB_add_space(output.get(), &certificate_bytes, encoded) ||
          i2d_X509(certificate, &certificate_bytes) != encoded) {
        return GM_ERROR_TLS;
      }
    }
  }
  return CBB_len(output.get()) == size ? GM_OK : GM_ERROR_TLS;
}

}  // namespace

namespace net::gm {

GmStatus CreateBackendConnection(GmContext* context,
                                 const GmConnectionConfig* config,
                                 base::span<const uint16_t> cipher_suites,
                                 GmConnection** out) {
  // An empty BoringSSL list means default suites, not "all disabled".
  if (cipher_suites.empty()) {
    if (out) {
      *out = nullptr;
    }
    return GM_ERROR_UNSUPPORTED;
  }
  return CreateConnectionImpl(context, config, cipher_suites, out);
}

}  // namespace net::gm

extern "C"
#if !defined(_WIN32)
    __attribute__((visibility("default")))
#endif
    GmStatus gm_backend_get_api(uint32_t version, GmBackendApi* api) {
  if (!api || api->struct_size != sizeof(*api) ||
      version != GM_BACKEND_ABI_VERSION) {
    return GM_ERROR_ABI;
  }
  *api = {sizeof(*api),
          GM_BACKEND_ABI_VERSION,
          GM_CAP_TLCP | GM_CAP_RFC8998 | GM_CAP_ASYNC_SM2 | GM_CAP_HTTP2 |
              GM_CAP_SESSION_RESUMPTION | GM_CAP_ASYNC_IDENTITY,
          CreateContext,
          DestroyContext,
          CreateConnection,
          DestroyConnection,
          Handshake,
          FeedCiphertext,
          DrainCiphertext,
          ReadPlaintext,
          WritePlaintext,
          GetKeyRequest,
          CompleteKeyRequest,
          Cancel,
          GetConnectionInfo,
          GetPeerCertificateCount,
          GetPeerCertificate,
          ExportKeyingMaterial,
          NotifyTransportEof,
          GetPendingPlaintext,
          GetPendingCiphertext,
          ExportSession,
          GetIdentityRequest,
          GetIdentityRequestCa,
          CompleteIdentityRequest};
  return GM_OK;
}
