#include "cross_transfer/https_relay_channel.h"
#include "cross_transfer/tls_transport_channel.h"
#include "cross_transfer/transfer_session.h"
#include "test_arguments.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace cross_transfer;

int main(int argc, char** argv) {
  const auto args = test::CommandLineArguments(argc, argv);
  assert(args.size() == 4);  // CA file, relay port, temporary directory.
  HttpsRelayOptions options;
  options.host = "127.0.0.1";
  options.port = static_cast<uint16_t>(std::stoi(args[2]));
  options.ca_file = args[1];
  options.room_id = "integration";
  options.connection_id = std::string(32, 'a');
  options.bearer_token = std::string(64, '1');
  options.timeout_ms = 5000;

  TransportError error;
  auto denied = options;
  denied.bearer_token = std::string(64, '3');
  assert(!HttpsRelayChannel::Connect(denied, nullptr, &error));
  assert(error == TransportError::kSecurity);
  auto untrusted = options;
  untrusted.ca_file = std::string(args[3]) + "/missing-ca.pem";
  assert(!HttpsRelayChannel::Connect(untrusted, nullptr, &error));
  assert(error == TransportError::kSecurity);

  TlsConfig wrong_name;
  wrong_name.ca_file = args[1];
  wrong_name.peer_name = "unpaired.invalid";
  assert(!internal::TlsStream::Connect(options.host, options.port, wrong_name, 1000, nullptr, &error));
  assert(error == TransportError::kSecurity);

  auto sender = HttpsRelayChannel::Connect(options, nullptr, &error);
  assert(sender && sender->IsSecure());
  // A second process cannot consume the same role's mailbox while leased.
  assert(!HttpsRelayChannel::Connect(options, nullptr, &error));
  assert(error == TransportError::kNotConnected);
  options.role = RelayRole::kReceiver;
  options.bearer_token = std::string(64, '2');
  auto receiver = HttpsRelayChannel::Connect(options);
  assert(receiver && receiver->IsSecure());

  FrameHeader header;
  header.cmd = FrameHeader::Command::kChunkData;
  header.task_id.fill(1);
  std::vector<uint8_t> data(128 * 1024, 42);
  header.payload_len = data.size();
  header.checksum = ComputeCRC32C(data);
  assert(sender->SendFrame(header, data));
  FrameHeader incoming;
  std::vector<uint8_t> received;
  assert(receiver->ReadNextFrame(incoming, received));
  assert(data == received && incoming.task_id == header.task_id);
  header.payload_len = kMaxFramePayload + 1;
  assert(!sender->SendFrame(header, {}));
  assert(sender->LastError() == TransportError::kProtocol);

  const std::filesystem::path directory(args[3]);
  const auto source = directory / "relay-payload.bin";
  {
    std::ofstream file(source, std::ios::binary);
    for (int i = 0; i < 3; ++i)
      file.write(reinterpret_cast<const char*>(data.data()), data.size());
  }
  TransferOptions receive_options;
  receive_options.authorize_receive = [](const auto&, const auto&) { return true; };
  bool receive_ok = false;
  std::string saved;
  std::thread receive_thread([&] {
    ReceiverSession session(receive_options);
    receive_ok = session.ReceiveFile(receiver.get(), (directory / "received").string(), &saved,
                                    nullptr, [](const auto& status) { std::cout << status << '\n'; });
    if (!receive_ok) std::cerr << "receiver failed: " << session.error() << '\n';
  });
  TransferOptions send_options;
  send_options.chunk_size = 64 * 1024;
  SenderSession session(send_options);
  bool send_ok = session.SendFile(sender.get(), source.string());
  if (!send_ok) std::cerr << "sender failed: " << session.error() << '\n';
  receive_thread.join();
  assert(send_ok && receive_ok);
  assert(StreamingFileReader::ComputeFileSHA256(source.string()) ==
         StreamingFileReader::ComputeFileSHA256(saved));
  std::atomic<bool> cancelled{true};
  receiver->SetCancellationFlag(&cancelled);
  assert(!receiver->ReadNextFrame(incoming, received));
  assert(receiver->LastError() == TransportError::kCancelled);
  assert(!receiver->IsConnected());
  receiver->SetCancellationFlag(nullptr);
  sender->Close();
  receiver->Close();

  // Both leases released: a new connection generation can reuse this room.
  options.connection_id = std::string(32, 'b');
  auto reconnect = HttpsRelayChannel::Connect(options);
  assert(reconnect);
  reconnect->SetIoTimeout(150);
  auto start = std::chrono::steady_clock::now();
  assert(!reconnect->ReadNextFrame(incoming, received));
  assert(reconnect->LastError() == TransportError::kTimeout);
  assert(!reconnect->IsConnected());
  assert(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
  reconnect->Close();
  std::cout << "[PASS] HTTPS relay authentication, limits, cancellation, deadline and file transfer\n";
}
