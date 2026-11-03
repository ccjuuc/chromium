#include "xenon_overlay/chrome/browser/account/webrtc_connection_cache.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"

namespace xenon {
namespace {

using cross_transfer::FrameHeader;
using cross_transfer::ITransportChannel;
using cross_transfer::TaskId;
using cross_transfer::TransportError;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr size_t kMaxConnections = 8;
constexpr auto kIdleLifetime = base::Minutes(10);
constexpr auto kHandshakeTimeout = std::chrono::seconds(5);
constexpr auto kPreparationTimeout = std::chrono::minutes(10);
constexpr std::array<uint8_t, 10> kReuseMagic = {'X', 'E', 'N', 'O', 'N',
                                                 'R', 'T', 'C', 1,   0};

enum class ReusePhase : uint8_t {
  kHello = 1,
  kReady = 2,
  kCommit = 3,
  kCommitted = 4,
};

bool Usable(const ITransportChannel& channel) {
  return channel.IsConnected() && channel.IsSecure() &&
         channel.GetType() == cross_transfer::TransportType::kWebRtcData;
}

std::optional<TaskId> ParseTaskId(std::string_view value) {
  if (value.size() != 32) {
    return std::nullopt;
  }
  const auto hex = [](char byte) {
    if (byte >= '0' && byte <= '9') {
      return byte - '0';
    }
    if (byte >= 'a' && byte <= 'f') {
      return byte - 'a' + 10;
    }
    if (byte >= 'A' && byte <= 'F') {
      return byte - 'A' + 10;
    }
    return -1;
  };
  TaskId id{};
  for (size_t i = 0; i < id.size(); ++i) {
    const int high = hex(value[i * 2]);
    const int low = hex(value[i * 2 + 1]);
    if (high < 0 || low < 0) {
      return std::nullopt;
    }
    id[i] = static_cast<uint8_t>(high * 16 + low);
  }
  return cross_transfer::IsValidTaskId(id) ? std::optional(id) : std::nullopt;
}

std::vector<uint8_t> ReusePayload(const TaskId& id,
                                  bool sending,
                                  ReusePhase phase) {
  std::vector<uint8_t> bytes(kReuseMagic.begin(), kReuseMagic.end());
  bytes.insert(bytes.end(), id.begin(), id.end());
  bytes.push_back(sending ? 1 : 2);
  bytes.push_back(static_cast<uint8_t>(phase));
  return bytes;
}

// This is an invitation-bound readiness/commit exchange on the already
// authenticated, encrypted connection. It deliberately rejects unexpected
// frames, rather than draining data that might belong to another transfer.
bool Handshake(ITransportChannel& channel,
               const TaskId& id,
               bool sending,
               const std::atomic<bool>* cancellation,
               TransportError* error) {
  auto deadline = Clock::now() + kHandshakeTimeout;
  const auto prepare_io = [&](Deadline until) {
    if (cancellation && cancellation->load()) {
      *error = TransportError::kCancelled;
      return false;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(until -
                                                              Clock::now());
    if (remaining.count() <= 0) {
      *error = TransportError::kTimeout;
      return false;
    }
    channel.SetIoTimeout(static_cast<int>(remaining.count()));
    return true;
  };
  const auto io_failed = [&] {
    *error = cancellation && cancellation->load() ? TransportError::kCancelled
                                                  : channel.LastError();
    if (*error == TransportError::kNone) {
      *error = TransportError::kNotConnected;
    }
    return false;
  };
  const auto send = [&](ReusePhase phase) {
    if (!prepare_io(deadline)) {
      return false;
    }
    auto bytes = ReusePayload(id, sending, phase);
    FrameHeader header;
    header.cmd = sending ? FrameHeader::Command::kHandshakeReq
                         : FrameHeader::Command::kHandshakeResp;
    header.task_id = id;
    header.payload_len = static_cast<uint32_t>(bytes.size());
    header.checksum = cross_transfer::ComputeCRC32C(bytes);
    return channel.SendFrame(header, bytes) || io_failed();
  };
  const auto receive = [&](ReusePhase phase, Deadline until) {
    if (!prepare_io(until)) {
      return false;
    }
    FrameHeader header;
    std::vector<uint8_t> bytes;
    if (!channel.ReadNextFrame(header, bytes)) {
      return io_failed();
    }
    const auto command = sending ? FrameHeader::Command::kHandshakeResp
                                 : FrameHeader::Command::kHandshakeReq;
    if (!cross_transfer::ValidateFrameHeader(header) || header.cmd != command ||
        header.task_id != id || header.payload_len != bytes.size() ||
        header.checksum != cross_transfer::ComputeCRC32C(bytes) ||
        bytes != ReusePayload(id, !sending, phase)) {
      *error = TransportError::kProtocol;
      return false;
    }
    return true;
  };
  if (sending) {
    if (!send(ReusePhase::kHello) || !receive(ReusePhase::kReady, deadline) ||
        !send(ReusePhase::kCommit) ||
        !receive(ReusePhase::kCommitted, deadline)) {
      return false;
    }
  } else {
    // The sender hashes the source before requesting a channel. Only this
    // first read gets a preparation budget; all remaining phases share 5 s.
    if (!receive(ReusePhase::kHello, Clock::now() + kPreparationTimeout)) {
      return false;
    }
    deadline = Clock::now() + kHandshakeTimeout;
    if (!send(ReusePhase::kReady) || !receive(ReusePhase::kCommit, deadline) ||
        !send(ReusePhase::kCommitted)) {
      return false;
    }
  }
  if (!prepare_io(deadline)) {
    return false;
  }
  channel.SetIoTimeout(30000);
  *error = TransportError::kNone;
  return true;
}

}  // namespace

struct WebRtcConnectionCache::Impl
    : public std::enable_shared_from_this<WebRtcConnectionCache::Impl> {
  using StoredChannel =
      std::unique_ptr<ITransportChannel, base::OnTaskRunnerDeleter>;
  struct Entry {
    StoredChannel channel;
    base::TimeTicks idle_since;
  };

  void ScheduleMaintenance(base::TimeDelta delay) {
    cleanup_runner->PostDelayedTask(FROM_HERE,
                                    base::BindOnce(
                                        [](std::weak_ptr<Impl> weak) {
                                          if (auto impl = weak.lock()) {
                                            impl->Expire();
                                          }
                                        },
                                        weak_from_this()),
                                    delay);
  }

  void Expire() {
    std::vector<StoredChannel> expired;
    std::optional<base::TimeDelta> next_delay;
    {
      std::lock_guard lock(mutex);
      const auto now = base::TimeTicks::Now();
      for (auto it = entries.begin(); it != entries.end();) {
        const auto remaining = it->second.idle_since + kIdleLifetime - now;
        if (remaining <= base::TimeDelta()) {
          expired.push_back(std::move(it->second.channel));
          it = entries.erase(it);
        } else {
          if (!next_delay || remaining < *next_delay) {
            next_delay = remaining;
          }
          ++it;
        }
      }
      maintenance_pending = next_delay.has_value();
    }
    if (next_delay) {
      ScheduleMaintenance(*next_delay);
    }
  }

  std::mutex mutex;
  bool accepting = true;
  bool maintenance_pending = false;
  std::map<std::string, Entry> entries;
  // Destroying a WebRTC channel waits on its signaling thread before joining
  // its threads. Cached channels are destroyed on this runner.
  const scoped_refptr<base::SequencedTaskRunner> cleanup_runner =
      base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::WithBaseSyncPrimitives(),
           base::TaskPriority::BEST_EFFORT,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN});
};

WebRtcConnectionCache::WebRtcConnectionCache()
    : impl_(std::make_shared<Impl>()) {}

WebRtcConnectionCache::~WebRtcConnectionCache() {
  Clear();
}

std::unique_ptr<ITransportChannel> WebRtcConnectionCache::TryReuse(
    const std::string& peer_key,
    const std::string& invitation_id,
    bool sending,
    const std::atomic<bool>* cancellation,
    TransportError* error) {
  TransportError local_error = TransportError::kNotConnected;
  if (!error) {
    error = &local_error;
  }
  *error = TransportError::kNotConnected;
  if (cancellation && cancellation->load()) {
    *error = TransportError::kCancelled;
    return nullptr;
  }
  const auto id = ParseTaskId(invitation_id);
  if (!id || peer_key.empty() || peer_key.size() > 1024) {
    *error = TransportError::kProtocol;
    return nullptr;
  }
  Impl::StoredChannel stored(nullptr,
                             base::OnTaskRunnerDeleter(impl_->cleanup_runner));
  bool expired = false;
  {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->entries.find(peer_key);
    if (!impl_->accepting || it == impl_->entries.end()) {
      return nullptr;
    }
    stored = std::move(it->second.channel);
    expired = base::TimeTicks::Now() - it->second.idle_since >= kIdleLifetime;
    impl_->entries.erase(it);
  }
  if (expired) {
    return nullptr;
  }
  std::unique_ptr<ITransportChannel> channel(stored.release());
  channel->SetCancellationFlag(cancellation);
  if (!Usable(*channel) ||
      !Handshake(*channel, *id, sending, cancellation, error) ||
      !Usable(*channel)) {
    if (*error == TransportError::kNone) {
      *error = TransportError::kNotConnected;
    }
    channel->Close();
    channel->SetCancellationFlag(nullptr);
    return nullptr;
  }
  return channel;
}

void WebRtcConnectionCache::Remember(
    const std::string& peer_key,
    std::unique_ptr<ITransportChannel> channel) {
  if (!channel) {
    return;
  }
  channel->SetCancellationFlag(nullptr);
  const bool usable = Usable(*channel);
  Impl::StoredChannel stored(channel.release(),
                             base::OnTaskRunnerDeleter(impl_->cleanup_runner));
  if (!usable || peer_key.empty() || peer_key.size() > 1024) {
    return;
  }
  std::vector<Impl::StoredChannel> replaced;
  bool schedule = false;
  {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->accepting) {
      return;
    }
    const auto existing = impl_->entries.find(peer_key);
    if (existing != impl_->entries.end()) {
      replaced.push_back(std::move(existing->second.channel));
      impl_->entries.erase(existing);
    }
    if (impl_->entries.size() >= kMaxConnections) {
      const auto oldest = std::ranges::min_element(
          impl_->entries, {},
          [](const auto& entry) { return entry.second.idle_since; });
      replaced.push_back(std::move(oldest->second.channel));
      impl_->entries.erase(oldest);
    }
    impl_->entries.emplace(
        peer_key, Impl::Entry{std::move(stored), base::TimeTicks::Now()});
    if (!impl_->maintenance_pending) {
      impl_->maintenance_pending = true;
      schedule = true;
    }
  }
  if (schedule) {
    impl_->ScheduleMaintenance(kIdleLifetime);
  }
}

void WebRtcConnectionCache::Clear() {
  std::map<std::string, Impl::Entry> released;
  {
    std::lock_guard lock(impl_->mutex);
    impl_->accepting = false;
    released.swap(impl_->entries);
  }
}

}  // namespace xenon
