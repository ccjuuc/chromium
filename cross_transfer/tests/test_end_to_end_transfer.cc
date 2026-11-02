#include "cross_transfer/transfer_session.h"
#include "cross_transfer/transport_channel.h"
#include "cross_transfer/streaming_file_io.h"

#include <cassert>
#include <iostream>
#include <filesystem>
#include <thread>
#include <fstream>
#include <random>

using namespace cross_transfer;

void CreateTestFile(const std::string& path, size_t size_bytes) {
  std::ofstream ofs(path, std::ios::binary);
  std::mt19937_64 rng(1337); // deterministic seed
  const size_t buf_size = 64 * 1024;
  std::vector<char> buffer(buf_size);

  size_t written = 0;
  while (written < size_bytes) {
    size_t chunk = std::min(buf_size, size_bytes - written);
    for (size_t i = 0; i < chunk; ++i) {
      buffer[i] = static_cast<char>(rng() & 0xFF);
    }
    ofs.write(buffer.data(), chunk);
    written += chunk;
  }
}

int main() {
  std::cout << "==================================================" << std::endl;
  std::cout << "Starting End-to-End High-Speed Cross Transfer Test" << std::endl;
  std::cout << "==================================================" << std::endl;

  std::string test_dir = "/tmp/cross_transfer_e2e";
  std::string src_dir = test_dir + "/source";
  std::string dest_dir = test_dir + "/dest";
  std::filesystem::create_directories(src_dir);
  std::filesystem::create_directories(dest_dir);

  std::string src_file = src_dir + "/sample_large_payload.bin";
  const size_t test_file_size = 8 * 1024 * 1024; // 8MB

  std::cout << "Generating test file: " << src_file << " (" << test_file_size << " bytes)..." << std::endl;
  CreateTestFile(src_file, test_file_size);

  std::string src_sha256 = StreamingFileReader::ComputeFileSHA256(src_file);
  std::cout << "Source file SHA-256: " << src_sha256 << std::endl;

  // 1. Start Server on ephemeral port
  LanTcpServer server;
  assert(server.Start(0));
  uint16_t port = server.listening_port();
  std::cout << "Receiver server listening on 127.0.0.1:" << port << std::endl;

  std::atomic<bool> receiver_success{false};
  std::string received_file_path;

  // 2. Launch Receiver in thread
  std::thread receiver_thread([&]() {
    auto conn = server.Accept(5000);
    assert(conn != nullptr);
    std::cout << "[Receiver] Accepted connection from " << conn->peer_ip() << ":" << conn->peer_port() << std::endl;

    TransferOptions rx_opts;
    rx_opts.allow_insecure_transport = true;  // Loopback TCP transport test only.
    rx_opts.authorize_receive = [](const auto&, const auto&) { return true; };
    rx_opts.device_name = "Target Mac Node";
    ReceiverSession receiver(rx_opts);

    bool ok = receiver.ReceiveFile(
        conn.get(), dest_dir, &received_file_path,
        [](uint64_t bytes, uint64_t total, float pct) {
          std::cout << "\r[Receiver Progress] " << bytes << " / " << total << " bytes (" << static_cast<int>(pct) << "%)" << std::flush;
        },
        [](const std::string& status) {
          std::cout << "\n[Receiver Status] " << status << std::endl;
        });

    receiver_success = ok;
  });

  // 3. Launch Sender
  std::cout << "[Sender] Connecting to 127.0.0.1:" << port << "..." << std::endl;
  auto client_conn = LanTcpClient::Connect("127.0.0.1", port, 3000);
  assert(client_conn != nullptr);
  std::cout << "[Sender] Connected successfully!" << std::endl;

  TransferOptions tx_opts;
  tx_opts.allow_insecure_transport = true;  // TLS is exercised separately.
  tx_opts.chunk_size = 1024 * 1024; // 1MB chunk
  tx_opts.device_name = "Source Mac Node";
  SenderSession sender(tx_opts);

  auto start_time = std::chrono::steady_clock::now();
  bool sender_ok = sender.SendFile(
      client_conn.get(), src_file,
      [](uint64_t bytes, uint64_t total, float pct) {
        std::cout << "\r[Sender Progress] " << bytes << " / " << total << " bytes (" << static_cast<int>(pct) << "%)" << std::flush;
      },
      [](const std::string& status) {
        std::cout << "\n[Sender Status] " << status << std::endl;
      });

  auto end_time = std::chrono::steady_clock::now();
  auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

  receiver_thread.join();
  std::cout << std::endl;

  assert(sender_ok);
  assert(receiver_success);
  assert(!received_file_path.empty());
  assert(std::filesystem::exists(received_file_path));

  // 4. Verify integrity
  uint64_t dest_size = std::filesystem::file_size(received_file_path);
  assert(dest_size == test_file_size);

  std::string dest_sha256 = StreamingFileReader::ComputeFileSHA256(received_file_path);
  std::cout << "Destination file SHA-256: " << dest_sha256 << std::endl;
  assert(dest_sha256 == src_sha256);

  double mb_transferred = static_cast<double>(test_file_size) / (1024.0 * 1024.0);
  double speed_mb_s = (mb_transferred / (duration_ms / 1000.0));

  std::cout << "==================================================" << std::endl;
  std::cout << "Transfer Successful!" << std::endl;
  std::cout << "Transferred: " << mb_transferred << " MB in " << duration_ms << " ms" << std::endl;
  std::cout << "Throughput:  " << speed_mb_s << " MB/s" << std::endl;
  std::cout << "SHA-256 Match: " << (dest_sha256 == src_sha256 ? "PERFECT 100%" : "MISMATCH") << std::endl;
  std::cout << "==================================================" << std::endl;

  // Cleanup
  std::filesystem::remove_all(test_dir);
  return 0;
}
