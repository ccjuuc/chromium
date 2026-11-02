#include "cross_transfer/streaming_file_io.h"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <vector>

using namespace cross_transfer;

void TestFileReadWritePreallocate() {
  std::string temp_dir = "/tmp/cross_transfer_test_io";
  std::filesystem::create_directories(temp_dir);
  std::string final_file = temp_dir + "/random_test.bin";

  const uint64_t total_size = 5 * 1024 * 1024; // 5MB
  const uint32_t chunk_size = 1024 * 1024;     // 1MB
  const uint64_t total_chunks = 5;

  // 1. Write file out-of-order: chunk 4, 1, 0, 3, 2
  StreamingFileWriter writer;
  assert(writer.OpenAndPreallocate(final_file, total_size));

  std::vector<uint64_t> write_order = {4, 1, 0, 3, 2};
  for (uint64_t idx : write_order) {
    std::vector<uint8_t> chunk(chunk_size, static_cast<uint8_t>(idx + 10));
    assert(writer.WriteChunk(idx, chunk_size, chunk));
  }

  assert(writer.Finalize(final_file));

  // 2. Read back and verify size and contents
  StreamingFileReader reader;
  assert(reader.Open(final_file));
  assert(reader.file_size() == total_size);

  for (uint64_t i = 0; i < total_chunks; ++i) {
    std::vector<uint8_t> read_data;
    assert(reader.ReadChunk(i, chunk_size, read_data));
    assert(read_data.size() == chunk_size);
    for (uint8_t b : read_data) {
      assert(b == static_cast<uint8_t>(i + 10));
    }
  }

  std::string hash = StreamingFileReader::ComputeFileSHA256(final_file);
  assert(!hash.empty());
  std::cout << "[PASS] TestFileReadWritePreallocate (SHA-256: " << hash << ")" << std::endl;

  std::filesystem::remove_all(temp_dir);
}

int main() {
  TestFileReadWritePreallocate();
  std::cout << "All StreamingFileIO tests passed successfully!" << std::endl;
  return 0;
}
