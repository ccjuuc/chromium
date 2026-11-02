#include "cross_transfer/connection_manager.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <utility>

namespace cross_transfer {
namespace {
using Clock = std::chrono::steady_clock;
int RemainingMs(Clock::time_point deadline) {
  auto duration = deadline - Clock::now();
  if (duration <= Clock::duration::zero()) {
    return 0;
  }
  auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
  return static_cast<int>(std::min<int64_t>(INT_MAX, std::max<int64_t>(1, ms)));
}
}  // namespace

ConnectionManager::ConnectionManager(DirectConnector direct,
                                     RelayConnector relay)
    : direct_(std::move(direct)), relay_(std::move(relay)) {}
ConnectionManager::~ConnectionManager() = default;

std::unique_ptr<ITransportChannel> ConnectionManager::Connect(
    const std::vector<CandidateEndpoint>& candidates,
    const ConnectionOptions& options) {
  auto cancelled = [&] {
    return options.cancellation && options.cancellation->load();
  };
  last_error_ = TransportError::kNotConnected;
  if (cancelled()) {
    last_error_ = TransportError::kCancelled;
    return nullptr;
  }
  if (options.total_timeout_ms <= 0) {
    last_error_ = TransportError::kTimeout;
    return nullptr;
  }
  if (options.direct_timeout_ms < 0 || options.io_timeout_ms <= 0) {
    last_error_ = TransportError::kProtocol;
    return nullptr;
  }
  auto deadline =
      Clock::now() + std::chrono::milliseconds(options.total_timeout_ms);
  auto accept = [&](std::unique_ptr<ITransportChannel>& channel,
                    Clock::time_point attempt_deadline) {
    if (cancelled()) {
      last_error_ = TransportError::kCancelled;
    } else if (Clock::now() >= attempt_deadline) {
      last_error_ = TransportError::kTimeout;
    } else if (!channel) {
      last_error_ = TransportError::kIo;
    } else if (!channel->IsConnected()) {
      auto reason = channel->LastError();
      last_error_ = reason == TransportError::kNone
                        ? TransportError::kNotConnected
                        : reason;
    } else if (options.require_secure && !channel->IsSecure()) {
      last_error_ = TransportError::kSecurity;
    } else {
      channel->SetCancellationFlag(options.cancellation);
      channel->SetIoTimeout(options.io_timeout_ms);
      last_error_ = TransportError::kNone;
      return true;
    }
    if (channel) {
      channel->Close();
    }
    channel.reset();
    return false;
  };

  // Never silently downgrade the default secure policy to plaintext LAN TCP.
  if (!candidates.empty() && options.direct_timeout_ms > 0 &&
      (direct_ || !options.require_secure)) {
    int budget = std::min(options.direct_timeout_ms, RemainingMs(deadline));
    if (budget == 0) {
      last_error_ = TransportError::kTimeout;
      return nullptr;
    }
    auto direct_deadline = Clock::now() + std::chrono::milliseconds(budget);
    std::unique_ptr<ITransportChannel> channel;
    if (direct_) {
      channel = direct_(candidates, budget, options.cancellation);
    } else {
      channel =
          LanTcpClient::RaceConnect(candidates, budget, options.cancellation);
    }
    if (accept(channel, direct_deadline)) {
      return channel;
    }
    if (cancelled()) {
      return nullptr;
    }
  }
  int remaining = RemainingMs(deadline);
  if (cancelled()) {
    last_error_ = TransportError::kCancelled;
    return nullptr;
  }
  if (remaining == 0) {
    last_error_ = TransportError::kTimeout;
    return nullptr;
  }
  if (relay_) {
    auto channel = relay_(remaining, options.cancellation);
    if (accept(channel, deadline)) {
      return channel;
    }
  }
  return nullptr;
}
}  // namespace cross_transfer
