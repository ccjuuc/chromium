#ifndef XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_WEBRTC_CONNECTION_CACHE_H_
#define XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_WEBRTC_CONNECTION_CACHE_H_

#include <atomic>
#include <memory>
#include <string>

#include "cross_transfer/transport_channel.h"

namespace xenon {

// A profile/account-session cache of successful, idle WebRTC connections.
// The owner supplies a key binding the account, local device and remote device.
// Each channel has one owner; concurrent transfers never share its byte stream.
class WebRtcConnectionCache {
 public:
  WebRtcConnectionCache();
  ~WebRtcConnectionCache();
  WebRtcConnectionCache(const WebRtcConnectionCache&) = delete;
  WebRtcConnectionCache& operator=(const WebRtcConnectionCache&) = delete;

  // Worker only. Takes an idle connection and authenticates this accepted
  // invitation over its existing encrypted stream. No signaling API is called.
  // A miss/failure returns null, allowing the normal ICE/relay path. The caller
  // must keep cancellation alive until it is detached or the channel closes.
  std::unique_ptr<cross_transfer::ITransportChannel> TryReuse(
      const std::string& peer_key,
      const std::string& invitation_id,
      bool sending,
      const std::atomic<bool>* cancellation,
      cross_transfer::TransportError* error);

  // Worker only, after a successful complete transfer. Detaches the job's
  // cancellation flag before retaining the connection. Up to eight connections
  // are kept for ten idle minutes; replacing/expiring entries closes them on a
  // worker. A cache that was cleared never accepts connections again.
  void Remember(const std::string& peer_key,
                std::unique_ptr<cross_transfer::ITransportChannel> channel);

  // Thread safe, including the UI thread. Permanently seals this cache and
  // schedules idle connection destruction on a worker. Checked-out connections
  // finish normally; a later Remember to this old cache only disposes them.
  void Clear();

 private:
  struct Impl;
  const std::shared_ptr<Impl> impl_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_ACCOUNT_WEBRTC_CONNECTION_CACHE_H_
