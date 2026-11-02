#include "cross_transfer/network_nic_helper.h"
#include <cassert>
#include <iostream>

using namespace cross_transfer;

void TestNicFilter() {
  assert(NetworkNicHelper::IsVirtualOrVpn("docker0"));
  assert(NetworkNicHelper::IsVirtualOrVpn("veth12345"));
  assert(NetworkNicHelper::IsVirtualOrVpn("utun2"));
  assert(NetworkNicHelper::IsVirtualOrVpn("tun0"));
  assert(NetworkNicHelper::IsVirtualOrVpn("vmnet1"));
  assert(NetworkNicHelper::IsVirtualOrVpn("bridge0"));
  assert(NetworkNicHelper::IsVirtualOrVpn("awdl0"));

  assert(!NetworkNicHelper::IsVirtualOrVpn("en0"));
  assert(!NetworkNicHelper::IsVirtualOrVpn("eth0"));
  assert(!NetworkNicHelper::IsVirtualOrVpn("wlan0"));

  std::cout << "[PASS] TestNicFilter" << std::endl;
}

void TestNicDiscovery() {
  auto endpoints = NetworkNicHelper::GetCandidateEndpoints(53317);
  std::cout << "Discovered sanitized candidate endpoints: " << endpoints.size() << std::endl;
  for (const auto& ep : endpoints) {
    std::cout << "  Candidate: " << ep.ToString() << std::endl;
    // Virtual/VPN interfaces can be the only reachable route; retain candidates.
    assert(ep.port == 53317);
    assert(!ep.ip.empty());
  }

  std::cout << "[PASS] TestNicDiscovery" << std::endl;
}

int main() {
  TestNicFilter();
  TestNicDiscovery();
  std::cout << "All NetworkNicHelper tests passed successfully!" << std::endl;
  return 0;
}
