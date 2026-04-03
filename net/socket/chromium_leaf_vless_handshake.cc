// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be found
// in the LICENSE file.

#include "net/socket/chromium_leaf_vless_handshake.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/strings/stringprintf.h"
#include "net/base/url_util.h"
#include "url/gurl.h"

namespace net {

bool LeafVlessParseUuid(std::string_view cred,
                        std::array<uint8_t, 16>* uuid_bytes) {
  auto from_hex_nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return 10 + c - 'a';
    }
    if (c >= 'A' && c <= 'F') {
      return 10 + c - 'A';
    }
    return -1;
  };
  std::string compact;
  compact.reserve(32);
  for (char c : cred) {
    if (c == '-') {
      continue;
    }
    if (from_hex_nibble(c) < 0) {
      return false;
    }
    compact.push_back(c);
  }
  if (compact.size() != 32) {
    return false;
  }
  for (size_t i = 0; i < 16; ++i) {
    int hi = from_hex_nibble(compact[2 * i]);
    int lo = from_hex_nibble(compact[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    (*uuid_bytes)[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

std::vector<uint8_t> LeafVlessBuildRequestHeader(
    const std::array<uint8_t, 16>& uuid,
    const std::string& dest_host,
    uint16_t dest_port) {
  std::vector<uint8_t> out;
  out.reserve(32 + dest_host.size());
  out.push_back(0);
  out.insert(out.end(), uuid.begin(), uuid.end());
  out.push_back(0);
  out.push_back(1);
  out.push_back(static_cast<uint8_t>(dest_port >> 8));
  out.push_back(static_cast<uint8_t>(dest_port & 0xff));
  out.push_back(2);
  if (dest_host.size() > 255) {
    return {};
  }
  out.push_back(static_cast<uint8_t>(dest_host.size()));
  out.insert(out.end(), dest_host.begin(), dest_host.end());
  return out;
}

std::string LeafVlessQueryLookup(std::string_view query, std::string_view key) {
  if (query.empty()) {
    return std::string();
  }
  GURL url("https://local.invalid/?" + std::string(query));
  if (!url.is_valid()) {
    return std::string();
  }
  std::string value;
  if (GetValueForKeyInQuery(url, std::string(key), &value)) {
    return value;
  }
  return std::string();
}

bool LeafVlessHttpResponseFirstLineIs101(const std::string& headers) {
  size_t line_end = headers.find("\r\n");
  if (line_end == std::string::npos) {
    line_end = headers.find('\n');
  }
  if (line_end == std::string::npos) {
    return false;
  }
  std::string_view line(headers.data(), line_end);
  const size_t sp1 = line.find(' ');
  if (sp1 == std::string_view::npos || sp1 == 0) {
    return false;
  }
  const size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string_view::npos) {
    return line.substr(sp1 + 1) == "101";
  }
  return line.substr(sp1 + 1, sp2 - sp1 - 1) == "101";
}

bool LeafVlessStripServerResponse(std::vector<uint8_t>* payload) {
  if (payload->size() < 2) {
    return false;
  }
  uint8_t m = (*payload)[1];
  if (payload->size() < static_cast<size_t>(2) + m) {
    return false;
  }
  payload->erase(payload->begin(), payload->begin() + 2 + m);
  return true;
}

std::string LeafVlessBuildWebSocketUpgradeRequest(std::string_view ws_path,
                                                  std::string_view ws_host,
                                                  std::string_view sec_ws_key) {
  return base::StringPrintf(
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: %s\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "User-Agent: ChromiumLeaf/1.0\r\n"
      "\r\n",
      std::string(ws_path).c_str(), std::string(ws_host).c_str(),
      std::string(sec_ws_key).c_str());
}

}  // namespace net
