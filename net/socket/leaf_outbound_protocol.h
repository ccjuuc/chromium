// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_SOCKET_LEAF_OUTBOUND_PROTOCOL_H_
#define NET_SOCKET_LEAF_OUTBOUND_PROTOCOL_H_

namespace net {

// Matches net::ProxyServer schemes SCHEME_VLESS / SCHEME_VMESS / SCHEME_TROJAN.
enum class LeafOutboundProtocol {
  kVless,
  kVmess,
  kTrojan,
};

}  // namespace net

#endif  // NET_SOCKET_LEAF_OUTBOUND_PROTOCOL_H_
