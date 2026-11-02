#include "cross_transfer/transfer_store.h"

#include "cross_transfer/streaming_file_io.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cross_transfer {
namespace {
constexpr std::array<uint8_t, 8> kStateMagic{'C', 'T', 'S', 'T', 0, 0, 0, 1};
constexpr size_t kStateLimit = 2 * 1024 * 1024;
std::filesystem::path Path(const std::string& value) {
  return std::filesystem::path(std::u8string(value.begin(), value.end()));
}
std::string Utf8(const std::filesystem::path& path) {
  auto value = path.u8string();
  return std::string(value.begin(), value.end());
}
std::string HexId(const std::array<uint8_t, 16>& id) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string output;
  for (uint8_t byte : id) { output += digits[byte >> 4]; output += digits[byte & 15]; }
  return output;
}
bool RegularOrMissing(const std::filesystem::path& path, bool* exists) {
  std::error_code ec;
  auto status = std::filesystem::symlink_status(path, ec);
  *exists = status.type() != std::filesystem::file_type::not_found;
  return (!*exists && (!ec || ec == std::errc::no_such_file_or_directory)) ||
         (!ec && std::filesystem::is_regular_file(status));
}
bool EnsureDirectory(const std::filesystem::path& path) {
  std::error_code ec;
  auto status = std::filesystem::symlink_status(path, ec);
  if (status.type() == std::filesystem::file_type::not_found) {
    ec.clear();
    std::filesystem::create_directories(path, ec);
    if (ec) return false;
    status = std::filesystem::symlink_status(path, ec);
  }
  return !ec && std::filesystem::is_directory(status);
}
bool SyncDirectory(const std::filesystem::path& path) {
#if defined(_WIN32)
  // Atomic file replacements use MOVEFILE_WRITE_THROUGH on Windows.
  (void)path;
  return true;
#else
  int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  int result;
  do { result = fsync(fd); } while (result < 0 && errno == EINTR);
  close(fd);
  return result == 0;
#endif
}
class FileLock {
 public:
  ~FileLock() {
#if defined(_WIN32)
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
#else
    if (fd_ >= 0) close(fd_);
#endif
  }
  bool Acquire(const std::filesystem::path& path) {
#if defined(_WIN32)
    handle_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(handle_, &info) && info.nNumberOfLinks == 1 &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY));
#else
    fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd_ < 0) return false;
    struct stat info{};
    return fstat(fd_, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
           flock(fd_, LOCK_EX | LOCK_NB) == 0;
#endif
  }
 private:
#if defined(_WIN32)
  HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
  int fd_ = -1;
#endif
};
bool ReadState(const std::filesystem::path& path, std::vector<uint8_t>* output) {
#if defined(_WIN32)
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  LARGE_INTEGER size{};
  bool ok = GetFileInformationByHandle(handle, &info) && GetFileSizeEx(handle, &size) &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
      info.nNumberOfLinks == 1 && size.QuadPart >= 0 && size.QuadPart <= kStateLimit;
  if (ok) {
    output->resize(static_cast<size_t>(size.QuadPart));
    DWORD received = 0;
    ok = ReadFile(handle, output->data(), static_cast<DWORD>(output->size()), &received, nullptr) &&
         received == output->size();
  }
  CloseHandle(handle);
  return ok;
#else
  int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat info{};
  bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_nlink == 1 &&
            info.st_size >= 0 && static_cast<uint64_t>(info.st_size) <= kStateLimit;
  if (ok) {
    output->resize(static_cast<size_t>(info.st_size));
    size_t offset = 0;
    while (offset < output->size()) {
      auto remaining = std::span(*output).subspan(offset);
      ssize_t n = read(fd, remaining.data(), remaining.size());
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) { ok = false; break; }
      offset += static_cast<size_t>(n);
    }
  }
  close(fd);
  return ok;
#endif
}
void Append32(std::vector<uint8_t>* output, size_t size) {
  for (int shift : {24, 16, 8, 0}) output->push_back(static_cast<uint8_t>(size >> shift));
}
bool Take32(std::span<const uint8_t>* data, size_t* result) {
  if (data->size() < 4) return false;
  *result = 0;
  for (size_t i = 0; i < 4; ++i) *result = (*result << 8) | (*data)[i];
  *data = data->subspan(4);
  return true;
}
}  // namespace

struct TransferStore::Impl {
  FileMetadataPayload meta;
  std::array<uint8_t, 16> task_id{};
  std::filesystem::path directory, staging, target, partial, checkpoint;
  FileLock directory_lock;
  StreamingFileWriter writer;
  bool open = false;
  bool committed = false;
};
TransferStore::TransferStore() = default;
TransferStore::~TransferStore() = default;
bool TransferStore::Fail(const std::string& error) {
  error_ = error;
  return false;
}

bool TransferStore::Open(const std::string& output_dir,
                         const FileMetadataPayload& metadata,
                         const std::array<uint8_t, 16>& task_id,
                         const std::atomic<bool>* cancelled) {
  impl_.reset();
  bitmap_.Reset(0);
  error_.clear();
  auto interrupted = [&] {
    if (!cancelled || !cancelled->load()) return false;
    impl_.reset();
    bitmap_.Reset(0);
    Fail("Opening transfer store cancelled");
    return true;
  };
  if (interrupted()) return false;
  if (!ValidateFileMetadata(metadata, &error_)) return false;
  if (!IsValidUtf8Path(output_dir) || !IsValidUtf8Path(metadata.file_name))
    return Fail("Paths must contain valid UTF-8 without NUL bytes");
  if (metadata.file_name == ".cross-transfer") return Fail("Reserved destination name");
  if (std::all_of(task_id.begin(), task_id.end(), [](uint8_t b) { return b == 0; }))
    return Fail("Missing transfer identity");
  auto next = std::make_unique<Impl>();
  next->meta = metadata;
  next->task_id = task_id;
  if (output_dir.empty() || !EnsureDirectory(Path(output_dir)))
    return Fail("Destination must be a real directory");
  std::error_code ec;
  next->directory = std::filesystem::canonical(Path(output_dir), ec);
  if (ec) return Fail("Cannot resolve destination directory");
  next->staging = next->directory / ".cross-transfer";
  if (!EnsureDirectory(next->staging)) return Fail("Unsafe transfer staging directory");
  if (!SyncDirectory(next->directory)) return Fail("Cannot persist staging directory");
  // One active receiving transaction per destination directory. This also avoids
  // case-folding and Unicode aliases acquiring different target-name locks.
  if (!next->directory_lock.Acquire(next->staging / "receiver.lock"))
    return Fail("Destination already has an active receiver or an unsafe lock");
  next->target = next->directory / Path(metadata.file_name);
  next->partial = next->staging / (HexId(task_id) + ".part");
  next->checkpoint = next->staging / (HexId(task_id) + ".state");
  bool target_exists = false, partial_exists = false, checkpoint_exists = false;
  if (!RegularOrMissing(next->target, &target_exists) ||
      !RegularOrMissing(next->partial, &partial_exists) ||
      !RegularOrMissing(next->checkpoint, &checkpoint_exists))
    return Fail("Transfer paths must be regular files, never symlinks");
  impl_ = std::move(next);
  if (interrupted()) return false;
  if (checkpoint_exists) {
    std::vector<uint8_t> bytes;
    if (!ReadState(impl_->checkpoint, &bytes) || bytes.size() < 8 + 16 + 4 + 4 + 64)
      return Fail("Invalid transfer checkpoint");
    std::span<const uint8_t> content(bytes.data(), bytes.size() - 64);
    std::string digest(bytes.end() - 64, bytes.end());
    if (ComputeSHA256(content) != digest ||
        !std::equal(kStateMagic.begin(), kStateMagic.end(), content.begin()) ||
        !std::equal(task_id.begin(), task_id.end(), content.begin() + 8))
      return Fail("Checkpoint integrity or identity mismatch");
    content = content.subspan(24);
    size_t meta_size = 0;
    auto encoded_meta = metadata.Serialize();
    if (!Take32(&content, &meta_size) || meta_size > content.size() ||
        meta_size != encoded_meta.size() ||
        !std::equal(encoded_meta.begin(), encoded_meta.end(), content.begin()))
      return Fail("Resume metadata does not match the original file");
    content = content.subspan(meta_size);
    size_t bitmap_size = 0;
    if (!Take32(&content, &bitmap_size) || bitmap_size != content.size())
      return Fail("Invalid checkpoint bitmap length");
    auto saved = ChunkBitmap::Deserialize(content);
    if (!saved || saved->total_chunks() != metadata.total_chunks)
      return Fail("Invalid checkpoint bitmap");
    bitmap_ = std::move(*saved);
    if (partial_exists) {
      if (!impl_->writer.Resume(Utf8(impl_->partial), metadata.file_size))
        return Fail("Cannot reopen partial file without truncation");
    } else {
      // A commit may have succeeded just before the final ACK was lost.
      uint64_t actual_size = target_exists ? std::filesystem::file_size(impl_->target, ec) : 0;
      if (!target_exists || ec || actual_size != metadata.file_size || !bitmap_.IsAllCompleted())
        return Fail("Checkpoint data is missing");
      std::string target_digest = StreamingFileReader::ComputeFileSHA256(Utf8(impl_->target), cancelled);
      if (interrupted()) return false;
      if (target_digest != metadata.file_sha256) return Fail("Committed file changed since checkpoint");
      impl_->committed = true;
    }
  } else {
    if (partial_exists) return Fail("Partial file exists without a trusted checkpoint");
    auto abort_initialization = [&](std::string reason) {
      impl_->writer.Close();
      // Only discard a partial created by this Open. If checkpoint replacement
      // succeeded but its directory sync failed, keep both files for recovery.
      bool state_exists = false;
      bool cleanup_ok = RegularOrMissing(impl_->checkpoint, &state_exists);
      if (cleanup_ok && !state_exists) {
        bool created_partial_exists = false;
        cleanup_ok = RegularOrMissing(impl_->partial, &created_partial_exists);
        if (cleanup_ok && created_partial_exists) {
          std::error_code cleanup_error;
          cleanup_ok = std::filesystem::remove(impl_->partial, cleanup_error) &&
                       !cleanup_error && SyncDirectory(impl_->staging);
        }
      }
      impl_.reset();
      bitmap_.Reset(0);
      if (!cleanup_ok) reason += "; cannot clean up uncheckpointed partial";
      return Fail(reason);
    };
    if (!bitmap_.Reset(metadata.total_chunks) ||
        !impl_->writer.Create(Utf8(impl_->partial), metadata.file_size) ||
        !impl_->writer.Flush())
      return abort_initialization("Cannot create durable partial file");
    if (!SaveCheckpoint(bitmap_)) return abort_initialization(error_);
  }
  if (interrupted()) return false;
  impl_->open = true;
  return true;
}

bool TransferStore::SaveCheckpoint(const ChunkBitmap& bitmap) {
  std::vector<uint8_t> bytes(kStateMagic.begin(), kStateMagic.end());
  bytes.insert(bytes.end(), impl_->task_id.begin(), impl_->task_id.end());
  auto meta = impl_->meta.Serialize();
  auto bits = bitmap.Serialize();
  Append32(&bytes, meta.size());
  bytes.insert(bytes.end(), meta.begin(), meta.end());
  Append32(&bytes, bits.size());
  bytes.insert(bytes.end(), bits.begin(), bits.end());
  std::string digest = ComputeSHA256(bytes);
  if (digest.empty()) return Fail("Cannot hash checkpoint");
  bytes.insert(bytes.end(), digest.begin(), digest.end());
  if (bytes.size() > kStateLimit) return Fail("Checkpoint exceeds storage limit");
  auto scratch = impl_->checkpoint;
  scratch += ".next";
  bool exists = false;
  if (!RegularOrMissing(scratch, &exists)) return Fail("Unsafe checkpoint temporary path");
  std::error_code ec;
  if (exists && !std::filesystem::remove(scratch, ec))
    return Fail("Cannot remove interrupted checkpoint write");
  StreamingFileWriter checkpoint;
  if (!checkpoint.Create(Utf8(scratch), bytes.size()) ||
      !checkpoint.WriteChunk(0, static_cast<uint32_t>(bytes.size()), bytes) ||
      !checkpoint.Finalize(Utf8(impl_->checkpoint)) || !SyncDirectory(impl_->staging))
    return Fail("Cannot persist checkpoint");
  return true;
}

bool TransferStore::WriteChunk(uint64_t index, std::span<const uint8_t> data) {
  if (!impl_ || !impl_->open) return Fail("Transfer store is not open");
  const auto& meta = impl_->meta;
  if (index >= meta.total_chunks) return Fail("Invalid chunk index");
  uint64_t offset = index * meta.chunk_size;
  size_t expected = static_cast<size_t>(std::min<uint64_t>(meta.chunk_size, meta.file_size - offset));
  if (data.size() != expected) return Fail("Invalid chunk length");
  if (bitmap_.IsCompleted(index)) return true;
  auto pending = ChunkBitmap::Deserialize(bitmap_.Serialize());
  if (!pending || !pending->MarkCompleted(index)) return Fail("Cannot update completion bitmap");
  if (!impl_->writer.WriteChunk(index, meta.chunk_size, data) || !impl_->writer.Flush())
    return Fail("Cannot persist file chunk");
  if (!SaveCheckpoint(*pending)) return false;
  bitmap_ = std::move(*pending);
  return true;
}

bool TransferStore::VerifyAndCommit(std::string* out_path,
                                    const std::atomic<bool>* cancelled,
                                    bool allow_overwrite) {
  if (!impl_ || !impl_->open) return Fail("Transfer store is not open");
  if (cancelled && cancelled->load()) return Fail("Verification cancelled");
  if (!bitmap_.IsAllCompleted()) return Fail("File still has missing chunks");
  if (!impl_->committed) {
    std::string digest = impl_->writer.ComputeSHA256(cancelled);
    if (cancelled && cancelled->load()) return Fail("Verification cancelled");
    if (digest != impl_->meta.file_sha256) {
      // Do not advertise corrupt data as resumable on the next connection.
      ChunkBitmap missing(impl_->meta.total_chunks);
      if (!SaveCheckpoint(missing)) return false;
      bitmap_ = std::move(missing);
      return Fail("SHA256 verification failed; existing destination was preserved");
    }
    if (cancelled && cancelled->load()) return Fail("Verification cancelled");
    if (!impl_->writer.Finalize(Utf8(impl_->target), allow_overwrite))
      return Fail("Cannot atomically commit file; destination may exist without overwrite permission");
    impl_->committed = true;
    if (!SyncDirectory(impl_->directory) || !SyncDirectory(impl_->staging))
      return Fail("Cannot sync committed file directory");
  }
  if (out_path) *out_path = Utf8(impl_->target);
  return true;
}
}  // namespace cross_transfer
