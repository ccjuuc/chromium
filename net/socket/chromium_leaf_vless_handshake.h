// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file.

#ifndef NET_SOCKET_CHROMIUM_LEAF_VLESS_HANDSHAKE_H_
#define NET_SOCKET_CHROMIUM_LEAF_VLESS_HANDSHAKE_H_

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "build/buildflag.h"
#include "net/base/net_export.h"
#include "net/net_buildflags.h"

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
#include "base/containers/span.h"
#include "third_party/chromium_leaf/src/lib.rs.h"
#endif

namespace net {

// Stateless helpers for VLESS outbound framing (used by LeafClientSocket).

bool LeafVlessParseUuid(std::string_view credential,
                        std::array<uint8_t, 16>* uuid_bytes);

std::vector<uint8_t> LeafVlessBuildRequestHeader(
    const std::array<uint8_t, 16>& uuid,
    const std::string& dest_host,
    uint16_t dest_port);

#if BUILDFLAG(ENABLE_CHROMIUM_LEAF)
// Xray Vision (xtls-rprx-vision) request header; same semantics as Leaf
// `build_vless_tcp_header` with domain (addr_type 2).
std::vector<uint8_t> LeafVlessBuildVisionRequestHeader(
    const std::array<uint8_t, 16>& uuid,
    const std::string& dest_host,
    uint16_t dest_port);

// Trojan (Xray-compatible): hex(SHA224(password)) + CRLF + command byte
// (TCP=1) + SOCKS-like address + CRLF. First payload on TCP or in the first
// WS binary frame after HTTP 101.
NET_EXPORT_PRIVATE std::vector<uint8_t> LeafTrojanBuildRelayHandshake(
    std::string_view password,
    const std::string& dest_host,
    uint16_t dest_port);

// Incremental RX parser matching Leaf `VisionParser` (no Tokio).
class NET_EXPORT_PRIVATE LeafVlessVisionParser {
 public:
  explicit LeafVlessVisionParser(const std::array<uint8_t, 16>& uuid);
  ~LeafVlessVisionParser();

  LeafVlessVisionParser(const LeafVlessVisionParser&) = delete;
  LeafVlessVisionParser& operator=(const LeafVlessVisionParser&) = delete;

  std::vector<uint8_t> Feed(base::span<const uint8_t> data);
  bool direct_copy_rx() const;
  bool vision_done() const;

 private:
  rust::Box<net::chromium_leaf::ChromiumLeafVisionParser> impl_;
};
#endif  // ENABLE_CHROMIUM_LEAF

std::string LeafVlessQueryLookup(std::string_view query, std::string_view key);

// True if the first line of |headers| is HTTP 1xx with status code 101.
bool LeafVlessHttpResponseFirstLineIs101(const std::string& headers);

// Removes VLESS response prelude from the first relay payload.
bool LeafVlessStripServerResponse(std::vector<uint8_t>* payload);

// GET request for RFC6455 WebSocket upgrade (caller supplies Sec-WebSocket-Key).
// Empty |user_agent| selects a browser-like default. Empty |origin| omits Origin.
// Proxy URI may include useragent=/origin= query keys (Xray streamSettings.headers).
std::string LeafVlessBuildWebSocketUpgradeRequest(
    std::string_view ws_path,
    std::string_view ws_host,
    std::string_view sec_ws_key,
    std::string_view user_agent,
    std::string_view origin);

}  // namespace net

#endif  // NET_SOCKET_CHROMIUM_LEAF_VLESS_HANDSHAKE_H_
