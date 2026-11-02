#include "cross_transfer/tls_transport_channel.h"
#include "cross_transfer/transfer_session.h"
#include "test_arguments.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

using namespace cross_transfer;

namespace {

std::string ReadPem(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  assert(file);
  std::ostringstream contents;
  contents << file.rdbuf();
  return contents.str();
}

void ExpectClientHandshake(const TlsConfig& client_config,
                           const TlsConfig& server_config,
                           bool should_connect) {
  LanTcpServer server;
  assert(server.Start(0));
  bool accepted = false;
  std::thread thread([&] {
    auto tcp = server.Accept(3000);
    auto channel = TlsTransportChannel::Accept(std::move(tcp), server_config, 3000);
    accepted = channel != nullptr;
  });
  TransportError error = TransportError::kNone;
  auto client = TlsTransportChannel::Connect(
      "127.0.0.1", server.listening_port(), client_config, 3000, nullptr, &error);
  thread.join();
  assert(static_cast<bool>(client) == should_connect);
  if (should_connect) {
    assert(accepted && error == TransportError::kNone);
  } else {
    assert(error == TransportError::kSecurity);
  }
}

}  // namespace

int main(int argc, char** argv) {
  const auto args = test::CommandLineArguments(argc, argv);
  assert(args.size() == 2);
  const std::filesystem::path directory(args[1]);
  const auto source = directory / "tls-payload.bin";
  {
    std::ofstream file(source, std::ios::binary);
    const std::string contents(128 * 1024, 't');
    file.write(contents.data(), contents.size());
  }
  TlsConfig sender;
  sender.ca_file = (directory / "ca.pem").string();
  sender.certificate_file = (directory / "sender.pem").string();
  sender.private_key_file = (directory / "sender.key").string();
  sender.peer_name = "receiver.test";
  TlsConfig receiver;
  receiver.ca_file = sender.ca_file;
  receiver.certificate_file = (directory / "receiver.pem").string();
  receiver.private_key_file = (directory / "receiver.key").string();
  receiver.peer_name = "sender.test";
  LanTcpServer server;
  assert(server.Start(0));
  bool accepted = false;
  bool received_file = false;
  std::string saved;
  std::thread thread([&] {
    auto tcp = server.Accept(3000);
    auto channel = TlsTransportChannel::Accept(std::move(tcp), receiver, 3000);
    assert(channel && channel->IsSecure());
    FrameHeader header;
    std::vector<uint8_t> body;
    assert(channel->ReadNextFrame(header, body));
    assert(body == std::vector<uint8_t>({1, 2, 3}));
    header.cmd = FrameHeader::Command::kChunkAck;
    header.payload_len = 0;
    header.checksum = 0;
    assert(channel->SendFrame(header, {}));
    accepted = true;
    TransferOptions options;
    options.authorize_receive = [](const auto&, const auto&) { return true; };
    ReceiverSession session(options);
    received_file = session.ReceiveFile(channel.get(), (directory / "tls-received").string(), &saved);
    if (!received_file) std::cerr << session.error() << '\n';
  });
  auto channel = TlsTransportChannel::Connect("127.0.0.1", server.listening_port(), sender, 3000);
  assert(channel);
  FrameHeader header;
  header.cmd = FrameHeader::Command::kChunkData;
  header.task_id.fill(1);
  const std::vector<uint8_t> body{1, 2, 3};
  header.payload_len = body.size();
  header.checksum = ComputeCRC32C(body);
  assert(channel->SendFrame(header, body));
  FrameHeader ack;
  std::vector<uint8_t> ack_body;
  assert(channel->ReadNextFrame(ack, ack_body));
  assert(ack.cmd == FrameHeader::Command::kChunkAck);
  TransferOptions options;
  options.chunk_size = 32 * 1024;
  SenderSession session(options);
  bool sent_file = session.SendFile(channel.get(), source.string());
  if (!sent_file) std::cerr << session.error() << '\n';
  thread.join();
  assert(accepted && sent_file && received_file);
  assert(StreamingFileReader::ComputeFileSHA256(source.string()) ==
         StreamingFileReader::ComputeFileSHA256(saved));
  channel->Close();

  // Embedded roots work on both endpoints without a CA file, including PEM
  // bundles with whitespace around entries.
  const std::string ca_pem = ReadPem(directory / "ca.pem");
  sender.ca_file.clear();
  sender.ca_pem = "\r\n \t" + ca_pem + "\n\t";
  receiver.ca_file.clear();
  receiver.ca_pem = ca_pem;
  ExpectClientHandshake(sender, receiver, true);

  TlsConfig invalid = sender;
  invalid.ca_pem = "-----BEGIN CERTIFICATE-----\ninvalid\n-----END CERTIFICATE-----";
  ExpectClientHandshake(invalid, receiver, false);
  invalid.ca_pem = " \t\r\n";
  ExpectClientHandshake(invalid, receiver, false);
  invalid.ca_pem = ca_pem + "not a certificate";
  ExpectClientHandshake(invalid, receiver, false);
  invalid.ca_pem = std::string(256 * 1024 + 1, 'x');
  ExpectClientHandshake(invalid, receiver, false);

  // A valid but unrelated certificate cannot establish trust in the receiver.
  invalid.ca_pem = ReadPem(directory / "sender.pem");
  ExpectClientHandshake(invalid, receiver, false);
  invalid = sender;
  invalid.ca_file = (directory / "ca.pem").string();
  ExpectClientHandshake(invalid, receiver, false);

  // Supplying an embedded root must not bypass the expected server identity.
  invalid = sender;
  invalid.peer_name = "different-peer.test";
  ExpectClientHandshake(invalid, receiver, false);

  // The server verifies client identity, not only the signing CA.
  receiver.peer_name = "different-peer.test";
  bool rejected = false;
  std::thread reject_thread([&] {
    auto tcp = server.Accept(3000);
    TransportError error;
    auto peer = TlsTransportChannel::Accept(std::move(tcp), receiver, 3000, nullptr, &error);
    rejected = !peer && error == TransportError::kSecurity;
  });
  auto bad_peer = TlsTransportChannel::Connect("127.0.0.1", server.listening_port(), sender, 3000);
  reject_thread.join();
  assert(rejected);
  std::cout << "[PASS] direct TLS mutual authentication, embedded trust, peer SAN and frame transport\n";
}
