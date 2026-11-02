#include "cross_transfer/chunk_bitmap.h"
#include "cross_transfer/protocol.h"

#include <bit>

namespace cross_transfer {

namespace {

void WriteUint64(std::span<uint8_t, 8> output, uint64_t value) {
  for (size_t i = 0; i < output.size(); ++i)
    output[i] = static_cast<uint8_t>(value >> (56 - i * 8));
}

uint64_t ReadUint64(std::span<const uint8_t, 8> input) {
  uint64_t value = 0;
  for (uint8_t byte : input) value = (value << 8) | byte;
  return value;
}

} // namespace

ChunkBitmap::ChunkBitmap() : ChunkBitmap(0) {}

ChunkBitmap::~ChunkBitmap() = default;

ChunkBitmap::ChunkBitmap(uint64_t total_chunks) {
  Reset(total_chunks);
}

ChunkBitmap::ChunkBitmap(ChunkBitmap&& other) noexcept {
  std::lock_guard<std::mutex> lock(other.mutex_);
  total_chunks_ = other.total_chunks_;
  completed_chunks_ = other.completed_chunks_;
  valid_ = other.valid_;
  words_ = std::move(other.words_);
  other.total_chunks_ = 0;
  other.completed_chunks_ = 0;
}

ChunkBitmap& ChunkBitmap::operator=(ChunkBitmap&& other) noexcept {
  if (this != &other) {
    std::scoped_lock lock(mutex_, other.mutex_);
    total_chunks_ = other.total_chunks_;
    completed_chunks_ = other.completed_chunks_;
    valid_ = other.valid_;
    words_ = std::move(other.words_);
    other.total_chunks_ = 0;
    other.completed_chunks_ = 0;
  }
  return *this;
}

bool ChunkBitmap::Reset(uint64_t total_chunks) {
  std::lock_guard<std::mutex> lock(mutex_);
  valid_ = total_chunks <= kMaxTotalChunks;
  if (!valid_) {
    total_chunks_ = completed_chunks_ = 0;
    words_.clear();
    return false;
  }
  total_chunks_ = total_chunks;
  completed_chunks_ = 0;
  size_t num_words = total_chunks / 64 + (total_chunks % 64 != 0);
  words_.assign(num_words, 0);
  return true;
}

bool ChunkBitmap::valid() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return valid_;
}

bool ChunkBitmap::MarkCompleted(uint64_t chunk_index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (chunk_index >= total_chunks_) return false;

  size_t word_idx = chunk_index / 64;
  uint64_t bit_mask = 1ULL << (chunk_index % 64);

  if ((words_[word_idx] & bit_mask) == 0) {
    words_[word_idx] |= bit_mask;
    completed_chunks_++;
    return true;
  }
  return false;
}

bool ChunkBitmap::IsCompleted(uint64_t chunk_index) const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (chunk_index >= total_chunks_) return false;

  size_t word_idx = chunk_index / 64;
  uint64_t bit_mask = 1ULL << (chunk_index % 64);
  return (words_[word_idx] & bit_mask) != 0;
}

uint64_t ChunkBitmap::total_chunks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return total_chunks_;
}

uint64_t ChunkBitmap::completed_chunks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return completed_chunks_;
}

bool ChunkBitmap::IsAllCompleted() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return valid_ && completed_chunks_ == total_chunks_;
}

float ChunkBitmap::Progress() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!valid_) return 0.0f;
  if (total_chunks_ == 0) return 1.0f;
  return static_cast<float>(completed_chunks_) / static_cast<float>(total_chunks_);
}

std::vector<uint64_t> ChunkBitmap::GetMissingChunks() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<uint64_t> missing;
  missing.reserve(total_chunks_ - completed_chunks_);

  for (uint64_t i = 0; i < total_chunks_; ++i) {
    size_t word_idx = i / 64;
    uint64_t bit_mask = 1ULL << (i % 64);
    if ((words_[word_idx] & bit_mask) == 0) {
      missing.push_back(i);
    }
  }
  return missing;
}

std::vector<uint8_t> ChunkBitmap::Serialize() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!valid_) return {};
  std::vector<uint8_t> buffer;
  // Format: [8 bytes total_chunks (net)][8 bytes words_count (net)][N * 8 bytes words (net)]
  buffer.resize(16 + words_.size() * sizeof(uint64_t));

  auto bytes = std::span(buffer);
  WriteUint64(bytes.first<8>(), total_chunks_);
  WriteUint64(bytes.subspan<8, 8>(), words_.size());

  for (size_t i = 0; i < words_.size(); ++i) {
    WriteUint64(bytes.subspan(16 + i * 8).first<8>(), words_[i]);
  }

  return buffer;
}

std::optional<ChunkBitmap> ChunkBitmap::Deserialize(std::span<const uint8_t> buffer) {
  if (buffer.size() < 16) return std::nullopt;

  uint64_t total_chunks = ReadUint64(buffer.first<8>());
  uint64_t num_words = ReadUint64(buffer.subspan<8, 8>());

  if (total_chunks > kMaxTotalChunks) return std::nullopt;
  uint64_t expected_words = total_chunks / 64 + (total_chunks % 64 != 0);
  if (num_words != expected_words) {
    return std::nullopt;
  }

  if (buffer.size() != 16 + num_words * sizeof(uint64_t)) {
    return std::nullopt;
  }

  ChunkBitmap bitmap(total_chunks);
  bitmap.words_.resize(num_words);
  for (size_t i = 0; i < num_words; ++i) {
    bitmap.words_[i] = ReadUint64(buffer.subspan(16 + i * 8).first<8>());
  }

  // Padding is not part of the file and must not carry completed chunks.
  if (total_chunks % 64 != 0 &&
      (bitmap.words_.back() >> (total_chunks % 64)) != 0) return std::nullopt;

  // Recalculate completed count
  bitmap.completed_chunks_ = 0;
  for (uint64_t word : bitmap.words_) bitmap.completed_chunks_ += std::popcount(word);

  return bitmap;
}

} // namespace cross_transfer
