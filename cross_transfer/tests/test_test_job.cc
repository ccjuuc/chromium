#include "cross_transfer/test_job.h"

#include "cross_transfer/streaming_file_io.h"
#include "test_arguments.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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

}  // namespace

int main(int argc, char** argv) {
  const auto args = test::CommandLineArguments(argc, argv);
  TestValidationAndStickyCancellation();
  TestCancelSourcePreparation();
  TestCancelListeningAndConcurrentSnapshots();
  if (args.size() == 2) TestRealMutualTls(args[1]);
  if (args.size() == 4) {
    TestRealMutualTls(args[3]);
    TestRealHttpsRelay(args[1], static_cast<uint16_t>(std::stoi(args[2])));
  }
  std::cout << "[PASS] native transfer test jobs, cancellation and safe snapshots\n";
}
