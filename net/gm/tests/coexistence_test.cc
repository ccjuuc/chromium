// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This executable links BoringSSL and loads the GM DLL through its C ABI.
// In particular, it must never include a header from Tongsuo.
#include "net/gm/gm_backend.h"

#include "net/gm/tests/backend_test_platform.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace {

class ScopedModule {
 public:
  explicit ScopedModule(gm_test::Module module) : module_(module) {}
  ~ScopedModule() {
    if (module_) {
      // Release this loader reference. Tongsuo's default PINSHARED behavior
      // keeps the initialized backend mapped until process exit.
      gm_test::CloseModule(module_);
    }
  }
  ScopedModule(const ScopedModule&) = delete;
  ScopedModule& operator=(const ScopedModule&) = delete;
  gm_test::Module get() const { return module_; }

 private:
  gm_test::Module module_;
};

bool CheckBoringSsl(SSL_CTX* context) {
  constexpr std::array<uint8_t, 32> kExpected = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
      0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
      0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
  };
  std::array<uint8_t, EVP_MAX_MD_SIZE> digest = {};
  unsigned int digest_size = 0;
  bssl::UniquePtr<SSL> connection(SSL_new(context));
  return connection &&
         SSL_set_min_proto_version(connection.get(), TLS1_2_VERSION) == 1 &&
         SSL_set_max_proto_version(connection.get(), TLS1_3_VERSION) == 1 &&
         EVP_Digest("abc", 3, digest.data(), &digest_size, EVP_sha256(),
                    nullptr) == 1 &&
         digest_size == kExpected.size() &&
         std::equal(kExpected.begin(), kExpected.end(), digest.begin());
}

bool CheckGmClientHello(const GmBackendApi& api,
                        GmProtocol protocol,
                        const std::vector<uint8_t>& root_der) {
  const GmBytes trust_anchor{root_der.data(), root_der.size()};
  GmContextConfig config = {};
  config.struct_size = sizeof(config);
  config.protocol = protocol;
  config.trust_anchors = &trust_anchor;
  config.trust_anchor_count = 1;
  config.revocation_mode = GM_REVOCATION_DISABLED;
  GmContext* context = nullptr;
  if (gm_test::CallModuleFunction(api.create_context, &config, &context) != GM_OK || !context) {
    std::cerr << "GM context creation failed for protocol " << protocol << '\n';
    return false;
  }
  constexpr std::array<uint8_t, 9> kServerName = {'l', 'o', 'c', 'a', 'l',
                                                  'h', 'o', 's', 't'};
  GmConnectionConfig connection_config = {};
  connection_config.struct_size = sizeof(connection_config);
  connection_config.server_name = {kServerName.data(), kServerName.size()};
  connection_config.hostname_mode = GM_HOSTNAME_EXACT_SAN;
  GmConnection* connection = nullptr;
  const GmStatus create_result =
      gm_test::CallModuleFunction(api.create_connection, context, &connection_config, &connection);
  bool passed = create_result == GM_OK && connection;
  if (passed) {
    GmConnectionInfo metadata = {};
    metadata.struct_size = sizeof(metadata);
    size_t unavailable = 0;
    passed = gm_test::CallModuleFunction(api.get_connection_info, connection, &metadata) == GM_ERROR_STATE &&
             gm_test::CallModuleFunction(api.export_session, connection, nullptr, 0, &unavailable) ==
                 GM_ERROR_STATE;
    // Keeping the BoringSSL context alive while the DLL drives both GM TLS
    // protocols exercises the independent SSL_CTX/SSL allocation boundaries.
    passed = passed && gm_test::CallModuleFunction(api.handshake, connection) == GM_WANT_INPUT;
    std::array<uint8_t, 65536> ciphertext = {};
    size_t written = 0;
    passed = passed &&
             gm_test::CallModuleFunction(api.drain_ciphertext, connection, ciphertext.data(),
                                  ciphertext.size(), &written) == GM_OK &&
             written >= 9 && written <= ciphertext.size();
    if (passed) {
      // TLS/TLCP record type Handshake, followed by a ClientHello. This is a
      // coexistence probe, not an assertion of successful server verification.
      passed = ciphertext[0] == 22 && ciphertext[5] == 1;
    }
    passed = gm_test::CallModuleFunction(api.destroy_connection, connection) == GM_OK && passed;
  }
  passed = gm_test::CallModuleFunction(api.destroy_context, context) == GM_OK && passed;
  if (!passed) {
    std::cerr << "GM ClientHello probe failed for protocol " << protocol
              << '\n';
  }
  return passed;
}

bool HasRequiredFunctions(const GmBackendApi& api) {
  return api.create_context && api.destroy_context && api.create_connection &&
         api.destroy_connection && api.handshake && api.feed_ciphertext &&
         api.drain_ciphertext && api.read_plaintext && api.write_plaintext &&
         api.get_key_request && api.complete_key_request && api.cancel &&
         api.get_connection_info && api.get_peer_certificate_count &&
         api.get_peer_certificate && api.export_keying_material &&
         api.notify_transport_eof && api.get_pending_plaintext &&
         api.get_pending_ciphertext && api.export_session &&
         api.get_identity_request && api.get_identity_request_ca &&
         api.complete_identity_request;
}

}  // namespace

#if defined(_WIN32)
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
  if (argc != 3) {
    std::cerr << "Usage: gm_coexistence_test.exe <absolute gm_backend.dll> "
                 "<root certificate DER>\n";
    return 2;
  }
  // SAFETY: wmain's CRT contract supplies argc NUL-terminated arguments, and
  // the check above established argc == 3. Copy them before any further use.
#if defined(__clang__)
#pragma clang unsafe_buffer_usage begin
#endif
  const std::filesystem::path dll_path(argv[1]);
  const std::filesystem::path certificate_path(argv[2]);
#if defined(__clang__)
#pragma clang unsafe_buffer_usage end
#endif
  if (!dll_path.is_absolute()) {
    std::cerr << "The backend DLL path must be absolute\n";
    return 2;
  }
  std::ifstream certificate(certificate_path, std::ios::binary);
  if (!certificate) {
    std::cerr << "Cannot open the DER trust anchor\n";
    return 2;
  }
  certificate.seekg(0, std::ios::end);
  const auto certificate_size = certificate.tellg();
  if (certificate_size <= 0 || certificate_size > 1024 * 1024) {
    std::cerr << "DER trust anchor must contain between 1 byte and 1 MiB\n";
    return 2;
  }
  certificate.seekg(0, std::ios::beg);
  std::vector<uint8_t> root_der(static_cast<size_t>(certificate_size));
  if (!certificate.read(reinterpret_cast<char*>(root_der.data()),
                        certificate_size)) {
    std::cerr << "Cannot read the DER trust anchor\n";
    return 2;
  }

  bssl::UniquePtr<SSL_CTX> boring_context(SSL_CTX_new(TLS_method()));
  if (!boring_context || !CheckBoringSsl(boring_context.get())) {
    std::cerr << "BoringSSL failed before loading the GM backend\n";
    return 1;
  }
  // Exercise two loader-reference and GM-context lifetimes while the original
  // BoringSSL context stays alive. Tongsuo's default PINSHARED behavior pins
  // the DLL: FreeLibrary releases our reference but does not prove unloading
  // or reinitialization. Production treats the backend as process-resident.
  for (int iteration = 0; iteration < 2; ++iteration) {
    {
      ScopedModule module(gm_test::LoadModule(dll_path));
      if (!module.get()) {
        std::cerr << "Cannot load GM backend\n";
        return 1;
      }
      const auto get_api = reinterpret_cast<GmGetBackendApiFn>(
          gm_test::FindFunction(module.get(), "gm_backend_get_api"));
      if (!get_api) {
        std::cerr << "GM backend API export is missing\n";
        return 1;
      }
      constexpr std::array kForbiddenExports = {
          "SSL_new", "SSL_CTX_new", "EVP_Digest", "OPENSSL_init_crypto"};
      for (const char* name : kForbiddenExports) {
        if (gm_test::FindFunction(module.get(), name)) {
          std::cerr << "GM backend leaks an upstream crypto symbol: " << name
                    << '\n';
          return 1;
        }
      }
      GmBackendApi api = {};
      api.struct_size = sizeof(api);
      constexpr uint64_t kRequiredCapabilities =
          GM_CAP_TLCP | GM_CAP_RFC8998 | GM_CAP_ASYNC_SM2 | GM_CAP_HTTP2 |
          GM_CAP_SESSION_RESUMPTION | GM_CAP_ASYNC_IDENTITY;
      if (gm_test::CallModuleFunction(get_api, GM_BACKEND_ABI_VERSION, &api) != GM_OK ||
          api.abi_version != GM_BACKEND_ABI_VERSION ||
          api.struct_size != sizeof(api) ||
          (api.capabilities & kRequiredCapabilities) != kRequiredCapabilities ||
          !HasRequiredFunctions(api)) {
        std::cerr << "GM backend ABI or required capabilities do not match\n";
        return 1;
      }

      GmBackendApi invalid_version = {};
      invalid_version.struct_size = sizeof(invalid_version);
      GmBackendApi invalid_size = {};
      invalid_size.struct_size = sizeof(uint32_t);
      if (gm_test::CallModuleFunction(get_api, GM_BACKEND_ABI_VERSION + 1, &invalid_version) !=
              GM_ERROR_ABI ||
          gm_test::CallModuleFunction(get_api, 1, &invalid_version) != GM_ERROR_ABI ||
          gm_test::CallModuleFunction(get_api, GM_BACKEND_ABI_VERSION, &invalid_size) != GM_ERROR_ABI) {
        std::cerr << "GM backend did not reject an incompatible API request\n";
        return 1;
      }

      GmContext* untrusted_context = nullptr;
      GmContextConfig no_trust = {};
      no_trust.struct_size = sizeof(no_trust);
      no_trust.protocol = GM_PROTOCOL_TLCP;
      const GmStatus no_trust_result =
          gm_test::CallModuleFunction(api.create_context, &no_trust, &untrusted_context);
      if (no_trust_result != GM_OK || !untrusted_context ||
          gm_test::CallModuleFunction(api.destroy_context, untrusted_context) != GM_OK) {
        std::cerr << "GM backend cannot represent an empty trust snapshot\n";
        return 1;
      }
      if (!CheckGmClientHello(api, GM_PROTOCOL_TLCP, root_der) ||
          !CheckBoringSsl(boring_context.get()) ||
          !CheckGmClientHello(api, GM_PROTOCOL_RFC8998, root_der) ||
          !CheckBoringSsl(boring_context.get())) {
        return 1;
      }
    }
    if (!CheckBoringSsl(boring_context.get())) {
      std::cerr << "BoringSSL failed after releasing the GM loader reference\n";
      return 1;
    }
  }
  std::cout << "PASS: BoringSSL SHA-256/TLS contexts and GM TLCP/RFC8998 "
               "ClientHello generation coexist across two loader-reference "
               "lifetimes; the GM backend remains process-resident\n";
  return 0;
}
