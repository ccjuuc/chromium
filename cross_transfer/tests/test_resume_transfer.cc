#include "cross_transfer/transfer_session.h"
#include "cross_transfer/transport_channel.h"
#include "cross_transfer/streaming_file_io.h"
#include "cross_transfer/chunk_bitmap.h"

#include <cassert>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <random>

using namespace cross_transfer;

void CreateTestFile(const std::string& path, size_t size_bytes) {
  std::ofstream ofs(path, std::ios::binary);
  std::mt19937_64 rng(42);
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
  std::cout << "Testing Resilient Broken-Transfer Resuming Simulation" << std::endl;
  std::cout << "==================================================" << std::endl;

  std::string test_dir = "/tmp/cross_transfer_resume_test";
  std::filesystem::create_directories(test_dir);
  std::string src_file = test_dir + "/source.bin";
  std::string dest_file = test_dir + "/dest.bin";

  const size_t file_size = 6 * 1024 * 1024; // 6MB
  const uint32_t chunk_size = 1024 * 1024;  // 1MB (6 chunks)
  const uint64_t total_chunks = 6;

  CreateTestFile(src_file, file_size);
  std::string src_sha256 = StreamingFileReader::ComputeFileSHA256(src_file);

  // 1. Simulate receiver having partial chunks (e.g. chunks 0, 1, 2 already received before network crash)
  StreamingFileWriter writer;
  assert(writer.OpenAndPreallocate(dest_file, file_size));

  ChunkBitmap receiver_bitmap(total_chunks);
  StreamingFileReader reader;
  assert(reader.Open(src_file));

  std::cout << "Pre-populating chunks 0, 1, 2 (simulating prior partial download)..." << std::endl;
  for (uint64_t i = 0; i < 3; ++i) {
    std::vector<uint8_t> chunk;
    assert(reader.ReadChunk(i, chunk_size, chunk));
    assert(writer.WriteChunk(i, chunk_size, chunk));
    receiver_bitmap.MarkCompleted(i);
  }
  assert(receiver_bitmap.completed_chunks() == 3);

  // 2. Query missing chunks
  auto missing_chunks = receiver_bitmap.GetMissingChunks();
  std::cout << "Receiver reports missing chunks: ";
  for (uint64_t c : missing_chunks) std::cout << c << " ";
  std::cout << std::endl;
  assert(missing_chunks.size() == 3);
  assert(missing_chunks[0] == 3);
  assert(missing_chunks[1] == 4);
  assert(missing_chunks[2] == 5);

  // 3. Sender sends only the missing chunks
  std::cout << "Sender resumes and sends ONLY missing chunks (3, 4, 5)..." << std::endl;
  for (uint64_t idx : missing_chunks) {
    std::vector<uint8_t> chunk;
    assert(reader.ReadChunk(idx, chunk_size, chunk));
    assert(writer.WriteChunk(idx, chunk_size, chunk));
    receiver_bitmap.MarkCompleted(idx);
  }

  assert(receiver_bitmap.IsAllCompleted());
  assert(writer.Finalize(dest_file));

  // 4. Verify integrity
  std::string dest_sha256 = StreamingFileReader::ComputeFileSHA256(dest_file);
  std::cout << "Resumed file SHA-256: " << dest_sha256 << std::endl;
  assert(dest_sha256 == src_sha256);

  std::cout << "[PASS] Resilient Resuming Test successfully verified zero-redundancy recovery!" << std::endl;
  std::cout << "==================================================" << std::endl;

  std::filesystem::remove_all(test_dir);
  return 0;
}
