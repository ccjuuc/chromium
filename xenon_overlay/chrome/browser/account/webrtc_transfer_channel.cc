#include "xenon_overlay/chrome/browser/account/webrtc_transfer_channel.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <span>
#include <utility>

#include "build/build_config.h"
#include "third_party/webrtc/api/create_modular_peer_connection_factory.h"
#include "third_party/webrtc/api/data_channel_interface.h"
#include "third_party/webrtc/api/jsep.h"
#include "third_party/webrtc/api/make_ref_counted.h"
#include "third_party/webrtc/api/peer_connection_interface.h"
#include "third_party/webrtc/api/set_local_description_observer_interface.h"
#include "third_party/webrtc/api/set_remote_description_observer_interface.h"
#include "third_party/webrtc/rtc_base/thread.h"
#include "third_party/webrtc_overrides/environment.h"

#if BUILDFLAG(IS_WIN)
#include "net/base/winsock_init.h"
#endif

namespace xenon {
namespace {

using cross_transfer::FrameHeader;
using cross_transfer::TransportError;
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;
constexpr size_t kMessageSize = 16 * 1024;
constexpr size_t kMaxBufferedBytes = 16 * 1024 * 1024;
constexpr uint64_t kSendHighWater = 1024 * 1024;
constexpr size_t kMaxSdpBytes = 64 * 1024;
constexpr size_t kMaxCandidates = 64;
constexpr size_t kMaxCandidateBytes = 2048;
constexpr size_t kMaxCandidateTotalBytes = 32 * 1024;
constexpr auto kPollInterval = std::chrono::milliseconds(50);
constexpr auto kExchangeInterval = std::chrono::milliseconds(250);

struct AsyncResult {
  std::mutex mutex;
  bool done = false;
  bool ok = false;
  std::optional<WebRtcDescription> description;

  void Complete(bool success) {
    std::lock_guard lock(mutex);
    ok = success;
    done = true;
  }
};

class CreateDescriptionObserver
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  explicit CreateDescriptionObserver(std::shared_ptr<AsyncResult> result)
      : result_(std::move(result)) {}
  void OnSuccess(webrtc::SessionDescriptionInterface* description) override {
    std::unique_ptr<webrtc::SessionDescriptionInterface> owned(description);
    std::lock_guard lock(result_->mutex);
    result_->description = WebRtcDescription{owned->type(), owned->ToString()};
    result_->ok = result_->description->sdp.size() <= kMaxSdpBytes;
    result_->done = true;
  }
  void OnFailure(webrtc::RTCError) override { result_->Complete(false); }

 private:
  const std::shared_ptr<AsyncResult> result_;
};

class LocalDescriptionObserver
    : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  explicit LocalDescriptionObserver(std::shared_ptr<AsyncResult> result)
      : result_(std::move(result)) {}
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    result_->Complete(error.ok());
  }

 private:
  const std::shared_ptr<AsyncResult> result_;
};

class RemoteDescriptionObserver
    : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  explicit RemoteDescriptionObserver(std::shared_ptr<AsyncResult> result)
      : result_(std::move(result)) {}
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    result_->Complete(error.ok());
  }

 private:
  const std::shared_ptr<AsyncResult> result_;
};

bool ValidateSignal(const WebRtcSignalState& signal) {
  if (!signal.ok || signal.candidates.size() > kMaxCandidates ||
      signal.stun_urls.size() > 8) {
    return false;
  }
  if (signal.description && (signal.description->sdp.empty() ||
                             signal.description->sdp.size() > kMaxSdpBytes ||
                             (signal.description->type != "offer" &&
                              signal.description->type != "answer"))) {
    return false;
  }
  size_t candidate_bytes = 0;
  for (const auto& candidate : signal.candidates) {
    if (candidate.candidate.empty() ||
        candidate.candidate.size() > kMaxCandidateBytes ||
        candidate.sdp_mid.size() > 64 || candidate.sdp_mline_index < 0 ||
        candidate.sdp_mline_index > 16) {
      return false;
    }
    candidate_bytes += candidate.candidate.size() + candidate.sdp_mid.size();
    if (candidate_bytes > kMaxCandidateTotalBytes) {
      return false;
    }
  }
  for (const auto& url : signal.stun_urls) {
    if (url.size() > 512 ||
        (!url.starts_with("stun:") && !url.starts_with("stuns:"))) {
      return false;
    }
  }
  return signal.route == "pending" || signal.route == "direct" ||
         signal.route == "relay";
}

class WebRtcChannel final : public cross_transfer::ITransportChannel,
                            public webrtc::PeerConnectionObserver,
                            public webrtc::DataChannelObserver {
 public:
  WebRtcChannel(std::string invitation_id,
                const std::atomic<bool>* cancellation)
      : label_("xenon-transfer:" + invitation_id) {
    state_->cancellation = cancellation;
  }

  ~WebRtcChannel() override {
    Close();
    if (signaling_) {
      signaling_->Stop();
    }
    if (worker_) {
      worker_->Stop();
    }
    if (network_) {
      network_->Stop();
    }
  }

  bool Initialize(const std::vector<std::string>& stun_urls, bool sending) {
#if BUILDFLAG(IS_WIN)
    net::EnsureWinsockInit();
#endif
    network_ = webrtc::Thread::CreateWithSocketServer();
    worker_ = webrtc::Thread::Create();
    signaling_ = webrtc::Thread::Create();
    network_->SetName("XenonRtcNetwork", nullptr);
    worker_->SetName("XenonRtcWorker", nullptr);
    signaling_->SetName("XenonRtcSignaling", nullptr);
    if (!network_->Start() || !worker_->Start() || !signaling_->Start()) {
      return Fail(TransportError::kIo);
    }
    signaling_started_ = true;
    return signaling_->BlockingCall([this, &stun_urls, sending] {
      webrtc::PeerConnectionFactoryDependencies dependencies;
      dependencies.env = WebRtcEnvironment();
      dependencies.network_thread = network_.get();
      dependencies.worker_thread = worker_.get();
      dependencies.signaling_thread = signaling_.get();
      factory_ =
          webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));
      if (!factory_) {
        return Fail(TransportError::kIo);
      }
      webrtc::PeerConnectionInterface::RTCConfiguration configuration;
      configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
      // Keep the default IPv4, IPv6, UDP and TCP ICE candidates. STUN discovers
      // NAT mappings; ICE performs connectivity checks in both directions.
      for (const auto& url : stun_urls) {
        webrtc::PeerConnectionInterface::IceServer server;
        server.urls.push_back(url);
        configuration.servers.push_back(std::move(server));
      }
      auto result = factory_->CreatePeerConnectionOrError(
          configuration, webrtc::PeerConnectionDependencies(this));
      if (!result.ok()) {
        return Fail(TransportError::kIo);
      }
      peer_ = result.MoveValue();
      if (sending) {
        webrtc::DataChannelInit init;
        init.ordered = true;
        init.protocol = "xenon-transfer-v2";
        auto channel = peer_->CreateDataChannelOrError(label_, &init);
        if (!channel.ok()) {
          return Fail(TransportError::kIo);
        }
        AttachDataChannel(channel.MoveValue());
      }
      return true;
    });
  }

  bool CreateLocalDescription(bool offer, Deadline deadline) {
    auto result = std::make_shared<AsyncResult>();
    signaling_->BlockingCall([this, result, offer] {
      auto observer =
          webrtc::make_ref_counted<CreateDescriptionObserver>(result);
      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
      if (offer) {
        peer_->CreateOffer(observer.get(), options);
      } else {
        peer_->CreateAnswer(observer.get(), options);
      }
    });
    if (!WaitForResult(result, deadline)) {
      return false;
    }
    std::optional<WebRtcDescription> description;
    {
      std::lock_guard lock(result->mutex);
      description = result->description;
    }
    if (!description || !SetDescription(*description, true, deadline)) {
      return false;
    }
    local_description_ = std::move(description);
    return true;
  }

  bool ApplyRemote(const WebRtcSignalState& signal,
                   bool sending,
                   Deadline deadline) {
    if (signal.description) {
      if (signal.description->type != (sending ? "answer" : "offer")) {
        return Fail(TransportError::kProtocol);
      }
      if (remote_description_) {
        if (remote_description_->sdp != signal.description->sdp) {
          return Fail(TransportError::kSecurity);
        }
      } else {
        if (!SetDescription(*signal.description, false, deadline)) {
          return false;
        }
        remote_description_ = signal.description;
        if (!sending && !CreateLocalDescription(false, deadline)) {
          return false;
        }
      }
    }
    if (!remote_description_) {
      return true;
    }
    for (const auto& candidate : signal.candidates) {
      const auto key = candidate.sdp_mid + "\n" +
                       std::to_string(candidate.sdp_mline_index) + "\n" +
                       candidate.candidate;
      if (!remote_candidates_.insert(key).second) {
        continue;
      }
      if (remote_candidates_.size() > kMaxCandidates) {
        return Fail(TransportError::kResourceLimit);
      }
      bool valid = signaling_->BlockingCall([this, &candidate] {
        auto ice = webrtc::IceCandidate::Create(
            candidate.sdp_mid, candidate.sdp_mline_index, candidate.candidate);
        if (!ice) {
          return false;
        }
        peer_->AddIceCandidate(std::move(ice),
                               [state = state_](webrtc::RTCError error) {
                                 if (!error.ok()) {
                                   std::lock_guard lock(state->mutex);
                                   state->error = TransportError::kProtocol;
                                   state->cv.notify_all();
                                 }
                               });
        return true;
      });
      if (!valid) {
        return Fail(TransportError::kProtocol);
      }
    }
    return true;
  }

  WebRtcSignalState Exchange(const WebRtcConnectionOptions& options) {
    std::vector<WebRtcIceCandidate> candidates;
    {
      std::lock_guard lock(state_->mutex);
      candidates = state_->candidates;
    }
    return options.exchange(local_description_, std::move(candidates));
  }

  bool CheckDeadline(Deadline deadline) {
    std::lock_guard lock(state_->mutex);
    return CheckLocked(deadline);
  }

  void WaitBriefly(Deadline deadline) {
    std::unique_lock lock(state_->mutex);
    state_->cv.wait_until(lock,
                          std::min(deadline, Clock::now() + kPollInterval));
  }

  bool SendFrame(const FrameHeader& header,
                 std::span<const uint8_t> payload) override {
    if (!cross_transfer::ValidateFrameHeader(header) ||
        header.payload_len != payload.size() ||
        header.checksum != cross_transfer::ComputeCRC32C(payload)) {
      return Fail(TransportError::kProtocol);
    }
    const auto deadline = IoDeadline();
    const auto bytes = header.Serialize();
    return SendBytes(bytes, deadline) && SendBytes(payload, deadline);
  }

  bool ReadNextFrame(FrameHeader& header,
                     std::vector<uint8_t>& payload) override {
    const auto deadline = IoDeadline();
    std::array<uint8_t, FrameHeader::kWireSize> bytes;
    if (!ReadBytes(bytes, deadline)) {
      return false;
    }
    auto parsed = FrameHeader::Deserialize(bytes);
    if (!parsed || !cross_transfer::ValidateFrameHeader(*parsed)) {
      return Fail(TransportError::kProtocol);
    }
    header = *parsed;
    payload.resize(header.payload_len);
    if (!ReadBytes(payload, deadline)) {
      return false;
    }
    if (header.checksum != cross_transfer::ComputeCRC32C(payload)) {
      return Fail(TransportError::kProtocol);
    }
    return true;
  }

  void Close() override {
    std::lock_guard close_lock(close_mutex_);
    bool graceful = false;
    {
      std::lock_guard lock(state_->mutex);
      graceful = state_->open && !state_->closed &&
                 state_->error == TransportError::kNone &&
                 !(state_->cancellation && state_->cancellation->load());
      state_->open = false;
    }
    if (signaling_started_) {
      if (graceful) {
        // Send() only enqueues SCTP data. Complete the data-channel close
        // handshake before destroying its transport so the final transfer ACK
        // and any pending fragments reach the peer.
        signaling_->BlockingCall([this] {
          if (channel_) {
            channel_->Close();
          }
        });
        const auto deadline = Clock::now() + std::chrono::seconds(2);
        std::unique_lock lock(state_->mutex);
        while (!state_->closed && state_->error == TransportError::kNone &&
               !(state_->cancellation && state_->cancellation->load()) &&
               Clock::now() < deadline) {
          state_->cv.wait_until(
              lock, std::min(deadline, Clock::now() + kPollInterval));
        }
      }
      signaling_->BlockingCall([this] {
        if (channel_) {
          channel_->UnregisterObserver();
          channel_->Close();
          channel_ = nullptr;
        }
        if (peer_) {
          peer_->Close();
          peer_ = nullptr;
        }
        factory_ = nullptr;
      });
    }
    std::lock_guard lock(state_->mutex);
    state_->closed = true;
    state_->open = false;
    state_->cv.notify_all();
  }

  bool IsConnected() const override {
    std::lock_guard lock(state_->mutex);
    return state_->open && !state_->closed &&
           state_->error == TransportError::kNone;
  }
  bool IsSecure() const override { return IsConnected(); }
  cross_transfer::TransportType GetType() const override {
    return cross_transfer::TransportType::kWebRtcData;
  }
  void SetCancellationFlag(const std::atomic<bool>* flag) override {
    std::lock_guard lock(state_->mutex);
    state_->cancellation = flag;
    state_->cv.notify_all();
  }
  void SetIoTimeout(int timeout_ms) override {
    std::lock_guard lock(state_->mutex);
    state_->io_timeout_ms = std::max(1, timeout_ms);
  }
  TransportError LastError() const override {
    std::lock_guard lock(state_->mutex);
    return state_->error;
  }
  bool Fail(TransportError error) {
    std::lock_guard lock(state_->mutex);
    if (state_->error == TransportError::kNone) {
      state_->error = error;
    }
    state_->cv.notify_all();
    return false;
  }

 private:
  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    bool closed = false;
    TransportError error = TransportError::kNone;
    cross_transfer::BorrowedPtr<const std::atomic<bool>> cancellation = nullptr;
    int io_timeout_ms = 30000;
    std::deque<std::vector<uint8_t>> received;
    size_t front_offset = 0;
    size_t received_size = 0;
    std::vector<WebRtcIceCandidate> candidates;
    size_t candidate_bytes = 0;
  };

  bool CheckLocked(Deadline deadline, bool has_buffered_data = false) {
    if (state_->error != TransportError::kNone) {
      return false;
    }
    if (state_->cancellation && state_->cancellation->load()) {
      state_->error = TransportError::kCancelled;
    } else if (state_->closed && !has_buffered_data) {
      state_->error = TransportError::kNotConnected;
    } else if (Clock::now() >= deadline) {
      state_->error = TransportError::kTimeout;
    }
    return state_->error == TransportError::kNone;
  }

  Deadline IoDeadline() const {
    std::lock_guard lock(state_->mutex);
    return Clock::now() + std::chrono::milliseconds(state_->io_timeout_ms);
  }

  bool WaitForResult(const std::shared_ptr<AsyncResult>& result,
                     Deadline deadline) {
    while (CheckDeadline(deadline)) {
      {
        std::lock_guard lock(result->mutex);
        if (result->done) {
          return result->ok || Fail(TransportError::kProtocol);
        }
      }
      WaitBriefly(deadline);
    }
    return false;
  }

  bool SetDescription(const WebRtcDescription& description,
                      bool local,
                      Deadline deadline) {
    auto result = std::make_shared<AsyncResult>();
    signaling_->BlockingCall([this, &description, local, result] {
      const auto type = description.type == "offer" ? webrtc::SdpType::kOffer
                                                    : webrtc::SdpType::kAnswer;
      auto parsed = webrtc::CreateSessionDescription(type, description.sdp);
      if (!parsed) {
        result->Complete(false);
        return;
      }
      if (local) {
        peer_->SetLocalDescription(
            std::move(parsed),
            webrtc::make_ref_counted<LocalDescriptionObserver>(result));
      } else {
        peer_->SetRemoteDescription(
            std::move(parsed),
            webrtc::make_ref_counted<RemoteDescriptionObserver>(result));
      }
    });
    return WaitForResult(result, deadline);
  }

  bool SendBytes(std::span<const uint8_t> bytes, Deadline deadline) {
    while (!bytes.empty() && CheckDeadline(deadline)) {
      auto fragment = bytes.first(std::min(bytes.size(), kMessageSize));
      const int result = signaling_->BlockingCall([this, fragment] {
        if (!channel_ ||
            channel_->state() != webrtc::DataChannelInterface::kOpen) {
          return -1;
        }
        if (channel_->buffered_amount() >= kSendHighWater) {
          return 0;
        }
        return channel_->Send(webrtc::DataBuffer(
                   webrtc::CopyOnWriteBuffer(fragment), true))
                   ? 1
                   : -1;
      });
      if (result < 0) {
        return Fail(TransportError::kIo);
      }
      if (result == 0) {
        WaitBriefly(deadline);
      } else {
        bytes = bytes.subspan(fragment.size());
      }
    }
    return bytes.empty() && CheckDeadline(deadline);
  }

  bool ReadBytes(std::span<uint8_t> bytes, Deadline deadline) {
    std::unique_lock lock(state_->mutex);
    while (!bytes.empty()) {
      if (!CheckLocked(deadline, !state_->received.empty())) {
        return false;
      }
      if (state_->received.empty()) {
        state_->cv.wait_until(lock,
                              std::min(deadline, Clock::now() + kPollInterval));
        continue;
      }
      auto available =
          std::span(state_->received.front()).subspan(state_->front_offset);
      const size_t count = std::min(bytes.size(), available.size());
      std::ranges::copy(available.first(count), bytes.begin());
      bytes = bytes.subspan(count);
      state_->received_size -= count;
      state_->front_offset += count;
      if (state_->front_offset == state_->received.front().size()) {
        state_->received.pop_front();
        state_->front_offset = 0;
      }
    }
    return true;
  }

  void AttachDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) {
    if (channel_ || channel->label() != label_ || !channel->reliable() ||
        !channel->ordered() || channel->protocol() != "xenon-transfer-v2") {
      channel->Close();
      Fail(TransportError::kSecurity);
      return;
    }
    channel_ = std::move(channel);
    channel_->RegisterObserver(this);
    RefreshState();
  }

  void RefreshState() {
    if (!channel_) {
      return;
    }
    const auto status = channel_->state();
    std::lock_guard lock(state_->mutex);
    state_->open = status == webrtc::DataChannelInterface::kOpen;
    if (status == webrtc::DataChannelInterface::kClosed) {
      state_->closed = true;
    }
    state_->cv.notify_all();
  }

  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState) override {}
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
    AttachDataChannel(std::move(channel));
  }
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override {
    WebRtcIceCandidate value{candidate->sdp_mid(), candidate->sdp_mline_index(),
                             candidate->ToString()};
    std::lock_guard lock(state_->mutex);
    const size_t bytes = value.candidate.size() + value.sdp_mid.size();
    if (state_->candidates.size() >= kMaxCandidates ||
        value.sdp_mid.size() > 64 ||
        value.candidate.size() > kMaxCandidateBytes ||
        bytes > kMaxCandidateTotalBytes - state_->candidate_bytes) {
      state_->error = TransportError::kResourceLimit;
    } else {
      state_->candidates.push_back(std::move(value));
      state_->candidate_bytes += bytes;
    }
    state_->cv.notify_all();
  }
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) override {
    if (state ==
        webrtc::PeerConnectionInterface::PeerConnectionState::kFailed) {
      Fail(TransportError::kIo);
    }
  }
  void OnStateChange() override {
    // WebRTC forbids API re-entry from data-channel callbacks.
    signaling_->PostTask([this] { RefreshState(); });
  }
  void OnBufferedAmountChange(uint64_t) override { state_->cv.notify_all(); }
  void OnMessage(const webrtc::DataBuffer& buffer) override {
    std::lock_guard lock(state_->mutex);
    if (state_->closed || state_->error != TransportError::kNone) {
      return;
    }
    if (!buffer.binary || buffer.size() > kMessageSize) {
      state_->error = TransportError::kProtocol;
    } else if (buffer.size() > kMaxBufferedBytes - state_->received_size) {
      state_->error = TransportError::kResourceLimit;
    } else if (buffer.size() != 0) {
      state_->received.emplace_back(buffer.data.begin(), buffer.data.end());
      state_->received_size += buffer.size();
    }
    state_->cv.notify_all();
  }

  const std::string label_;
  std::mutex close_mutex_;
  const std::shared_ptr<State> state_ = std::make_shared<State>();
  std::unique_ptr<webrtc::Thread> network_;
  std::unique_ptr<webrtc::Thread> worker_;
  std::unique_ptr<webrtc::Thread> signaling_;
  bool signaling_started_ = false;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> peer_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> channel_;
  std::optional<WebRtcDescription> local_description_;
  std::optional<WebRtcDescription> remote_description_;
  std::set<std::string> remote_candidates_;
};

}  // namespace

std::unique_ptr<cross_transfer::ITransportChannel> ConnectWebRtc(
    const WebRtcConnectionOptions& options,
    const std::atomic<bool>* cancellation,
    TransportError* error) {
  auto channel =
      std::make_unique<WebRtcChannel>(options.invitation_id, cancellation);
  auto failed = [&]() -> std::unique_ptr<cross_transfer::ITransportChannel> {
    if (error) {
      *error = channel->LastError();
    }
    return nullptr;
  };
  if (options.invitation_id.empty() || options.invitation_id.size() > 256 ||
      !options.exchange || !options.confirm_direct ||
      options.negotiation_timeout_ms <= 0 || options.connect_timeout_ms <= 0) {
    channel->Fail(TransportError::kProtocol);
    return failed();
  }
  auto deadline =
      Clock::now() + std::chrono::milliseconds(options.negotiation_timeout_ms);
  if (!channel->CheckDeadline(deadline)) {
    return failed();
  }
  auto signal = options.exchange(std::nullopt, {});
  if (!ValidateSignal(signal) || signal.route == "relay") {
    channel->Fail(TransportError::kNotConnected);
    return failed();
  }
  if (!channel->Initialize(
          options.use_stun ? signal.stun_urls : std::vector<std::string>{},
          options.sending)) {
    return failed();
  }
  bool negotiating = options.sending || signal.description.has_value();
  if (negotiating) {
    deadline = std::min(
        deadline,
        Clock::now() + std::chrono::milliseconds(options.connect_timeout_ms));
  }
  if (options.sending && !channel->CreateLocalDescription(true, deadline)) {
    return failed();
  }
  auto next_exchange = Clock::now();
  while (channel->CheckDeadline(deadline)) {
    if (!ValidateSignal(signal) || signal.route == "relay") {
      channel->Fail(TransportError::kNotConnected);
      break;
    }
    if (!negotiating && signal.description) {
      negotiating = true;
      deadline = std::min(
          deadline,
          Clock::now() + std::chrono::milliseconds(options.connect_timeout_ms));
    }
    if (!channel->ApplyRemote(signal, options.sending, deadline)) {
      break;
    }
    if (Clock::now() >= next_exchange) {
      if (channel->IsConnected()) {
        const auto route = options.confirm_direct();
        if (route == "direct" && channel->CheckDeadline(deadline)) {
          if (error) {
            *error = TransportError::kNone;
          }
          return channel;
        }
        if (route != "pending") {
          channel->Fail(TransportError::kNotConnected);
          break;
        }
      }
      signal = channel->Exchange(options);
      next_exchange = Clock::now() + kExchangeInterval;
    }
    channel->WaitBriefly(std::min(deadline, next_exchange));
  }
  return failed();
}

}  // namespace xenon
