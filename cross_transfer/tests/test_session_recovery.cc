#include "cross_transfer/transfer_session.h"
#include <openssl/rand.h>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace cross_transfer;
namespace fs = std::filesystem;

class InspectChannel final : public ITransportChannel {
 public:
  explicit InspectChannel(std::unique_ptr<LanTcpConnection> conn) : conn_(std::move(conn)) {}
  bool SendFrame(const FrameHeader& h, std::span<const uint8_t> data) override {
    if (h.cmd == FrameHeader::Command::kTransferFinish && finish_delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(finish_delay_ms));
    }
    if (h.cmd == FrameHeader::Command::kChunkData) {
      chunks.push_back(h.chunk_index);
      if (corrupt) {
        auto copy = std::vector<uint8_t>(data.begin(), data.end());
        copy[0] ^= 1;
        auto modified = h;
        modified.checksum = ComputeCRC32C(copy);
        return conn_->SendFrame(modified, copy);
      }
    }
    return conn_->SendFrame(h, data);
  }
  bool ReadNextFrame(FrameHeader& h, std::vector<uint8_t>& data) override {
    return conn_->ReadNextFrame(h, data);
  }
  void Close() override { conn_->Close(); }
  bool IsConnected() const override { return conn_->IsConnected(); }
  TransportType GetType() const override { return conn_->GetType(); }
  void SetCancellationFlag(const std::atomic<bool>* c) override { conn_->SetCancellationFlag(c); }
  void SetIoTimeout(int ms) override { conn_->SetIoTimeout(ms); }
  TransportError LastError() const override { return conn_->LastError(); }
  bool corrupt = false;
  int finish_delay_ms = 0;
  std::vector<uint64_t> chunks;
 private:
  std::unique_ptr<LanTcpConnection> conn_;
};

TransferOptions Options() {
  TransferOptions o;
  o.allow_insecure_transport = true;  // Loopback only; production defaults stay secure.
  o.authorize_receive = [](const auto&, const auto&) { return true; };
  o.chunk_size = 64;
  o.max_in_flight_bytes = 64;
  o.io_timeout_ms = 2000;
  return o;
}

std::string Read(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(stream), {}};
}

int main() {
  TaskId unique;
  assert(RAND_bytes(unique.data(), unique.size()) == 1);
  const fs::path root = fs::temp_directory_path() /
      ("cross_transfer_session_" + ComputeSHA256(unique).substr(0, 16));
  fs::create_directories(root / "source");
  fs::create_directories(root / "dest");
  const auto source = root / "source" / "sample.bin";
  const std::string content(64 * 3 + 17, 's');
  std::ofstream(source, std::ios::binary) << content;
  auto opts = Options();
  TaskId saved_id{};
  {
    LanTcpServer server;
    assert(server.Start(0, "127.0.0.1"));
    ReceiverSession receiver(opts);
    bool rx = true;
    std::thread worker([&] {
      auto conn = server.Accept(2000);
      assert(conn);
      rx = receiver.ReceiveFile(conn.get(), (root / "dest").string(), nullptr,
          [&](uint64_t n, uint64_t, float) { if (n >= 64) receiver.Cancel(); });
    });
    InspectChannel client(LanTcpClient::Connect("127.0.0.1", server.listening_port()));
    SenderSession sender(opts);
    assert(!sender.SendFile(&client, source.string()));
    saved_id = sender.task_id();
    worker.join();
    assert(!rx);
    assert(receiver.bitmap().completed_chunks() == 1);
  }
  // New session objects represent a process restart; restore only persisted ID.
  {
    opts.task_id = saved_id;
    LanTcpServer server;
    assert(server.Start(0, "127.0.0.1"));
    bool rx = false;
    std::thread worker([&] {
      auto conn = server.Accept(2000);
      assert(conn);
      ReceiverSession receiver(opts);
      rx = receiver.ReceiveFile(conn.get(), (root / "dest").string());
    });
    InspectChannel client(LanTcpClient::Connect("127.0.0.1", server.listening_port()));
    SenderSession sender(opts);
    assert(sender.SendFile(&client, source.string()));
    worker.join();
    assert(rx);
    assert((client.chunks == std::vector<uint64_t>{1, 2, 3}));
    assert(Read(root / "dest" / "sample.bin") == content);
  }
  // Empty file has zero chunks and commits successfully.
  {
    opts = Options();
    std::ofstream(root / "source" / "empty.bin");
    LanTcpServer server;
    assert(server.Start(0, "127.0.0.1"));
    bool rx = false;
    std::thread worker([&] {
      auto conn = server.Accept(2000);
      ReceiverSession receiver(opts);
      rx = receiver.ReceiveFile(conn.get(), (root / "dest").string());
    });
    InspectChannel client(LanTcpClient::Connect("127.0.0.1", server.listening_port()));
    SenderSession sender(opts);
    assert(sender.SendFile(&client, (root / "source" / "empty.bin").string()));
    worker.join();
    assert(rx && client.chunks.empty());
    assert(fs::file_size(root / "dest" / "empty.bin") == 0);
  }
  // A file created during receiving is preserved unless overwrite was granted.
  for (bool overwrite : {false, true}) {
    auto collision_options = Options();
    collision_options.allow_overwrite = overwrite;
    auto destination = root / (overwrite ? "allow-replace" : "deny-replace");
    LanTcpServer server;
    assert(server.Start(0, "127.0.0.1"));
    bool rx = false;
    std::thread worker([&] {
      auto conn = server.Accept(2000);
      assert(conn);
      ReceiverSession receiver(collision_options);
      rx = receiver.ReceiveFile(conn.get(), destination.string(), nullptr,
          [&](uint64_t bytes, uint64_t, float) {
            if (bytes == 64)
              std::ofstream(destination / "sample.bin") << "late-file";
          });
    });
    auto client = LanTcpClient::Connect("127.0.0.1", server.listening_port());
    SenderSession sender(collision_options);
    const bool tx = sender.SendFile(client.get(), source.string());
    worker.join();
    assert(tx == overwrite && rx == overwrite);
    assert(Read(destination / "sample.bin") == (overwrite ? content : "late-file"));
  }
  // Corrupt bytes with a valid frame CRC still fail the file hash before commit.
  {
    opts = Options();
    opts.max_in_flight_bytes = 256;
    opts.allow_overwrite = true;
    std::ofstream(root / "dest" / "sample.bin") << "keep-original";
    LanTcpServer server;
    assert(server.Start(0, "127.0.0.1"));
    bool rx = true;
    std::thread worker([&] {
      auto conn = server.Accept(2000);
      ReceiverSession receiver(opts);
      rx = receiver.ReceiveFile(conn.get(), (root / "dest").string());
    });
    InspectChannel client(LanTcpClient::Connect("127.0.0.1", server.listening_port()));
    client.corrupt = true;
    SenderSession sender(opts);
    assert(!sender.SendFile(&client, source.string()));
    worker.join();
    assert(!rx && Read(root / "dest" / "sample.bin") == "keep-original");
  }
  // Cancel a sender while the peer is connected but never answers the handshake.
  {
    LanTcpServer server;
    assert(server.Start(0, "127.0.0.1"));
    auto client = LanTcpClient::Connect("127.0.0.1", server.listening_port());
    auto idle_peer = server.Accept(2000);
    assert(client && idle_peer);
    SenderSession sender(Options());
    bool result = true;
    std::thread worker([&] { result = sender.SendFile(client.get(), source.string()); });
    for (int n = 0; n < 200 && sender.state() != TransferState::kHandshaking; ++n)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    auto start = std::chrono::steady_clock::now();
    sender.Cancel();
    worker.join();
    assert(!result && sender.state() == TransferState::kCancelled);
    assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
  }
  // A caller's cancellation must survive PrepareFile and SendFile entry points.
  {
    auto external_options = Options();
    std::atomic<bool> cancelled{true};
    external_options.external_cancellation = &cancelled;
    SenderSession sender(external_options);
    assert(!sender.PrepareFile(source.string()));
    assert(sender.state() == TransferState::kCancelled);
    assert(cancelled.load());
    cancelled = false;
    sender.Cancel();
    assert(cancelled.load());
  }
  // Silent verification may exceed idle I/O timeout but has a finite phase limit.
  for (int delay : {160, 800}) {
    auto timed = Options();
    timed.allow_overwrite = true;
    timed.io_timeout_ms = 100;
    timed.stage_timeout_ms = 400;
    LanTcpServer server;
    assert(server.Start(0, "127.0.0.1"));
    bool rx = false;
    std::thread worker([&] {
      InspectChannel conn(server.Accept(2000));
      conn.finish_delay_ms = delay;
      ReceiverSession receiver(timed);
      rx = receiver.ReceiveFile(&conn, (root / "dest").string());
    });
    auto client = LanTcpClient::Connect("127.0.0.1", server.listening_port());
    SenderSession sender(timed);
    bool tx = sender.SendFile(client.get(), source.string());
    worker.join();
    assert(tx == (delay == 160));
    if (delay == 160) assert(rx);
    // A delayed final ACK can still be accepted by the local socket after the
    // sender timed out. The receiver has already committed in either case.
    assert(Read(root / "dest" / "sample.bin") == content);
  }
  fs::remove_all(root);
  std::cout << "Session restart/resume, empty file, corruption and cancellation passed\n";
}
