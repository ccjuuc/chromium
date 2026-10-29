// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Test-only independent GmSSL 3 peer. Never linked into Chromium or gm_backend.
// The runner pins GmSSL and creates ephemeral fixture keys before starting it.
#include <gmssl/pem.h>
#include <gmssl/tls.h>
#include <gmssl/x509_key.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void require(int ok, const char* message) {
  if (!ok) {
    fprintf(stderr, "FAIL GmSSL peer: %s\n", message);
    exit(1);
  }
}

static int convert_key(const char* source, const char* target) {
  FILE* input = fopen(source, "rb");
  FILE* output = NULL;
  uint8_t der[1024];
  size_t size = 0;
  const uint8_t* cursor = der;
  const uint8_t* attributes;
  size_t attributes_size;
  X509_KEY key;
  memset(&key, 0, sizeof(key));
  require(input != NULL &&
              pem_read(input, "PRIVATE KEY", der, &size, sizeof(der)) == 1,
          "read test-only unencrypted PKCS8 fixture");
  fclose(input);
  require(x509_private_key_info_from_der(&key, &attributes, &attributes_size,
                                         &cursor, &size) == 1 &&
              size == 0,
          "decode test-only PKCS8 fixture");
  output = fopen(target, "wb");
  require(output != NULL && x509_private_key_info_encrypt_to_pem(
                                &key, "gm-interop-test-only", output) == 1,
          "encode GmSSL encrypted test fixture");
  fclose(output);
  x509_key_cleanup(&key);
  memset(der, 0, sizeof(der));
  return 0;
}

int main(int argc, char** argv) {
  TLS_CTX* context;
  TLS_CONNECT* connection;
  int protocol;
  int cipher;
  int group = TLS_curve_sm2p256v1;
  int signature = TLS_sig_sm2sig_sm3;
  char* alpn[] = {"http/1.1"};
  WSADATA wsa;
  SOCKET listener;
  SOCKET peer;
  struct sockaddr_in address;
  DWORD timeout = 15000;
  BOOL exclusive = TRUE;
  long port;
  char* port_end;
  char request[16384];
  size_t request_size = 0;
  size_t received = 0;
  size_t sent = 0;
  int records = 0;
  const char response[] =
      "HTTP/1.1 200 OK\r\nContent-Length: 26\r\n"
      "Connection: close\r\n\r\nGM TLS connection verified";

  if (argc == 4 && strcmp(argv[1], "--convert") == 0) {
    return convert_key(argv[2], argv[3]);
  }
  require(argc == 6 || argc == 7,
          "usage: peer <port> <tlcp|rfc8998> <chain.pem> <keys.pem> <mode> "
          "[root.pem]");
  port = strtol(argv[1], &port_end, 10);
  require(port > 0 && port <= 65535 && *port_end == '\0', "valid port");
  require(strcmp(argv[2], "tlcp") == 0 || strcmp(argv[2], "rfc8998") == 0,
          "explicit protocol");
  protocol =
      strcmp(argv[2], "tlcp") == 0 ? TLS_protocol_tlcp : TLS_protocol_tls13;
  cipher = protocol == TLS_protocol_tlcp ? TLS_cipher_ecc_sm4_gcm_sm3
                                         : TLS_cipher_sm4_gcm_sm3;
  require((argc == 6 && strcmp(argv[5], "server-auth") == 0) ||
              (argc == 7 && strcmp(argv[5], "mutual") == 0),
          "explicit authentication mode");
  context = (TLS_CTX*)calloc(1, sizeof(TLS_CTX));
  connection = (TLS_CONNECT*)calloc(1, sizeof(TLS_CONNECT));
  require(context != NULL && connection != NULL &&
              tls_ctx_init(context, protocol, TLS_server_mode) == 1 &&
              tls_ctx_set_cipher_suites(context, &cipher, 1) == 1 &&
              tls_ctx_set_application_layer_protocol_negotiation(context, alpn,
                                                                 1) == 1,
          "initialize independent TLS implementation");
  if (protocol == TLS_protocol_tlcp) {
    require(tlcp_ctx_add_server_certificate_and_keys(
                context, argv[3], argv[4], "gm-interop-test-only") == 1,
            "TLCP dual certificate identity");
  } else {
    require(tls_ctx_set_supported_groups(context, &group, 1) == 1 &&
                tls_ctx_set_signature_algorithms(context, &signature, 1) == 1 &&
                tls_ctx_add_certificate_chain_and_key(
                    context, argv[3], argv[4], "gm-interop-test-only") == 1,
            "RFC8998 identity and algorithms");
  }
  if (argc == 7) {
    require(tls_ctx_set_ca_certificates(context, argv[6],
                                        TLS_DEFAULT_VERIFY_DEPTH) == 1 &&
                tls_ctx_enable_certificate_request(context, 1) == 1 &&
                tls_ctx_enable_client_certificate_optional(context, 0) == 1,
            "mandatory client certificate authentication");
  }
  require(WSAStartup(MAKEWORD(2, 2), &wsa) == 0, "Winsock");
  listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons((u_short)port);
  require(listener != INVALID_SOCKET &&
              setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                         (const char*)&exclusive, sizeof(exclusive)) == 0 &&
              bind(listener, (const struct sockaddr*)&address,
                   sizeof(address)) == 0 &&
              listen(listener, 1) == 0,
          "exclusive loopback listener");
  puts("READY independent GmSSL 3 loopback peer");
  fflush(stdout);
  peer = accept(listener, NULL, NULL);
  require(peer != INVALID_SOCKET, "accept");
  closesocket(listener);
  setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout,
             sizeof(timeout));
  setsockopt(peer, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout,
             sizeof(timeout));
  require(tls_init(connection, context) == 1 &&
              tls_set_socket(connection, peer) == 1 &&
              tls_do_handshake(connection) == 1,
          "independent handshake");
  if (argc == 7) {
    require(
        connection->verify_result == 0 && connection->peer_cert_chain_len != 0,
        "client certificate was received and verified");
  }
  do {
    int result;
    require(request_size + 1 < sizeof(request), "bounded HTTP request");
    result = tls_recv(connection, (uint8_t*)request + request_size,
                      sizeof(request) - request_size - 1, &received);
    if (result != 1) {
      fprintf(stderr, "receive status=%d length=%zu handshake_state=%d\n",
              result, received, connection->handshake_state);
    }
    // TLCP peers may send an authenticated empty application record first.
    require(result == 1 && ++records <= 128, "receive HTTP request");
    request_size += received;
    request[request_size] = '\0';
  } while (strstr(request, "\r\n\r\n") == NULL);
  require(strncmp(request, "GET / HTTP/1.1\r\n", 16) == 0,
          "authenticated application request");
  require(tls_send(connection, (const uint8_t*)response, sizeof(response) - 1,
                   &sent) == 1 &&
              sent == sizeof(response) - 1,
          "authenticated application response");
  // The client reads close_notify then closes TCP; shutdown can complete then.
  tls_shutdown(connection);
  closesocket(peer);
  tls_cleanup(connection);
  tls_ctx_cleanup(context);
  free(connection);
  free(context);
  WSACleanup();
  printf("PASS independent GmSSL %s %s\n", argv[2], argv[5]);
  return 0;
}
