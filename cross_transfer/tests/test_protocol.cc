#include "cross_transfer/protocol.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace cross_transfer;

void TestFrameHeader() {
  FrameHeader hdr{};
  hdr.cmd = FrameHeader::Command::kChunkData;
  hdr.flags = 0;
  for (size_t i = 0; i < 16; ++i) {
    hdr.task_id[i] = static_cast<uint8_t>(i + 1);
  }
  hdr.chunk_index = 42;
  hdr.payload_len = 1024 * 1024;
  hdr.checksum = 0xDEADBEEF;

  auto serialized = hdr.Serialize();
  assert(serialized.size() == 38);

  auto deserialized = FrameHeader::Deserialize(serialized);
  assert(deserialized.has_value());
  assert(deserialized->magic == FrameHeader::kMagic);
  assert(deserialized->version == FrameHeader::kVersion);
  assert(deserialized->cmd == FrameHeader::Command::kChunkData);
  assert(deserialized->flags == 0);
  assert(deserialized->task_id == hdr.task_id);
  assert(deserialized->chunk_index == 42);
  assert(deserialized->payload_len == 1024 * 1024);
  assert(deserialized->checksum == 0xDEADBEEF);

  std::cout << "[PASS] TestFrameHeader" << std::endl;
}

void TestCRC32C() {
  std::vector<uint8_t> test_data = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  uint32_t crc = ComputeCRC32C(test_data);
  // Standard CRC32-C of "123456789" is 0xE3069283
  assert(crc == 0xE3069283);

  std::cout << "[PASS] TestCRC32C (Castagnoli 0xE3069283)" << std::endl;
}

void TestFileMetadata() {
  FileMetadataPayload meta;
  meta.file_name = "test_video.mp4";
  meta.file_size = 1024ULL * 1024ULL * 500ULL; // 500MB
  meta.chunk_size = 1024 * 1024 * 2;           // 2MB
  meta.total_chunks = 250;
  meta.file_sha256 = "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

  auto bytes = meta.Serialize();
  auto opt = FileMetadataPayload::Deserialize(bytes);
  assert(opt.has_value());
  assert(opt->file_name == meta.file_name);
  assert(opt->file_size == meta.file_size);
  assert(opt->chunk_size == meta.chunk_size);
  assert(opt->total_chunks == meta.total_chunks);
  assert(opt->file_sha256 == meta.file_sha256);

  std::cout << "[PASS] TestFileMetadata" << std::endl;
}

void TestHandshake() {
  HandshakePayload hs;
  hs.device_name = "MacBook Pro M3";
  hs.device_os = "mac";
  hs.public_key_hex = "0123456789abcdef";
  hs.accepted = true;
  hs.message = "Ready for transfer";

  auto bytes = hs.Serialize();
  auto opt = HandshakePayload::Deserialize(bytes);
  assert(opt.has_value());
  assert(opt->device_name == hs.device_name);
  assert(opt->device_os == hs.device_os);
  assert(opt->public_key_hex == hs.public_key_hex);
  assert(opt->accepted == hs.accepted);
  assert(opt->message == hs.message);

  std::cout << "[PASS] TestHandshake" << std::endl;
}

int main() {
  TestFrameHeader();
  TestCRC32C();
  TestFileMetadata();
  TestHandshake();
  FrameHeader bad;
  bad.cmd = FrameHeader::Command::kChunkData;
  bad.payload_len = UINT32_MAX;
  assert(!FrameHeader::Deserialize(bad.Serialize()));
  bad.payload_len = 1;
  bad.cmd = static_cast<FrameHeader::Command>(255);
  assert(!FrameHeader::Deserialize(bad.Serialize()));
  FileMetadataPayload meta;
  meta.file_name = "safe.bin";
  meta.file_size = 3;
  meta.chunk_size = 3;
  meta.total_chunks = 1;
  meta.file_sha256 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  const auto good = meta.Serialize();
  for (size_t n = 0; n < good.size(); ++n)
    assert(!FileMetadataPayload::Deserialize(std::span(good).first(n)));
  auto trailing = good;
  trailing.push_back(0);
  assert(!FileMetadataPayload::Deserialize(trailing));
  meta.chunk_size = 0;
  assert(!ValidateFileMetadata(meta));
  meta.chunk_size = 3;
  meta.total_chunks = UINT64_MAX;
  assert(!ValidateFileMetadata(meta));
  meta.total_chunks = 1;
  for (const char* name : {"../escape", "..\\escape", "/absolute", "NUL", "x.", "x:stream"}) {
    meta.file_name = name;
    assert(!ValidateFileMetadata(meta));
  }
  std::cout << "All Protocol tests passed successfully!" << std::endl;
  return 0;
}
