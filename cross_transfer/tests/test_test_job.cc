#include "cross_transfer/test_job.h"

#include "cross_transfer/streaming_file_io.h"
#include "cross_transfer/https_relay_channel.h"
#include "cross_transfer/transfer_session.h"
#include "test_arguments.h"

#include <atomic>
#include <array>
#include <cassert>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <random>
#include <thread>

namespace {
using namespace cross_transfer;
using Clock = std::chrono::steady_clock;

struct TemporaryDirectory {
  std::filesystem::path path;
  TemporaryDirectory() {
    std::random_device random;
    for (int i = 0; i < 100; ++i) {
      path = std::filesystem::temp_directory_path() /
          ("cross-transfer-job-" + std::to_string(random()));
      if (std::filesystem::create_directory(path)) return;
    }
    assert(false);
  }
  ~TemporaryDirectory() { std::filesystem::remove_all(path); }
};

uint16_t WaitForListener(TestTransferJob& job) {
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < deadline) {
    auto view = job.snapshot();
    if (view.listening_port) return view.listening_port;
    if (view.state == "failed" || view.state == "cancelled") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return 0;
}

TestJobOptions ReceiveOptions(const std::filesystem::path& output) {
  TestJobOptions options;
  options.role = "receive";
  options.output_dir = output.string();
  options.host = "127.0.0.1";
  options.port = 0;
  options.ca_file = "fixture-ca.pem";
  options.certificate_file = "fixture-receiver.pem";
  options.private_key_file = "fixture-receiver.key";
  options.peer_name = "sender.test";
  options.connect_timeout_ms = 5000;
  options.io_timeout_ms = 5000;
  return options;
}

void TestValidationAndStickyCancellation() {
  TestJobOptions options;
  options.role = "unsupported";
  options.bearer_token = "private-sentinel-token";
  options.private_key_file = "private-sentinel-key";
  TestTransferJob invalid(options);
  assert(!invalid.Run());
  auto view = invalid.snapshot();
  assert(view.state == "failed" && !view.active && !view.error.empty());
  assert(view.error.find("private-sentinel") == std::string::npos);
  assert(view.status.find("private-sentinel") == std::string::npos);
  assert(!invalid.Run());
  invalid.Cancel();
  assert(invalid.snapshot().state == "failed");

  TestTransferJob cancelled(TestJobOptions{});
  cancelled.Cancel();
  assert(!cancelled.Run());
  view = cancelled.snapshot();
  assert(view.state == "cancelled" && !view.active && view.error.empty());

  options = ReceiveOptions("unused");
  options.task_id = std::string(32, '0');
  TestTransferJob zero_identity(options);
  assert(!zero_identity.Run());
  assert(zero_identity.snapshot().error.find("Task ID") != std::string::npos);

  options.private_key_file.clear();
  options.task_id.clear();
  TestTransferJob no_tls(options);
  assert(!no_tls.Run());
  assert(no_tls.snapshot().error.find("certificate") != std::string::npos);
}

void TestCancelListeningAndConcurrentSnapshots() {
  TemporaryDirectory directory;
  auto options = ReceiveOptions(directory.path);
  options.ca_file.clear();  // System trust is allowed; there is no plaintext mode.
  TestTransferJob job(options);
  bool result = true;
  std::thread worker([&] { result = job.Run(); });
  assert(WaitForListener(job));
  std::atomic<bool> stop_polling{false};
  std::thread poller([&] {
    while (!stop_polling) {
      auto view = job.snapshot();
      assert(view.percent >= 0 && view.percent <= 100);
      std::this_thread::yield();
    }
  });
  const auto start = Clock::now();
  job.Cancel();
  worker.join();
  stop_polling = true;
  poller.join();
  assert(Clock::now() - start < std::chrono::seconds(2));
  assert(!result && job.snapshot().state == "cancelled");
  assert(!job.snapshot().active);
}

void TestCancelSourcePreparation() {
  TemporaryDirectory directory;
  const auto source = directory.path / "preparation.bin";
  {
    // A sparse source makes the hash stage observable without allocating the
    // complete file in memory or requiring a network connection.
    std::ofstream file(source, std::ios::binary);
    file.seekp(256 * 1024 * 1024 - 1);
    file.put('\0');
    assert(file.good());
  }
  TestJobOptions options;
  options.source_path = source.string();
  options.host = "127.0.0.1";
  options.port = 1;
  options.certificate_file = "unused.pem";
  options.private_key_file = "unused.key";
  options.peer_name = "receiver.test";
  TestTransferJob job(options);
  bool result = true;
  std::thread worker([&] { result = job.Run(); });
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < deadline && job.snapshot().state != "preparing")
    std::this_thread::yield();
  assert(job.snapshot().state == "preparing");
  job.Cancel();
  worker.join();
  assert(!result && job.snapshot().state == "cancelled");
  assert(!job.snapshot().active && job.snapshot().task_id.size() == 32);
}

void SetCertificates(TestJobOptions* options, const std::filesystem::path& directory,
                     const std::string& role) {
  options->ca_file = (directory / "ca.pem").string();
  options->certificate_file = (directory / (role + ".pem")).string();
  options->private_key_file = (directory / (role + ".key")).string();
  options->peer_name = role == "sender" ? "receiver.test" : "sender.test";
}

void TestRealMutualTls(const std::filesystem::path& certificates) {
  TemporaryDirectory directory;
  const auto source = directory.path / "job-payload.bin";
  {
    std::ofstream file(source, std::ios::binary);
    const std::string block(1024 * 1024, 'j');
    file.write(block.data(), block.size());
  }
  auto receiver_options = ReceiveOptions(directory.path / "received");
  SetCertificates(&receiver_options, certificates, "receiver");
  receiver_options.expected_file_name = "job-payload.bin";
  receiver_options.expected_file_size = 1024 * 1024;
  TestTransferJob receiver(receiver_options);
  bool received = false;
  std::thread receive_thread([&] { received = receiver.Run(); });
  const auto port = WaitForListener(receiver);
  assert(port);
  TestJobOptions sender_options;
  sender_options.source_path = source.string();
  sender_options.host = "127.0.0.1";
  sender_options.port = port;
  SetCertificates(&sender_options, certificates, "sender");
  TestTransferJob sender(sender_options);
  const bool sent = sender.Run();
  receive_thread.join();
  if (!sent) std::cerr << "sender: " << sender.snapshot().error << '\n';
  if (!received) std::cerr << "receiver: " << receiver.snapshot().error << '\n';
  assert(sent && received);
  auto sender_view = sender.snapshot();
  auto receiver_view = receiver.snapshot();
  assert(sender_view.state == "completed" && receiver_view.state == "completed");
  assert(sender_view.task_id.size() == 32 && sender_view.task_id == receiver_view.task_id);
  assert(sender_view.bytes == 1024 * 1024 && receiver_view.bytes == sender_view.bytes);
  assert(StreamingFileReader::ComputeFileSHA256(source.string()) ==
         StreamingFileReader::ComputeFileSHA256(receiver_view.saved_path));

  // A second attempt must not overwrite a destination without explicit consent.
  receiver_options.port = 0;
  TestTransferJob denied_receiver(receiver_options);
  received = true;
  std::thread denied_thread([&] { received = denied_receiver.Run(); });
  sender_options.port = WaitForListener(denied_receiver);
  assert(sender_options.port);
  TestTransferJob denied_sender(sender_options);
  assert(!denied_sender.Run());
  denied_thread.join();
  assert(!received && denied_receiver.snapshot().state == "failed");
  assert(StreamingFileReader::ComputeFileSHA256(source.string()) ==
         StreamingFileReader::ComputeFileSHA256(receiver_view.saved_path));

  // An exact committed task may reconnect after its ACK was lost without
  // enabling overwrite for unrelated files or new jobs.
  assert(!receiver_options.allow_overwrite);
  receiver_options.task_id = sender_view.task_id;
  TestTransferJob resumed_receiver(receiver_options);
  std::thread resumed_thread([&] { received = resumed_receiver.Run(); });
  sender_options.port = WaitForListener(resumed_receiver);
  assert(sender_options.port);
  sender_options.task_id = sender_view.task_id;
  TestTransferJob resumed_sender(sender_options);
  assert(resumed_sender.Run());
  resumed_thread.join();
  assert(received && resumed_receiver.snapshot().task_id == sender_view.task_id);

  // Explicit overwrite consent must reach the atomic store commit.
  receiver_options.task_id.clear();
  receiver_options.allow_overwrite = true;
  TestTransferJob overwrite_receiver(receiver_options);
  std::thread overwrite_thread([&] { received = overwrite_receiver.Run(); });
  sender_options.port = WaitForListener(overwrite_receiver);
  assert(sender_options.port);
  sender_options.task_id.clear();
  TestTransferJob overwrite_sender(sender_options);
  assert(overwrite_sender.Run());
  overwrite_thread.join();
  assert(received && overwrite_receiver.snapshot().state == "completed");

  // Account invitations authorize one exact file name and size. A valid TLS
  // peer must not substitute another file after the user accepts the offer.
  for (bool mismatch_name : {true, false}) {
    auto bound = receiver_options;
    bound.output_dir = (directory.path / (mismatch_name ? "name" : "size")).string();
    bound.allow_overwrite = false;
    if (mismatch_name) bound.expected_file_name = "different.bin";
    else bound.expected_file_size = 7;
    TestTransferJob bound_receiver(bound);
    std::thread bound_thread([&] { received = bound_receiver.Run(); });
    sender_options.port = WaitForListener(bound_receiver);
    assert(sender_options.port);
    TestTransferJob bound_sender(sender_options);
    assert(!bound_sender.Run());
    bound_thread.join();
    assert(!received);
    assert(!std::filesystem::exists(std::filesystem::path(bound.output_dir) / "job-payload.bin"));
  }
}

void TestRealHttpsRelay(const std::string& ca_file, uint16_t port) {
  TemporaryDirectory directory;
  const auto source = directory.path / "relay-job.bin";
  {
    std::ofstream file(source, std::ios::binary);
    const std::string block(64 * 1024, 'r');
    file.write(block.data(), block.size());
  }
  auto receiver_options = ReceiveOptions(directory.path / "received");
  receiver_options.transport = "relay";
  receiver_options.port = port;
  receiver_options.ca_file = ca_file;
  {
    std::ifstream roots(ca_file, std::ios::binary);
    receiver_options.ca_pem.assign(std::istreambuf_iterator<char>(roots), {});
    assert(!receiver_options.ca_pem.empty());
    receiver_options.ca_file.clear();
  }
  receiver_options.expected_file_name = "relay-job.bin";
  receiver_options.expected_file_size = 64 * 1024;
  receiver_options.room_id = "jobs";
  receiver_options.connection_id = std::string(32, 'd');
  receiver_options.bearer_token = std::string(64, '2');
  TestTransferJob receiver(receiver_options);
  bool received = false;
  std::thread receive_thread([&] { received = receiver.Run(); });
  auto sender_options = receiver_options;
  sender_options.role = "send";
  sender_options.source_path = source.string();
  sender_options.bearer_token = std::string(64, '1');
  TestTransferJob sender(sender_options);
  bool sent = sender.Run();
  receive_thread.join();
  if (!sent) std::cerr << "relay sender: " << sender.snapshot().error << '\n';
  if (!received) std::cerr << "relay receiver: " << receiver.snapshot().error << '\n';
  assert(sent && received);
  assert(sender.snapshot().task_id == receiver.snapshot().task_id);
  assert(StreamingFileReader::ComputeFileSHA256(source.string()) ==
         StreamingFileReader::ComputeFileSHA256(receiver.snapshot().saved_path));

  // Cancelling an established relay read keeps the job token set even when
  // ReceiveFile enters and initializes its private session state.
  receiver_options.connection_id = std::string(32, 'e');
  TestTransferJob waiting_receiver(receiver_options);
  std::thread waiting_thread([&] { received = waiting_receiver.Run(); });
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (Clock::now() < deadline && waiting_receiver.snapshot().state != "handshaking")
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  assert(waiting_receiver.snapshot().state == "handshaking");
  const auto start = Clock::now();
  waiting_receiver.Cancel();
  waiting_thread.join();
  assert(!received && waiting_receiver.snapshot().state == "cancelled");
  assert(Clock::now() - start < std::chrono::seconds(2));
}

// Models the account server's locked, per-invitation agreement. The transport
// itself still generates identities and uses real TCP/mTLS, and fallback tests
// use the real HTTPS relay fixture.
class AutoCoordinator {
 public:
  AutoConnectionOptions Options(bool sending, const std::string& id,
                                const std::string& ca_file = {}, uint16_t port = 0,
                                const std::string& room = {}) {
    AutoConnectionOptions options;
    options.sending = sending;
    options.prefer_relay = prefer_relay;
    options.invitation_id = id;
    options.direct_timeout_ms = 2500;
    options.negotiation_timeout_ms = 8000;
    const size_t role = sending ? 0 : 1;
    options.exchange = [this, role](std::string certificate,
                                    std::vector<CandidateEndpoint> endpoints) {
      std::lock_guard lock(mutex_);
      assert(!prefer_relay);
      assert(certificate.find("PRIVATE") == std::string::npos);
      assert(!certificate.empty() && certificate.size() <= 8192);
      if (!certificates_[role].empty()) assert(certificates_[role] == certificate);
      certificates_[role] = std::move(certificate);
      endpoints_[role] = std::move(endpoints);
      published[role] = true;
      return State(role);
    };
    options.select_route = [this, role](std::string requested) {
      std::lock_guard lock(mutex_);
      if (requested == "relay") {
        route_ = "relay";
      } else if (route_ != "relay") {
        if (!(withhold_sender_ready && role == 0)) ready_[role] = true;
        if (ready_[0] && ready_[1]) route_ = "direct";
      }
      return State(role);
    };
    options.relay = [this, sending, id, ca_file, port, room](
                        const std::atomic<bool>* cancelled, TransportError* error)
        -> std::unique_ptr<ITransportChannel> {
      ++relay_connections;
      if (port == 0) return nullptr;
      HttpsRelayOptions relay;
      relay.host = "127.0.0.1";
      relay.port = port;
      relay.room_id = room;
      relay.connection_id = id;
      relay.ca_file = ca_file;
      relay.role = sending ? RelayRole::kSender : RelayRole::kReceiver;
      relay.bearer_token = std::string(64, sending ? '1' : '2');
      relay.timeout_ms = 5000;
      return HttpsRelayChannel::Connect(std::move(relay), cancelled, error);
    };
    return options;
  }

  std::array<std::atomic<bool>, 2> published{};
  std::atomic<int> relay_connections{0};
  bool unreachable{false};
  bool wrong_certificate{false};
  bool withhold_sender_ready{false};
  bool prefer_relay{false};


 private:
  PeerConnectionState State(size_t role) {
    const size_t peer = 1 - role;
    auto candidates = endpoints_[peer];
    if (unreachable) {
      // A numeric unicast address with no listening service forces the same
      // fallback as a firewall without relying on a network timeout.
      for (auto& endpoint : candidates) endpoint.port = 1;
    }
    return {true, route_, wrong_certificate && role == 0
                             ? certificates_[role] : certificates_[peer],
            std::move(candidates)};
  }
  std::mutex mutex_;
  std::array<std::string, 2> certificates_;
  std::array<std::vector<CandidateEndpoint>, 2> endpoints_;
  std::array<bool, 2> ready_{};
  std::string route_{"pending"};
};

void TestAutomaticTransfer(AutoCoordinator& coordinator, const std::string& id,
                           const std::string& expected_route,
                           const std::string& ca_file = {}, uint16_t port = 0,
                           const std::string& room = {}) {
  TemporaryDirectory directory;
  const auto source = directory.path / "automatic.bin";
  {
    std::ofstream file(source, std::ios::binary);
    const std::string data(64 * 1024, 'a');
    file.write(data.data(), data.size());
  }
  TestJobOptions receive;
  receive.role = "receive";
  receive.transport = "auto";
  receive.task_id = id;
  receive.expected_file_name = "automatic.bin";
  receive.expected_file_size = 64 * 1024;
  receive.output_dir = (directory.path / "received").string();
  receive.automatic = coordinator.Options(false, id, ca_file, port, room);
  TestTransferJob receiver(receive);
  bool received = false;
  std::thread worker([&] { received = receiver.Run(); });
  auto send = receive;
  send.role = "send";
  send.source_path = source.string();
  send.automatic = coordinator.Options(true, id, ca_file, port, room);
  TestTransferJob sender(send);
  const bool sent = sender.Run();
  worker.join();
  if (!sent) std::cerr << "automatic sender: " << sender.snapshot().error << '\n';
  if (!received) std::cerr << "automatic receiver: " << receiver.snapshot().error << '\n';
  assert(sent && received);
  assert(sender.snapshot().completion_requested);
  assert(!receiver.snapshot().completion_requested);
  assert(sender.snapshot().transport == expected_route);
  assert(receiver.snapshot().transport == expected_route);
  assert(StreamingFileReader::ComputeFileSHA256(source.string()) ==
         StreamingFileReader::ComputeFileSHA256(receiver.snapshot().saved_path));
  assert(coordinator.relay_connections == (expected_route == "direct" ? 0 : 2));
}

void TestAutomaticDirectAndCancellation() {
  AutoCoordinator direct;
  TestAutomaticTransfer(direct, std::string(32, '7'), "direct");

  AutoCoordinator cancelled;
  std::atomic<bool> cancellation{false};
  auto options = cancelled.Options(false, std::string(32, '8'));
  // Preparing a large sender must not consume the receiver's short direct
  // budget before that sender has even published its ready identity.
  options.direct_timeout_ms = 100;
  TransportError error{TransportError::kNone};
  std::unique_ptr<ITransportChannel> result;
  std::thread receiver([&] { result = ConnectAutomatically(options, &cancellation, &error); });
  const auto deadline = Clock::now() + std::chrono::seconds(3);
  while (!cancelled.published[1] && Clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(cancelled.published[1]);
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  assert(cancelled.relay_connections == 0);
  cancellation = true;
  receiver.join();
  assert(!result && error == TransportError::kCancelled);
  assert(cancelled.relay_connections == 0);
}

void TestAutomaticFallbacks(const std::string& ca_file, uint16_t port) {
  AutoCoordinator unreachable;
  unreachable.unreachable = true;
  TestAutomaticTransfer(unreachable, std::string(32, '9'), "relay", ca_file, port,
                        "auto-unreachable");
  AutoCoordinator wrong_certificate;
  wrong_certificate.wrong_certificate = true;
  TestAutomaticTransfer(wrong_certificate, std::string(32, 'a'), "relay", ca_file, port,
                        "auto-certificate");
  AutoCoordinator incomplete_agreement;
  incomplete_agreement.withhold_sender_ready = true;
  TestAutomaticTransfer(incomplete_agreement, std::string(32, 'b'), "relay", ca_file, port,
                        "auto-agreement");
  AutoCoordinator small_payload;
  small_payload.prefer_relay = true;
  TestAutomaticTransfer(small_payload, std::string(32, 'd'), "relay", ca_file, port,
                        "auto-policy");
  assert(!small_payload.published[0] && !small_payload.published[1]);
}

// Route selection has no payload I/O: these channels exercise adapter admission
// and fallback without depending on an ICE server or a second browser process.
class RoutingChannel final : public ITransportChannel {
 public:
  RoutingChannel(bool connected, bool secure, TransportType type,
                 std::function<void()> destroyed = {})
      : connected_(connected), secure_(secure), type_(type),
        destroyed_(std::move(destroyed)) {}
  ~RoutingChannel() override {
    if (destroyed_) destroyed_();
  }
  bool SendFrame(const FrameHeader&, std::span<const uint8_t>) override {
    assert(false);
    return false;
  }
  bool ReadNextFrame(FrameHeader&, std::vector<uint8_t>&) override {
    assert(false);
    return false;
  }
  void Close() override { connected_ = false; }
  bool IsConnected() const override { return connected_; }
  bool IsSecure() const override { return secure_; }
  TransportType GetType() const override { return type_; }

 private:
  bool connected_;
  bool secure_;
  TransportType type_;
  std::function<void()> destroyed_;
};

struct WebRtcRoutingProbe {
  AutoConnectionOptions Options() {
    AutoConnectionOptions options;
    options.invitation_id = std::string(32, 'e');
    options.webrtc = [this](const std::atomic<bool>* flag, TransportError* error)
        -> std::unique_ptr<ITransportChannel> {
      assert(flag == &cancelled);
      ++webrtc_calls;
      if (cancel_in_callback) cancelled = true;
      *error = callback_error;
      if (!provide_channel) return nullptr;
      return std::make_unique<RoutingChannel>(connected, secure, type);
    };
    options.exchange = [this](std::string, std::vector<CandidateEndpoint>) {
      ++tls_exchanges;
      return PeerConnectionState{};
    };
    options.select_route = [this](std::string route) {
      ++route_selections;
      assert(route == "relay" && !cancelled);
      return PeerConnectionState{true, "relay", {}, {}};
    };
    options.relay = [this](const std::atomic<bool>* flag, TransportError* error)
        -> std::unique_ptr<ITransportChannel> {
      assert(flag == &cancelled && !cancelled);
      assert(route_selections == 1);
      ++relay_calls;
      *error = TransportError::kNone;
      return std::make_unique<RoutingChannel>(true, true,
                                             TransportType::kCloudRelay443);
    };
    return options;
  }

  std::atomic<bool> cancelled{false};
  int webrtc_calls{0};
  int tls_exchanges{0};
  int route_selections{0};
  int relay_calls{0};
  bool provide_channel{true};
  bool connected{true};
  bool secure{true};
  bool cancel_in_callback{false};
  TransportType type{TransportType::kWebRtcData};
  TransportError callback_error{TransportError::kNone};
};

void TestAutomaticWebRtcRouting() {
  WebRtcRoutingProbe success;
  auto options = success.Options();
  // An ICE adapter supplies its own signaling; TLS certificate exchange is
  // optional when that adapter is present.
  options.exchange = {};
  TransportError error = TransportError::kIo;
  auto channel = ConnectAutomatically(options, &success.cancelled, &error);
  assert(channel && channel->GetType() == TransportType::kWebRtcData);
  assert(error == TransportError::kNone && success.webrtc_calls == 1);
  assert(success.tls_exchanges == 0 && success.route_selections == 0 &&
         success.relay_calls == 0);

  // A callback cannot admit an unauthenticated, closed or mislabeled transport.
  // Every failed attempt goes straight to the agreed HTTPS relay, skipping TLS.
  for (int failure = 0; failure < 5; ++failure) {
    WebRtcRoutingProbe rejected;
    if (failure == 0) {
      rejected.provide_channel = false;
      rejected.callback_error = TransportError::kTimeout;
    } else if (failure == 1) {
      rejected.connected = false;
    } else if (failure == 2) {
      rejected.secure = false;
    } else if (failure == 3) {
      rejected.type = TransportType::kLanDirectTcp;
    } else {
      rejected.type = TransportType::kCloudRelay443;
    }
    channel = ConnectAutomatically(rejected.Options(), &rejected.cancelled, &error);
    assert(channel && channel->GetType() == TransportType::kCloudRelay443);
    assert(error == TransportError::kNone && rejected.webrtc_calls == 1);
    assert(rejected.tls_exchanges == 0 && rejected.route_selections == 1 &&
           rejected.relay_calls == 1);
  }

  WebRtcRoutingProbe small_payload;
  options = small_payload.Options();
  options.prefer_relay = true;
  channel = ConnectAutomatically(options, &small_payload.cancelled, &error);
  assert(channel && channel->GetType() == TransportType::kCloudRelay443);
  assert(small_payload.webrtc_calls == 0 && small_payload.tls_exchanges == 0);
  assert(small_payload.route_selections == 1 && small_payload.relay_calls == 1);

  // Cancellation may come from the job flag or from the adapter's independent
  // account-session lifetime. Neither permits creating another connection.
  for (int stage = 0; stage < 3; ++stage) {
    WebRtcRoutingProbe cancellation;
    if (stage == 0) cancellation.cancelled = true;
    if (stage == 1) cancellation.cancel_in_callback = true;
    if (stage == 2) {
      cancellation.provide_channel = false;
      cancellation.callback_error = TransportError::kCancelled;
    }
    error = TransportError::kNone;
    channel = ConnectAutomatically(cancellation.Options(), &cancellation.cancelled,
                                   &error);
    assert(!channel && error == TransportError::kCancelled);
    assert(cancellation.webrtc_calls == (stage == 0 ? 0 : 1));
    assert(cancellation.tls_exchanges == 0 && cancellation.route_selections == 0 &&
           cancellation.relay_calls == 0);
  }
}

void TestAutomaticWebRtcReuseRouting() {
  WebRtcRoutingProbe cached;
  auto options = cached.Options();
  options.prefer_relay = true;
  int reuse_calls = 0;
  options.try_reuse = [&](const std::atomic<bool>* flag, TransportError* error)
      -> std::unique_ptr<ITransportChannel> {
    assert(flag == &cached.cancelled && *error == TransportError::kNone);
    ++reuse_calls;
    return std::make_unique<RoutingChannel>(true, true, TransportType::kWebRtcData);
  };
  TransportError error = TransportError::kCancelled;
  auto channel = ConnectAutomatically(options, &cached.cancelled, &error);
  assert(channel && channel->GetType() == TransportType::kWebRtcData);
  assert(error == TransportError::kNone && reuse_calls == 1);
  assert(cached.webrtc_calls == 0 && cached.route_selections == 0 && cached.relay_calls == 0);

  for (bool prefer_relay : {false, true}) {
    for (int failure = 0; failure < 5; ++failure) {
      WebRtcRoutingProbe fallback;
      options = fallback.Options();
      options.prefer_relay = prefer_relay;
      bool destroyed = false;
      options.try_reuse = [&](const std::atomic<bool>*, TransportError* attempt_error)
          -> std::unique_ptr<ITransportChannel> {
        assert(*attempt_error == TransportError::kNone);
        // A cache miss may leave its output untouched. A previous caller error
        // must not turn that miss into cancellation or suppress normal ICE.
        if (failure == 0) return nullptr;
        *attempt_error = TransportError::kSecurity;
        return std::make_unique<RoutingChannel>(failure != 1, failure != 2,
            failure == 3 ? TransportType::kLanDirectTcp :
            failure == 4 ? TransportType::kCloudRelay443 : TransportType::kWebRtcData,
            [&] { destroyed = true; });
      };
      error = TransportError::kCancelled;
      channel = ConnectAutomatically(options, &fallback.cancelled, &error);
      assert(channel && error == TransportError::kNone);
      assert(channel->GetType() == (prefer_relay ? TransportType::kCloudRelay443
                                                : TransportType::kWebRtcData));
      assert(fallback.webrtc_calls == (prefer_relay ? 0 : 1));
      assert(fallback.route_selections == (prefer_relay ? 1 : 0));
      assert(destroyed == (failure != 0));
    }
  }

  for (int stage = 0; stage < 4; ++stage) {
    WebRtcRoutingProbe cancelled;
    options = cancelled.Options();
    options.prefer_relay = true;
    reuse_calls = 0;
    if (stage == 0) cancelled.cancelled = true;
    options.try_reuse = [&](const std::atomic<bool>*, TransportError* attempt_error)
        -> std::unique_ptr<ITransportChannel> {
      ++reuse_calls;
      if (stage == 1) cancelled.cancelled = true;
      if (stage >= 2) *attempt_error = TransportError::kCancelled;
      if (stage == 2) return nullptr;
      return std::make_unique<RoutingChannel>(true, true, TransportType::kWebRtcData);
    };
    channel = ConnectAutomatically(options, &cancelled.cancelled, &error);
    assert(!channel && error == TransportError::kCancelled);
    assert(reuse_calls == (stage == 0 ? 0 : 1));
    assert(cancelled.webrtc_calls == 0 && cancelled.route_selections == 0 && cancelled.relay_calls == 0);
    if (stage == 2) {
      assert(!ConnectAutomatically(options, &cancelled.cancelled, nullptr));
      assert(cancelled.route_selections == 0);
    }
  }
}

struct RetainedChannelState {
  bool closed{false};
  bool destroyed{false};
  BorrowedPtr<const std::atomic<bool>> cancellation{nullptr};
};

// A deterministic peer acknowledges valid sender frames. The real sender
// session still performs its handshake, resume, data and final-commit protocol.
class CompletingChannel final : public ITransportChannel {
 public:
  enum class Fault { kNone, kFinalRequest, kFinalResponse, kChunk, kUnexpectedFinish };

  CompletingChannel(std::shared_ptr<RetainedChannelState> state,
                    bool fail, TransportType type, std::function<void()> finish,
                    Fault fault = Fault::kNone)
      : state_(std::move(state)), fail_(fail), type_(type),
        finish_(std::move(finish)), fault_(fault) {}
  ~CompletingChannel() override { state_->destroyed = true; }
  bool SendFrame(const FrameHeader& header, std::span<const uint8_t> payload) override {
    if (fail_ || state_->closed) return false;
    assert(ValidateFrameHeader(header) && header.checksum == ComputeCRC32C(payload));
    if ((fault_ == Fault::kFinalRequest &&
         header.cmd == FrameHeader::Command::kTransferFinish) ||
        (fault_ == Fault::kChunk && header.cmd == FrameHeader::Command::kChunkData))
      return false;
    FrameHeader reply = header;
    std::vector<uint8_t> bytes;
    switch (header.cmd) {
      case FrameHeader::Command::kHandshakeReq:
        finish_requests_ = 0;
        if (fault_ == Fault::kUnexpectedFinish) {
          reply.cmd = FrameHeader::Command::kTransferFinish;
        } else {
          reply.cmd = FrameHeader::Command::kHandshakeResp;
          bytes.assign(payload.begin(), payload.end());
        }
        break;
      case FrameHeader::Command::kFileMetadata: {
        const auto metadata = FileMetadataPayload::Deserialize(payload);
        assert(metadata);
        chunks_ = metadata->total_chunks;
        reply.cmd = FrameHeader::Command::kChunkAck;
        break;
      }
      case FrameHeader::Command::kSyncBitmapReq:
        reply.cmd = FrameHeader::Command::kSyncBitmapResp;
        bytes = ChunkBitmap(chunks_).Serialize();
        break;
      case FrameHeader::Command::kChunkData:
        reply.cmd = FrameHeader::Command::kChunkAck;
        break;
      case FrameHeader::Command::kTransferFinish:
        // The third message confirms receipt and has no reply.
        if (++finish_requests_ == 2) return true;
        break;
      default:
        assert(false);
    }
    reply.payload_len = static_cast<uint32_t>(bytes.size());
    reply.checksum = ComputeCRC32C(bytes);
    replies_.emplace_back(reply, std::move(bytes));
    return true;
  }
  bool ReadNextFrame(FrameHeader& header, std::vector<uint8_t>& bytes) override {
    if (replies_.empty() || state_->closed) return false;
    header = replies_.front().first;
    bytes = std::move(replies_.front().second);
    replies_.pop_front();
    if (fault_ == Fault::kFinalResponse &&
        header.cmd == FrameHeader::Command::kTransferFinish)
      return false;
    if (header.cmd == FrameHeader::Command::kTransferFinish && finish_) finish_();
    return true;
  }
  void Close() override { state_->closed = true; }
  bool IsConnected() const override { return !state_->closed; }
  bool IsSecure() const override { return true; }
  TransportType GetType() const override { return type_; }
  void SetCancellationFlag(const std::atomic<bool>* flag) override { state_->cancellation = flag; }

 private:
  std::shared_ptr<RetainedChannelState> state_;
  bool fail_;
  TransportType type_;
  std::function<void()> finish_;
  Fault fault_;
  uint64_t chunks_{0};
  int finish_requests_{0};
  std::deque<std::pair<FrameHeader, std::vector<uint8_t>>> replies_;
};

void TestJobCompletionRequestSurvivesFailedFallback() {
  TemporaryDirectory directory;
  const auto source = directory.path / "completion.bin";
  std::ofstream(source, std::ios::binary) << "receiver completion may be unknown";
  using Fault = CompletingChannel::Fault;
  for (const auto fault : {Fault::kFinalRequest, Fault::kFinalResponse,
                           Fault::kChunk, Fault::kUnexpectedFinish, Fault::kNone}) {
    for (const bool relay_connected : {false, true}) {
      TestJobOptions options;
      options.transport = "auto";
      options.source_path = source.string();
      options.task_id = std::string(32, 'a');
      options.automatic.invitation_id = options.task_id;
      options.automatic.webrtc = [fault](const std::atomic<bool>*, TransportError* error)
          -> std::unique_ptr<ITransportChannel> {
        // Also cover a failure before any file protocol has started.
        if (fault == Fault::kNone) {
          *error = TransportError::kNotConnected;
          return nullptr;
        }
        *error = TransportError::kNone;
        return std::make_unique<CompletingChannel>(
            std::make_shared<RetainedChannelState>(), false,
            TransportType::kWebRtcData, std::function<void()>{}, fault);
      };
      int relay_attempts = 0;
      options.automatic.select_route = [](std::string route) {
        assert(route == "relay");
        return PeerConnectionState{true, "relay", {}, {}};
      };
      options.automatic.relay = [&](const std::atomic<bool>*, TransportError* error)
          -> std::unique_ptr<ITransportChannel> {
        ++relay_attempts;
        if (!relay_connected) {
          *error = TransportError::kIo;
          return nullptr;
        }
        *error = TransportError::kNone;
        // The same task can reach relay but then fail its new handshake.
        return std::make_unique<CompletingChannel>(
            std::make_shared<RetainedChannelState>(), true,
            TransportType::kCloudRelay443, std::function<void()>{});
      };
      TestTransferJob job(std::move(options));
      assert(!job.snapshot().completion_requested);
      assert(!job.Run());
      const auto snapshot = job.snapshot();
      assert(snapshot.state == "failed" && !snapshot.active);
      assert(relay_attempts == 1);
      assert(snapshot.completion_requested ==
             (fault == Fault::kFinalRequest || fault == Fault::kFinalResponse));
      if (relay_connected) assert(snapshot.transport == "relay");
    }
  }
}

struct FinishChannelStats {
  int chunks{0};
  int finishes{0};
  int drop_finish{0};
  bool report_dropped_send_success{false};
};

// Real loopback frame I/O with fault injection at the adapter boundary. The
// sessions, file commit, checkpoint recovery and route retry run unchanged.
class FinishFaultChannel final : public ITransportChannel {
 public:
  FinishFaultChannel(std::unique_ptr<LanTcpConnection> channel,
                     TransportType type,
                     std::shared_ptr<FinishChannelStats> stats)
      : channel_(std::move(channel)), type_(type), stats_(std::move(stats)) {}
  bool SendFrame(const FrameHeader& header,
                 std::span<const uint8_t> payload) override {
    if (header.cmd == FrameHeader::Command::kChunkData) ++stats_->chunks;
    if (header.cmd == FrameHeader::Command::kTransferFinish &&
        ++stats_->finishes == stats_->drop_finish)
      return stats_->report_dropped_send_success;
    return channel_->SendFrame(header, payload);
  }
  bool ReadNextFrame(FrameHeader& header, std::vector<uint8_t>& payload) override {
    return channel_->ReadNextFrame(header, payload);
  }
  void Close() override { channel_->Close(); }
  bool IsConnected() const override { return channel_->IsConnected(); }
  // Authentication is supplied by this test adapter, as in CompletingChannel.
  bool IsSecure() const override { return true; }
  TransportType GetType() const override { return type_; }
  void SetCancellationFlag(const std::atomic<bool>* flag) override {
    channel_->SetCancellationFlag(flag);
  }
  void SetIoTimeout(int timeout) override { channel_->SetIoTimeout(timeout); }
  TransportError LastError() const override { return channel_->LastError(); }

 private:
  std::unique_ptr<LanTcpConnection> channel_;
  TransportType type_;
  std::shared_ptr<FinishChannelStats> stats_;
};

std::array<std::unique_ptr<ITransportChannel>, 2> FinishChannelPair(
    TransportType type,
    const std::array<std::shared_ptr<FinishChannelStats>, 2>& stats) {
  LanTcpServer listener;
  assert(listener.Start(0, "127.0.0.1"));
  auto sender = LanTcpClient::Connect("127.0.0.1", listener.listening_port());
  auto receiver = listener.Accept(2000);
  assert(sender && receiver);
  return {std::make_unique<FinishFaultChannel>(std::move(sender), type, stats[0]),
          std::make_unique<FinishFaultChannel>(std::move(receiver), type, stats[1])};
}

void TestJobRecoversCommittedReceiptWithoutResendingPayload() {
  TemporaryDirectory directory;
  const auto source = directory.path / "message.txt";
  const std::string text(4097, 't');
  std::ofstream(source, std::ios::binary) << text;
  // Lost receiver receipt with and without relay, then lost sender confirmation.
  for (int failure = 0; failure < 3; ++failure) {
    const bool relay_available = failure == 0;
    const std::array<std::shared_ptr<FinishChannelStats>, 2> direct_stats = {
        std::make_shared<FinishChannelStats>(), std::make_shared<FinishChannelStats>()};
    const std::array<std::shared_ptr<FinishChannelStats>, 2> relay_stats = {
        std::make_shared<FinishChannelStats>(), std::make_shared<FinishChannelStats>()};
    if (failure == 2) {
      direct_stats[0]->drop_finish = 2;
    } else {
      direct_stats[1]->drop_finish = 1;
      direct_stats[1]->report_dropped_send_success = true;
    }
    auto direct = FinishChannelPair(TransportType::kWebRtcData, direct_stats);
    std::array<std::unique_ptr<ITransportChannel>, 2> relay;
    if (relay_available) relay = FinishChannelPair(TransportType::kCloudRelay443, relay_stats);
    std::array<int, 2> relay_attempts{};
    std::array<int, 2> retained{};
    const auto output = directory.path / ("received-" + std::to_string(failure));
    auto options_for = [&](size_t index) {
      TestJobOptions options;
      options.role = index == 0 ? "send" : "receive";
      options.transport = "auto";
      options.task_id = std::string(32, static_cast<char>('1' + failure));
      options.source_path = source.string();
      options.output_dir = output.string();
      options.expected_file_name = "message.txt";
      options.expected_file_size = text.size();
      options.io_timeout_ms = 500;
      options.stage_timeout_ms = 5000;
      options.automatic.invitation_id = options.task_id;
      options.automatic.webrtc = [&, index](const std::atomic<bool>*, TransportError*) {
        return std::move(direct[index]);
      };
      options.automatic.select_route = [](std::string route) {
        assert(route == "relay");
        return PeerConnectionState{true, "relay", {}, {}};
      };
      options.automatic.relay = [&, index](const std::atomic<bool>*, TransportError* error) {
        ++relay_attempts[index];
        if (!relay_available) *error = TransportError::kIo;
        return std::move(relay[index]);
      };
      options.automatic.retain_direct = [&, index](std::unique_ptr<ITransportChannel>) {
        ++retained[index];
      };
      return options;
    };
    TestTransferJob receiver(options_for(1));
    TestTransferJob sender(options_for(0));
    bool received = false;
    std::thread worker([&] { received = receiver.Run(); });
    const bool sent = sender.Run();
    worker.join();
    assert(received && sent == (failure != 1));
    const auto snapshot = receiver.snapshot();
    assert(snapshot.committed && snapshot.state == "completed" && !snapshot.active);
    assert(!snapshot.saved_path.empty() && snapshot.error.empty());
    assert(snapshot.bytes == text.size() && snapshot.total == text.size() &&
           snapshot.percent == 100);
    std::ifstream saved(snapshot.saved_path, std::ios::binary);
    const std::string received_text{std::istreambuf_iterator<char>(saved), {}};
    assert(received_text == text);
    assert(sender.snapshot().completion_requested && !sender.snapshot().committed);
    assert(direct_stats[0]->chunks == 1 && relay_stats[0]->chunks == 0);
    assert(relay_attempts[1] == 1 && relay_attempts[0] == (failure == 2 ? 0 : 1));
    assert(retained[0] == 0 && retained[1] == 0);
    if (relay_available) {
      assert(snapshot.transport == "relay" && sender.snapshot().transport == "relay");
      assert(relay_stats[0]->finishes == 2 && relay_stats[1]->finishes == 1);
    }
  }
}

void TestJobRetainsOnlySuccessfulWebRtcChannels() {
  TemporaryDirectory directory;
  const auto source = directory.path / "retained.bin";
  std::ofstream(source, std::ios::binary) << "reuse this authenticated connection";
  // Success, failed transfer, cancellation at final response, relay success,
  // and a successful session whose connection has already closed.
  for (int outcome = 0; outcome < 5; ++outcome) {
    auto state = std::make_shared<RetainedChannelState>();
    std::unique_ptr<ITransportChannel> retained;
    TestJobOptions options;
    options.transport = "auto";
    options.source_path = source.string();
    options.task_id = std::string(32, 'd');
    options.automatic.invitation_id = options.task_id;
    options.automatic.prefer_relay = outcome == 3;
    TestTransferJob* running_job = nullptr;
    auto make_channel = [&](const std::atomic<bool>*, TransportError* error)
        -> std::unique_ptr<ITransportChannel> {
      *error = TransportError::kNone;
      return std::make_unique<CompletingChannel>(state, outcome == 1,
          outcome == 3 ? TransportType::kCloudRelay443 : TransportType::kWebRtcData,
          [&] {
            if (outcome == 2) running_job->Cancel();
            if (outcome == 4) state->closed = true;
          });
    };
    options.automatic.webrtc = make_channel;
    options.automatic.relay = make_channel;
    options.automatic.select_route = [&](std::string route) {
      assert(route == "relay");
      // Failed direct sessions are still allowed their ordinary relay retry,
      // but neither failure may enter the reusable-channel cache.
      return PeerConnectionState{outcome != 1, "relay", {}, {}};
    };
    options.automatic.retain_direct = [&](std::unique_ptr<ITransportChannel> channel) {
      assert(!retained && state->cancellation == nullptr && !state->closed);
      retained = std::move(channel);
    };
    {
      TestTransferJob job(options);
      running_job = &job;
      const bool success = job.Run();
      assert(success == (outcome != 1));
      assert(static_cast<bool>(retained) == (outcome == 0));
    }
    assert(state->cancellation == nullptr);
    if (outcome == 0) {
      assert(!state->closed && !state->destroyed && retained->IsConnected());
      options.task_id = std::string(32, 'b');
      options.automatic.invitation_id = options.task_id;
      options.automatic.prefer_relay = true;
      options.automatic.try_reuse = [&](const std::atomic<bool>*, TransportError*) {
        assert(state->cancellation == nullptr);
        return std::move(retained);
      };
      options.automatic.webrtc = [](const std::atomic<bool>*, TransportError*)
          -> std::unique_ptr<ITransportChannel> {
        assert(false);  // The next small payload must use the same channel.
        return nullptr;
      };
      {
        TestTransferJob next(options);
        running_job = &next;
        assert(next.Run());
      }
      assert(retained && retained->IsConnected() && state->cancellation == nullptr);
      assert(!state->closed && !state->destroyed);
      retained->Close();
      retained.reset();
    }
    assert(state->closed && state->destroyed);
  }
}

class DropAfterChunk final : public ITransportChannel {
 public:
  explicit DropAfterChunk(std::unique_ptr<ITransportChannel> channel)
      : channel_(std::move(channel)) {}
  bool SendFrame(const FrameHeader& header, std::span<const uint8_t> payload) override {
    return channel_->SendFrame(header, payload);
  }
  bool ReadNextFrame(FrameHeader& header, std::vector<uint8_t>& payload) override {
    if (received_chunk_) {
      channel_->Close();
      return false;
    }
    if (!channel_->ReadNextFrame(header, payload)) return false;
    received_chunk_ = header.cmd == FrameHeader::Command::kChunkData;
    return true;
  }
  void Close() override { channel_->Close(); }
  bool IsConnected() const override { return channel_->IsConnected(); }
  bool IsSecure() const override { return channel_->IsSecure(); }
  TransportType GetType() const override { return channel_->GetType(); }
  void SetCancellationFlag(const std::atomic<bool>* flag) override {
    channel_->SetCancellationFlag(flag);
  }
  void SetIoTimeout(int timeout) override { channel_->SetIoTimeout(timeout); }
 private:
  std::unique_ptr<ITransportChannel> channel_;
  bool received_chunk_{false};
};

void TestInterruptedDirectResumesOverRelay(const std::string& ca_file, uint16_t port) {
  TemporaryDirectory directory;
  const auto source = directory.path / "resume-auto.bin";
  {
    std::ofstream file(source, std::ios::binary);
    const std::string data(2 * 1024 * 1024 + 17, 'q');
    file.write(data.data(), data.size());
  }
  AutoCoordinator coordinator;
  const std::string id(32, 'c');
  auto receive_options = coordinator.Options(false, id, ca_file, port, "auto-resume");
  std::atomic<bool> cancelled{false};
  bool received = false;
  std::string saved;
  std::thread worker([&] {
    TransportError error;
    auto direct = ConnectAutomatically(receive_options, &cancelled, &error);
    assert(direct && direct->GetType() == TransportType::kLanDirectTcp);
    TransferOptions transfer;
    transfer.task_id.fill(0xcc);
    transfer.io_timeout_ms = 5000;
    transfer.stage_timeout_ms = 10000;
    transfer.authorize_receive = [](const auto&, const auto& metadata) {
      return metadata.file_name == "resume-auto.bin" &&
             metadata.file_size == 2 * 1024 * 1024 + 17;
    };
    const auto output = (directory.path / "received").string();
    {
      DropAfterChunk interrupted(std::move(direct));
      ReceiverSession first(transfer);
      uint64_t durable_bytes = 0;
      assert(!first.ReceiveFile(&interrupted, output, &saved,
          [&](uint64_t bytes, uint64_t, float) { durable_bytes = bytes; }));
      assert(durable_bytes == 1024 * 1024);
    }
    auto relay = ConnectAutomaticRelay(receive_options, &cancelled, &error);
    assert(relay && relay->GetType() == TransportType::kCloudRelay443);
    ReceiverSession resumed(transfer);
    received = resumed.ReceiveFile(relay.get(), output, &saved);
    if (!received) std::cerr << "resumed receiver: " << resumed.error() << '\n';
  });
  TestJobOptions send;
  send.transport = "auto";
  send.task_id = id;
  send.source_path = source.string();
  send.io_timeout_ms = 5000;
  send.automatic = coordinator.Options(true, id, ca_file, port, "auto-resume");
  TestTransferJob sender(send);
  const bool sent = sender.Run();
  worker.join();
  if (!sent) std::cerr << "resumed sender: " << sender.snapshot().error << ", route="
                       << sender.snapshot().transport << ", relay opens="
                       << coordinator.relay_connections << '\n';
  assert(sent && received);
  assert(sender.snapshot().transport == "relay");
  assert(coordinator.relay_connections == 2);
  assert(StreamingFileReader::ComputeFileSHA256(source.string()) ==
         StreamingFileReader::ComputeFileSHA256(saved));
}

}  // namespace

int main(int argc, char** argv) {
  const auto args = test::CommandLineArguments(argc, argv);
  TestValidationAndStickyCancellation();
  TestCancelSourcePreparation();
  TestCancelListeningAndConcurrentSnapshots();
  TestAutomaticDirectAndCancellation();
  TestAutomaticWebRtcRouting();
  TestAutomaticWebRtcReuseRouting();
  TestJobCompletionRequestSurvivesFailedFallback();
  TestJobRecoversCommittedReceiptWithoutResendingPayload();
  TestJobRetainsOnlySuccessfulWebRtcChannels();
  if (args.size() == 2) TestRealMutualTls(args[1]);
  if (args.size() == 4) {
    TestRealMutualTls(args[3]);
    TestRealHttpsRelay(args[1], static_cast<uint16_t>(std::stoi(args[2])));
    TestAutomaticFallbacks(args[1], static_cast<uint16_t>(std::stoi(args[2])));
    TestInterruptedDirectResumesOverRelay(args[1], static_cast<uint16_t>(std::stoi(args[2])));
  }
  std::cout << "[PASS] native transfer test jobs, cancellation and safe snapshots\n";
}
