#include "cross_transfer/network_nic_helper.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string_view>
#include <tuple>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace cross_transfer {
namespace {
void AddAddress(std::vector<CandidateEndpoint>& candidates,
                const std::string& interface_name,
                const sockaddr* address,
                uint32_t scope_id,
                uint16_t port,
                bool physical = false) {
  if (!address) {
    return;
  }
  CandidateEndpoint endpoint;
  endpoint.if_name = interface_name;
  endpoint.port = port;
  char text[INET6_ADDRSTRLEN]{};
  if (address->sa_family == AF_INET) {
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
    uint32_t ip = ntohl(ipv4->sin_addr.s_addr);
    if (ip == 0 || (ip >> 24) == 127 || (ip >> 28) == 0xe || ip == 0xffffffff) {
      return;
    }
    if (!inet_ntop(AF_INET, &ipv4->sin_addr, text, sizeof(text))) {
      return;
    }
  } else if (address->sa_family == AF_INET6) {
    const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
    if (IN6_IS_ADDR_UNSPECIFIED(&ipv6->sin6_addr) ||
        IN6_IS_ADDR_LOOPBACK(&ipv6->sin6_addr) ||
        IN6_IS_ADDR_MULTICAST(&ipv6->sin6_addr)) {
      return;
    }
    if (!inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text))) {
      return;
    }
    endpoint.is_ipv6 = true;
    if (IN6_IS_ADDR_LINKLOCAL(&ipv6->sin6_addr)) {
      endpoint.scope_id = ipv6->sin6_scope_id ? ipv6->sin6_scope_id : scope_id;
      if (endpoint.scope_id == 0) {
        return;
      }
    }
  } else {
    return;
  }
  endpoint.ip = text;
  if (endpoint.scope_id) {
    endpoint.ip += "%" + std::to_string(endpoint.scope_id);
  }
  endpoint.priority_score = NetworkNicHelper::ScoreAddress(
      interface_name, endpoint.ip, endpoint.is_ipv6);
  if (physical && endpoint.priority_score < 80) {
    endpoint.priority_score += 20;
  }
  candidates.push_back(std::move(endpoint));
}
}  // namespace

std::string CandidateEndpoint::ToString() const {
  std::stringstream out;
  out << '[' << if_name << "] ";
  if (is_ipv6) {
    out << '[' << ip << ']';
  } else {
    out << ip;
  }
  out << ':' << port << " (score: " << priority_score << ')';
  return out.str();
}

bool NetworkNicHelper::IsVirtualOrVpn(const std::string& if_name) {
  // Classification only: VPN or bridge interfaces can be the only valid route.
  static constexpr std::array<std::string_view, 15> prefixes = {
      "docker", "veth", "utun", "tun", "tap", "vmnet", "vboxnet", "bridge",
      "p2p",    "awdl", "llw",  "gif", "stf", "anpi",  "ap"};
  std::string lower = if_name;
  std::transform(
      lower.begin(), lower.end(), lower.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return std::any_of(
      prefixes.begin(), prefixes.end(),
      [&](std::string_view prefix) { return lower.rfind(prefix, 0) == 0; });
}

int NetworkNicHelper::ScoreAddress(const std::string& if_name,
                                   const std::string&,
                                   bool is_ipv6) {
  // Ranking is a hint. Reachability is determined by actual connection
  // attempts, not by private IPv4 prefixes. Do not eliminate VPN or IPv6
  // candidates.
  int score = 50;
  if (if_name.rfind("en", 0) == 0 || if_name.rfind("eth", 0) == 0 ||
      if_name.rfind("wlan", 0) == 0) {
    score += 30;
  }
  if (IsVirtualOrVpn(if_name)) {
    score -= 10;
  }
  if (is_ipv6) {
    score += 1;
  }
  return score;
}

std::vector<CandidateEndpoint> NetworkNicHelper::GetCandidateEndpoints(
    uint16_t port) {
  std::vector<CandidateEndpoint> candidates;
#if defined(_WIN32)
  static const bool initialized = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  if (!initialized) {
    return candidates;
  }
  ULONG size = 32 * 1024;
  std::unique_ptr<IP_ADAPTER_ADDRESSES, decltype(&std::free)> buffer(
      nullptr, &std::free);
  ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int attempt = 0; attempt < 3 && result == ERROR_BUFFER_OVERFLOW;
       ++attempt) {
    if (size > 4 * 1024 * 1024) {
      return candidates;
    }
    // Allocate size bytes for the variable-length structures and strings.
    buffer.reset(static_cast<IP_ADAPTER_ADDRESSES*>(std::calloc(1, size)));
    if (!buffer) {
      return candidates;
    }
    result = GetAdaptersAddresses(
        AF_UNSPEC,
        GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER,
        nullptr, buffer.get(), &size);
  }
  if (result != NO_ERROR) {
    return candidates;
  }
  for (auto* adapter = buffer.get(); adapter; adapter = adapter->Next) {
    if (adapter->OperStatus != IfOperStatusUp ||
        adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
      continue;
    }
    bool physical = adapter->IfType == IF_TYPE_ETHERNET_CSMACD ||
                    adapter->IfType == IF_TYPE_IEEE80211;
    std::string name = adapter->AdapterName ? adapter->AdapterName : "";
    for (auto* item = adapter->FirstUnicastAddress; item; item = item->Next) {
      if (item->DadState != IpDadStatePreferred &&
          item->DadState != IpDadStateDeprecated) {
        continue;
      }
      AddAddress(candidates, name, item->Address.lpSockaddr,
                 adapter->Ipv6IfIndex, port, physical);
    }
  }
#else
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0) {
    return candidates;
  }
  for (const auto* item = interfaces; item; item = item->ifa_next) {
    if (!item->ifa_addr || !item->ifa_name || !(item->ifa_flags & IFF_UP) ||
        (item->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }
    AddAddress(candidates, item->ifa_name, item->ifa_addr,
               if_nametoindex(item->ifa_name), port);
  }
  freeifaddrs(interfaces);
#endif
  std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) {
              if (a.priority_score != b.priority_score) {
                return a.priority_score > b.priority_score;
              }
              return std::tie(a.ip, a.port, a.if_name) <
                     std::tie(b.ip, b.port, b.if_name);
            });
  candidates.erase(std::unique(candidates.begin(), candidates.end(),
                               [](const auto& a, const auto& b) {
                                 return a.ip == b.ip && a.port == b.port &&
                                        a.if_name == b.if_name;
                               }),
                   candidates.end());
  return candidates;
}
}  // namespace cross_transfer
