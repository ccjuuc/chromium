#ifndef XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_WEBRTC_TRANSFER_CHANNEL_H_
#define XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_WEBRTC_TRANSFER_CHANNEL_H_

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "cross_transfer/transport_channel.h"

namespace xenon {

struct WebRtcDescription {
  std::string type;
  std::string sdp;
};

struct WebRtcIceCandidate {
  std::string sdp_mid;
  int sdp_mline_index = 0;
  std::string candidate;
};

struct WebRtcSignalState {
  bool ok = false;
  std::string route;
  std::optional<WebRtcDescription> description;
  std::vector<WebRtcIceCandidate> candidates;
  std::vector<std::string> stun_urls;
};

struct WebRtcConnectionOptions {
  bool sending = false;
  bool use_stun = false;
  std::string invitation_id;
  int negotiation_timeout_ms = 600000;
  int connect_timeout_ms = 15000;
  // Called on the invoking worker, with the complete local description and
  // candidates. These callbacks must authenticate the transfer's two devices,
  // enforce their own finite I/O deadlines, and observe cancellation.
  std::function<WebRtcSignalState(std::optional<WebRtcDescription>,
                                  std::vector<WebRtcIceCandidate>)>
      exchange;
  std::function<std::string()> confirm_direct;
};

// Blocks a worker until ICE/DTLS and the reliable ordered data channel are
// ready, then commits the direct route through authenticated signaling. A
// failure leaves the caller free to select the existing HTTPS relay.
std::unique_ptr<cross_transfer::ITransportChannel> ConnectWebRtc(
    const WebRtcConnectionOptions& options,
    const std::atomic<bool>* cancellation,
    cross_transfer::TransportError* error);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_WEBRTC_TRANSFER_CHANNEL_H_
