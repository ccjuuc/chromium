#include "cross_transfer/tls_transport_channel.h"
#include "cross_transfer/transfer_session.h"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace cross_transfer;

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::filesystem::path directory(argv[1]);
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
  std::cout << "[PASS] direct TLS mutual authentication, peer SAN and frame transport\n";
}
