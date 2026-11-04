// Copyright 2019 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/common/verifier_formats.h"

#include "components/crx_file/crx_verifier.h"

namespace extensions {

namespace {

// When disabled, `GetWebstoreVerifierFormat` returns `VerifierFormat::CRX3` to
// skip verification of publisher keys in the CRX. Can be overridden in tests.
bool g_enable_publisher_key_verification = true;

}  // namespace

crx_file::VerifierFormat GetWebstoreVerifierFormat(
    bool test_publisher_enabled) {
  return g_enable_publisher_key_verification
             ? test_publisher_enabled
                   ? crx_file::VerifierFormat::CRX3_WITH_TEST_PUBLISHER_PROOF
                   : crx_file::VerifierFormat::CRX3_WITH_PUBLISHER_PROOF
             : crx_file::VerifierFormat::CRX3;
}

crx_file::VerifierFormat GetPolicyVerifierFormat() {
  return crx_file::VerifierFormat::CRX3;
}

std::vector<std::vector<uint8_t>> GetXlWebstorePublisherKeyHashes() {
  // SHA256 of the DER SPKI in
  // tools/xl_extension_store/xl_publisher_public_key.pem. The private key
  // belongs to the store and is never included in the browser.
  return {crx_file::GetProductionPublisherKeyHash(),
          {0x78, 0x8e, 0x07, 0x08, 0x76, 0xc3, 0x2d, 0xf8, 0xc7, 0x45, 0xca,
           0xec, 0xf9, 0x7a, 0x05, 0xe8, 0x43, 0xd3, 0x9d, 0x01, 0x24, 0xc7,
           0x7f, 0xa6, 0x5e, 0xec, 0xd0, 0x3c, 0x95, 0xe5, 0x9c, 0x68}};
}

crx_file::VerifierFormat GetExternalVerifierFormat() {
  return crx_file::VerifierFormat::CRX3;
}

crx_file::VerifierFormat GetTestVerifierFormat() {
  return crx_file::VerifierFormat::CRX3;
}

base::AutoReset<bool> DisablePublisherKeyVerificationForTests() {
  return {&g_enable_publisher_key_verification, false};
}

}  // namespace extensions
