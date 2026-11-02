// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Test peer and software token only. Production gm_backend never imports a
// software private key. This executable has its own Tongsuo instance.
#include "net/gm/tests/backend_test_platform.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "net/gm/gm_backend.h"

namespace {
GmBackendApi api = {};
int cases = 0;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Certificate = std::unique_ptr<X509, decltype(&X509_free)>;
using Server = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;

void Require(bool passed, const char* message) {
  if (!passed) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ERR_print_errors_fp(stderr);
    std::exit(1);
  }
}

void Pass(const std::string& name) {
  ++cases;
  std::printf("PASS %s\n", name.c_str());
}

GmBytes Bytes(const std::vector<unsigned char>& bytes) {
  return {bytes.data(), bytes.size()};
}

GmBytes Bytes(const std::string& bytes) {
  return {reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size()};
}

Key NewKey() {
  Key key(EVP_PKEY_Q_keygen(nullptr, nullptr, "SM2"), EVP_PKEY_free);
  Require(key != nullptr, "SM2 key generation");
  return key;
}

void AddExtension(X509* certificate, X509* issuer, int nid, const char* value) {
  X509V3_CTX context;
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509_EXTENSION* extension =
      X509V3_EXT_conf_nid(nullptr, &context, nid, const_cast<char*>(value));
  Require(extension && X509_add_ext(certificate, extension, -1),
          "X509 extension");
  X509_EXTENSION_free(extension);
}

Certificate NewCertificate(
    EVP_PKEY* key,
    X509* issuer,
    EVP_PKEY* issuer_key,
    bool ca,
    bool encryption,
    bool expired = false,
    const char* san = "DNS:gm.test,DNS:localhost,IP:127.0.0.1",
    const char* common_name = nullptr) {
  // GmSSL's independent certificate profile requires at least four serial
  // octets. Keep generated test certificates distinct within this process.
  static long serial = 0x10000000;
  Certificate cert(X509_new(), X509_free);
  Require(cert && X509_set_version(cert.get(), 2) &&
              ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), serial++) &&
              X509_gmtime_adj(X509_getm_notBefore(cert.get()), -86400) &&
              X509_gmtime_adj(X509_getm_notAfter(cert.get()),
                              expired ? -60 : 86400) &&
              X509_set_pubkey(cert.get(), key),
          "certificate initialization");
  X509_NAME* subject = X509_get_subject_name(cert.get());
  const char* cn = common_name ? common_name : ca ? "GM test root" : "gm.test";
  Require(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                     reinterpret_cast<const unsigned char*>(cn),
                                     -1, -1, 0) &&
              X509_set_issuer_name(
                  cert.get(), issuer ? X509_get_subject_name(issuer) : subject),
          "certificate issuer");
  AddExtension(cert.get(), issuer ? issuer : cert.get(), NID_basic_constraints,
               ca ? "critical,CA:TRUE" : "critical,CA:FALSE");
  AddExtension(cert.get(), issuer ? issuer : cert.get(), NID_key_usage,
               ca ? "critical,keyCertSign,cRLSign"
               : encryption
                   ? "critical,keyEncipherment,dataEncipherment,keyAgreement"
                   : "critical,digitalSignature");
  if (!ca) {
    AddExtension(cert.get(), issuer, NID_ext_key_usage,
                 "serverAuth,clientAuth");
    if (san) {
      AddExtension(cert.get(), issuer, NID_subject_alt_name, san);
    }
  }
  Require(X509_sign(cert.get(), issuer_key ? issuer_key : key, EVP_sm3()) > 0,
          "SM2 certificate signing");
  return cert;
}

std::vector<unsigned char> Der(X509* certificate) {
  const int size = i2d_X509(certificate, nullptr);
  Require(size > 0, "certificate DER size");
  std::vector<unsigned char> result(size);
  unsigned char* cursor = result.data();
  Require(i2d_X509(certificate, &cursor) == size, "certificate DER encoding");
  return result;
}

std::vector<unsigned char> CrlDer(X509* issuer,
                                  EVP_PKEY* signer,
                                  X509* revoked = nullptr,
                                  bool expired = false) {
  X509_CRL* crl = X509_CRL_new();
  ASN1_TIME* last = ASN1_TIME_adj(nullptr, std::time(nullptr), 0, -300);
  ASN1_TIME* next =
      ASN1_TIME_adj(nullptr, std::time(nullptr), 0, expired ? -60 : 3600);
  Require(crl && last && next && X509_CRL_set_version(crl, 1) &&
              X509_CRL_set_issuer_name(crl, X509_get_subject_name(issuer)) &&
              X509_CRL_set1_lastUpdate(crl, last) &&
              X509_CRL_set1_nextUpdate(crl, next),
          "CRL initialization");
  ASN1_TIME_free(last);
  ASN1_TIME_free(next);
  if (revoked) {
    X509_REVOKED* entry = X509_REVOKED_new();
    ASN1_TIME* date = ASN1_TIME_adj(nullptr, std::time(nullptr), 0, -120);
    Require(entry && date &&
                X509_REVOKED_set_serialNumber(entry,
                                              X509_get_serialNumber(revoked)) &&
                X509_REVOKED_set_revocationDate(entry, date) &&
                X509_CRL_add0_revoked(crl, entry),
            "CRL revoked entry");
    ASN1_TIME_free(date);
  }
  Require(X509_CRL_sort(crl) && X509_CRL_sign(crl, signer, EVP_sm3()) > 0,
          "CRL signing");
  const int size = i2d_X509_CRL(crl, nullptr);
  Require(size > 0, "CRL DER size");
  std::vector<unsigned char> result(size);
  unsigned char* cursor = result.data();
  Require(i2d_X509_CRL(crl, &cursor) == size, "CRL DER encoding");
  X509_CRL_free(crl);
  return result;
}

struct Fixtures {
  Key root_key = NewKey();
  Certificate root =
      NewCertificate(root_key.get(), nullptr, nullptr, true, false);
  Key wrong_root_key = NewKey();
  Certificate wrong_root = NewCertificate(wrong_root_key.get(),
                                          nullptr,
                                          nullptr,
                                          true,
                                          false,
                                          false,
                                          nullptr,
                                          "Wrong test root");
  Key server_key = NewKey();
  Certificate server = NewCertificate(server_key.get(),
                                      root.get(),
                                      root_key.get(),
                                      false,
                                      false);
  Key server_enc_key = NewKey();
  Certificate server_enc = NewCertificate(server_enc_key.get(),
                                          root.get(),
                                          root_key.get(),
                                          false,
                                          true);
  Key client_key = NewKey();
  Certificate client = NewCertificate(client_key.get(),
                                      root.get(),
                                      root_key.get(),
                                      false,
                                      false);
  Key client_enc_key = NewKey();
  Certificate client_enc = NewCertificate(client_enc_key.get(),
                                          root.get(),
                                          root_key.get(),
                                          false,
                                          true);
  std::vector<unsigned char> root_der = Der(root.get());
  std::vector<unsigned char> wrong_root_der = Der(wrong_root.get());
  std::vector<unsigned char> client_der = Der(client.get());
  std::vector<unsigned char> client_enc_der = Der(client_enc.get());
};

struct Options {
  bool mutual = false;
  bool wrong_host = false;
  bool wrong_root = false;
  bool expired = false;
  bool cn_only = false;
  bool wildcard = false;
  bool browser_hostname = false;
  bool h2 = false;
  // 1 expired, 2 untrusted, 3 invalid key usage.
  int bad_encryption = 0;
  // 1 valid, 2 missing, 3 revoked signing, 4 stale, 5 bad signature,
  // 6 revoked encryption, 7 valid chain mode.
  int crl_case = 0;
  std::string partition;
};

Server ServerContext(Fixtures& fixtures,
                     uint32_t protocol,
                     const Options& options) {
  Server server(SSL_CTX_new(protocol == GM_PROTOCOL_TLCP ? NTLS_server_method()
                                                         : TLS_server_method()),
                SSL_CTX_free);
  Require(server != nullptr, "server context");
  Certificate changed(nullptr, X509_free);
  X509* sign = fixtures.server.get();
  if (options.expired || options.cn_only || options.wildcard) {
    changed =
        NewCertificate(fixtures.server_key.get(), fixtures.root.get(),
                       fixtures.root_key.get(), false, false, options.expired,
                       options.cn_only    ? nullptr
                       : options.wildcard ? "DNS:*.example.test"
                                          : "DNS:gm.test");
    sign = changed.get();
  }
  if (protocol == GM_PROTOCOL_TLCP) {
    SSL_CTX_enable_ntls(server.get());
    Certificate bad_enc(nullptr, X509_free);
    X509* enc = fixtures.server_enc.get();
    if (options.bad_encryption || options.wildcard) {
      bad_enc = NewCertificate(
          fixtures.server_enc_key.get(),
          options.bad_encryption == 2 ? fixtures.wrong_root.get()
                                      : fixtures.root.get(),
          options.bad_encryption == 2 ? fixtures.wrong_root_key.get()
                                      : fixtures.root_key.get(),
          false, true, options.bad_encryption == 1,
          options.wildcard ? "DNS:*.example.test" : "DNS:gm.test");
      enc = bad_enc.get();
    }
    Require(SSL_CTX_set_cipher_list(server.get(), "ECC-SM2-SM4-GCM-SM3") &&
                SSL_CTX_use_sign_certificate(server.get(), sign) &&
                SSL_CTX_use_sign_PrivateKey(server.get(),
                                            fixtures.server_key.get()) &&
                SSL_CTX_use_enc_certificate(server.get(), enc) &&
                SSL_CTX_use_enc_PrivateKey(server.get(),
                                           fixtures.server_enc_key.get()),
            "TLCP server setup");
  } else {
    Require(SSL_CTX_set_min_proto_version(server.get(), TLS1_3_VERSION) &&
                SSL_CTX_set_max_proto_version(server.get(), TLS1_3_VERSION) &&
                SSL_CTX_set_ciphersuites(server.get(), "TLS_SM4_GCM_SM3") &&
                SSL_CTX_set1_groups_list(server.get(), "SM2") &&
                SSL_CTX_set1_sigalgs_list(server.get(), "sm2sig_sm3") &&
                SSL_CTX_use_certificate(server.get(), sign) &&
                SSL_CTX_use_PrivateKey(server.get(), fixtures.server_key.get()),
            "RFC8998 server setup");
  }
  Require(X509_STORE_add_cert(SSL_CTX_get_cert_store(server.get()),
                              fixtures.root.get()),
          "server explicit trust");
  SSL_CTX_set_verify(server.get(),
                     options.mutual
                         ? SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT
                         : SSL_VERIFY_NONE,
                     nullptr);
  const unsigned char sid[] = {42};
  Require(SSL_CTX_set_session_id_context(server.get(), sid, sizeof(sid)),
          "server session context");
  if (options.h2) {
    SSL_CTX_set_alpn_select_cb(
        server.get(),
        [](SSL*, const unsigned char** out, unsigned char* outlen,
           const unsigned char* in, unsigned int inlen, void*) {
          if (inlen >= 3 && in[0] == 2 && in[1] == 'h' && in[2] == '2') {
            *out = in + 1;
            *outlen = 2;
            return SSL_TLSEXT_ERR_OK;
          }
          return SSL_TLSEXT_ERR_ALERT_FATAL;
        },
        nullptr);
  }
  return server;
}

struct ClientConfig {
  GmBytes root;
  std::vector<unsigned char> crl;
  GmBytes crl_bytes = {};
  GmContextConfig context = {};
  GmIdentity identity = {};
  GmConnectionConfig connection = {};
  std::string hostname;
  std::string partition;
  std::array<unsigned char, 12> h2_alpn = {2,   'h', '2', 8,   'h', 't',
                                           't', 'p', '/', '1', '.', '1'};

  ClientConfig(Fixtures& fixtures, uint32_t protocol, const Options& options)
      : root(Bytes(options.wrong_root ? fixtures.wrong_root_der
                                      : fixtures.root_der)),
        hostname(options.wrong_host ? "wrong.test"
                 : options.wildcard ? "gm.example.test"
                                    : "gm.test"),
        partition(options.partition) {
    if (options.crl_case && options.crl_case != 2) {
      crl = CrlDer(fixtures.root.get(),
                   options.crl_case == 5 ? fixtures.wrong_root_key.get()
                                         : fixtures.root_key.get(),
                   options.crl_case == 3   ? fixtures.server.get()
                   : options.crl_case == 6 ? fixtures.server_enc.get()
                                           : nullptr,
                   options.crl_case == 4);
    }
    crl_bytes = Bytes(crl);
    context.struct_size = sizeof(context);
    context.protocol = protocol;
    context.trust_anchors = &root;
    context.trust_anchor_count = 1;
    context.crls = crl.empty() ? nullptr : &crl_bytes;
    context.crl_count = crl.empty() ? 0 : 1;
    context.revocation_mode =
        options.crl_case == 7 ? GM_REVOCATION_REQUIRE_CHAIN_CRLS
        : options.crl_case    ? GM_REVOCATION_REQUIRE_LEAF_CRL
                              : GM_REVOCATION_DISABLED;
    identity.struct_size = sizeof(identity);
    identity.signing_certificate = Bytes(fixtures.client_der);
    identity.encryption_certificate = Bytes(fixtures.client_enc_der);
    identity.signing_key_id = 7;
    identity.encryption_key_id = 8;
    connection.struct_size = sizeof(connection);
    connection.server_name = Bytes(hostname);
    connection.identity = options.mutual ? &identity : nullptr;
    connection.hostname_mode = options.browser_hostname
                                   ? GM_HOSTNAME_BROWSER_SAN
                                   : GM_HOSTNAME_EXACT_SAN;
    if (options.h2) {
      connection.alpn = {h2_alpn.data(), h2_alpn.size()};
    }
    connection.session_binding = Bytes(partition);
  }
};

struct Pair {
  Fixtures& fixtures;
  GmContext* context = nullptr;
  GmConnection* client = nullptr;
  SSL* server = nullptr;
  bool server_done = false;
  int key_requests = 0;
  int identity_requests = 0;
  int client_hellos = 0;
  uint64_t last_identity_request = 0;
  const GmIdentity* selected_identity = nullptr;
  GmBytes identity_binding = {};
  bool owns_context = true;

  Pair(Fixtures& value,
       ClientConfig& config,
       SSL_CTX* peer,
       GmContext* shared_context = nullptr,
       bool check_lifecycle = true)
      : fixtures(value),
        context(shared_context),
        owns_context(shared_context == nullptr) {
    selected_identity = &config.identity;
    identity_binding = config.connection.session_binding;
    if (owns_context) {
      Require(api.create_context(&config.context, &context) == GM_OK,
              "backend context creation");
    }
    Require(
        api.create_connection(context, &config.connection, &client) == GM_OK,
        "backend connection creation");
    if (check_lifecycle) {
      Require(api.destroy_context(context) == GM_ERROR_STATE,
              "live connection preserves context");
      std::thread wrong_thread([&] {
        Require(api.handshake(client) == GM_ERROR_THREAD,
                "wrong thread rejected");
      });
      wrong_thread.join();
    }
    server = SSL_new(peer);
    BIO* input = BIO_new(BIO_s_mem());
    BIO* output = BIO_new(BIO_s_mem());
    Require(server && input && output, "server memory BIO");
    BIO_set_mem_eof_return(input, -1);
    SSL_set_bio(server, input, output);
    SSL_set_accept_state(server);
    SSL_set_msg_callback(server, [](int write, int, int type, const void* bytes,
                                    size_t size, SSL*, void* arg) {
      if (!write && type == SSL3_RT_HANDSHAKE && size &&
          *static_cast<const unsigned char*>(bytes) == SSL3_MT_CLIENT_HELLO) {
        ++static_cast<Pair*>(arg)->client_hellos;
      }
    });
    SSL_set_msg_callback_arg(server, this);
  }
  ~Pair() {
    Require(api.destroy_connection(client) == GM_OK, "connection cleanup");
    if (owns_context) {
      Require(api.destroy_context(context) == GM_OK, "context cleanup");
    }
    SSL_free(server);
  }
  void ToServer() {
    std::array<unsigned char, 8192> buffer;
    size_t size = 0;
    do {
      Require(api.drain_ciphertext(client, buffer.data(), buffer.size(),
                                   &size) == GM_OK,
              "drain client ciphertext");
      if (size) {
        Require(BIO_write(SSL_get_rbio(server), buffer.data(),
                          static_cast<int>(size)) == static_cast<int>(size),
                "feed server");
      }
    } while (size);
  }
  void ToClient() {
    std::array<unsigned char, 8192> buffer;
    while (BIO_ctrl_pending(SSL_get_wbio(server))) {
      int size = BIO_read(SSL_get_wbio(server), buffer.data(),
                          static_cast<int>(buffer.size()));
      size_t consumed = 0;
      Require(size > 0 &&
                  api.feed_ciphertext(client, buffer.data(), size, &consumed) ==
                      GM_OK &&
                  consumed == static_cast<size_t>(size),
              "feed client ciphertext");
    }
  }
  void CompleteSignature() {
    GmKeyRequest request = {};
    request.struct_size = sizeof(request);
    Require(
        api.get_key_request(client, &request) == GM_OK && request.key_id == 7,
        "external request");
    Require(api.complete_key_request(client, request.request_id + 1, GM_OK,
                                     nullptr, 0) == GM_ERROR_STATE,
            "wrong request completion rejected");
    const unsigned char invalid[] = {0x30, 0};
    Require(api.complete_key_request(client, request.request_id, GM_OK, invalid,
                                     sizeof(invalid)) == GM_ERROR_ARGUMENT,
            "malformed signature rejected");
    EVP_PKEY_CTX* signer = EVP_PKEY_CTX_new(fixtures.client_key.get(), nullptr);
    Require(signer && EVP_PKEY_sign_init(signer) > 0 &&
                EVP_PKEY_CTX_set_signature_md(signer, EVP_sm3()) > 0,
            "simulated token setup");
    unsigned char signature[72];
    size_t size = sizeof(signature);
    Require(EVP_PKEY_sign(signer, signature, &size, request.digest,
                          sizeof(request.digest)) > 0,
            "SM2 digest signing");
    EVP_PKEY_CTX_free(signer);
    Require(api.complete_key_request(client, request.request_id, GM_OK,
                                     signature, size) == GM_OK,
            "external signature completion");
    Require(api.complete_key_request(client, request.request_id, GM_OK,
                                     signature, size) == GM_ERROR_STATE,
            "duplicate completion rejected");
  }
  void CompleteIdentity() {
    GmIdentityRequest request = {};
    request.struct_size = sizeof(request);
    Require(api.get_identity_request(client, &request) == GM_OK,
            "identity request snapshot");
    Require(api.complete_identity_request(client, request.request_id + 1, GM_OK,
                                          selected_identity,
                                          identity_binding) == GM_ERROR_STATE,
            "wrong identity completion rejected");
    Require(api.complete_identity_request(client, request.request_id, GM_OK,
                                          selected_identity,
                                          identity_binding) == GM_OK,
            "identity installed without replacing SSL");
    Require(api.complete_identity_request(client, request.request_id, GM_OK,
                                          selected_identity,
                                          identity_binding) == GM_ERROR_STATE,
            "duplicate identity completion rejected");
  }
  GmStatus Handshake(bool stop_at_key = false, bool stop_at_identity = false) {
    for (int step = 0; step < 256; ++step) {
      GmStatus result = api.handshake(client);
      if (result == GM_WANT_IDENTITY) {
        GmIdentityRequest request = {};
        request.struct_size = sizeof(request);
        Require(api.get_identity_request(client, &request) == GM_OK,
                "pending identity snapshot");
        if (last_identity_request != request.request_id) {
          last_identity_request = request.request_id;
          ++identity_requests;
        }
        if (stop_at_identity) {
          return result;
        }
        CompleteIdentity();
        continue;
      }
      if (result == GM_WANT_KEY) {
        ++key_requests;
        if (stop_at_key) {
          return result;
        }
        CompleteSignature();
        continue;
      }
      if (result < 0) {
        return result;
      }
      ToServer();
      if (!server_done) {
        ERR_clear_error();
        int server_result = SSL_do_handshake(server);
        if (server_result == 1) {
          server_done = true;
        } else {
          int error = SSL_get_error(server, server_result);
          Require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE,
                  "server handshake progression");
        }
      }
      ToClient();
      if (result == GM_OK && server_done) {
        return GM_OK;
      }
    }
    Require(false, "handshake progress bound");
    return GM_ERROR_TLS;
  }
  GmConnectionInfo Info() {
    GmConnectionInfo result = {};
    result.struct_size = sizeof(result);
    Require(api.get_connection_info(client, &result) == GM_OK,
            "connection info");
    return result;
  }
  void Exchange() {
    const char request[] = "GET / HTTP/1.1\r\nHost: gm.test\r\n\r\n";
    size_t size = 0;
    Require(
        api.write_plaintext(client, reinterpret_cast<const uint8_t*>(request),
                            sizeof(request) - 1, &size) == GM_OK &&
            size == sizeof(request) - 1,
        "client plaintext write");
    ToServer();
    std::array<char, 256> received;
    int count =
        SSL_read(server, received.data(), static_cast<int>(received.size()));
    Require(count == sizeof(request) - 1 &&
                !std::memcmp(received.data(), request, count),
            "server plaintext receipt");
    const char reply[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK";
    Require(SSL_write(server, reply, sizeof(reply) - 1) == sizeof(reply) - 1,
            "server response write");
    ToClient();
    size_t pending = 0;
    Require(
        api.get_pending_ciphertext(client, &pending) == GM_OK && pending > 0,
        "encrypted input pending");
    Require(
        api.read_plaintext(client, reinterpret_cast<uint8_t*>(received.data()),
                           3, &size) == GM_OK &&
            size == 3,
        "partial plaintext read");
    Require(api.get_pending_plaintext(client, &pending) == GM_OK &&
                pending == sizeof(reply) - 1 - 3,
            "pending plaintext count");
    Require(api.read_plaintext(client,
                               reinterpret_cast<uint8_t*>(received.data()) + 3,
                               received.size() - 3, &size) == GM_OK &&
                size == sizeof(reply) - 1 - 3 &&
                !std::memcmp(received.data(), reply, sizeof(reply) - 1),
            "client complete response");
    std::array<unsigned char, 32> ours, peer;
    const std::string label = "EXPORTER-gm-test";
    GmStatus export_status = api.export_keying_material(
        client, Bytes(label), {}, 0, ours.data(), ours.size());
    if (Info().protocol == GM_PROTOCOL_TLCP) {
      Require(export_status == GM_ERROR_UNSUPPORTED,
              "TLCP exporter explicitly unsupported");
      return;
    }
    Require(export_status == GM_OK &&
                SSL_export_keying_material(server, peer.data(), peer.size(),
                                           label.data(), label.size(), nullptr,
                                           0, 0) &&
                ours == peer,
            "matching TLS exporter");
  }
  std::vector<unsigned char> Session() {
    size_t size = 0;
    GmStatus status = api.export_session(client, nullptr, 0, &size);
    if (status != GM_OK) {
      std::fprintf(stderr, "session export=%d\n", status);
    }
    Require(status == GM_OK && size > 0, "session export length");
    std::vector<unsigned char> bytes(size);
    Require(
        api.export_session(client, bytes.data(), bytes.size(), &size) == GM_OK,
        "session export");
    return bytes;
  }
};

void CheckChain(Pair& pair, uint32_t role, bool expected) {
  size_t count = 0;
  Require(api.get_peer_certificate_count(pair.client, role, &count) == GM_OK &&
              (count > 0) == expected,
          "role-specific chain count");
  for (size_t i = 0; i < count; ++i) {
    size_t size = 0;
    Require(api.get_peer_certificate(pair.client, role, i, nullptr, 0, &size) ==
                    GM_OK &&
                size > 0,
            "certificate length query");
    std::vector<unsigned char> bytes(size);
    Require(api.get_peer_certificate(pair.client, role, i, bytes.data(),
                                     size - 1, &size) == GM_ERROR_LIMIT,
            "certificate undersized output");
    Require(api.get_peer_certificate(pair.client, role, i, bytes.data(),
                                     bytes.size(), &size) == GM_OK,
            "certificate copy");
    const unsigned char* cursor = bytes.data();
    X509* cert = d2i_X509(nullptr, &cursor, static_cast<long>(bytes.size()));
    Require(cert && cursor == bytes.data() + bytes.size(),
            "exported certificate valid DER");
    X509_free(cert);
  }
}

void RunCase(Fixtures& fixtures,
             uint32_t protocol,
             const std::string& name,
             Options options = {}) {
  std::printf("RUN %s\n", name.c_str());
  std::fflush(stdout);
  ClientConfig config(fixtures, protocol, options);
  Server server = ServerContext(fixtures, protocol, options);
  Pair pair(fixtures, config, server.get());
  if (options.bad_encryption == 3) {
    // The SSL_CTX helper rejects this fixture up front; per-connection install
    // permits transmitting it so the client rejection path is exercised.
    Certificate invalid =
        NewCertificate(fixtures.server_enc_key.get(), fixtures.root.get(),
                       fixtures.root_key.get(), false, false);
    Require(SSL_use_enc_certificate(pair.server, invalid.get()),
            "install invalid peer usage fixture");
  }
  const GmStatus result = pair.Handshake();
  const bool cert_error = options.wrong_host || options.wrong_root ||
                          options.expired || options.cn_only ||
                          (options.wildcard && !options.browser_hostname) ||
                          options.bad_encryption ||
                          (options.crl_case >= 2 && options.crl_case <= 6);
  GmConnectionInfo info = pair.Info();
  if (cert_error) {
    if (result != GM_ERROR_CERTIFICATE) {
      std::fprintf(stderr, "status=%d verify=%lld\n", result,
                   info.verify_result);
    }
    Require(result == GM_ERROR_CERTIFICATE && info.certificate_errors != 0,
            "invalid certificate rejected");
    if (options.wrong_host) {
      Require(info.certificate_errors & GM_CERT_ERROR_NAME,
              "hostname error mapping");
    }
    if (options.expired) {
      Require(info.certificate_errors & GM_CERT_ERROR_DATE,
              "date error mapping");
    }
    if (options.wrong_root) {
      Require(info.certificate_errors & GM_CERT_ERROR_AUTHORITY,
              "authority error mapping");
    }
    if (options.crl_case == 3 || options.crl_case == 6) {
      Require(info.certificate_errors & GM_CERT_ERROR_REVOKED,
              "revoked error mapping");
    }
    if (options.crl_case == 2 || options.crl_case == 4 ||
        options.crl_case == 5) {
      Require(info.certificate_errors & GM_CERT_ERROR_REVOCATION_UNAVAILABLE,
              "revocation unavailable mapping");
    }
    size_t signing_count = 0;
    size_t encryption_count = 0;
    Require(api.get_peer_certificate_count(pair.client, GM_CERTIFICATE_SIGNING,
                                           &signing_count) == GM_OK &&
                api.get_peer_certificate_count(pair.client,
                                               GM_CERTIFICATE_ENCRYPTION,
                                               &encryption_count) == GM_OK &&
                signing_count + encryption_count > 0,
            "failed verification retains diagnostic certificates");
    if (protocol == GM_PROTOCOL_TLCP &&
        (options.wrong_host || options.wrong_root)) {
      Require(signing_count > 0,
              "TLCP encryption-first failure preserves signing diagnostics");
      const auto expected = Der(fixtures.server.get());
      std::vector<unsigned char> actual(expected.size());
      size_t size = 0;
      Require(api.get_peer_certificate(pair.client, GM_CERTIFICATE_SIGNING, 0,
                                       actual.data(), actual.size(),
                                       &size) == GM_OK &&
                  size == actual.size() && actual == expected,
              "diagnostic signing leaf is not the encryption certificate");
    }
  } else {
    if (result != GM_OK) {
      std::fprintf(stderr, "status=%d verify=%lld\n", result,
                   info.verify_result);
    }
    Require(result == GM_OK && (info.flags & GM_INFO_ESTABLISHED),
            "authenticated handshake");
    Require(info.protocol == protocol &&
                info.wire_version == (protocol == GM_PROTOCOL_TLCP
                                          ? NTLS1_1_VERSION
                                          : TLS1_3_VERSION),
            "honest protocol metadata");
    Require(pair.key_requests == (options.mutual ? 1 : 0),
            "exact external signing count");
    if (options.mutual) {
      Require(info.flags & GM_INFO_CLIENT_CERTIFICATE,
              "client authentication metadata");
    }
    if (protocol == GM_PROTOCOL_TLCP) {
      Require(
          !info.key_exchange_group && !(info.flags & GM_INFO_FORWARD_SECRET),
          "static ECC has no forward secrecy");
    } else {
      Require(info.key_exchange_group == 41 &&
                  (info.flags & GM_INFO_FORWARD_SECRET) &&
                  info.peer_signature_algorithm == 0x0708,
              "SM2 group and signature metadata");
    }
    if (options.h2) {
      Require(info.alpn_size == 2 && !std::memcmp(info.alpn, "h2", 2),
              "H2 ALPN metadata");
    }
    CheckChain(pair, GM_CERTIFICATE_SIGNING, true);
    CheckChain(pair, GM_CERTIFICATE_ENCRYPTION, protocol == GM_PROTOCOL_TLCP);
    pair.Exchange();
  }
  Pass(name);
}

void TestCancel(Fixtures& fixtures, uint32_t protocol) {
  Options options;
  options.mutual = true;
  ClientConfig config(fixtures, protocol, options);
  Server server = ServerContext(fixtures, protocol, options);
  Pair pending(fixtures, config, server.get());
  Require(pending.Handshake(true) == GM_WANT_KEY, "signing pause");
  GmKeyRequest request = {};
  request.struct_size = sizeof(request);
  Require(api.get_key_request(pending.client, &request) == GM_OK,
          "pending request before other connection");
  RunCase(fixtures,
          protocol == GM_PROTOCOL_TLCP ? GM_PROTOCOL_RFC8998 : GM_PROTOCOL_TLCP,
          "connection B progresses while A awaits Key");
  GmKeyRequest after = {};
  after.struct_size = sizeof(after);
  Require(api.get_key_request(pending.client, &after) == GM_OK &&
              after.request_id == request.request_id,
          "other connection preserves pending job");
  Require(api.cancel(pending.client) == GM_OK &&
              api.handshake(pending.client) == GM_ERROR_CANCELLED &&
              api.cancel(pending.client) == GM_OK,
          "cancel resumes and unwinds exactly once");
  Require(
      api.complete_key_request(pending.client, request.request_id, GM_ERROR_TLS,
                               nullptr, 0) == GM_ERROR_CANCELLED,
      "late result rejected after cancellation");
  Pass(protocol == GM_PROTOCOL_TLCP ? "TLCP cancel external signing"
                                    : "RFC8998 cancel external signing");
}

void TestEof(Fixtures& fixtures, uint32_t protocol, bool clean) {
  Options options;
  ClientConfig config(fixtures, protocol, options);
  Server server = ServerContext(fixtures, protocol, options);
  Pair pair(fixtures, config, server.get());
  Require(pair.Handshake() == GM_OK, "EOF test handshake");
  pair.Exchange();
  if (clean) {
    Require(SSL_shutdown(pair.server) >= 0, "peer close_notify");
    pair.ToClient();
  }
  Require(api.notify_transport_eof(pair.client) == GM_OK, "transport EOF");
  unsigned char data[8];
  size_t size = 0;
  Require(api.read_plaintext(pair.client, data, sizeof(data), &size) ==
              (clean ? GM_CLOSED : GM_ERROR_TLS),
          "clean vs truncated EOF distinction");
  Pass(clean ? "authenticated close_notify" : "unclean transport EOF rejected");
}

void TestResumption(Fixtures& fixtures, uint32_t protocol) {
  Options options;
  options.mutual = true;
  options.crl_case = 1;
  options.partition = "network-context=1|nak=1|trust=1|identity=1";
  ClientConfig config(fixtures, protocol, options);
  Server server = ServerContext(fixtures, protocol, options);
  std::vector<unsigned char> session;
  {
    Pair first(fixtures, config, server.get());
    Require(first.Handshake() == GM_OK, "initial resumption handshake");
    first.Exchange();
    const uint64_t generation = first.Info().session_generation;
    Require(generation != 0, "resumable session generation");
    first.Exchange();
    Require(first.Info().session_generation == generation,
            "application reads preserve session generation");
    if (protocol == GM_PROTOCOL_RFC8998) {
      Require(SSL_new_session_ticket(first.server) == 1,
              "request fresh TLS1.3 ticket");
      first.Exchange();
      Require(first.Info().session_generation > generation,
              "fresh TLS1.3 ticket advances session generation");
    }
    session = first.Session();
    Require(first.key_requests == 1, "first connection uses Key");
  }
  config.connection.session_blob = Bytes(session);
  {
    Pair resumed(fixtures, config, server.get());
    Require(resumed.Handshake() == GM_OK, "resumed handshake");
    GmConnectionInfo info = resumed.Info();
    Require(info.session_reused == 1 && resumed.key_requests == 0 &&
                (info.flags & GM_INFO_CLIENT_CERTIFICATE),
            "authenticated session avoids repeated Key signing");
    resumed.Exchange();
    CheckChain(resumed, GM_CERTIFICATE_SIGNING, true);
  }
  {
    // A fresh server context rejects the old ticket/session ID and sends a
    // different, expired leaf. Import-time validation of the old session must
    // neither authorize this full handshake nor hide its rejected certificate.
    Options expired = options;
    expired.expired = true;
    Server replacement = ServerContext(fixtures, protocol, expired);
    Pair rejected(fixtures, config, replacement.get());
    Require(rejected.Handshake() == GM_ERROR_CERTIFICATE &&
                !rejected.Info().session_reused &&
                (rejected.Info().certificate_errors & GM_CERT_ERROR_DATE) &&
                !(rejected.Info().flags & GM_INFO_PROTOCOL_RETRY_ALLOWED) &&
                rejected.key_requests == 0 && rejected.identity_requests == 0,
            "rejected ticket does not authenticate the new expired peer");
    for (int i = 0; i < 3; ++i) {
      Require(api.handshake(rejected.client) == GM_ERROR_CERTIFICATE,
              "certificate failure cannot be retried into success");
    }
    size_t size = 0;
    Require(api.get_peer_certificate(rejected.client, GM_CERTIFICATE_SIGNING, 0,
                                     nullptr, 0, &size) == GM_OK &&
                size > 0,
            "rejected replacement leaf is available for diagnostics");
    std::vector<unsigned char> leaf(size);
    X509* rejected_leaf = protocol == GM_PROTOCOL_TLCP
                              ? SSL_get_sign_certificate_ntls(rejected.server)
                              : SSL_get_certificate(rejected.server);
    Require(
        api.get_peer_certificate(rejected.client, GM_CERTIFICATE_SIGNING, 0,
                                 leaf.data(), leaf.size(), &size) == GM_OK &&
            rejected_leaf && leaf == Der(rejected_leaf) &&
            leaf != Der(fixtures.server.get()),
        "diagnostics use the new rejected leaf rather than imported trust");
  }
  {
    Options anonymous = options;
    anonymous.mutual = false;
    Server replacement = ServerContext(fixtures, protocol, anonymous);
    config.connection.require_client_auth = 1;
    // Session binding includes this policy, so export a matching authenticated
    // session before checking rejection by the replacement server.
    config.connection.session_blob = {};
    std::vector<unsigned char> required_session;
    {
      Pair authenticated(fixtures, config, server.get());
      Require(authenticated.Handshake() == GM_OK,
              "client-auth policy initial handshake");
      authenticated.Exchange();
      required_session = authenticated.Session();
    }
    config.connection.session_blob = Bytes(required_session);
    Pair rejected(fixtures, config, replacement.get());
    Require(rejected.Handshake() == GM_ERROR_CLIENT_AUTH &&
                !rejected.Info().session_reused &&
                !(rejected.Info().flags & GM_INFO_CLIENT_CERTIFICATE) &&
                rejected.key_requests == 0,
            "rejected ticket cannot retain prior client authentication");
    config.connection.require_client_auth = 0;
    config.connection.session_blob = Bytes(session);
  }
  GmContext* context = nullptr;
  Require(api.create_context(&config.context, &context) == GM_OK,
          "partition test context");
  const std::string different =
      "network-context=1|nak=other|trust=1|identity=2";
  config.connection.session_binding = Bytes(different);
  GmConnection* rejected = nullptr;
  Require(api.create_connection(context, &config.connection, &rejected) ==
                  GM_ERROR_ARGUMENT &&
              !rejected,
          "cross partition session rejected");
  config.connection.session_binding = Bytes(config.partition);
  config.identity.signing_key_id = 17;
  Require(api.create_connection(context, &config.connection, &rejected) ==
                  GM_ERROR_ARGUMENT &&
              !rejected,
          "cross identity session rejected");
  config.identity.signing_key_id = 7;
  std::vector<unsigned char> broken = session;
  broken.pop_back();
  config.connection.session_blob = Bytes(broken);
  Require(api.create_connection(context, &config.connection, &rejected) ==
                  GM_ERROR_ARGUMENT &&
              !rejected,
          "truncated session envelope rejected");
  Require(api.destroy_context(context) == GM_OK, "partition test cleanup");
  Pass(protocol == GM_PROTOCOL_TLCP
           ? "TLCP authenticated resumption and isolation"
           : "RFC8998 authenticated resumption and isolation");
}

void TestDeferredIdentity(Fixtures& fixtures, uint32_t protocol) {
  Options options;
  options.mutual = true;
  options.partition = "deferred|identity=7|generation=1";
  ClientConfig config(fixtures, protocol, options);
  config.connection.identity = nullptr;
  config.connection.defer_identity = 1;
  config.connection.require_client_auth = 1;
  Server server = ServerContext(fixtures, protocol, options);
  Require(SSL_CTX_add_client_CA(server.get(), fixtures.root.get()) == 1,
          "advertise acceptable client CA");
  std::vector<unsigned char> session;
  {
    Pair pair(fixtures, config, server.get());
    GmConnection* original = pair.client;
    Require(pair.Handshake(false, true) == GM_WANT_IDENTITY &&
                pair.key_requests == 0 && pair.client_hellos == 1,
            "pause one handshake before token authorization");
    GmIdentityRequest request = {};
    request.struct_size = sizeof(request);
    Require(api.get_identity_request(pair.client, &request) == GM_OK &&
                request.ca_name_count == 1 && request.protocol == protocol &&
                request.required_roles ==
                    (GM_IDENTITY_ROLE_SIGNING |
                     (protocol == GM_PROTOCOL_TLCP ? GM_IDENTITY_ROLE_ENCRYPTION
                                                   : 0)),
            "server request exposes CA protocol and roles");
    if (protocol == GM_PROTOCOL_RFC8998) {
      Require(request.signature_algorithm_count > 0,
              "TLS1.3 exposes signature algorithms");
    }
    size_t name_size = 0;
    Require(api.get_identity_request_ca(pair.client, 0, nullptr, 0,
                                        &name_size) == GM_OK &&
                name_size > 0,
            "CA name query");
    std::vector<unsigned char> name(name_size);
    Require(api.get_identity_request_ca(pair.client, 0, name.data(),
                                        name.size(), &name_size) == GM_OK,
            "CA name copy");
    const unsigned char* cursor = name.data();
    X509_NAME* decoded =
        d2i_X509_NAME(nullptr, &cursor, static_cast<long>(name.size()));
    Require(decoded && cursor == name.data() + name.size() &&
                X509_NAME_cmp(decoded,
                              X509_get_subject_name(fixtures.root.get())) == 0,
            "CA name is canonical issuer DER");
    X509_NAME_free(decoded);
    CheckChain(pair, GM_CERTIFICATE_SIGNING, true);
    Require((pair.Info().flags & GM_INFO_ESTABLISHED) == 0 &&
                api.handshake(pair.client) == GM_WANT_IDENTITY,
            "authorization pause does not establish the connection");
    for (int i = 0; i < 3; ++i) {
      Require(pair.Handshake(false, true) == GM_WANT_IDENTITY &&
                  pair.identity_requests == 1 && pair.key_requests == 0 &&
                  pair.client_hellos == 1,
              "identity polls preserve one authenticated handshake request");
    }
    pair.CompleteIdentity();
    Require(pair.Handshake(true) == GM_WANT_KEY,
            "authorized identity reaches asynchronous signing");
    GmKeyRequest key_request = {};
    key_request.struct_size = sizeof(key_request);
    Require(api.get_key_request(pair.client, &key_request) == GM_OK,
            "pending signature snapshot");
    for (int i = 0; i < 3; ++i) {
      GmKeyRequest repeated = {};
      repeated.struct_size = sizeof(repeated);
      Require(api.handshake(pair.client) == GM_WANT_KEY &&
                  api.get_key_request(pair.client, &repeated) == GM_OK &&
                  repeated.request_id == key_request.request_id,
              "signature polls preserve the original signing request");
    }
    pair.CompleteSignature();
    Require(
        pair.Handshake() == GM_OK && pair.client == original &&
            pair.client_hellos == 1 && pair.identity_requests == 1 &&
            pair.key_requests == 1,
        "same SSL and one ClientHello continue into separate async signing");
    pair.Exchange();
    session = pair.Session();
  }
  config.connection.identity = &config.identity;
  config.connection.session_blob = Bytes(session);
  {
    Pair resumed(fixtures, config, server.get());
    Require(resumed.Handshake() == GM_OK && resumed.Info().session_reused &&
                resumed.identity_requests == 0 && resumed.key_requests == 0 &&
                (resumed.Info().flags & GM_INFO_CLIENT_CERTIFICATE),
            "authorized matching identity resumes without CertificateRequest");
  }
  config.connection.session_blob = {};
  config.connection.identity = nullptr;
  {
    Pair cancelled(fixtures, config, server.get());
    Require(cancelled.Handshake(false, true) == GM_WANT_IDENTITY,
            "selection cancellation pause");
    Require(api.cancel(cancelled.client) == GM_OK &&
                api.complete_identity_request(
                    cancelled.client, cancelled.last_identity_request, GM_OK,
                    &config.identity,
                    config.connection.session_binding) == GM_ERROR_CANCELLED &&
                !cancelled.key_requests,
            "cancel selection rejects late result without touching key");
  }
  Options anonymous;
  Server server_only = ServerContext(fixtures, protocol, anonymous);
  {
    Pair required(fixtures, config, server_only.get());
    Require(required.Handshake() == GM_ERROR_CLIENT_AUTH &&
                required.identity_requests == 0 && required.key_requests == 0,
            "explicit identity policy rejects server-only success without "
            "prompting");
  }
  config.connection.require_client_auth = 0;
  {
    Pair ordinary(fixtures, config, server_only.get());
    Require(ordinary.Handshake() == GM_OK && ordinary.identity_requests == 0 &&
                ordinary.key_requests == 0,
            "no CertificateRequest never enumerates or signs");
  }
  {
    Server optional = ServerContext(fixtures, protocol, options);
    SSL_CTX_set_verify(optional.get(), SSL_VERIFY_PEER, nullptr);
    Pair anonymous_client(fixtures, config, optional.get());
    Require(
        anonymous_client.Handshake(false, true) == GM_WANT_IDENTITY &&
            api.complete_identity_request(
                anonymous_client.client, anonymous_client.last_identity_request,
                GM_OK, nullptr, config.connection.session_binding) == GM_OK &&
            anonymous_client.Handshake() == GM_OK &&
            anonymous_client.client_hellos == 1 &&
            anonymous_client.key_requests == 0 &&
            !(anonymous_client.Info().flags & GM_INFO_CLIENT_CERTIFICATE),
        "optional CertificateRequest accepts no identity on same handshake");
    anonymous_client.Exchange();
  }
  config.context.trust_anchors = nullptr;
  config.context.trust_anchor_count = 0;
  {
    Pair untrusted(fixtures, config, server.get());
    Require(
        untrusted.Handshake() == GM_ERROR_CERTIFICATE &&
            untrusted.identity_requests == 0 && untrusted.key_requests == 0 &&
            !(untrusted.Info().flags & GM_INFO_PROTOCOL_RETRY_ALLOWED),
        "empty trust store fails verification before identity authorization");
  }
  config.context.untrusted_intermediates = &config.root;
  config.context.untrusted_intermediate_count = 1;
  {
    Pair untrusted_issuer(fixtures, config, server.get());
    Require(untrusted_issuer.Handshake() == GM_ERROR_CERTIFICATE &&
                untrusted_issuer.identity_requests == 0,
            "an issuer candidate does not become a trust anchor");
  }
  config.context.trust_anchors = &config.root;
  config.context.trust_anchor_count = 1;
  config.context.disallowed_certificates = &config.root;
  config.context.disallowed_certificate_count = 1;
  {
    Pair distrusted(fixtures, config, server.get());
    Require(distrusted.Handshake() == GM_ERROR_CERTIFICATE &&
                distrusted.identity_requests == 0 &&
                !(distrusted.Info().flags & GM_INFO_PROTOCOL_RETRY_ALLOWED),
            "disallowed root rejects the entire chain before authorization");
  }
  Pass(protocol == GM_PROTOCOL_TLCP ? "TLCP same-handshake async identity"
                                    : "RFC8998 same-handshake async identity");
}

void TestProtocolFailureClassification(Fixtures& fixtures, uint32_t protocol) {
  Options options;
  ClientConfig config(fixtures, protocol, options);
  config.connection.defer_identity = 1;
  Server server = ServerContext(fixtures, protocol, options);
  for (uint8_t description : {uint8_t{70}, uint8_t{40}}) {
    Pair pair(fixtures, config, server.get());
    Require(api.handshake(pair.client) == GM_WANT_INPUT,
            "initial ClientHello before protocol rejection");
    const uint8_t major = protocol == GM_PROTOCOL_TLCP ? 1 : 3;
    const uint8_t minor = protocol == GM_PROTOCOL_TLCP ? 1 : 3;
    const uint8_t alert[] = {21, major, minor, 0, 2, 2, description};
    size_t consumed = 0;
    Require(api.feed_ciphertext(pair.client, alert, sizeof(alert), &consumed) ==
                    GM_OK &&
                consumed == sizeof(alert) &&
                api.handshake(pair.client) == GM_ERROR_TLS,
            "peer fatal alert classified");
    Require(((pair.Info().flags & GM_INFO_PROTOCOL_RETRY_ALLOWED) != 0) ==
                (description == 70),
            "only explicit protocol_version permits protocol retry");
  }
  Pass("precise pre-certificate protocol incompatibility");
}

void TestInvalidAlpn(Fixtures& fixtures) {
  Options options;
  options.h2 = true;
  ClientConfig config(fixtures, GM_PROTOCOL_TLCP, options);
  GmContext* context = nullptr;
  Require(api.create_context(&config.context, &context) == GM_OK,
          "ALPN test context");
  GmConnection* connection = nullptr;
  Require(api.create_connection(context, &config.connection, &connection) ==
                  GM_ERROR_ARGUMENT &&
              !connection,
          "TLCP refuses H2");
  Require(api.destroy_context(context) == GM_OK, "ALPN cleanup");
  Pass("TLCP H2 prohibited");
}

using BenchmarkClock = std::chrono::steady_clock;

double MillisecondsSince(BenchmarkClock::time_point start) {
  return std::chrono::duration<double, std::milli>(BenchmarkClock::now() -
                                                   start)
      .count();
}

void PrintLatency(const char* name, std::vector<double> samples) {
  std::sort(samples.begin(), samples.end());
  double sum = 0;
  for (double sample : samples) {
    sum += sample;
  }
  std::printf("\"%s\":{\"mean_ms\":%.4f,\"p50_ms\":%.4f,\"p95_ms\":%.4f}", name,
              sum / samples.size(), samples[samples.size() / 2],
              samples[(samples.size() * 95) / 100]);
}

double ConcurrentHandshakes(Fixtures& fixtures,
                            ClientConfig& config,
                            SSL_CTX* server,
                            GmContext* context,
                            bool resumed) {
  constexpr size_t kConnections = 32;
  std::vector<std::unique_ptr<Pair>> pairs;
  std::array<bool, kConnections> done = {};
  const auto start = BenchmarkClock::now();
  for (size_t i = 0; i < kConnections; ++i) {
    pairs.push_back(
        std::make_unique<Pair>(fixtures, config, server, context, false));
  }
  size_t completed = 0;
  for (int round = 0; round < 256 && completed < kConnections; ++round) {
    std::vector<Pair*> key_waiters;
    for (size_t i = 0; i < kConnections; ++i) {
      if (done[i]) {
        continue;
      }
      Pair& pair = *pairs[i];
      GmStatus status = api.handshake(pair.client);
      if (status == GM_WANT_KEY) {
        ++pair.key_requests;
        key_waiters.push_back(&pair);
        continue;
      }
      Require(status == GM_OK || status == GM_WANT_INPUT,
              "concurrent benchmark handshake");
      pair.ToServer();
      if (!pair.server_done) {
        const int result = SSL_do_handshake(pair.server);
        if (result == 1) {
          pair.server_done = true;
        } else {
          int error = SSL_get_error(pair.server, result);
          Require(error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE,
                  "concurrent peer progress");
        }
      }
      pair.ToClient();
      if (status == GM_OK && pair.server_done) {
        Require(pair.Info().session_reused == static_cast<uint32_t>(resumed) &&
                    pair.key_requests == (resumed ? 0 : 1),
                "concurrent signature count");
        done[i] = true;
        ++completed;
      }
    }
    // Keep every connection's operation pending until all peers have made
    // progress. This exercises 32 independent suspended asynchronous jobs.
    for (Pair* pair : key_waiters) {
      pair->CompleteSignature();
    }
  }
  Require(completed == kConnections, "concurrent benchmark completion");
  return MillisecondsSince(start);
}

double RecordThroughput(Pair& pair) {
  std::array<unsigned char, 16384> payload, received;
  payload.fill(0x5a);
  constexpr int kRecords = 2048;
  const auto start = BenchmarkClock::now();
  for (int record = 0; record < kRecords; ++record) {
    size_t size = 0;
    Require(api.write_plaintext(pair.client, payload.data(), payload.size(),
                                &size) == GM_OK &&
                size == payload.size(),
            "throughput client write");
    pair.ToServer();
    Require(SSL_read_ex(pair.server, received.data(), received.size(), &size) ==
                    1 &&
                size == payload.size() && received == payload,
            "throughput peer read");
    Require(
        SSL_write_ex(pair.server, payload.data(), payload.size(), &size) == 1 &&
            size == payload.size(),
        "throughput peer write");
    pair.ToClient();
    Require(api.read_plaintext(pair.client, received.data(), received.size(),
                               &size) == GM_OK &&
                size == payload.size() && received == payload,
            "throughput client read");
  }
  const double seconds = MillisecondsSince(start) / 1000;
  return 2.0 * kRecords * payload.size() / (1024 * 1024) / seconds;
}

int Benchmark(Fixtures& fixtures,
              int iterations,
              double load_ms,
              uintmax_t backend_bytes) {
  Require(iterations >= 20 && iterations <= 2000,
          "benchmark iteration range 20..2000");
  std::printf(
      "{\n\"method\":\"in-process Tongsuo peer, memory BIO transport, "
      "software SM2 external signer, no USB or IPC latency\",\n"
      "\"iterations\":%d,\"record_bytes\":16384,\"bidirectional_payload_"
      "bytes\":67108864,\n"
      "\"backend_load_ms\":%.4f,\"backend_bytes\":%llu,\n\"protocols\":[\n",
      iterations, load_ms, static_cast<unsigned long long>(backend_bytes));
  bool comma = false;
  for (uint32_t protocol : {GM_PROTOCOL_TLCP, GM_PROTOCOL_RFC8998}) {
    Options options;
    options.mutual = true;
    options.crl_case = 1;
    options.partition = "benchmark-context|partition=1|identity=1";
    ClientConfig config(fixtures, protocol, options);
    Server server = ServerContext(fixtures, protocol, options);
    GmContext* context = nullptr;
    Require(api.create_context(&config.context, &context) == GM_OK,
            "warm benchmark context");
    std::vector<unsigned char> session;
    {
      Pair warmup(fixtures, config, server.get(), context, false);
      Require(warmup.Handshake() == GM_OK, "benchmark warmup");
      warmup.Exchange();  // Process TLS1.3 tickets before session export.
      session = warmup.Session();
    }
    if (comma) {
      std::printf(",\n");
    }
    comma = true;
    std::printf("{\"protocol\":\"%s\",",
                protocol == GM_PROTOCOL_TLCP ? "TLCP" : "RFC8998");
    for (int mode = 0; mode < 3; ++mode) {
      std::vector<double> samples;
      config.connection.session_blob = mode == 2 ? Bytes(session) : GmBytes{};
      for (int i = 0; i < iterations; ++i) {
        const auto start = BenchmarkClock::now();
        Pair pair(fixtures, config, server.get(), mode == 0 ? nullptr : context,
                  false);
        Require(pair.Handshake() == GM_OK &&
                    pair.Info().session_reused ==
                        static_cast<uint32_t>(mode == 2) &&
                    pair.key_requests == (mode == 2 ? 0 : 1),
                "benchmark mTLS handshake");
        samples.push_back(MillisecondsSince(start));
      }
      if (mode) {
        std::printf(",");
      }
      PrintLatency(mode == 0   ? "new_context_full_mtls"
                   : mode == 1 ? "warm_context_full_mtls"
                               : "warm_context_resumed_mtls",
                   std::move(samples));
    }
    config.connection.session_blob = {};
    const double concurrent_full =
        ConcurrentHandshakes(fixtures, config, server.get(), context, false);
    config.connection.session_blob = Bytes(session);
    const double concurrent_resumed =
        ConcurrentHandshakes(fixtures, config, server.get(), context, true);
    config.connection.session_blob = {};
    double throughput = 0;
    {
      Pair pair(fixtures, config, server.get(), context, false);
      Require(pair.Handshake() == GM_OK, "throughput handshake");
      throughput = RecordThroughput(pair);
    }
    Require(api.destroy_context(context) == GM_OK, "benchmark context cleanup");
    options.mutual = false;
    ClientConfig single_config(fixtures, protocol, options);
    Server single_server = ServerContext(fixtures, protocol, options);
    Require(api.create_context(&single_config.context, &context) == GM_OK,
            "server-only benchmark context");
    std::vector<double> samples;
    for (int i = 0; i < iterations; ++i) {
      const auto start = BenchmarkClock::now();
      Pair pair(fixtures, single_config, single_server.get(), context, false);
      Require(pair.Handshake() == GM_OK && pair.key_requests == 0,
              "server-only benchmark handshake");
      samples.push_back(MillisecondsSince(start));
    }
    Require(api.destroy_context(context) == GM_OK,
            "server-only benchmark cleanup");
    std::printf(",");
    PrintLatency("warm_context_full_server_auth", std::move(samples));
    std::printf(
        ",\"concurrent_connections\":32,\"concurrent_full_mtls_ms\":%.4f,"
        "\"concurrent_resumed_mtls_ms\":%.4f,\"bidirectional_record_payload_"
        "mib_s\":%.3f}",
        concurrent_full, concurrent_resumed, throughput);
  }
  std::printf("\n]}\n");
  return 0;
}

void WriteBytes(const std::filesystem::path& path,
                const std::vector<unsigned char>& bytes) {
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  Require(output.good(), "write test fixture");
}

void WritePem(const std::filesystem::path& path,
              X509* certificate,
              EVP_PKEY* key) {
  BIO* bio = BIO_new(BIO_s_mem());
  Require(
      bio && (certificate ? PEM_write_bio_X509(bio, certificate)
                          : PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr,
                                                     0, nullptr, nullptr)),
      "encode PEM fixture");
  char* data = nullptr;
  long size = BIO_get_mem_data(bio, &data);
  std::ofstream output(path, std::ios::binary);
  output.write(data, size);
  Require(output.good(), "write PEM fixture");
  BIO_free(bio);
}

void ExportFixtures(Fixtures& fixtures,
                    const std::filesystem::path& directory) {
  std::filesystem::create_directories(directory);
  WriteBytes(directory / "server-root.der", fixtures.root_der);
  WriteBytes(directory / "server-crl.der",
             CrlDer(fixtures.root.get(), fixtures.root_key.get()));
  WriteBytes(directory / "server-sign.der", Der(fixtures.server.get()));
  WriteBytes(directory / "server-enc.der", Der(fixtures.server_enc.get()));
  WritePem(directory / "server-root.pem", fixtures.root.get(), nullptr);
  WritePem(directory / "server-sign.pem", fixtures.server.get(), nullptr);
  WritePem(directory / "server-sign-key.pem", nullptr,
           fixtures.server_key.get());
  WritePem(directory / "server-enc.pem", fixtures.server_enc.get(), nullptr);
  WritePem(directory / "server-enc-key.pem", nullptr,
           fixtures.server_enc_key.get());
  WriteBytes(directory / "client-root.der", fixtures.root_der);
  WritePem(directory / "client-sign.pem", fixtures.client.get(), nullptr);
  WritePem(directory / "client-sign-key.pem", nullptr,
           fixtures.client_key.get());
  WritePem(directory / "client-enc.pem", fixtures.client_enc.get(), nullptr);
  WritePem(directory / "client-enc-key.pem", nullptr,
           fixtures.client_enc_key.get());
}

bool SslReadExact(SSL* ssl, void* output, size_t size) {
  auto* bytes = static_cast<unsigned char*>(output);
  while (size) {
    const int count = SSL_read(ssl, bytes, static_cast<int>(size));
    if (count <= 0) {
      return false;
    }
    bytes += count;
    size -= count;
  }
  return true;
}

std::string HttpBody(uint64_t connection_id = 0, int client_hellos = 1) {
  return "<!doctype html><meta charset=utf-8><title>GM TLS integration</title>"
         "<h1 id=gm-result data-connection-id=" +
         std::to_string(connection_id) +
         " data-client-hellos=" + std::to_string(client_hellos) +
         ">GM TLS connection verified</h1>";
}

std::vector<unsigned char> H2Frame(unsigned char type,
                                   unsigned char flags,
                                   uint32_t stream,
                                   const std::string& payload) {
  Require(payload.size() <= 16384 && stream <= 0x7fffffffu,
          "bounded HTTP2 test frame");
  const size_t size = payload.size();
  std::vector<unsigned char> frame = {static_cast<unsigned char>(size >> 16),
                                      static_cast<unsigned char>(size >> 8),
                                      static_cast<unsigned char>(size),
                                      type,
                                      flags,
                                      static_cast<unsigned char>(stream >> 24),
                                      static_cast<unsigned char>(stream >> 16),
                                      static_cast<unsigned char>(stream >> 8),
                                      static_cast<unsigned char>(stream)};
  frame.insert(frame.end(), payload.begin(), payload.end());
  return frame;
}

struct H2Header {
  size_t size;
  unsigned char type;
  unsigned char flags;
  uint32_t stream;
};

H2Header ParseH2Header(const std::array<unsigned char, 9>& header) {
  return {static_cast<size_t>(header[0]) << 16 |
              static_cast<size_t>(header[1]) << 8 | header[2],
          header[3], header[4],
          (static_cast<uint32_t>(header[5] & 0x7f) << 24) |
              (static_cast<uint32_t>(header[6]) << 16) |
              (static_cast<uint32_t>(header[7]) << 8) | header[8]};
}

bool SslSendH2(SSL* ssl,
               unsigned char type,
               unsigned char flags,
               uint32_t stream,
               const std::string& payload = {}) {
  const auto frame = H2Frame(type, flags, stream, payload);
  return SSL_write(ssl, frame.data(), static_cast<int>(frame.size())) ==
         static_cast<int>(frame.size());
}

// A bounded test-only static-resource HTTP/2 peer, not a general HTTP server.
// It consumes real preface/SETTINGS/HEADERS/CONTINUATION frames and emits HPACK
// headers and DATA on the requested streams. Request HPACK values are ignored:
// every path serves the same small fixture and no application input is used.
bool HandleH2Client(SSL* ssl, const std::string& body) {
  constexpr char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  std::array<char, sizeof(preface) - 1> received;
  if (!SslReadExact(ssl, received.data(), received.size()) ||
      std::memcmp(received.data(), preface, received.size()) ||
      !SslSendH2(ssl, 4, 0, 0)) {
    return false;
  }
  uint32_t continuation = 0;
  uint32_t last_stream = 0;
  bool ended = false;
  size_t header_bytes = 0;
  for (int frames = 0; frames < 512; ++frames) {
    std::array<unsigned char, 9> bytes;
    if (!SslReadExact(ssl, bytes.data(), bytes.size())) {
      return last_stream != 0;
    }
    H2Header header = ParseH2Header(bytes);
    if (header.size > 16384 ||
        (continuation && (header.type != 9 || header.stream != continuation))) {
      return false;
    }
    std::string payload(header.size, '\0');
    if (!SslReadExact(ssl, payload.data(), payload.size())) {
      return false;
    }
    switch (header.type) {
      case 4:  // SETTINGS, including the peer's acknowledgement.
        if (header.stream || header.size % 6 ||
            ((header.flags & 1) && header.size)) {
          return false;
        }
        if (!(header.flags & 1) && !SslSendH2(ssl, 4, 1, 0)) {
          return false;
        }
        break;
      case 6:  // PING.
        if (header.stream || header.size != 8) {
          return false;
        }
        if (!(header.flags & 1) && !SslSendH2(ssl, 6, 1, 0, payload)) {
          return false;
        }
        break;
      case 8:  // WINDOW_UPDATE; the tiny response never exhausts either window.
        if (header.size != 4) {
          return false;
        }
        break;
      case 2:  // PRIORITY.
        if (!header.stream || header.size != 5) {
          return false;
        }
        break;
      case 3:  // RST_STREAM.
        if (!header.stream || header.size != 4) {
          return false;
        }
        break;
      case 7:  // GOAWAY.
        return !header.stream && header.size >= 8;
      case 1:  // HEADERS. Test resource requests must have no request body.
        if (!(header.stream & 1) || header.stream <= last_stream ||
            !(header.flags & 1) || header.stream > 127) {
          return false;
        }
        last_stream = header.stream;
        header_bytes = header.size;
        ended = (header.flags & 1) != 0;
        continuation = (header.flags & 4) ? 0 : header.stream;
        if (continuation) {
          break;
        }
        [[fallthrough]];
      case 9: {  // CONTINUATION.
        if (header.type == 9) {
          if (!continuation || header.stream != continuation) {
            return false;
          }
          header_bytes += header.size;
          if (header_bytes > 65536) {
            return false;
          }
          if (!(header.flags & 4)) {
            break;
          }
          continuation = 0;
        }
        if (!ended) {
          return false;
        }
        // HPACK static table index 8 (:status: 200), followed by a literal
        // without indexing using name index 31 (content-type).
        const std::string headers =
            std::string("\x88\x0f\x10\x09", 4) + "text/html";
        if (!SslSendH2(ssl, 1, 4, header.stream, headers) ||
            !SslSendH2(ssl, 0, 1, header.stream, body)) {
          return false;
        }
        break;
      }
      default:
        // Unknown extension frames are ignored, as required by HTTP/2.
        if (header.type == 0) {
          return false;
        }
        break;
    }
  }
  return false;
}

std::string TestHeader(const std::string& headers, const std::string& name) {
  size_t cursor = headers.find("\r\n");
  while (cursor != std::string::npos) {
    cursor += 2;
    const size_t end = headers.find("\r\n", cursor);
    const size_t colon = headers.find(':', cursor);
    if (colon != std::string::npos && colon < end &&
        colon - cursor == name.size()) {
      bool match = true;
      for (size_t i = 0; i < name.size(); ++i) {
        const char value = headers[cursor + i];
        match &= (value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value) ==
                 name[i];
      }
      if (match) {
        size_t start = colon + 1;
        while (start < end && headers[start] == ' ') {
          ++start;
        }
        return headers.substr(start, end - start);
      }
    }
    cursor = end;
  }
  return {};
}

std::string TestRedirect(const std::string& headers) {
  constexpr std::string_view prefix = "GET /redirect?url=";
  if (!headers.starts_with(prefix)) {
    return {};
  }
  const size_t end = headers.find(' ', prefix.size());
  if (end == std::string::npos) {
    return {};
  }
  const auto hex = [](char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  std::string target;
  for (size_t i = prefix.size(); i < end; ++i) {
    char value = headers[i];
    if (value == '%') {
      if (i + 2 >= end || hex(headers[i + 1]) < 0 || hex(headers[i + 2]) < 0) {
        return {};
      }
      value =
          static_cast<char>((hex(headers[i + 1]) << 4) | hex(headers[i + 2]));
      i += 2;
    }
    if (value == '\r' || value == '\n' || value == '\0') {
      return {};
    }
    target.push_back(value);
  }
  return target.starts_with("https://") || target.starts_with("http://")
             ? target
             : std::string();
}

bool ServeTestWebSocket(SSL* ssl, const std::string& key) {
  if (key.size() != 24) {
    return false;
  }
  const std::string challenge = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  std::array<unsigned char, 20> digest;
  unsigned int size = 0;
  if (!EVP_Digest(challenge.data(), challenge.size(), digest.data(), &size,
                  EVP_sha1(), nullptr) ||
      size != digest.size()) {
    return false;
  }
  std::array<unsigned char, 29> encoded;
  if (EVP_EncodeBlock(encoded.data(), digest.data(),
                      static_cast<int>(digest.size())) != 28) {
    return false;
  }
  const std::string response =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
      std::string(reinterpret_cast<const char*>(encoded.data()), 28) +
      "\r\n\r\n";
  if (SSL_write(ssl, response.data(), static_cast<int>(response.size())) !=
      static_cast<int>(response.size())) {
    return false;
  }
  // A bounded RFC6455 echo fixture: complete masked text frames up to 125
  // bytes.
  for (int frame = 0; frame < 16; ++frame) {
    std::array<unsigned char, 2> header;
    if (!SslReadExact(ssl, header.data(), header.size())) {
      return true;
    }
    const size_t length = header[1] & 127;
    if ((header[0] & 15) == 8) {
      return true;
    }
    if (header[0] != 0x81 || !(header[1] & 128) || length > 125) {
      return false;
    }
    std::array<unsigned char, 4> mask;
    std::vector<unsigned char> reply(length + 2);
    if (!SslReadExact(ssl, mask.data(), mask.size()) ||
        !SslReadExact(ssl, reply.data() + 2, length)) {
      return false;
    }
    reply[0] = 0x81;
    reply[1] = static_cast<unsigned char>(length);
    for (size_t i = 0; i < length; ++i) {
      reply[i + 2] ^= mask[i % 4];
    }
    if (SSL_write(ssl, reply.data(), static_cast<int>(reply.size())) !=
        static_cast<int>(reply.size())) {
      return false;
    }
  }
  return true;
}

struct PeerHandshake {
  const std::filesystem::path* directory;
  uint64_t connection_id;
  int client_hellos = 0;
};

bool HandleHttpClient(SSL_CTX* context,
                      gm_test::Socket socket,
                      const std::filesystem::path* directory = nullptr) {
  static std::atomic<uint64_t> next_connection_id{0};
  PeerHandshake handshake{directory, directory ? ++next_connection_id : 0};
  if (directory) {
    std::ofstream accepted(
        *directory /
        ("accepted-connection-" + std::to_string(handshake.connection_id)));
    Require(accepted.good(), "record accepted test connection");
  }
  gm_test::SetSocketTimeout(socket, 30000);
  SSL* ssl = SSL_new(context);
  if (ssl) {
    SSL_set_msg_callback(ssl, [](int write, int, int type, const void* bytes,
                                 size_t length, SSL*, void* argument) {
      if (type != SSL3_RT_HANDSHAKE || !length) {
        return;
      }
      auto& state = *static_cast<PeerHandshake*>(argument);
      const auto message = *static_cast<const unsigned char*>(bytes);
      if (!write && message == SSL3_MT_CLIENT_HELLO) {
        ++state.client_hellos;
        if (state.directory && length >= 6) {
          const auto* hello = static_cast<const unsigned char*>(bytes);
          const bool tlcp = hello[4] == 1 && hello[5] == 1;
          std::ofstream marker(
              *state.directory /
              (std::string(tlcp ? "tlcp-client-hello-" : "tls-client-hello-") +
               std::to_string(state.connection_id)));
          Require(marker.good(), "record received test ClientHello");
        }
      }
      if (write && message == SSL3_MT_CERTIFICATE_REQUEST && state.directory) {
        // The browser records these connection IDs before accepting its
        // PIN prompt, then checks that its response uses that same SSL.
        std::ofstream marker(
            *state.directory /
            ("identity-request-" + std::to_string(state.connection_id)));
        Require(marker.good(), "publish pending handshake connection");
      }
    });
    SSL_set_msg_callback_arg(ssl, &handshake);
  }
  if (!ssl || socket > static_cast<gm_test::Socket>(std::numeric_limits<int>::max()) ||
      !SSL_set_fd(ssl, static_cast<int>(socket)) || SSL_accept(ssl) != 1) {
    SSL_free(ssl);
    gm_test::CloseSocket(socket);
    return false;
  }
  const unsigned char* alpn = nullptr;
  unsigned int alpn_size = 0;
  SSL_get0_alpn_selected(ssl, &alpn, &alpn_size);
  const std::string body =
      HttpBody(handshake.connection_id, handshake.client_hellos);
  if (alpn_size == 2 && !std::memcmp(alpn, "h2", 2)) {
    const bool ok = HandleH2Client(ssl, body);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    gm_test::CloseSocket(socket);
    return ok;
  }
  bool served = false;
  std::string pending;
  std::array<char, 8192> buffer;
  for (int request = 0; request < 64; ++request) {
    while (pending.find("\r\n\r\n") == std::string::npos &&
           pending.size() < 65536) {
      int received =
          SSL_read(ssl, buffer.data(), static_cast<int>(buffer.size()));
      if (received <= 0) {
        goto done;
      }
      pending.append(buffer.data(), received);
    }
    const size_t end = pending.find("\r\n\r\n");
    if (end == std::string::npos) {
      break;
    }
    const std::string headers = pending.substr(0, end + 2);
    const bool close = TestHeader(headers, "connection") == "close";
    pending.erase(0, end + 4);
    const std::string websocket = TestHeader(headers, "sec-websocket-key");
    if (!websocket.empty()) {
      served = ServeTestWebSocket(ssl, websocket);
      break;
    }
    const std::string redirect = TestRedirect(headers);
    if (!redirect.empty()) {
      const std::string response =
          "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n"
          "Cache-Control: no-store\r\nConnection: close\r\nLocation: " +
          redirect + "\r\n\r\n";
      served =
          SSL_write(ssl, response.data(), static_cast<int>(response.size())) ==
          static_cast<int>(response.size());
      break;
    }
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\nContent-Length: " +
        std::to_string(body.size()) +
        "\r\nConnection: " + (close ? "close" : "keep-alive") + "\r\n\r\n" +
        body;
    if (SSL_write(ssl, response.data(), static_cast<int>(response.size())) !=
        static_cast<int>(response.size())) {
      break;
    }
    served = true;
    if (close) {
      break;
    }
  }
done:
  SSL_shutdown(ssl);
  SSL_free(ssl);
  gm_test::CloseSocket(socket);
  return served;
}

class TcpClient {
 public:
  TcpClient(Fixtures& fixtures, ClientConfig& config, u_short port)
      : fixtures_(fixtures) {
    Require(api.create_context(&config.context, &context_) == GM_OK &&
                api.create_connection(context_, &config.connection,
                                      &connection_) == GM_OK,
            "TCP backend connection");
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    Require(socket_ != gm_test::kInvalidSocket, "TCP client socket");
    gm_test::SetSocketTimeout(socket_, 10000);
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Require(connect(socket_, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0,
            "TCP loopback connection");
  }
  ~TcpClient() {
    gm_test::CloseSocket(socket_);
    Require(api.destroy_connection(connection_) == GM_OK &&
                api.destroy_context(context_) == GM_OK,
            "TCP backend cleanup");
  }
  void Handshake(bool mutual, bool h2) {
    int signatures = 0;
    for (int steps = 0; steps < 128; ++steps) {
      const GmStatus status = api.handshake(connection_);
      Flush();
      if (status == GM_OK) {
        GmConnectionInfo info = {};
        info.struct_size = sizeof(info);
        Require(api.get_connection_info(connection_, &info) == GM_OK &&
                    (info.flags & GM_INFO_ESTABLISHED) &&
                    signatures == (mutual ? 1 : 0) &&
                    static_cast<bool>(info.flags &
                                      GM_INFO_CLIENT_CERTIFICATE) == mutual &&
                    (!h2 ||
                     (info.alpn_size == 2 && !std::memcmp(info.alpn, "h2", 2))),
                "real TCP authenticated TLS and ALPN");
        return;
      }
      if (status == GM_WANT_KEY) {
        ++signatures;
        GmKeyRequest request = {};
        request.struct_size = sizeof(request);
        Require(api.get_key_request(connection_, &request) == GM_OK,
                "TCP external signing request");
        EVP_PKEY_CTX* signer =
            EVP_PKEY_CTX_new(fixtures_.client_key.get(), nullptr);
        unsigned char signature[72];
        size_t size = sizeof(signature);
        Require(signer && EVP_PKEY_sign_init(signer) > 0 &&
                    EVP_PKEY_CTX_set_signature_md(signer, EVP_sm3()) > 0 &&
                    EVP_PKEY_sign(signer, signature, &size, request.digest,
                                  sizeof(request.digest)) > 0 &&
                    api.complete_key_request(connection_, request.request_id,
                                             GM_OK, signature, size) == GM_OK,
                "TCP simulated Key signature");
        EVP_PKEY_CTX_free(signer);
      } else {
        Require(status == GM_WANT_INPUT, "TCP handshake progression");
        Receive();
      }
    }
    Require(false, "TCP handshake bounded progress");
  }
  void Write(const void* data, size_t size) {
    size_t written = 0;
    Require(api.write_plaintext(connection_, static_cast<const uint8_t*>(data),
                                size, &written) == GM_OK &&
                written == size,
            "TCP application write");
    Flush();
  }
  size_t Read(void* data, size_t capacity) {
    for (;;) {
      size_t size = 0;
      const GmStatus status = api.read_plaintext(
          connection_, static_cast<uint8_t*>(data), capacity, &size);
      Flush();
      if (status == GM_OK) {
        return size;
      }
      if (status == GM_CLOSED) {
        return 0;
      }
      Require(status == GM_WANT_INPUT, "TCP application read");
      Receive();
    }
  }
  void ReadExact(void* data, size_t size) {
    auto* bytes = static_cast<unsigned char*>(data);
    while (size) {
      size_t received = Read(bytes, size);
      Require(received != 0, "complete HTTP2 TCP frame");
      bytes += received;
      size -= received;
    }
  }

 private:
  void Flush() {
    std::array<unsigned char, 16384> bytes;
    for (;;) {
      size_t size = 0;
      Require(api.drain_ciphertext(connection_, bytes.data(), bytes.size(),
                                   &size) == GM_OK,
              "TCP output drain");
      if (!size) {
        return;
      }
      size_t sent = 0;
      while (sent < size) {
        const int count =
            send(socket_, reinterpret_cast<const char*>(bytes.data() + sent),
                 static_cast<int>(size - sent), 0);
        Require(count > 0, "TCP ciphertext send");
        sent += count;
      }
    }
  }
  void Receive() {
    std::array<unsigned char, 16384> bytes;
    const int size = recv(socket_, reinterpret_cast<char*>(bytes.data()),
                          static_cast<int>(bytes.size()), 0);
    Require(size >= 0, "TCP ciphertext receive");
    if (!size) {
      Require(api.notify_transport_eof(connection_) == GM_OK, "TCP EOF");
      return;
    }
    size_t consumed = 0;
    Require(api.feed_ciphertext(connection_, bytes.data(), size, &consumed) ==
                    GM_OK &&
                consumed == static_cast<size_t>(size),
            "TCP input feed");
  }
  Fixtures& fixtures_;
  GmContext* context_ = nullptr;
  GmConnection* connection_ = nullptr;
  gm_test::Socket socket_ = gm_test::kInvalidSocket;
};

void FetchHttp(TcpClient& client, bool h2) {
  if (!h2) {
    const std::string request =
        "GET / HTTP/1.1\r\nHost: gm.test\r\nConnection: close\r\n\r\n";
    client.Write(request.data(), request.size());
    std::string response;
    std::array<char, 4096> bytes;
    for (;;) {
      size_t size = client.Read(bytes.data(), bytes.size());
      if (!size) {
        break;
      }
      response.append(bytes.data(), size);
      Require(response.size() < 1024 * 1024, "bounded HTTP1 response");
    }
    Require(
        response.starts_with("HTTP/1.1 200 OK\r\n") &&
            response.find("GM TLS connection verified") != std::string::npos,
        "real TCP HTTP1 response body");
    return;
  }
  constexpr char preface[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
  client.Write(preface, sizeof(preface) - 1);
  auto settings = H2Frame(4, 0, 0, {});
  client.Write(settings.data(), settings.size());
  // Indexed method GET, scheme HTTPS, path /, literal authority gm.test.
  const std::string headers =
      std::string("\x82\x87\x84\x01\x07", 5) + "gm.test";
  for (uint32_t stream : {1u, 3u}) {
    auto frame = H2Frame(1, 5, stream, headers);
    client.Write(frame.data(), frame.size());
  }
  std::array<std::string, 2> body;
  std::array<bool, 2> saw_headers = {}, ended = {};
  bool saw_settings = false, settings_ack = false;
  for (int frame_count = 0; frame_count < 64 && !(ended[0] && ended[1]);
       ++frame_count) {
    std::array<unsigned char, 9> bytes;
    client.ReadExact(bytes.data(), bytes.size());
    const H2Header header = ParseH2Header(bytes);
    Require(header.size <= 16384, "bounded incoming HTTP2 frame");
    std::string payload(header.size, '\0');
    client.ReadExact(payload.data(), payload.size());
    if (header.type == 4) {
      Require(!header.stream && !(header.size % 6), "HTTP2 peer SETTINGS");
      if (header.flags & 1) {
        settings_ack = true;
      } else {
        saw_settings = true;
        auto ack = H2Frame(4, 1, 0, {});
        client.Write(ack.data(), ack.size());
      }
      continue;
    }
    Require(header.stream == 1 || header.stream == 3, "HTTP2 response stream");
    const size_t index = header.stream / 2;
    if (header.type == 1) {
      Require(!saw_headers[index] && (header.flags & 4) && !payload.empty() &&
                  static_cast<unsigned char>(payload[0]) == 0x88,
              "HTTP2 HPACK status 200");
      saw_headers[index] = true;
    } else {
      Require(header.type == 0 && saw_headers[index] && !ended[index],
              "HTTP2 DATA sequence");
      body[index] += payload;
      ended[index] = (header.flags & 1) != 0;
    }
  }
  Require(saw_settings && settings_ack && ended[0] && ended[1] &&
              body[0] == HttpBody() && body[1] == HttpBody(),
          "real TCP HTTP2 settings and concurrent stream responses");
  auto goaway = H2Frame(7, 0, 0, std::string(8, '\0'));
  client.Write(goaway.data(), goaway.size());
}

void TestTcp(Fixtures& fixtures, uint32_t protocol, bool mutual, bool h2) {
  Require(gm_test::InitializeSockets(),
          "TCP test socket initialization");
  gm_test::Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  gm_test::SocketLength length = sizeof(address);
  Require(listener != gm_test::kInvalidSocket &&
              bind(listener, reinterpret_cast<sockaddr*>(&address),
                   sizeof(address)) == 0 &&
              listen(listener, 1) == 0 &&
              getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                          &length) == 0,
          "ephemeral loopback test listener");
  Options options;
  options.mutual = mutual;
  options.h2 = h2;
  ClientConfig config(fixtures, protocol, options);
  Server server = ServerContext(fixtures, protocol, options);
  bool served = false;
  std::thread peer([&] {
    gm_test::Socket accepted = accept(listener, nullptr, nullptr);
    Require(accepted != gm_test::kInvalidSocket, "TCP peer accept");
    served = HandleHttpClient(server.get(), accepted);
  });
  {
    TcpClient client(fixtures, config, ntohs(address.sin_port));
    client.Handshake(mutual, h2);
    FetchHttp(client, h2);
  }
  peer.join();
  Require(served, "TCP peer served complete response");
  gm_test::CloseSocket(listener);
  gm_test::CleanupSockets();
  Pass(std::string(protocol == GM_PROTOCOL_TLCP ? "TLCP" : "RFC8998") +
       (h2 ? " real TCP H2 multiplexing" : " real TCP H1") +
       (mutual ? " mutual" : " server auth"));
}

int Serve(int argc, char** argv) {
  const bool late_client_root =
      argc == 7 && std::strcmp(argv[6], "--wait-client-root") == 0;
  Require(argc == 5 || argc == 6 || late_client_root,
          "usage: gm_backend_smoke.exe --serve <port> "
          "<tlcp|tlcp-version-alert|rfc8998|rfc8998-h2> <fixture_dir> "
          "[client_root.der [--wait-client-root]]");
  const long port = std::strtol(argv[2], nullptr, 10);
  Require(port >= 0 && port <= 65535, "server port");
  const std::string protocol_name = argv[3];
  Require(protocol_name == "tlcp" || protocol_name == "tlcp-version-alert" ||
              protocol_name == "rfc8998" || protocol_name == "rfc8998-h2",
          "server protocol");
  const uint32_t protocol = protocol_name.starts_with("tlcp")
                                ? GM_PROTOCOL_TLCP
                                : GM_PROTOCOL_RFC8998;
  const std::filesystem::path directory(argv[4]);
  Require(directory.is_absolute(), "absolute fixture directory");
  Fixtures fixtures;
  ExportFixtures(fixtures, directory);
  Options options;
  options.mutual = argc >= 6;
  options.h2 = protocol_name == "rfc8998-h2";
  Server context = ServerContext(fixtures, protocol, options);
  if (protocol_name == "tlcp-version-alert") {
    // A test-only TLCP endpoint that explicitly rejects TLS ClientHello by
    // protocol version. The default Tongsuo peer instead sends a generic
    // handshake_failure, which production automatic discovery must reject.
    SSL_CTX_set_client_hello_cb(
        context.get(),
        [](SSL* ssl, int* alert, void*) {
          if (SSL_client_hello_get0_legacy_version(ssl) != NTLS_VERSION) {
            *alert = SSL_AD_PROTOCOL_VERSION;
            return SSL_CLIENT_HELLO_ERROR;
          }
          return SSL_CLIENT_HELLO_SUCCESS;
        },
        nullptr);
  }
  auto load_client_root = [&] {
    std::ifstream input(argv[5], std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)),
                                     {});
    const unsigned char* cursor = bytes.data();
    X509* root = d2i_X509(nullptr, &cursor, static_cast<long>(bytes.size()));
    Require(
        root && cursor == bytes.data() + bytes.size() &&
            X509_STORE_add_cert(SSL_CTX_get_cert_store(context.get()), root),
        "client root fixture");
    X509_free(root);
  };
  if (options.mutual && !late_client_root) {
    load_client_root();
  }
  if (late_client_root) {
    // The simulator creates a fresh CA only when the real KeyManager first
    // configures its process after CertificateRequest. The browser publishes
    // that CA atomically before completing the PIN prompt. Load it when the
    // resulting client certificate arrives, without delaying ClientHello or
    // changing the shared SSL_CTX trust store from concurrent connections.
    SSL_CTX_set_cert_verify_callback(
        context.get(),
        [](X509_STORE_CTX* verification, void* argument) {
          std::ifstream input(static_cast<const char*>(argument),
                              std::ios::binary);
          std::vector<unsigned char> bytes(
              (std::istreambuf_iterator<char>(input)), {});
          const unsigned char* cursor = bytes.data();
          X509* root =
              d2i_X509(nullptr, &cursor, static_cast<long>(bytes.size()));
          STACK_OF(X509)* trusted = sk_X509_new_null();
          if (!root || cursor != bytes.data() + bytes.size() || !trusted ||
              !sk_X509_push(trusted, root)) {
            X509_free(root);
            sk_X509_free(trusted);
            X509_STORE_CTX_set_error(verification,
                                     X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT);
            return 0;
          }
          X509_STORE_CTX_set0_trusted_stack(verification, trusted);
          const int result = X509_verify_cert(verification);
          X509_STORE_CTX_set0_trusted_stack(verification, nullptr);
          sk_X509_pop_free(trusted, X509_free);
          return result;
        },
        argv[5]);
  }
  Require(gm_test::InitializeSockets(), "socket initialization");
  gm_test::Socket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  Require(listener != gm_test::kInvalidSocket, "listening socket");
  Require(gm_test::SetExclusiveListener(listener),
          "exclusive test listener");
  sockaddr_in address = {};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  Require(bind(listener, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) == 0 &&
              listen(listener, 8) == 0,
          "loopback listener bind");
  gm_test::SocketLength address_size = sizeof(address);
  Require(getsockname(listener, reinterpret_cast<sockaddr*>(&address),
                      &address_size) == 0,
          "actual listening port");
  const unsigned int actual_port = ntohs(address.sin_port);
  // Publish readiness only after fixture export and successful listen. Rename
  // prevents a polling test process from observing a partially written port.
  {
    std::ofstream ready(directory / "ready.tmp");
    ready << actual_port << '\n';
    Require(ready.good(), "test peer readiness file");
  }
  std::filesystem::rename(directory / "ready.tmp", directory / "ready");
  std::printf("READY %s 127.0.0.1:%u; create fixture_dir/stop to exit\n",
              protocol_name.c_str(), actual_port);
  std::fflush(stdout);
  std::vector<std::thread> clients;
  while (!std::filesystem::exists(directory / "stop")) {
    fd_set sockets;
    FD_ZERO(&sockets);
    FD_SET(listener, &sockets);
    timeval wait = {0, 250000};
    const int ready = gm_test::WaitReadable(listener, &sockets, &wait);
    if (ready < 0) {
      break;
    }
    if (ready == 0) {
      continue;
    }
    gm_test::Socket peer = accept(listener, nullptr, nullptr);
    if (peer == gm_test::kInvalidSocket) {
      continue;
    }
    if (clients.size() >= 128) {
      gm_test::CloseSocket(peer);
      break;
    }
    clients.emplace_back(HandleHttpClient, context.get(), peer, &directory);
  }
  gm_test::CloseSocket(listener);
  for (auto& client : clients) {
    client.join();
  }
  gm_test::CleanupSockets();
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Require(OPENSSL_init_ssl(OPENSSL_INIT_NO_LOAD_CONFIG, nullptr),
          "test crypto init");
  if (argc > 1 && std::strcmp(argv[1], "--serve") == 0) {
    return Serve(argc, argv);
  }
  if (argc == 3 && (std::strcmp(argv[1], "--export-fixtures") == 0 ||
                    std::strcmp(argv[1], "--export-interop-fixtures") == 0)) {
    Fixtures fixtures;
    if (std::strcmp(argv[1], "--export-interop-fixtures") == 0) {
      // GmSSL 3.2.0 rejects the standard empty DER sequence for CA:FALSE.
      // End-entity BasicConstraints is optional under RFC5280; omit it in the
      // cross-library fixture without changing either implementation's checks.
      for (X509* cert : {fixtures.client.get(), fixtures.client_enc.get()}) {
        const int index = X509_get_ext_by_NID(cert, NID_basic_constraints, -1);
        Require(index >= 0, "interop fixture BasicConstraints");
        X509_EXTENSION_free(X509_delete_ext(cert, index));
        Require(X509_sign(cert, fixtures.root_key.get(), EVP_sm3()) > 0,
                "re-sign interoperable end-entity fixture");
      }
    }
    Require(std::filesystem::path(argv[2]).is_absolute(),
            "absolute fixture directory");
    ExportFixtures(fixtures, argv[2]);
    return 0;
  }
  const bool fetch = argc > 1 && std::strcmp(argv[1], "--fetch") == 0;
  const bool benchmark = argc > 1 && std::strcmp(argv[1], "--benchmark") == 0;
  Require(fetch       ? argc == 6 || argc == 7
          : benchmark ? argc == 3 || argc == 4
                      : argc == 2 || argc == 3,
          "usage: gm_backend_smoke.exe <absolute backend.dll> [root.der]; or "
          "--fetch <backend.dll> <port> <tlcp|rfc8998|rfc8998-h2> "
          "<server-root.der> [client_fixture_dir]");
  const char* backend_path = argv[fetch || benchmark ? 2 : 1];
  const auto load_start = BenchmarkClock::now();
  gm_test::Module module = gm_test::LoadModule(backend_path);
  Require(module != nullptr, "load isolated backend");
  auto get_api = reinterpret_cast<GmGetBackendApiFn>(
      gm_test::FindFunction(module, "gm_backend_get_api"));
  api.struct_size = sizeof(api);
  Require(get_api && get_api(GM_BACKEND_ABI_VERSION, &api) == GM_OK,
          "ABI negotiation");
  const double load_ms = MillisecondsSince(load_start);
  Fixtures fixtures;
  if (benchmark) {
    return Benchmark(fixtures, argc == 4 ? std::atoi(argv[3]) : 100, load_ms,
                     std::filesystem::file_size(backend_path));
  }
  if (fetch) {
    const long port = std::strtol(argv[3], nullptr, 10);
    const std::string protocol_name = argv[4];
    Require(port > 0 && port <= 65535 &&
                (protocol_name == "tlcp" || protocol_name == "rfc8998" ||
                 protocol_name == "rfc8998-h2"),
            "external test peer endpoint");
    std::ifstream input(argv[5], std::ios::binary);
    std::vector<unsigned char> root((std::istreambuf_iterator<char>(input)),
                                    {});
    Require(!root.empty(), "external test trust root");
    Options options;
    options.h2 = protocol_name == "rfc8998-h2";
    options.mutual = argc == 7;
    if (options.mutual) {
      const std::filesystem::path directory(argv[6]);
      auto read_certificate = [&](const char* name) {
        BIO* bio = BIO_new_file((directory / name).string().c_str(), "rb");
        X509* cert =
            bio ? PEM_read_bio_X509(bio, nullptr, nullptr, nullptr) : nullptr;
        BIO_free(bio);
        Require(cert != nullptr, "external client certificate");
        return Certificate(cert, X509_free);
      };
      fixtures.client = read_certificate("client-sign.pem");
      fixtures.client_enc = read_certificate("client-enc.pem");
      fixtures.client_der = Der(fixtures.client.get());
      fixtures.client_enc_der = Der(fixtures.client_enc.get());
      BIO* bio = BIO_new_file(
          (directory / "client-sign-key.pem").string().c_str(), "rb");
      EVP_PKEY* key =
          bio ? PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr)
              : nullptr;
      BIO_free(bio);
      Require(key != nullptr, "external test-only client signing key");
      fixtures.client_key.reset(key);
    }
    ClientConfig config(
        fixtures,
        protocol_name == "tlcp" ? GM_PROTOCOL_TLCP : GM_PROTOCOL_RFC8998,
        options);
    config.root = Bytes(root);
    Require(gm_test::InitializeSockets(),
            "external TCP socket initialization");
    {
      TcpClient client(fixtures, config, static_cast<uint16_t>(port));
      client.Handshake(options.mutual, options.h2);
      FetchHttp(client, options.h2);
    }
    gm_test::CleanupSockets();
    Pass("external " + protocol_name +
         (options.mutual ? " mutual" : " server auth") + " TCP HTTP response");
    return 0;
  }
  if (argc == 3) {
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char*>(fixtures.root_der.data()),
                 fixtures.root_der.size());
    Require(output.good(), "write test trust root");
  }
  for (uint32_t protocol : {GM_PROTOCOL_RFC8998, GM_PROTOCOL_TLCP}) {
    const std::string prefix =
        protocol == GM_PROTOCOL_TLCP ? "TLCP " : "RFC8998 ";
    RunCase(fixtures, protocol, prefix + "server authentication");
    Options options;
    options.mutual = true;
    RunCase(fixtures, protocol, prefix + "asynchronous mutual authentication",
            options);
    options = {};
    options.wrong_host = true;
    RunCase(fixtures, protocol, prefix + "wrong hostname", options);
    options = {};
    options.wrong_root = true;
    RunCase(fixtures, protocol, prefix + "untrusted root", options);
    options = {};
    options.expired = true;
    RunCase(fixtures, protocol, prefix + "expired certificate", options);
    options = {};
    options.cn_only = true;
    RunCase(fixtures, protocol, prefix + "CN-only rejected", options);
    options = {};
    options.wildcard = true;
    RunCase(fixtures, protocol, prefix + "wildcard exact-mode rejection",
            options);
    options.browser_hostname = true;
    RunCase(fixtures, protocol, prefix + "browser SAN wildcard mode", options);
    TestCancel(fixtures, protocol);
    for (int crl : {1, 2, 3, 4, 5, 7}) {
      options = {};
      options.crl_case = crl;
      RunCase(fixtures, protocol, prefix + "CRL policy " + std::to_string(crl),
              options);
    }
    TestEof(fixtures, protocol, true);
    TestEof(fixtures, protocol, false);
    TestResumption(fixtures, protocol);
    TestDeferredIdentity(fixtures, protocol);
    TestProtocolFailureClassification(fixtures, protocol);
  }
  for (int type : {1, 2, 3}) {
    Options options;
    options.bad_encryption = type;
    RunCase(fixtures, GM_PROTOCOL_TLCP,
            "TLCP encryption certificate failure " + std::to_string(type),
            options);
  }
  Options options;
  options.crl_case = 6;
  RunCase(fixtures, GM_PROTOCOL_TLCP, "TLCP encryption certificate revoked",
          options);
  options = {};
  options.h2 = true;
  RunCase(fixtures, GM_PROTOCOL_RFC8998, "RFC8998 H2 ALPN", options);
  TestInvalidAlpn(fixtures);
  for (bool mutual : {false, true}) {
    TestTcp(fixtures, GM_PROTOCOL_TLCP, mutual, false);
    TestTcp(fixtures, GM_PROTOCOL_RFC8998, mutual, false);
    TestTcp(fixtures, GM_PROTOCOL_RFC8998, mutual, true);
  }
  std::printf(
      "PASS: %d backend integration cases (software token simulator; no "
      "hardware claims)\n",
      cases);
  gm_test::CloseModule(
      module);  // Upstream PINSHARED keeps initialized backend resident.
  return 0;
}
