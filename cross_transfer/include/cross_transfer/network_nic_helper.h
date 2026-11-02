#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_NETWORK_NIC_HELPER_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_NETWORK_NIC_HELPER_H_

#include <cstdint>
#include <string>
#include <vector>

namespace cross_transfer {

struct CandidateEndpoint {
  std::string if_name;
  std::string ip;
  uint16_t port{0};
  int priority_score{0};
  bool is_ipv6{false};
  // Scope indexes are local to the enumerating machine. Peer discovery must map
  // a link-local address to the receiving machine's interface before
  // connecting.
  uint32_t scope_id{0};

  std::string ToString() const;
};

class NetworkNicHelper {
 public:
  // Enumerates usable local unicast addresses, including VPN/bridge interfaces.
  // This is not peer discovery; reachability is checked by connection attempts.
  static std::vector<CandidateEndpoint> GetCandidateEndpoints(
      uint16_t listening_port);

  // Classification hint only; it must not be used to exclude viable routes.
  static bool IsVirtualOrVpn(const std::string& if_name);

  // Calculates an advisory priority, without assuming private subnets are
  // reachable.
  static int ScoreAddress(const std::string& if_name,
                          const std::string& ip,
                          bool is_ipv6);
};

}  // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_NETWORK_NIC_HELPER_H_
