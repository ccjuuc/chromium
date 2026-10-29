// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_BACKEND_H_
#define NET_GM_GM_BACKEND_H_

// This C facade isolates the national TLS socket integration from BoringSSL
// internals. Do not add OpenSSL/BoringSSL types, C++ objects, or
// allocator-owned results to this interface. All calls for a context and its
// connections must run on the OS thread that created the context, including
// cancellation/destruction.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GM_BACKEND_ABI_VERSION 3u
#define GM_CAP_TLCP (UINT64_C(1) << 0)
#define GM_CAP_RFC8998 (UINT64_C(1) << 1)
#define GM_CAP_ASYNC_SM2 (UINT64_C(1) << 2)
#define GM_CAP_HTTP2 (UINT64_C(1) << 3)
#define GM_CAP_SESSION_RESUMPTION (UINT64_C(1) << 4)
#define GM_CAP_ASYNC_IDENTITY (UINT64_C(1) << 5)
#define GM_INFO_ESTABLISHED (1u << 0)
#define GM_INFO_CLIENT_CERTIFICATE (1u << 1)
#define GM_INFO_FORWARD_SECRET (1u << 2)
#define GM_INFO_PROTOCOL_RETRY_ALLOWED (1u << 3)
#define GM_IDENTITY_ROLE_SIGNING (1u << 0)
#define GM_IDENTITY_ROLE_ENCRYPTION (1u << 1)
#define GM_CERT_ERROR_AUTHORITY (1u << 0)
#define GM_CERT_ERROR_DATE (1u << 1)
#define GM_CERT_ERROR_NAME (1u << 2)
#define GM_CERT_ERROR_REVOKED (1u << 3)
#define GM_CERT_ERROR_REVOCATION_UNAVAILABLE (1u << 4)
#define GM_CERT_ERROR_INVALID (1u << 5)
#define GM_CERT_ERROR_WEAK_SIGNATURE (1u << 6)

typedef struct GmContext GmContext;
typedef struct GmConnection GmConnection;

typedef enum GmStatus {
  GM_OK = 0,
  GM_WANT_INPUT = 1,
  GM_WANT_KEY = 2,
  GM_CLOSED = 3,
  GM_WANT_IDENTITY = 4,
  GM_ERROR_ARGUMENT = -1,
  GM_ERROR_STATE = -2,
  GM_ERROR_TLS = -3,
  GM_ERROR_CERTIFICATE = -4,
  GM_ERROR_CANCELLED = -5,
  GM_ERROR_UNSUPPORTED = -6,
  GM_ERROR_LIMIT = -7,
  GM_ERROR_THREAD = -8,
  GM_ERROR_ABI = -9,
  GM_ERROR_CLIENT_AUTH = -10
} GmStatus;

typedef enum GmProtocol {
  GM_PROTOCOL_TLCP = 1,
  GM_PROTOCOL_RFC8998 = 2
} GmProtocol;

typedef enum GmCertificateRole {
  GM_CERTIFICATE_SIGNING = 1,
  GM_CERTIFICATE_ENCRYPTION = 2
} GmCertificateRole;

typedef enum GmRevocationMode {
  GM_REVOCATION_DISABLED = 0,
  GM_REVOCATION_REQUIRE_LEAF_CRL = 1,
  GM_REVOCATION_REQUIRE_CHAIN_CRLS = 2
} GmRevocationMode;

typedef enum GmHostnameMode {
  GM_HOSTNAME_EXACT_SAN = 0,
  // Still prohibits CN fallback and partial wildcards. The caller MUST run
  // Chromium's X509Certificate::VerifyNameMatch (including PSL rules) on the
  // returned leaf before releasing application data or caching the session.
  GM_HOSTNAME_BROWSER_SAN = 1
} GmHostnameMode;

typedef struct GmBytes {
  const uint8_t* data;
  size_t size;
} GmBytes;

typedef struct GmContextConfig {
  uint32_t struct_size;
  uint32_t protocol;
  // Explicit DER roots only. System stores, files, and environment-configured
  // providers are never searched. An empty store rejects server certificates.
  const GmBytes* trust_anchors;
  size_t trust_anchor_count;
  // Explicit DER CRLs; no AIA, OCSP, CRL or other network retrieval is
  // performed by the backend. Required missing/stale/invalid CRLs reject the
  // handshake.
  const GmBytes* crls;
  size_t crl_count;
  uint32_t revocation_mode;
  // Issuer candidates, never trust anchors. Disallowed certificates reject
  // every verified role chain, including otherwise trusted roots.
  const GmBytes* untrusted_intermediates;
  size_t untrusted_intermediate_count;
  const GmBytes* disallowed_certificates;
  size_t disallowed_certificate_count;
} GmContextConfig;

typedef struct GmIdentity {
  uint32_t struct_size;
  GmBytes signing_certificate;
  GmBytes encryption_certificate;  // Required for TLCP client authentication.
  const GmBytes* intermediates;
  size_t intermediate_count;
  // Opaque, nonzero device-service identity references. No private key bytes
  // are accepted. The encryption reference is reserved for future KEX support;
  // this backend currently enables only static ECC TLCP suites.
  uint64_t signing_key_id;
  uint64_t encryption_key_id;
} GmIdentity;

typedef struct GmConnectionConfig {
  uint32_t struct_size;
  // An ASCII DNS name or IP literal without a port. No embedded NUL allowed.
  GmBytes server_name;
  const GmIdentity* identity;  // NULL permits server-only authentication.
  uint32_t hostname_mode;
  // ALPN wire encoding. Empty means http/1.1. Only h2 and http/1.1 are
  // accepted; TLCP permits http/1.1 only. Early data and renegotiation remain
  // disabled.
  GmBytes alpn;
  // Full, caller-owned cache partition identity, e.g. NetworkContext nonce,
  // NAK, effective trust/policy/identity generations. Empty disables
  // resumption. The backend additionally binds protocol, roots, CRLs, hostname,
  // certificates, private-key IDs and offered ALPN. Device
  // presence/authorization is the caller's responsibility BEFORE selecting a
  // cached session or a pooled connection.
  GmBytes session_binding;
  // A backend-exported session blob. It contains session secrets: keep only in
  // this partition's memory, never log/persist or send it to the device
  // service.
  GmBytes session_blob;
  // Zero preserves the preinstalled-identity API for standalone consumers.
  // When set, CertificateRequest suspends the same SSL until completion.
  uint32_t defer_identity;
  uint32_t require_client_auth;
} GmConnectionConfig;

typedef struct GmIdentityRequest {
  uint32_t struct_size;
  uint32_t protocol;
  uint32_t required_roles;
  uint32_t ca_name_count;
  uint64_t request_id;
  uint32_t signature_algorithm_count;
  uint32_t certificate_type_count;
  uint16_t signature_algorithms[128];
  uint8_t certificate_types[256];
} GmIdentityRequest;

typedef struct GmKeyRequest {
  uint32_t struct_size;
  uint32_t reserved;
  uint64_t request_id;
  uint64_t key_id;
  // SM3(ZA || message), ready for SKF_ECCSignData. Do NOT hash it again.
  uint8_t digest[32];
} GmKeyRequest;

typedef struct GmConnectionInfo {
  uint32_t struct_size;
  uint32_t protocol;
  uint16_t wire_version;
  uint16_t cipher_suite;
  uint32_t session_reused;
  int64_t verify_result;
  uint32_t certificate_errors;
  uint32_t flags;
  // IANA identifiers; zero when no such signature/group was negotiated.
  uint16_t peer_signature_algorithm;
  uint16_t key_exchange_group;
  uint32_t alpn_size;
  uint8_t alpn[16];
  // Connection-local generation of the newest resumable session. Zero means
  // none is available. Changes only when a session/ticket is installed; normal
  // application reads do not change it. Compare before calling export_session.
  uint64_t session_generation;
} GmConnectionInfo;

typedef struct GmBackendApi {
  uint32_t struct_size;
  uint32_t abi_version;
  uint64_t capabilities;
  GmStatus (*create_context)(const GmContextConfig*, GmContext**);
  // Fails with GM_ERROR_STATE while any connection still uses the context.
  GmStatus (*destroy_context)(GmContext*);
  GmStatus (*create_connection)(GmContext*,
                                const GmConnectionConfig*,
                                GmConnection**);
  // Cancellation discards pending private-key operations on the owning thread
  // before destroying SSL state. No device callback is called by this ABI.
  GmStatus (*destroy_connection)(GmConnection*);
  GmStatus (*handshake)(GmConnection*);
  GmStatus (*feed_ciphertext)(GmConnection*, const uint8_t*, size_t, size_t*);
  GmStatus (*drain_ciphertext)(GmConnection*, uint8_t*, size_t, size_t*);
  GmStatus (*read_plaintext)(GmConnection*, uint8_t*, size_t, size_t*);
  GmStatus (*write_plaintext)(GmConnection*, const uint8_t*, size_t, size_t*);
  GmStatus (*get_key_request)(GmConnection*, GmKeyRequest*);
  // Supply one canonical DER SM2 signature for the matching pending request.
  // A non-GM_OK result rejects it. Duplicate/late/mismatched results fail.
  GmStatus (*complete_key_request)(GmConnection*,
                                   uint64_t,
                                   GmStatus,
                                   const uint8_t*,
                                   size_t);
  GmStatus (*cancel)(GmConnection*);
  // Available after handshake success or a terminal failure (for diagnostics).
  // Returns GM_ERROR_STATE while a handshake is still in progress.
  // Constant-time metadata query: no chain validation or DER serialization.
  GmStatus (*get_connection_info)(GmConnection*, GmConnectionInfo*);
  // Verified role-specific chains, leaf first, available after authentication.
  // On a terminal certificate error, returns the received role leaf/chain for
  // diagnostics; those certificates MUST NOT be treated as verified.
  GmStatus (*get_peer_certificate_count)(GmConnection*,
                                         uint32_t role,
                                         size_t* count);
  // Query required length with data=NULL/capacity=0, then copy into caller
  // memory. An undersized buffer returns GM_ERROR_LIMIT and still reports
  // required size.
  GmStatus (*get_peer_certificate)(GmConnection*,
                                   uint32_t role,
                                   size_t index,
                                   uint8_t* data,
                                   size_t capacity,
                                   size_t* written);
  // RFC8998 only. The upstream public exporter rejects TLCP; that protocol
  // returns GM_ERROR_UNSUPPORTED rather than returning invented key material.
  GmStatus (*export_keying_material)(GmConnection*,
                                     GmBytes label,
                                     GmBytes context,
                                     uint32_t use_context,
                                     uint8_t* out,
                                     size_t size);
  // Marks transport EOF without fabricating close_notify. Pending authenticated
  // plaintext can still be read; truncated TLS records/unclean EOF fail closed.
  GmStatus (*notify_transport_eof)(GmConnection*);
  GmStatus (*get_pending_plaintext)(GmConnection*, size_t* bytes);
  GmStatus (*get_pending_ciphertext)(GmConnection*, size_t* bytes);
  // Returns the newest resumable session, with cached peer certificates for
  // revalidation on import. Query/copy convention matches certificate copying.
  // GM_ERROR_STATE means no eligible ticket yet (TLS1.3 tickets arrive on
  // read).
  GmStatus (*export_session)(GmConnection*,
                             uint8_t* data,
                             size_t capacity,
                             size_t* written);
  // Available only while GM_WANT_IDENTITY is pending. Server role chains and
  // connection metadata may also be queried then, before the caller authorizes
  // an identity. Finished has not yet been authenticated.
  GmStatus (*get_identity_request)(GmConnection*, GmIdentityRequest*);
  GmStatus (*get_identity_request_ca)(GmConnection*,
                                      size_t index,
                                      uint8_t*,
                                      size_t capacity,
                                      size_t* written);
  // GM_OK + NULL identity explicitly declines client authentication; negative
  // result aborts. A final cache partition binding is supplied with the chosen
  // identity; empty disables export. No session may be imported at this stage.
  GmStatus (*complete_identity_request)(GmConnection*,
                                        uint64_t request_id,
                                        GmStatus result,
                                        const GmIdentity*,
                                        GmBytes session_binding);
} GmBackendApi;

typedef GmStatus (*GmGetBackendApiFn)(uint32_t abi_version, GmBackendApi* api);

// Returns the in-process BoringSSL backend. The caller sets api->struct_size.
GmStatus gm_backend_get_api(uint32_t abi_version, GmBackendApi* api);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // NET_GM_GM_BACKEND_H_
