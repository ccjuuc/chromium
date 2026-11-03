#include "xenon_overlay/chrome/browser/account/webrtc_transfer_channel.h"

#include <array>
#include <atomic>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "cross_transfer/test_job.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/account/webrtc_connection_cache.h"

namespace xenon {
namespace {

using cross_transfer::FrameHeader;
using cross_transfer::ITransportChannel;
using cross_transfer::TransportError;

class WebRtcTransferChannelTest : public testing::Test {
 protected:
  struct Endpoint {
    std::optional<WebRtcDescription> description;
    std::vector<WebRtcIceCandidate> candidates;
    bool confirmed = false;
  };

  WebRtcConnectionOptions Options(size_t index) {
    WebRtcConnectionOptions options;
    options.sending = index == 0;
    options.use_stun = use_stun_;
    options.invitation_id = "authenticated-invitation";
    options.connect_timeout_ms = 10000;
    options.negotiation_timeout_ms = 15000;
    options.exchange = [this, index](
                           std::optional<WebRtcDescription> description,
                           std::vector<WebRtcIceCandidate> candidates) {
      std::lock_guard lock(mutex_);
      if (description) {
        endpoints_[index].description = std::move(description);
      }
      endpoints_[index].candidates = std::move(candidates);
      WebRtcSignalState signal;
      signal.ok = true;
      signal.route = "pending";
      signal.description = endpoints_[1 - index].description;
      signal.candidates = endpoints_[1 - index].candidates;
      signal.stun_urls = stun_urls_;
      if (corrupt_fingerprint_ && signal.description) {
        auto& sdp = signal.description->sdp;
        const auto offset = sdp.find("a=fingerprint:sha-256 ");
        if (offset != std::string::npos) {
          const size_t start =
              offset + std::string("a=fingerprint:sha-256 ").size();
          sdp[start] = sdp[start] == '0' ? '1' : '0';
        }
      }
      return signal;
    };
    options.confirm_direct = [this, index] {
      std::lock_guard lock(mutex_);
      endpoints_[index].confirmed = true;
      return endpoints_[0].confirmed && endpoints_[1].confirmed ? "direct"
                                                                : "pending";
    };
    return options;
  }

  void ConnectPair(int timeout_ms = 10000) {
    auto sender = Options(0);
    auto receiver = Options(1);
    sender.connect_timeout_ms = timeout_ms;
    receiver.connect_timeout_ms = timeout_ms;
    std::thread receiving([this, receiver] {
      channels_[1] = ConnectWebRtc(receiver, &cancelled_, &errors_[1]);
    });
    channels_[0] = ConnectWebRtc(sender, &cancelled_, &errors_[0]);
    if (!channels_[0]) {
      cancelled_.store(true);
    }
    receiving.join();
  }

  base::test::TaskEnvironment task_environment_;
  std::atomic<bool> cancelled_{false};
  std::mutex mutex_;
  std::array<Endpoint, 2> endpoints_;
  std::array<std::unique_ptr<ITransportChannel>, 2> channels_;
  std::array<TransportError, 2> errors_;
  bool corrupt_fingerprint_ = false;
  bool use_stun_ = true;
  std::vector<std::string> stun_urls_;
};

TEST_F(WebRtcTransferChannelTest, TransfersMaximumFrameInBothDirections) {
  ConnectPair();
  ASSERT_TRUE(channels_[0]) << static_cast<int>(errors_[0]);
  ASSERT_TRUE(channels_[1]) << static_cast<int>(errors_[1]);
  EXPECT_EQ(channels_[0]->GetType(),
            cross_transfer::TransportType::kWebRtcData);
  EXPECT_TRUE(channels_[0]->IsSecure());
  std::vector<uint8_t> payload(cross_transfer::kMaxFramePayload);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i * 31);
  }
  FrameHeader sent;
  sent.cmd = FrameHeader::Command::kChunkData;
  sent.task_id[0] = 1;
  sent.payload_len = static_cast<uint32_t>(payload.size());
  sent.checksum = cross_transfer::ComputeCRC32C(payload);
  ASSERT_TRUE(channels_[0]->SendFrame(sent, payload));
  FrameHeader received;
  std::vector<uint8_t> actual;
  ASSERT_TRUE(channels_[1]->ReadNextFrame(received, actual));
  EXPECT_EQ(actual, payload);
  EXPECT_EQ(received.Serialize(), sent.Serialize());

  sent.cmd = FrameHeader::Command::kChunkAck;
  sent.payload_len = 0;
  sent.checksum = cross_transfer::ComputeCRC32C({});
  ASSERT_TRUE(channels_[1]->SendFrame(sent, {}));
  ASSERT_TRUE(channels_[0]->ReadNextFrame(received, actual));
  EXPECT_TRUE(actual.empty());
  EXPECT_EQ(received.Serialize(), sent.Serialize());
}

TEST_F(WebRtcTransferChannelTest, FileTransferJobsCompleteOverWebRtc) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto source = directory.GetPath().AppendASCII("webrtc-payload.bin");
  const auto destination = directory.GetPath().AppendASCII("received");
  std::string payload(2 * 1024 * 1024 + 17, '\0');
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<char>(i * 31);
  }
  ASSERT_TRUE(base::WriteFile(source, payload));
  ASSERT_TRUE(base::CreateDirectory(destination));

  constexpr char kInvitationId[] = "1234567890abcdef1234567890abcdef";
  std::atomic<int> relay_attempts{0};
  auto job_options = [&](size_t index) {
    cross_transfer::TestJobOptions options;
    options.role = index == 0 ? "send" : "receive";
    options.transport = "auto";
    options.task_id = kInvitationId;
    options.source_path = source.AsUTF8Unsafe();
    options.output_dir = destination.AsUTF8Unsafe();
    options.expected_file_name = source.BaseName().AsUTF8Unsafe();
    options.expected_file_size = payload.size();
    options.connect_timeout_ms = 10000;
    options.io_timeout_ms = 10000;
    options.stage_timeout_ms = 10000;
    options.automatic.invitation_id = kInvitationId;
    auto rtc_options = Options(index);
    rtc_options.invitation_id = kInvitationId;
    rtc_options.use_stun = false;
    options.automatic.webrtc = [rtc_options](
                                   const std::atomic<bool>* cancellation,
                                   TransportError* error) {
      return ConnectWebRtc(rtc_options, cancellation, error);
    };
    options.automatic.select_route = [](std::string route) {
      cross_transfer::PeerConnectionState state;
      state.ok = true;
      state.route = std::move(route);
      return state;
    };
    options.automatic.relay =
        [&relay_attempts](
            const std::atomic<bool>*,
            TransportError* error) -> std::unique_ptr<ITransportChannel> {
      ++relay_attempts;
      *error = TransportError::kIo;
      return nullptr;
    };
    return options;
  };

  cross_transfer::TestTransferJob receiver(job_options(1));
  cross_transfer::TestTransferJob sender(job_options(0));
  bool received = false;
  std::thread receiving([&] { received = receiver.Run(); });
  const bool sent = sender.Run();
  if (!sent) {
    receiver.Cancel();
  }
  receiving.join();

  const auto sender_view = sender.snapshot();
  const auto receiver_view = receiver.snapshot();
  ASSERT_TRUE(sent) << sender_view.error;
  ASSERT_TRUE(received) << receiver_view.error;
  EXPECT_EQ(relay_attempts.load(), 0);
  for (const auto& view : {sender_view, receiver_view}) {
    EXPECT_EQ(view.state, "completed");
    EXPECT_FALSE(view.active);
    EXPECT_EQ(view.transport, "direct");
    EXPECT_EQ(view.protocol, "webrtc");
    EXPECT_EQ(view.task_id, kInvitationId);
    EXPECT_EQ(view.bytes, payload.size());
    EXPECT_EQ(view.total, payload.size());
    EXPECT_EQ(view.percent, 100);
  }
  base::FilePath expected_path;
  ASSERT_TRUE(base::NormalizeFilePath(destination.Append(source.BaseName()),
                                      &expected_path));
  base::FilePath saved_path;
  ASSERT_TRUE(base::NormalizeFilePath(
      base::FilePath::FromUTF8Unsafe(receiver_view.saved_path), &saved_path));
  EXPECT_EQ(saved_path, expected_path);
  std::string actual;
  ASSERT_TRUE(base::ReadFileToString(
      base::FilePath::FromUTF8Unsafe(receiver_view.saved_path), &actual));
  EXPECT_EQ(actual, payload);
}

TEST_F(WebRtcTransferChannelTest,
       SuccessfulJobsReuseChannelForNewAndReverseTasks) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const std::array<std::shared_ptr<WebRtcConnectionCache>, 2> caches = {
      std::make_shared<WebRtcConnectionCache>(),
      std::make_shared<WebRtcConnectionCache>()};
  const std::array<std::string, 2> keys = {"account:device0:device1",
                                           "account:device1:device0"};
  std::array<const ITransportChannel*, 2> retained_channel{};
  std::atomic<int> cold_connections{0};
  std::atomic<int> relay_attempts{0};
  for (size_t round = 0; round < 3; ++round) {
    SCOPED_TRACE(round);
    const std::string invitation(32, static_cast<char>('1' + round));
    const auto source = directory.GetPath().AppendASCII(
        "source-" + std::to_string(round) + ".bin");
    const auto destination =
        directory.GetPath().AppendASCII("received-" + std::to_string(round));
    const std::string payload(round == 0 ? 2 * 1024 * 1024 + 17 : 31,
                              static_cast<char>('a' + round));
    ASSERT_TRUE(base::WriteFile(source, payload));
    ASSERT_TRUE(base::CreateDirectory(destination));
    const size_t sending_index = round == 2 ? 1 : 0;
    auto job_options = [&](size_t index) {
      const bool sending = index == sending_index;
      cross_transfer::TestJobOptions options;
      options.role = sending ? "send" : "receive";
      options.transport = "auto";
      options.task_id = invitation;
      options.source_path = source.AsUTF8Unsafe();
      options.output_dir = destination.AsUTF8Unsafe();
      options.expected_file_name = source.BaseName().AsUTF8Unsafe();
      options.expected_file_size = payload.size();
      options.io_timeout_ms = 10000;
      options.stage_timeout_ms = 10000;
      options.automatic.invitation_id = invitation;
      // Later tiny messages must use the local live connection even when the
      // server's cold-connection policy prefers HTTPS for their size.
      options.automatic.prefer_relay = round != 0;
      options.automatic.try_reuse = [cache = caches[index], key = keys[index],
                                     invitation, sending](
                                        const std::atomic<bool>* cancellation,
                                        TransportError* error) {
        return cache->TryReuse(key, invitation, sending, cancellation, error);
      };
      options.automatic.retain_direct =
          [&, index](std::unique_ptr<ITransportChannel> channel) {
            if (retained_channel[index]) {
              EXPECT_EQ(channel.get(), retained_channel[index]);
            } else {
              retained_channel[index] = channel.get();
            }
            caches[index]->Remember(keys[index], std::move(channel));
          };
      auto rtc_options = Options(index);
      rtc_options.sending = sending;
      rtc_options.invitation_id = invitation;
      options.automatic.webrtc = [rtc_options, &cold_connections](
                                     const std::atomic<bool>* cancellation,
                                     TransportError* error) {
        ++cold_connections;
        return ConnectWebRtc(rtc_options, cancellation, error);
      };
      options.automatic.select_route = [](std::string route) {
        cross_transfer::PeerConnectionState state;
        state.ok = true;
        state.route = std::move(route);
        return state;
      };
      options.automatic.relay =
          [&relay_attempts](
              const std::atomic<bool>*,
              TransportError* error) -> std::unique_ptr<ITransportChannel> {
        ++relay_attempts;
        *error = TransportError::kIo;
        return nullptr;
      };
      return options;
    };
    cross_transfer::TestTransferJob receiver(job_options(1 - sending_index));
    cross_transfer::TestTransferJob sender(job_options(sending_index));
    bool received = false;
    std::thread receiving([&] { received = receiver.Run(); });
    const bool sent = sender.Run();
    if (!sent) {
      receiver.Cancel();
    }
    receiving.join();
    ASSERT_TRUE(sent) << sender.snapshot().error;
    ASSERT_TRUE(received) << receiver.snapshot().error;
    EXPECT_EQ(cold_connections.load(), 2);
    EXPECT_EQ(relay_attempts.load(), 0);
    for (const auto& view : {sender.snapshot(), receiver.snapshot()}) {
      EXPECT_EQ(view.state, "completed");
      EXPECT_EQ(view.transport, "direct");
      EXPECT_EQ(view.protocol, "webrtc");
      EXPECT_EQ(view.task_id, invitation);
      EXPECT_EQ(view.bytes, payload.size());
    }
    std::string actual;
    ASSERT_TRUE(base::ReadFileToString(
        base::FilePath::FromUTF8Unsafe(receiver.snapshot().saved_path),
        &actual));
    EXPECT_EQ(actual, payload);
  }
  caches[0]->Clear();
  caches[1]->Clear();
  task_environment_.RunUntilIdle();
}

TEST_F(WebRtcTransferChannelTest, ReuseRejectsMismatchedInvitation) {
  ConnectPair();
  ASSERT_TRUE(channels_[0]);
  ASSERT_TRUE(channels_[1]);
  WebRtcConnectionCache sender;
  WebRtcConnectionCache receiver;
  sender.Remember("peer", std::move(channels_[0]));
  receiver.Remember("peer", std::move(channels_[1]));
  std::thread receiving([&] {
    channels_[1] = receiver.TryReuse("peer", std::string(32, '2'), false,
                                     &cancelled_, &errors_[1]);
  });
  channels_[0] = sender.TryReuse("peer", std::string(32, '1'), true,
                                 &cancelled_, &errors_[0]);
  receiving.join();
  EXPECT_FALSE(channels_[0]);
  EXPECT_FALSE(channels_[1]);
  EXPECT_EQ(errors_[1], TransportError::kProtocol);
  EXPECT_FALSE(sender.TryReuse("peer", std::string(32, '1'), true, &cancelled_,
                               nullptr));
}

TEST_F(WebRtcTransferChannelTest, ReuseRejectsResidualFrames) {
  ConnectPair();
  ASSERT_TRUE(channels_[0]);
  ASSERT_TRUE(channels_[1]);
  FrameHeader residual;
  residual.cmd = FrameHeader::Command::kChunkAck;
  residual.task_id[0] = 1;
  ASSERT_TRUE(channels_[0]->SendFrame(residual, {}));
  WebRtcConnectionCache cache;
  cache.Remember("peer", std::move(channels_[1]));
  TransportError error;
  EXPECT_FALSE(
      cache.TryReuse("peer", std::string(32, '1'), false, &cancelled_, &error));
  EXPECT_EQ(error, TransportError::kProtocol);
}

TEST_F(WebRtcTransferChannelTest, ReuseRejectsTwoSenders) {
  ConnectPair();
  ASSERT_TRUE(channels_[0]);
  ASSERT_TRUE(channels_[1]);
  WebRtcConnectionCache first;
  WebRtcConnectionCache second;
  first.Remember("peer", std::move(channels_[0]));
  second.Remember("peer", std::move(channels_[1]));
  std::thread connecting([&] {
    channels_[1] = second.TryReuse("peer", std::string(32, '1'), true,
                                   &cancelled_, &errors_[1]);
  });
  channels_[0] = first.TryReuse("peer", std::string(32, '1'), true, &cancelled_,
                                &errors_[0]);
  connecting.join();
  EXPECT_FALSE(channels_[0]);
  EXPECT_FALSE(channels_[1]);
  EXPECT_EQ(errors_[0], TransportError::kProtocol);
  EXPECT_EQ(errors_[1], TransportError::kProtocol);
}

TEST_F(WebRtcTransferChannelTest,
       ClearingOnePeerInvalidatesOtherCachedChannel) {
  ConnectPair();
  ASSERT_TRUE(channels_[0]);
  ASSERT_TRUE(channels_[1]);
  WebRtcConnectionCache first;
  WebRtcConnectionCache second;
  first.Remember("peer", std::move(channels_[0]));
  second.Remember("peer", std::move(channels_[1]));
  first.Clear();
  task_environment_.RunUntilIdle();
  TransportError error;
  EXPECT_FALSE(
      second.TryReuse("peer", std::string(32, '1'), true, &cancelled_, &error));
  EXPECT_EQ(error, TransportError::kNotConnected);
}

TEST_F(WebRtcTransferChannelTest, CancellationInterruptsReuseHandshake) {
  ConnectPair();
  ASSERT_TRUE(channels_[0]);
  ASSERT_TRUE(channels_[1]);
  WebRtcConnectionCache cache;
  cache.Remember("peer", std::move(channels_[0]));
  std::atomic<bool> cancel_reuse{false};
  std::thread connecting([&] {
    channels_[0] = cache.TryReuse("peer", std::string(32, '1'), true,
                                  &cancel_reuse, &errors_[0]);
  });
  FrameHeader hello;
  std::vector<uint8_t> payload;
  channels_[1]->SetIoTimeout(2000);
  const bool got_hello = channels_[1]->ReadNextFrame(hello, payload);
  cancel_reuse.store(true);
  connecting.join();
  EXPECT_TRUE(got_hello);
  EXPECT_FALSE(channels_[0]);
  EXPECT_EQ(errors_[0], TransportError::kCancelled);
}

TEST_F(WebRtcTransferChannelTest, RejectsIncorrectDtlsFingerprint) {
  corrupt_fingerprint_ = true;
  ConnectPair(2500);
  EXPECT_FALSE(channels_[0]);
  EXPECT_FALSE(channels_[1]);
  EXPECT_FALSE(endpoints_[0].confirmed);
  EXPECT_FALSE(endpoints_[1].confirmed);
}

TEST_F(WebRtcTransferChannelTest, DisabledStunIgnoresAdvertisedServer) {
  use_stun_ = false;
  // An invalid server port would make PeerConnection creation fail if this
  // advertised server were passed to WebRTC. Host ICE still connects normally.
  stun_urls_ = {"stun:127.0.0.1:invalid"};
  ConnectPair();
  ASSERT_TRUE(channels_[0]) << static_cast<int>(errors_[0]);
  ASSERT_TRUE(channels_[1]) << static_cast<int>(errors_[1]);
}

TEST_F(WebRtcTransferChannelTest, OptionalPublicStunProducesMappedCandidate) {
  const auto stun_url =
      base::Environment::Create()->GetVar("XENON_TEST_STUN_URL");
  if (!stun_url) {
    GTEST_SKIP() << "Set XENON_TEST_STUN_URL to run the public STUN probe";
  }
  auto options = Options(0);
  options.use_stun = true;
  options.connect_timeout_ms = 8000;
  bool mapped_candidate = false;
  options.exchange = [&](std::optional<WebRtcDescription>,
                         std::vector<WebRtcIceCandidate> candidates) {
    for (const auto& candidate : candidates) {
      if (candidate.candidate.find(" typ srflx ") != std::string::npos) {
        mapped_candidate = true;
        cancelled_.store(true);
      }
    }
    WebRtcSignalState signal;
    signal.ok = true;
    signal.route = "pending";
    signal.stun_urls = {*stun_url};
    return signal;
  };
  TransportError error;
  EXPECT_FALSE(ConnectWebRtc(options, &cancelled_, &error));
  EXPECT_TRUE(mapped_candidate)
      << "No server-reflexive candidate from " << *stun_url;
}

TEST_F(WebRtcTransferChannelTest, ClosingSenderFlushesLastFrame) {
  ConnectPair();
  ASSERT_TRUE(channels_[0]);
  ASSERT_TRUE(channels_[1]);
  std::vector<uint8_t> payload(cross_transfer::kMaxFramePayload, 0x5a);
  FrameHeader sent;
  sent.cmd = FrameHeader::Command::kChunkData;
  sent.task_id[0] = 1;
  sent.payload_len = static_cast<uint32_t>(payload.size());
  sent.checksum = cross_transfer::ComputeCRC32C(payload);
  ASSERT_TRUE(channels_[0]->SendFrame(sent, payload));
  channels_[0].reset();
  FrameHeader received;
  std::vector<uint8_t> actual;
  ASSERT_TRUE(channels_[1]->ReadNextFrame(received, actual));
  EXPECT_EQ(actual, payload);
  EXPECT_EQ(received.Serialize(), sent.Serialize());
}

TEST_F(WebRtcTransferChannelTest, CancellationInterruptsWaitingForOffer) {
  auto options = Options(1);
  std::promise<void> exchanged;
  auto ready = exchanged.get_future();
  options.exchange = [&exchanged, first = true](
                         std::optional<WebRtcDescription>,
                         std::vector<WebRtcIceCandidate>) mutable {
    if (first) {
      first = false;
      exchanged.set_value();
    }
    WebRtcSignalState signal;
    signal.ok = true;
    signal.route = "pending";
    return signal;
  };
  std::thread connecting(
      [&] { channels_[1] = ConnectWebRtc(options, &cancelled_, &errors_[1]); });
  ready.wait();
  cancelled_.store(true);
  connecting.join();
  EXPECT_FALSE(channels_[1]);
  EXPECT_EQ(errors_[1], TransportError::kCancelled);
}

TEST_F(WebRtcTransferChannelTest, DoesNotConnectWhenRelayAlreadyChosen) {
  auto options = Options(0);
  options.exchange = [](std::optional<WebRtcDescription>,
                        std::vector<WebRtcIceCandidate>) {
    WebRtcSignalState signal;
    signal.ok = true;
    signal.route = "relay";
    return signal;
  };
  TransportError error;
  EXPECT_FALSE(ConnectWebRtc(options, &cancelled_, &error));
  EXPECT_EQ(error, TransportError::kNotConnected);
}

TEST_F(WebRtcTransferChannelTest, RejectsUnboundedSignaling) {
  auto options = Options(0);
  options.exchange = [](std::optional<WebRtcDescription>,
                        std::vector<WebRtcIceCandidate>) {
    WebRtcSignalState signal;
    signal.ok = true;
    signal.route = "pending";
    signal.description = WebRtcDescription{"answer", std::string(65537, 'a')};
    return signal;
  };
  TransportError error;
  EXPECT_FALSE(ConnectWebRtc(options, &cancelled_, &error));
  EXPECT_NE(error, TransportError::kNone);
}

TEST_F(WebRtcTransferChannelTest, RejectsTooManyIceCandidates) {
  auto options = Options(0);
  options.exchange = [](std::optional<WebRtcDescription>,
                        std::vector<WebRtcIceCandidate>) {
    WebRtcSignalState signal;
    signal.ok = true;
    signal.route = "pending";
    signal.candidates.resize(65);
    return signal;
  };
  TransportError error;
  EXPECT_FALSE(ConnectWebRtc(options, &cancelled_, &error));
  EXPECT_NE(error, TransportError::kNone);
}

TEST_F(WebRtcTransferChannelTest, ReadDeadlineAndCloseAreBounded) {
  ConnectPair();
  ASSERT_TRUE(channels_[0]);
  ASSERT_TRUE(channels_[1]);
  channels_[0]->SetIoTimeout(50);
  FrameHeader header;
  std::vector<uint8_t> payload;
  EXPECT_FALSE(channels_[0]->ReadNextFrame(header, payload));
  EXPECT_EQ(channels_[0]->LastError(), TransportError::kTimeout);
  channels_[0]->Close();
  channels_[0]->Close();
  EXPECT_FALSE(channels_[0]->IsConnected());
}

struct CacheChannelState {
  std::atomic<int> destroyed{0};
  std::atomic<bool> has_cancellation{false};
};

class CacheTestChannel : public ITransportChannel {
 public:
  explicit CacheTestChannel(std::shared_ptr<CacheChannelState> state,
                            bool secure = true)
      : state_(std::move(state)), secure_(secure) {}
  ~CacheTestChannel() override { ++state_->destroyed; }
  bool SendFrame(const FrameHeader&, std::span<const uint8_t>) override {
    return false;
  }
  bool ReadNextFrame(FrameHeader&, std::vector<uint8_t>&) override {
    return false;
  }
  void Close() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }
  bool IsSecure() const override { return secure_; }
  cross_transfer::TransportType GetType() const override {
    return cross_transfer::TransportType::kWebRtcData;
  }
  void SetCancellationFlag(const std::atomic<bool>* flag) override {
    state_->has_cancellation.store(flag != nullptr);
  }

 private:
  const std::shared_ptr<CacheChannelState> state_;
  const bool secure_;
  bool connected_ = true;
};

class WebRtcConnectionCacheTest : public testing::Test {
 protected:
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
};

TEST_F(WebRtcConnectionCacheTest, ExpiresIdleChannelWithoutAnotherLookup) {
  WebRtcConnectionCache cache;
  const auto state = std::make_shared<CacheChannelState>();
  auto channel = std::make_unique<CacheTestChannel>(state);
  std::atomic<bool> cancellation{false};
  channel->SetCancellationFlag(&cancellation);
  cache.Remember("account:local:peer", std::move(channel));
  EXPECT_FALSE(state->has_cancellation.load());
  task_environment_.FastForwardBy(base::Minutes(9));
  EXPECT_EQ(state->destroyed.load(), 0);
  task_environment_.FastForwardBy(base::Minutes(1));
  EXPECT_EQ(state->destroyed.load(), 1);
  TransportError error;
  EXPECT_FALSE(cache.TryReuse("account:local:peer", std::string(32, '1'), true,
                              &cancellation, &error));
  EXPECT_EQ(error, TransportError::kNotConnected);
}

TEST_F(WebRtcConnectionCacheTest, LimitsIdleConnectionsAndEvictsOldest) {
  WebRtcConnectionCache cache;
  std::vector<std::shared_ptr<CacheChannelState>> states;
  for (int i = 0; i < 9; ++i) {
    states.push_back(std::make_shared<CacheChannelState>());
    cache.Remember("peer-" + std::to_string(i),
                   std::make_unique<CacheTestChannel>(states.back()));
    task_environment_.FastForwardBy(base::Seconds(1));
  }
  EXPECT_EQ(states.front()->destroyed.load(), 1);
  for (size_t i = 1; i < states.size(); ++i) {
    EXPECT_EQ(states[i]->destroyed.load(), 0);
  }
  cache.Clear();
  task_environment_.RunUntilIdle();
  for (const auto& state : states) {
    EXPECT_EQ(state->destroyed.load(), 1);
  }
}

TEST_F(WebRtcConnectionCacheTest,
       ClearRejectsLateRememberAndKeepsJobsDetached) {
  WebRtcConnectionCache cache;
  cache.Clear();
  const auto state = std::make_shared<CacheChannelState>();
  auto channel = std::make_unique<CacheTestChannel>(state);
  std::atomic<bool> cancellation{false};
  channel->SetCancellationFlag(&cancellation);
  cache.Remember("peer", std::move(channel));
  EXPECT_FALSE(state->has_cancellation.load());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(state->destroyed.load(), 1);
  EXPECT_FALSE(cache.TryReuse("peer", std::string(32, '1'), true, &cancellation,
                              nullptr));
}

TEST_F(WebRtcConnectionCacheTest, DoesNotRetainUnauthenticatedChannel) {
  WebRtcConnectionCache cache;
  const auto state = std::make_shared<CacheChannelState>();
  cache.Remember("peer", std::make_unique<CacheTestChannel>(state, false));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(state->destroyed.load(), 1);
  EXPECT_FALSE(
      cache.TryReuse("peer", std::string(32, '1'), true, nullptr, nullptr));
}

}  // namespace
}  // namespace xenon
