#ifndef CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_STREAMING_FILE_IO_H_
#define CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_STREAMING_FILE_IO_H_

#include <cstdint>
#include <string>
#include <vector>
#include <span>
#include <memory>
#include <fstream>
#include <atomic>
#include <string_view>

namespace cross_transfer {

// Returns standard lowercase SHA-256, or an empty string on failure.
std::string ComputeSHA256(std::span<const uint8_t> data);
// File APIs accept UTF-8 on every platform and reject embedded NULs.
bool IsValidUtf8Path(std::string_view path);

class StreamingFileReader {
 public:
  StreamingFileReader();
  ~StreamingFileReader();

  bool Open(const std::string& path);
  bool ReadChunk(uint64_t chunk_index, uint32_t chunk_size, std::vector<uint8_t>& out_data);
  uint64_t file_size() const { return file_size_; }
  void Close();

  static std::string ComputeFileSHA256(const std::string& path,
                                      const std::atomic<bool>* cancelled = nullptr);

 private:
  std::ifstream file_;
  uint64_t file_size_{0};
};

class StreamingFileWriter {
 public:
  StreamingFileWriter();
  ~StreamingFileWriter();

  // Creates path + ".downloading". Never truncates an existing partial file.
  bool OpenAndPreallocate(const std::string& path, uint64_t total_size);

  bool Create(const std::string& temporary_path, uint64_t total_size);
  bool Resume(const std::string& temporary_path, uint64_t total_size);

  // All methods require one owner sequence.
  bool WriteChunk(uint64_t chunk_index, uint32_t chunk_size, std::span<const uint8_t> data);

  bool Flush();
  std::string ComputeSHA256(const std::atomic<bool>* cancelled = nullptr) const;

  // Atomic commit; when replacement is disabled, an existing target is never
  // overwritten, including one created after the transfer was authorized.
  bool Finalize(const std::string& final_target_path, bool allow_overwrite = true);

  void Close();

 private:
  bool OpenTemporary(const std::string& path, uint64_t total_size, bool create);
  std::string temp_path_;
  uint64_t total_size_{0};
  int fd_{-1};
};

} // namespace cross_transfer

#endif  // CROSS_TRANSFER_INCLUDE_CROSS_TRANSFER_STREAMING_FILE_IO_H_
