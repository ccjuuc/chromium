#include "cross_transfer/chunk_bitmap.h"
#include <cassert>
#include <iostream>

using namespace cross_transfer;

void TestBitmapBasics() {
  ChunkBitmap bitmap(130); // Needs 3 64-bit words
  assert(bitmap.total_chunks() == 130);
  assert(bitmap.completed_chunks() == 0);
  assert(!bitmap.IsAllCompleted());

  // Mark chunk 0, 63, 64, 129
  assert(bitmap.MarkCompleted(0));
  assert(bitmap.MarkCompleted(63));
  assert(bitmap.MarkCompleted(64));
  assert(bitmap.MarkCompleted(129));

  assert(!bitmap.MarkCompleted(0)); // idempotent, already marked

  assert(bitmap.IsCompleted(0));
  assert(bitmap.IsCompleted(63));
  assert(bitmap.IsCompleted(64));
  assert(bitmap.IsCompleted(129));
  assert(!bitmap.IsCompleted(1));
  assert(!bitmap.IsCompleted(65));
  assert(!bitmap.IsCompleted(128));

  assert(bitmap.completed_chunks() == 4);
  assert(!bitmap.IsAllCompleted());

  std::cout << "[PASS] TestBitmapBasics" << std::endl;
}

void TestBitmapSerializationAndMissing() {
  ChunkBitmap bitmap(5);
  bitmap.MarkCompleted(0);
  bitmap.MarkCompleted(2);
  bitmap.MarkCompleted(4);

  auto missing = bitmap.GetMissingChunks();
  assert(missing.size() == 2);
  assert(missing[0] == 1);
  assert(missing[1] == 3);

  auto bytes = bitmap.Serialize();
  auto opt_deserialized = ChunkBitmap::Deserialize(bytes);
  assert(opt_deserialized.has_value());
  assert(opt_deserialized->total_chunks() == 5);
  assert(opt_deserialized->completed_chunks() == 3);
  assert(opt_deserialized->IsCompleted(0));
  assert(!opt_deserialized->IsCompleted(1));
  assert(opt_deserialized->IsCompleted(2));
  assert(!opt_deserialized->IsCompleted(3));
  assert(opt_deserialized->IsCompleted(4));

  std::cout << "[PASS] TestBitmapSerializationAndMissing" << std::endl;
}

int main() {
  TestBitmapBasics();
  TestBitmapSerializationAndMissing();
  ChunkBitmap invalid;
  assert(!invalid.Reset(UINT64_MAX));
  assert(!invalid.valid());
  assert(!invalid.IsAllCompleted());
  assert(!invalid.MarkCompleted(0));
  assert(invalid.Serialize().empty());
  std::vector<uint8_t> malformed(16, 0);
  std::fill(malformed.begin(), malformed.begin() + 8, 255);
  assert(!ChunkBitmap::Deserialize(malformed));
  ChunkBitmap one(1);
  auto padded = one.Serialize();
  padded.back() = 2;  // Bit 1 is padding, not a valid completed chunk.
  assert(!ChunkBitmap::Deserialize(padded));
  std::cout << "All ChunkBitmap tests passed successfully!" << std::endl;
  return 0;
}
