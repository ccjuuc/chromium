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

namespace net {

// Stateless helpers for VLESS outbound framing (used by LeafClientSocket).

bool LeafVlessParseUuid(std::string_view credential,
                        std::array<uint8_t, 16>* uuid_bytes);

std::vector<uint8_t> LeafVlessBuildRequestHeader(
    const std::array<uint8_t, 16>& uuid,
    const std::string& dest_host,
    uint16_t dest_port);

std::string LeafVlessQueryLookup(std::string_view query, std::string_view key);

// True if the first line of |headers| is HTTP 1xx with status code 101.
bool LeafVlessHttpResponseFirstLineIs101(const std::string& headers);

// Removes VLESS response prelude from the first relay payload.
bool LeafVlessStripServerResponse(std::vector<uint8_t>* payload);

// GET request for RFC6455 WebSocket upgrade (caller supplies Sec-WebSocket-Key).
std::string LeafVlessBuildWebSocketUpgradeRequest(std::string_view ws_path,
                                                  std::string_view ws_host,
                                                  std::string_view sec_ws_key);

}  // namespace net

#endif  // NET_SOCKET_CHROMIUM_LEAF_VLESS_HANDSHAKE_H_
