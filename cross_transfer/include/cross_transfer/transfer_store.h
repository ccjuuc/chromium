#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TRANSFER_STORE_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TRANSFER_STORE_H_

#include "cross_transfer/chunk_bitmap.h"
#include "cross_transfer/protocol.h"

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <string>

namespace cross_transfer {

// One owner sequence. A transfer outlives its network connection: destruction
// releases locks but preserves the checked metadata, partial file and bitmap.
// A successful WriteChunk acknowledges durable data and a durable checkpoint.
// One active receiving transaction is allowed per output directory; this also
// prevents concurrent targets that alias under case folding or normalization.
class TransferStore {
 public:
  TransferStore();
  ~TransferStore();
  TransferStore(const TransferStore&) = delete;
  TransferStore& operator=(const TransferStore&) = delete;

  bool Open(const std::string& output_dir, const FileMetadataPayload& metadata,
            const std::array<uint8_t, 16>& task_id,
            const std::atomic<bool>* cancelled = nullptr);
  const ChunkBitmap& bitmap() const { return bitmap_; }
  bool WriteChunk(uint64_t index, std::span<const uint8_t> data);
  bool VerifyAndCommit(std::string* out_path = nullptr,
                       const std::atomic<bool>* cancelled = nullptr,
                       bool allow_overwrite = false);
  const std::string& error() const { return error_; }

 private:
  bool SaveCheckpoint(const ChunkBitmap& bitmap);
  bool Fail(const std::string& error);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  ChunkBitmap bitmap_;
  std::string error_;
};

}  // namespace cross_transfer
#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_TRANSFER_STORE_H_
