// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_BACKEND_LOADER_H_
#define NET_GM_GM_BACKEND_LOADER_H_

#include <type_traits>

#include "base/containers/span.h"
#include "net/base/net_export.h"
#include "net/gm/gm_backend.h"

namespace net::gm {

// The internal facade preserves connection lifecycle contracts while every
// operation uses Chromium's BoringSSL and participates in the normal CFI graph.
template <typename... Args>
GmStatus CallBackendFunction(GmStatus (*function)(Args...),
                             std::type_identity_t<Args>... args) {
  return function(args...);
}

// Returns the statically linked implementation, or null when GM is disabled.
NET_EXPORT const GmBackendApi* GetBackendApi();

// Creates a browser connection with an explicit, nonempty cipher allowlist.
// Applies it before importing a session; a ticket using a removed cipher is
// rejected so the caller can discard it and perform a full handshake. The
// standalone interoperability ABI retains its existing default-suite API.
NET_EXPORT GmStatus
CreateBackendConnection(GmContext* context,
                        const GmConnectionConfig* config,
                        base::span<const uint16_t> cipher_suites,
                        GmConnection** out);

}  // namespace net::gm

#endif  // NET_GM_GM_BACKEND_LOADER_H_
