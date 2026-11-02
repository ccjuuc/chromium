#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_CONNECTION_MANAGER_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_CONNECTION_MANAGER_H_

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "cross_transfer/transport_channel.h"

namespace cross_transfer {

struct ConnectionOptions {
  int total_timeout_ms{10000};
  int direct_timeout_ms{1500};
  int io_timeout_ms{30000};
  BorrowedPtr<const std::atomic<bool>> cancellation{nullptr};
  bool require_secure{true};
};

// Factories execute on the calling worker. They must honor the remaining time
// and cancellation flag, including DNS, TCP and authentication handshakes. They
// return only fully established channels and retain no reference to arguments.
// A secure direct factory can wrap TCP in peer-authenticated TLS. A relay
// factory owns the HTTPS/WSS endpoint and credentials, never the file-transfer
// state.
using DirectConnector = std::function<std::unique_ptr<ITransportChannel>(
    const std::vector<CandidateEndpoint>&,
    int,
    const std::atomic<bool>*)>;
using RelayConnector =
    std::function<std::unique_ptr<ITransportChannel>(int,
                                                     const std::atomic<bool>*)>;

// Synchronous connection policy on a worker thread, separate from file
// sessions. Reconnect with the same session state after a network change. No
// bytes are sent by the manager; discovery, authorization and transfer are
// separate concerns.
class ConnectionManager {
 public:
  explicit ConnectionManager(DirectConnector direct = {},
                             RelayConnector relay = {});
  ~ConnectionManager();
  std::unique_ptr<ITransportChannel> Connect(
      const std::vector<CandidateEndpoint>& candidates,
      const ConnectionOptions& options = {});
  TransportError LastError() const { return last_error_.load(); }

 private:
  DirectConnector direct_;
  RelayConnector relay_;
  std::atomic<TransportError> last_error_{TransportError::kNone};
};

}  // namespace cross_transfer
#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_CONNECTION_MANAGER_H_
