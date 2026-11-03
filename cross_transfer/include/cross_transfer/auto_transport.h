#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_AUTO_TRANSPORT_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_AUTO_TRANSPORT_H_

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "cross_transfer/transport_channel.h"

namespace cross_transfer {

struct PeerConnectionState {
  bool ok{false};
  std::string route;
  std::string peer_certificate;
  std::vector<CandidateEndpoint> endpoints;
};

// Callbacks run synchronously on the transfer worker and must have bounded
// network deadlines and observe account-session cancellation. Direct identity
// is bound to authenticated signaling; private keys never leave the device.
struct AutoConnectionOptions {
  bool sending{true};
  // The caller's small-payload policy skips identity generation/listening.
  bool prefer_relay{false};
  std::string invitation_id;
  int negotiation_timeout_ms{10 * 60 * 1000};
  int direct_timeout_ms{5000};
  // Optional browser-owned authenticated channel cache. The callback must
  // authorize this invitation and its peer before returning a cached channel.
  // Reuse precedes the small-payload relay preference; a miss uses the normal
  // policy below. A cancelled callback must not create a new connection.
  std::function<std::unique_ptr<ITransportChannel>(const std::atomic<bool>*,
                                                   TransportError*)>
      try_reuse;
  // Receives only a successful, still-connected WebRTC channel, after its
  // job-local cancellation flag has been detached. Runs on the transfer worker.
  std::function<void(std::unique_ptr<ITransportChannel>)> retain_direct;
  // Optional native ICE/DTLS adapter. It commits the direct route only after
  // both peers have opened their authenticated data channel. Failures use the
  // peer-coordinated relay decision, without switching direct protocols.
  std::function<std::unique_ptr<ITransportChannel>(const std::atomic<bool>*,
                                                   TransportError*)>
      webrtc;
  std::function<PeerConnectionState(std::string,
                                    std::vector<CandidateEndpoint>)>
      exchange;
  // "direct" records this role's authenticated handshake. Only both ready
  // roles commit direct. "relay" is a monotonic fallback, including after a
  // direct connection has failed during payload transfer.
  std::function<PeerConnectionState(std::string)> select_route;
  std::function<std::unique_ptr<ITransportChannel>(const std::atomic<bool>*,
                                                   TransportError*)>
      relay;
};

std::unique_ptr<ITransportChannel> ConnectAutomatically(
    const AutoConnectionOptions& options,
    const std::atomic<bool>* cancellation,
    TransportError* error);
std::unique_ptr<ITransportChannel> ConnectAutomaticRelay(
    const AutoConnectionOptions& options,
    const std::atomic<bool>* cancellation,
    TransportError* error);

}  // namespace cross_transfer
#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_AUTO_TRANSPORT_H_
