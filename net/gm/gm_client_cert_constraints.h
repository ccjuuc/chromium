// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_CLIENT_CERT_CONSTRAINTS_H_
#define NET_GM_GM_CLIENT_CERT_CONSTRAINTS_H_

#include "base/containers/span.h"
#include "base/time/time.h"
#include "net/gm/gm_client_identity.h"

namespace net::gm {

// Bounds the copied CertificateRequest before IPC, parsing or caching.
NET_EXPORT bool IsValidClientCertificateRequest(
    const ClientCertificateRequest& request);

// Validates a GM request against the connection's negotiation policy. A joint
// TLS 1.3 policy accepts a concrete RFC 8998 request; ordinary client-auth is
// handled by Chromium's ordinary TLS path. kTls13Auto is never a wire protocol.
NET_EXPORT bool IsClientCertificateRequestForProfile(
    const ConnectionProfile& profile,
    const ClientCertificateRequest& request);

// Selection only: verifies public certificate shape, validity, SM2 curve,
// roles, clientAuth and the server's issuer/algorithm constraints. The peer
// remains responsible for validating the client's chain and proof of key use.
NET_EXPORT bool MatchesClientCertificateRequest(
    base::span<const uint8_t> signing_certificate,
    base::span<const uint8_t> encryption_certificate,
    const std::vector<std::vector<uint8_t>>& intermediates,
    const ClientCertificateRequest& request,
    base::Time now = base::Time::Now());

}  // namespace net::gm

#endif  // NET_GM_GM_CLIENT_CERT_CONSTRAINTS_H_
