#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_CHUNK_BITMAP_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_CHUNK_BITMAP_H_

#include <cstdint>
#include <vector>
#include <mutex>
#include <span>
#include <optional>

namespace cross_transfer {

class ChunkBitmap {
 public:
  explicit ChunkBitmap(uint64_t total_chunks);
  ~ChunkBitmap();
  ChunkBitmap();
  ChunkBitmap(ChunkBitmap&& other) noexcept;
  ChunkBitmap& operator=(ChunkBitmap&& other) noexcept;

  ChunkBitmap(const ChunkBitmap&) = delete;
  ChunkBitmap& operator=(const ChunkBitmap&) = delete;

  // Rejects unreasonable sizes without allocating; invalid bitmaps never complete.
  bool Reset(uint64_t total_chunks);
  bool valid() const;

  bool MarkCompleted(uint64_t chunk_index);
  bool IsCompleted(uint64_t chunk_index) const;

  uint64_t total_chunks() const;
  uint64_t completed_chunks() const;
  bool IsAllCompleted() const;
  float Progress() const;

  // Retrieve list of all chunk indices that are not yet received
  std::vector<uint64_t> GetMissingChunks() const;

  // Serialize bitmap for syncing between sender and receiver
  std::vector<uint8_t> Serialize() const;

  // Deserialize and merge or load bitmap
  static std::optional<ChunkBitmap> Deserialize(std::span<const uint8_t> buffer);

 private:
  uint64_t total_chunks_{0};
  uint64_t completed_chunks_{0};
  bool valid_{true};
  std::vector<uint64_t> words_;
  mutable std::mutex mutex_;
};

} // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_CHUNK_BITMAP_H_
