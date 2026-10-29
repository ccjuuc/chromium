// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Public certificates and CRL produced independently by Tongsuo 8.5.0 through
// out/gm_interop_reference/gm_backend_smoke.exe --export-fixtures <directory>.
// Exporter SHA-256:
// 4c6d78da091c00834fb5231d6704d8d5685aaa0014bd1616ef6726b503a0f4af
// The verifier test fixes its clock to the export interval. No private keys
// are embedded; regeneration creates new ephemeral test keys.
#ifndef NET_GM_TESTS_CERTIFICATE_VERIFIER_TEST_DATA_H_
#define NET_GM_TESTS_CERTIFICATE_VERIFIER_TEST_DATA_H_

#include <cstdint>
#include <vector>

#include "base/base64.h"
#include "base/check.h"

namespace net::gm::test {

inline constexpr int64_t kTongsuoFixtureUnixTime = 1793173036;

// server-root.der SHA-256:
// b669770efec2a2b5113334ef5ea9ada8d9745525fba0401f02244ca44ff4f458
inline std::vector<uint8_t> TongsuoRoot() {
  auto value = base::Base64Decode(
      "MIIBQjCB6aADAgECAgQQAAAAMAoGCCqBHM9VAYN1MBcxFTATBgNVBAMMDEdNIHRlc3Qgcm9v"
      "dDAeFw0yNjEwMjcwNzMxMjZaFw0yNjEwMjkwNzMxMjZaMBcxFTATBgNVBAMMDEdNIHRlc3Qg"
      "cm9vdDBZMBMGByqGSM49AgEGCCqBHM9VAYItA0IABGZMFnaEzNbuRmMwTo0acjpMo80b9k0i"
      "ujqdFDWD5JjpHqSsUrB5DOBAWmH2Mh/Nldt4zg34+v3/gticBa9+X/OjIzAhMA8GA1UdEwEB"
      "/wQFMAMBAf8wDgYDVR0PAQH/BAQDAgEGMAoGCCqBHM9VAYN1A0gAMEUCIDDMICQ3QHr2OD8s"
      "uE7ijeozP5oagNUwZQlFjjZoLDNeAiEAwanHF5fBPiNrEz1evBTo4F6MHHJzwMUtacSR2KoD"
      "xGc=");
  CHECK(value);
  return {value->begin(), value->end()};
}

// server-sign.der SHA-256:
// 337d2fe6c7feaebff7680e967ca7bce23af9d506178b87957c592c4cd64a6ab7
inline std::vector<uint8_t> TongsuoSigning() {
  auto value = base::Base64Decode(
      "MIIBfzCCASWgAwIBAgIEEAAAAjAKBggqgRzPVQGDdTAXMRUwEwYDVQQDDAxHTSB0ZXN0IHJv"
      "b3QwHhcNMjYxMDI3MDczMTI2WhcNMjYxMDI5MDczMTI2WjASMRAwDgYDVQQDDAdnbS50ZXN0"
      "MFkwEwYHKoZIzj0CAQYIKoEcz1UBgi0DQgAE/u9cIAZtWxTTV9MLRLcBP+igWg3KUnEtCkUQ"
      "JnEKh3gcBoqvdHcw+gJl+Dfe1ZdztvjM/03kgCu1FV5hlj2Cm6NkMGIwDAYDVR0TAQH/BAIw"
      "ADAOBgNVHQ8BAf8EBAMCB4AwHQYDVR0lBBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMCMCMGA1Ud"
      "EQQcMBqCB2dtLnRlc3SCCWxvY2FsaG9zdIcEfwAAATAKBggqgRzPVQGDdQNIADBFAiA0/r6E"
      "fyaB630ufkpVw0rtT25d8ufWxtLVRlZyHAYMaAIhALYw0pD1wAOEEp+ZCLSRkd074lNOsynP"
      "koThJtJ9p5Wi");
  CHECK(value);
  return {value->begin(), value->end()};
}

// server-enc.der SHA-256:
// 743d198db8435ce99d7ac42549554d09ea3c578a4f199da7f45d4ae695b586c4
inline std::vector<uint8_t> TongsuoEncryption() {
  auto value = base::Base64Decode(
      "MIIBfjCCASWgAwIBAgIEEAAAAzAKBggqgRzPVQGDdTAXMRUwEwYDVQQDDAxHTSB0ZXN0IHJv"
      "b3QwHhcNMjYxMDI3MDczMTI2WhcNMjYxMDI5MDczMTI2WjASMRAwDgYDVQQDDAdnbS50ZXN0"
      "MFkwEwYHKoZIzj0CAQYIKoEcz1UBgi0DQgAE0/8zkKoepJB9j4HB7dAkTfdLUdNCfh7X8nL5"
      "aiN1Itejb3Wknl4nWrVFJwa05/x549uI7205zPRSLay2hm3VlKNkMGIwDAYDVR0TAQH/BAIw"
      "ADAOBgNVHQ8BAf8EBAMCAzgwHQYDVR0lBBYwFAYIKwYBBQUHAwEGCCsGAQUFBwMCMCMGA1Ud"
      "EQQcMBqCB2dtLnRlc3SCCWxvY2FsaG9zdIcEfwAAATAKBggqgRzPVQGDdQNHADBEAiAGUkCD"
      "J99HWxORFJjnkLfp2br6EUVkz5cZ6l/Mo1zaVwIgA8tzQuxpqhiklipU5o3j+zHq2c1QAYVe"
      "eFZhjs/toQM=");
  CHECK(value);
  return {value->begin(), value->end()};
}

// server-crl.der SHA-256:
// d648dc76f81a9797152e79f32f77ac0f39febd3e7107c2f458bc6d9a86198a9d
inline std::vector<uint8_t> TongsuoCrl() {
  auto value = base::Base64Decode(
      "MIGeMEYCAQEwCgYIKoEcz1UBg3UwFzEVMBMGA1UEAwwMR00gdGVzdCByb290Fw0yNjEwMjgw"
      "NzI2MjZaFw0yNjEwMjgwODMxMjZaMAoGCCqBHM9VAYN1A0gAMEUCICVkAAh8o/Qw9lDzg6G3"
      "wj+5TsJH1PfwssgzUruFNZm1AiEA40kckPTeZj22TSJ9zgj1+fr+fLfUSHUqKap2+"
      "srQSi4=");
  CHECK(value);
  return {value->begin(), value->end()};
}

}  // namespace net::gm::test

#endif  // NET_GM_TESTS_CERTIFICATE_VERIFIER_TEST_DATA_H_
