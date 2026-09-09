// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "uv.h"

namespace xenon::ipc {
namespace {

std::optional<int> PrefixLength(base::span<const uint8_t> mask) {
  int prefix = 0;
  bool seen_zero = false;
  for (uint8_t byte : mask) {
    for (int bit = 7; bit >= 0; --bit) {
      if (byte & (1 << bit)) {
        if (seen_zero) {
          return std::nullopt;
        }
        ++prefix;
      } else {
        seen_zero = true;
      }
    }
  }
  return prefix;
}

mojom::IpcResultPtr Failure(int status) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = std::string(uv_err_name(status)) + ": " +
                  uv_strerror(status) + ", uv_interface_addresses";
  return result;
}

}  // namespace

mojom::IpcResultPtr GetNetworkInterfaces() {
  uv_interface_address_t* raw_addresses = nullptr;
  int count = 0;
  int status = uv_interface_addresses(&raw_addresses, &count);
  if (status != 0) {
    return Failure(status);
  }
  base::ScopedClosureRunner cleanup(
      base::BindOnce(&uv_free_interface_addresses, raw_addresses, count));
  // libuv owns exactly |count| entries until uv_free_interface_addresses.
  auto addresses =
      UNSAFE_BUFFERS(base::span(raw_addresses, static_cast<size_t>(count)));
  base::DictValue interfaces;
  for (const auto& address : addresses) {
    const bool ipv6 = address.address.address4.sin_family == AF_INET6;
    if (!ipv6 && address.address.address4.sin_family != AF_INET) {
      continue;
    }
    char ip[INET6_ADDRSTRLEN] = {};
    char netmask[INET6_ADDRSTRLEN] = {};
    status = ipv6 ? uv_ip6_name(&address.address.address6, ip, sizeof(ip))
                  : uv_ip4_name(&address.address.address4, ip, sizeof(ip));
    if (status != 0) {
      return Failure(status);
    }
    status =
        ipv6 ? uv_ip6_name(&address.netmask.netmask6, netmask, sizeof(netmask))
             : uv_ip4_name(&address.netmask.netmask4, netmask, sizeof(netmask));
    if (status != 0) {
      return Failure(status);
    }
    const auto prefix = ipv6 ? PrefixLength(base::as_byte_span(
                                   address.netmask.netmask6.sin6_addr.s6_addr))
                             : PrefixLength(base::byte_span_from_ref(
                                   address.netmask.netmask4.sin_addr.s_addr));
    std::string mac;
    for (unsigned char byte : address.phys_addr) {
      if (!mac.empty()) {
        mac += ':';
      }
      mac +=
          base::ToLowerASCII(base::HexEncode(base::byte_span_from_ref(byte)));
    }
    base::DictValue entry;
    entry.Set("address", ip);
    entry.Set("netmask", netmask);
    entry.Set("family", ipv6 ? "IPv6" : "IPv4");
    entry.Set("mac", mac);
    entry.Set("internal", address.is_internal != 0);
    entry.Set("cidr", prefix ? base::Value(std::string(ip) + "/" +
                                           base::NumberToString(*prefix))
                             : base::Value());
    if (ipv6) {
      entry.Set("scopeid",
                static_cast<double>(address.address.address6.sin6_scope_id));
    }
    base::ListValue* entries = interfaces.FindList(address.name);
    if (!entries) {
      entries = &interfaces.Set(address.name, base::ListValue())->GetList();
    }
    entries->Append(std::move(entry));
  }
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = base::Value(std::move(interfaces));
  return result;
}

}  // namespace xenon::ipc
