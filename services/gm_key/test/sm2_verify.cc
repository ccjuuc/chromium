// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Separate test process using the same BoringSSL baseline as Chromium. It only
// receives public certificates, a digest and a signature from the IPC test.
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/ec_key.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/boringssl/src/include/openssl/sm2.h"
#include "third_party/boringssl/src/include/openssl/x509.h"

std::vector<unsigned char> Read(const char* path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
  if (argc != 4) {
    return 2;
  }
  // SAFETY: The CRT supplies argc pointers and argc == 4 was checked above.
  const auto arguments = UNSAFE_BUFFERS(base::span(argv, size_t{4}));
  auto certificate = Read(arguments[1]);
  auto digest = Read(arguments[2]);
  auto signature = Read(arguments[3]);
  if (certificate.empty() ||
      certificate.size() >
          static_cast<size_t>(std::numeric_limits<long>::max()) ||
      digest.size() != SM2_DIGEST_LENGTH || signature.empty()) {
    return 2;
  }
  const auto* cursor = certificate.data();
  bssl::UniquePtr<X509> cert(
      d2i_X509(nullptr, &cursor, static_cast<long>(certificate.size())));
  const auto certificate_bytes = base::span(certificate);
  if (!cert || cursor != certificate_bytes.subspan(certificate.size()).data()) {
    return 1;
  }
  EVP_PKEY* key = X509_get0_pubkey(cert.get());
  const EC_KEY* ec = key && EVP_PKEY_id(key) == EVP_PKEY_EC
                         ? EVP_PKEY_get0_EC_KEY(key)
                         : nullptr;
  const EC_GROUP* group = ec ? EC_KEY_get0_group(ec) : nullptr;
  bool verified =
      group && EC_GROUP_get_curve_name(group) == NID_sm2 &&
      SM2_verify_digest(digest.data(), signature.data(), signature.size(), ec);
  return verified ? 0 : 1;
}
