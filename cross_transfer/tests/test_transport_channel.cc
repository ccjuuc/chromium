#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

#include "cross_transfer/connection_manager.h"
#include "cross_transfer/transport_channel.h"

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#endif

using namespace cross_transfer;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {
void CloseNative(NativeSocket fd) {
#if defined(_WIN32)
  closesocket(fd);
#else
  close(fd);
#endif
}
void WriteRaw(NativeSocket fd, std::span<const uint8_t> data) {
  int flags = 0;
#if defined(MSG_NOSIGNAL)
  flags = MSG_NOSIGNAL;
#endif
  auto sent = send(fd, reinterpret_cast<const char*>(data.data()),
                   static_cast<int>(data.size()), flags);
  assert(sent == static_cast<decltype(sent)>(data.size()));
}
struct Pair {
  LanTcpServer server;
  std::unique_ptr<LanTcpConnection> client;
  std::unique_ptr<LanTcpConnection> receiver;
  Pair() {
    assert(server.Start(0, "127.0.0.1"));
    client = LanTcpClient::Connect("127.0.0.1", server.listening_port(), 1000);
    assert(client);
    receiver = server.Accept(1000);
    assert(receiver);
  }
};

void TestRoundTrip() {
  Pair pair;
  FrameHeader header;
  header.cmd = FrameHeader::Command::kChunkData;
  header.chunk_index = 7;
  std::vector<uint8_t> data{0, 1, 2, 255};
  header.payload_len = static_cast<uint32_t>(data.size());
  header.checksum = ComputeCRC32C(data);
  assert(pair.client->SendFrame(header, data));
  FrameHeader received;
  std::vector<uint8_t> body;
  assert(pair.receiver->ReadNextFrame(received, body));
  assert(body == data && received.chunk_index == 7);
  assert(!pair.client->IsSecure());
  assert(pair.client->LastError() == TransportError::kNone);
}

void TestRejectOversizedFrameBeforeAllocation() {
  Pair pair;
  NativeSocket raw = pair.client->ReleaseSocket();
  FrameHeader header;
  header.cmd = FrameHeader::Command::kChunkData;
  header.payload_len = kMaxFramePayload + 1;
  WriteRaw(raw, header.Serialize());
  FrameHeader received;
  std::vector<uint8_t> body;
  assert(!pair.receiver->ReadNextFrame(received, body));
  assert(pair.receiver->LastError() == TransportError::kProtocol);
  assert(body.capacity() == 0);
  CloseNative(raw);
}

void TestPartialFrameDeadline() {
  Pair pair;
  NativeSocket raw = pair.client->ReleaseSocket();
  FrameHeader header;
  header.cmd = FrameHeader::Command::kChunkAck;
  auto bytes = header.Serialize();
  WriteRaw(raw, std::span(bytes).first(5));
  pair.receiver->SetIoTimeout(60);
  FrameHeader received;
  std::vector<uint8_t> body;
  auto start = Clock::now();
  assert(!pair.receiver->ReadNextFrame(received, body));
  assert(pair.receiver->LastError() == TransportError::kTimeout);
  assert(Clock::now() - start < 2s);
  CloseNative(raw);
}

void TestChecksumFailure() {
  Pair pair;
  NativeSocket raw = pair.client->ReleaseSocket();
  FrameHeader header;
  header.cmd = FrameHeader::Command::kChunkData;
  std::vector<uint8_t> data{1, 2, 3};
  header.payload_len = static_cast<uint32_t>(data.size());
  header.checksum = ComputeCRC32C(data) ^ 1;
  WriteRaw(raw, header.Serialize());
  WriteRaw(raw, data);
  FrameHeader received;
  std::vector<uint8_t> body;
  assert(!pair.receiver->ReadNextFrame(received, body));
  assert(pair.receiver->LastError() == TransportError::kProtocol);
  assert(body.empty());
  CloseNative(raw);
}

void TestCancellationAndCrossThreadClose() {
  for (bool use_close : {false, true}) {
    Pair pair;
    std::atomic<bool> cancelled{false};
    pair.receiver->SetCancellationFlag(&cancelled);
    pair.receiver->SetIoTimeout(5000);
    std::thread reader([&] {
      FrameHeader header;
      std::vector<uint8_t> body;
      assert(!pair.receiver->ReadNextFrame(header, body));
    });
    std::this_thread::sleep_for(30ms);
    auto start = Clock::now();
    if (use_close) {
      pair.receiver->Close();
    } else {
      cancelled = true;
    }
    reader.join();
    assert(Clock::now() - start < 2s);
    assert(pair.receiver->LastError() == TransportError::kCancelled);
    pair.receiver->SetCancellationFlag(nullptr);
  }
  LanTcpServer server;
  assert(server.Start(0, "127.0.0.1"));
  std::thread acceptor([&] { assert(!server.Accept(5000)); });
  std::this_thread::sleep_for(30ms);
  server.Stop();
  acceptor.join();
  assert(server.LastError() == TransportError::kCancelled);
}

void TestDisconnectedSend() {
#if !defined(_WIN32)
  // The child has default signal handling: this catches SIGPIPE regressions
  // even if the application embedding the library happens to ignore SIGPIPE.
  pid_t child = fork();
  assert(child >= 0);
  if (child == 0) {
    std::signal(SIGPIPE, SIG_DFL);
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) != 0) {
      _exit(2);
    }
    close(sockets[1]);
    LanTcpConnection channel(sockets[0], "local", 0);
    FrameHeader header;
    header.cmd = FrameHeader::Command::kChunkAck;
    _exit(channel.SendFrame(header, {}) ? 3 : 0);
  }
  int status = 0;
  assert(waitpid(child, &status, 0) == child);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}

void TestHostnameRaceAndIpv6() {
  LanTcpServer server;
  assert(server.Start(0, "127.0.0.1"));
  LanTcpServer unused;
  assert(unused.Start(0, "127.0.0.1"));
  uint16_t closed_port = unused.listening_port();
  unused.Stop();
  CandidateEndpoint bad;
  bad.ip = "127.0.0.1";
  bad.port = closed_port;
  bad.priority_score = 100;
  CandidateEndpoint good;
  good.ip = "localhost";
  good.port = server.listening_port();
  auto channel = LanTcpClient::RaceConnect({bad, good}, 2000);
  assert(channel && channel->peer_port() == server.listening_port());
  assert(server.Accept(1000));

  LanTcpServer ipv6;
  if (ipv6.Start(0, "::1")) {
    auto connection = LanTcpClient::Connect("::1", ipv6.listening_port(), 1000);
    assert(connection && connection->peer_ip() == "::1");
    assert(ipv6.Accept(1000));
  } else {
    std::cout << "[SKIP] Host has no IPv6 loopback\n";
  }
  std::atomic<bool> cancelled{true};
  TransportError error;
  assert(!LanTcpClient::Connect("localhost", 12345, 1000, &cancelled, &error));
  assert(error == TransportError::kCancelled);
  assert(!LanTcpClient::Connect("localhost", 12345, 0, nullptr, &error));
  assert(error == TransportError::kTimeout);
}

class FakeChannel : public ITransportChannel {
 public:
  explicit FakeChannel(bool secure, std::atomic<int>* close_count = nullptr)
      : secure_(secure), close_count_(close_count) {}
  bool SendFrame(const FrameHeader&, std::span<const uint8_t>) override {
    return connected_;
  }
  bool ReadNextFrame(FrameHeader&, std::vector<uint8_t>&) override {
    return connected_;
  }
  void Close() override {
    connected_ = false;
    if (close_count_) {
      ++*close_count_;
    }
  }
  bool IsConnected() const override { return connected_; }
  TransportType GetType() const override {
    return TransportType::kCloudRelay443;
  }
  bool IsSecure() const override { return secure_; }
  void SetCancellationFlag(const std::atomic<bool>* flag) override {
    cancellation = flag;
  }
  void SetIoTimeout(int timeout_ms) override { timeout = timeout_ms; }
  const std::atomic<bool>* cancellation{nullptr};
  int timeout{0};

 private:
  bool secure_;
  bool connected_{true};
  std::atomic<int>* close_count_;
};

void TestConnectionPolicy() {
  CandidateEndpoint endpoint;
  endpoint.ip = "127.0.0.1";
  endpoint.port = 1;
  std::atomic<bool> cancelled{false};
  std::vector<int> calls;
  int relay_budget = 0;
  ConnectionManager manager(
      [&](const auto&, int budget,
          const auto* flag) -> std::unique_ptr<ITransportChannel> {
        calls.push_back(1);
        assert(budget <= 40 && flag == &cancelled);
        std::this_thread::sleep_for(10ms);
        return nullptr;
      },
      [&](int budget, const auto* flag) -> std::unique_ptr<ITransportChannel> {
        calls.push_back(2);
        relay_budget = budget;
        assert(flag == &cancelled);
        return std::make_unique<FakeChannel>(true);
      });
  ConnectionOptions options;
  options.cancellation = &cancelled;
  options.total_timeout_ms = 200;
  options.direct_timeout_ms = 40;
  options.io_timeout_ms = 1234;
  auto channel = manager.Connect({endpoint}, options);
  assert(channel && calls == std::vector<int>({1, 2}));
  assert(relay_budget > 0 && relay_budget < 200);
  assert(static_cast<FakeChannel*>(channel.get())->timeout == 1234);
  assert(static_cast<FakeChannel*>(channel.get())->cancellation == &cancelled);

  std::atomic<int> closed{0};
  ConnectionManager insecure(
      {}, [&](int, const auto*) -> std::unique_ptr<ITransportChannel> {
        return std::make_unique<FakeChannel>(false, &closed);
      });
  assert(!insecure.Connect({}, options));
  assert(insecure.LastError() == TransportError::kSecurity && closed == 1);

  bool relayed_after_cancel = false;
  ConnectionManager cancel_manager(
      [&](const auto&, int, const auto*) -> std::unique_ptr<ITransportChannel> {
        cancelled = true;
        return std::make_unique<FakeChannel>(true, &closed);
      },
      [&](int, const auto*) -> std::unique_ptr<ITransportChannel> {
        relayed_after_cancel = true;
        return nullptr;
      });
  assert(!cancel_manager.Connect({endpoint}, options));
  assert(cancel_manager.LastError() == TransportError::kCancelled);
  assert(!relayed_after_cancel && closed == 2);
}
}  // namespace

int main() {
  TestRoundTrip();
  TestRejectOversizedFrameBeforeAllocation();
  TestPartialFrameDeadline();
  TestChecksumFailure();
  TestCancellationAndCrossThreadClose();
  TestDisconnectedSend();
  TestHostnameRaceAndIpv6();
  TestConnectionPolicy();
  std::cout << "[PASS] Transport bounds, deadlines, cancellation, IPv6/DNS and "
               "fallback policy\n";
}
