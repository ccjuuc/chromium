// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GM_GM_CLIENT_CERT_CHAIN_WIN_H_
#define CHROME_BROWSER_GM_GM_CLIENT_CERT_CHAIN_WIN_H_

#include <windows.h>

#include <wincrypt.h>

#include <optional>
#include <vector>

#include "base/containers/span.h"

namespace gm_browser {

// Blocking, local-only issuer discovery for the public certificates from SKF.
// Run on a MayBlock worker before certificate-request filtering/PIN prompting.
// Reads registry-backed CA/ROOT stores; never invokes a Windows chain engine,
// downloads certificates, loads a device driver or establishes trust. Returns
// reachable CA candidates, nearest issuers first, with at most 16 certificates
// / 4 MiB. Only locally verified self-signed roots are omitted; same-name CA
// rollovers and candidates with unsupported signature algorithms are retained.
// The TLS peer still verifies the resulting chain.
// nullopt means malformed input, incomplete store reads or a resource limit;
// an empty vector is a successful lookup with no intermediate candidates.
std::optional<std::vector<std::vector<uint8_t>>>
FindGmClientCertificateIntermediates(
    base::span<const uint8_t> signing_certificate,
    base::span<const uint8_t> encryption_certificate,
    const std::vector<std::vector<uint8_t>>& existing_intermediates);

// Uses caller-owned stores only. Never opens or mutates OS certificate stores.
std::optional<std::vector<std::vector<uint8_t>>>
FindGmClientCertificateIntermediatesFromStoresForTesting(
    base::span<const uint8_t> signing_certificate,
    base::span<const uint8_t> encryption_certificate,
    const std::vector<std::vector<uint8_t>>& existing_intermediates,
    HCERTSTORE roots,
    HCERTSTORE intermediates);

}  // namespace gm_browser

#endif  // CHROME_BROWSER_GM_GM_CLIENT_CERT_CHAIN_WIN_H_
