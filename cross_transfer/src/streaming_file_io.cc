#include "cross_transfer/streaming_file_io.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <filesystem>
#include <limits>
#include <memory>
#include <string_view>
#include <openssl/evp.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <stdio.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/syscall.h>
#endif
#endif

namespace cross_transfer {
namespace {
std::filesystem::path NativePath(const std::string& path) {
  return std::filesystem::path(std::u8string(path.begin(), path.end()));
}
std::string Hex(std::span<const uint8_t> bytes) {
  constexpr std::string_view digits = "0123456789abcdef";
  std::string result(bytes.size() * 2, '0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    result[2 * i] = digits[bytes[i] >> 4];
    result[2 * i + 1] = digits[bytes[i] & 15];
  }
  return result;
}
class Sha256 {
 public:
  Sha256() : context_(EVP_MD_CTX_new(), EVP_MD_CTX_free) {
    valid_ = context_ && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1;
  }
  ~Sha256() = default;
  bool Update(std::span<const uint8_t> bytes) {
    if (valid_ && !bytes.empty())
      valid_ = EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) == 1;
    return valid_;
  }
  std::string Finish() {
    std::array<uint8_t, 32> digest{};
    unsigned int size = 0;
    return valid_ && EVP_DigestFinal_ex(context_.get(), digest.data(), &size) == 1 &&
                   size == digest.size() ? Hex(digest) : std::string{};
  }
 private:
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context_;
  bool valid_ = false;
};
bool IsRegularOrAbsent(const std::string& path) {
  std::error_code error;
  auto status = std::filesystem::symlink_status(NativePath(path), error);
  return (!error && std::filesystem::is_regular_file(status)) ||
         status.type() == std::filesystem::file_type::not_found;
}
}  // namespace

std::string ComputeSHA256(std::span<const uint8_t> data) {
  Sha256 hash;
  return hash.Update(data) ? hash.Finish() : std::string{};
}
bool IsValidUtf8Path(std::string_view path) {
  for (size_t offset = 0; offset < path.size();) {
    uint8_t first = static_cast<uint8_t>(path[offset++]);
    if (first == 0) return false;
    if (first < 0x80) continue;
    unsigned continuation = 0;
    uint32_t value = 0, minimum = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation = 1; value = first & 0x1f; minimum = 0x80;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation = 2; value = first & 0x0f; minimum = 0x800;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation = 3; value = first & 7; minimum = 0x10000;
    } else return false;
    if (continuation > path.size() - offset) return false;
    while (continuation--) {
      uint8_t byte = static_cast<uint8_t>(path[offset++]);
      if ((byte & 0xc0) != 0x80) return false;
      value = (value << 6) | (byte & 0x3f);
    }
    if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
      return false;
  }
  return true;
}
StreamingFileReader::StreamingFileReader() = default;
StreamingFileReader::~StreamingFileReader() { Close(); }
bool StreamingFileReader::Open(const std::string& path) {
  Close();
  if (!IsValidUtf8Path(path)) return false;
  std::error_code error;
  if (!std::filesystem::is_regular_file(NativePath(path), error) || error) return false;
  file_.open(NativePath(path), std::ios::binary | std::ios::ate);
  if (!file_.is_open()) return false;
  auto size = file_.tellg();
  if (size < 0) { Close(); return false; }
  file_size_ = static_cast<uint64_t>(size);
  file_.seekg(0, std::ios::beg);
  return file_.good();
}
bool StreamingFileReader::ReadChunk(uint64_t index, uint32_t size,
                                   std::vector<uint8_t>& data) {
  data.clear();
  if (!file_.is_open() || size == 0 || index > file_size_ / size) return false;
  uint64_t offset = index * size;
  if (offset >= file_size_) return false;
  size_t count = static_cast<size_t>(std::min<uint64_t>(size, file_size_ - offset));
  data.resize(count);
  file_.clear();
  file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  file_.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(count));
  if (file_.gcount() != static_cast<std::streamsize>(count) || file_.bad()) {
    data.clear();
    return false;
  }
  return true;
}
void StreamingFileReader::Close() {
  if (file_.is_open()) file_.close();
  file_.clear();
  file_size_ = 0;
}
std::string StreamingFileReader::ComputeFileSHA256(const std::string& path,
                                                  const std::atomic<bool>* cancelled) {
  if (!IsValidUtf8Path(path)) return {};
  std::ifstream input(NativePath(path), std::ios::binary);
  if (!input.is_open()) return {};
  Sha256 hash;
  std::array<uint8_t, 64 * 1024> data{};
  while (input) {
    if (cancelled && cancelled->load()) return {};
    input.read(reinterpret_cast<char*>(data.data()), data.size());
    if (!hash.Update(std::span(data).first(static_cast<size_t>(input.gcount())))) return {};
  }
  return input.eof() && !input.bad() ? hash.Finish() : std::string{};
}

StreamingFileWriter::StreamingFileWriter() = default;
StreamingFileWriter::~StreamingFileWriter() { Close(); }
bool StreamingFileWriter::OpenAndPreallocate(const std::string& path, uint64_t size) {
  return Create(path + ".downloading", size);
}
bool StreamingFileWriter::Create(const std::string& path, uint64_t size) {
  return OpenTemporary(path, size, true);
}
bool StreamingFileWriter::Resume(const std::string& path, uint64_t size) {
  return OpenTemporary(path, size, false);
}
bool StreamingFileWriter::OpenTemporary(const std::string& path, uint64_t size,
                                        bool create) {
  Close();
  temp_path_.clear();
  total_size_ = 0;
  if (!IsValidUtf8Path(path)) return false;
  if (size > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) return false;
#if defined(_WIN32)
  HANDLE handle = CreateFileW(NativePath(path).c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ, nullptr, create ? CREATE_NEW : OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  LARGE_INTEGER actual{};
  bool valid = GetFileInformationByHandle(handle, &info) &&
      !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) &&
      info.nNumberOfLinks == 1 && GetFileSizeEx(handle, &actual);
  if (valid && create) {
    LARGE_INTEGER wanted{}; wanted.QuadPart = static_cast<LONGLONG>(size);
    valid = SetFilePointerEx(handle, wanted, nullptr, FILE_BEGIN) && SetEndOfFile(handle);
  } else if (valid) valid = static_cast<uint64_t>(actual.QuadPart) == size;
  if (!valid) { CloseHandle(handle); return false; }
  fd_ = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_BINARY);
  if (fd_ < 0) { CloseHandle(handle); return false; }
#else
  int flags = O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT | O_EXCL : 0);
  fd_ = open(NativePath(path).c_str(), flags, 0600);
  if (fd_ < 0) return false;
  struct stat status{};
  bool valid = fstat(fd_, &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 1;
  if (valid && create) valid = ftruncate(fd_, static_cast<off_t>(size)) == 0;
  else if (valid) valid = static_cast<uint64_t>(status.st_size) == size;
  if (!valid) { Close(); return false; }
#endif
  temp_path_ = path;
  total_size_ = size;
  return true;
}
bool StreamingFileWriter::WriteChunk(uint64_t index, uint32_t size,
                                     std::span<const uint8_t> data) {
  if (fd_ < 0 || size == 0 || index > total_size_ / size) return false;
  uint64_t offset = index * size;
  if (data.size() > total_size_ - offset) return false;
  while (!data.empty()) {
#if defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd_));
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return false;
    DWORD written = 0;
    DWORD count = static_cast<DWORD>(std::min(data.size(), size_t{0x7fffffff}));
    if (!WriteFile(handle, data.data(), count, &written, nullptr) || written == 0) return false;
    size_t count_written = written;
#else
    ssize_t written = pwrite(fd_, data.data(), data.size(), static_cast<off_t>(offset));
    if (written < 0 && errno == EINTR) continue;
    if (written <= 0) return false;
    size_t count_written = static_cast<size_t>(written);
#endif
    offset += count_written;
    data = data.subspan(count_written);
  }
  return true;
}
bool StreamingFileWriter::Flush() {
  if (fd_ < 0) return false;
#if defined(_WIN32)
  return FlushFileBuffers(reinterpret_cast<HANDLE>(_get_osfhandle(fd_))) != 0;
#else
  int result;
  do { result = fsync(fd_); } while (result < 0 && errno == EINTR);
  return result == 0;
#endif
}
std::string StreamingFileWriter::ComputeSHA256(const std::atomic<bool>* cancelled) const {
  if (fd_ < 0) return {};
  if (cancelled && cancelled->load()) return {};
  Sha256 hash;
  std::array<uint8_t, 64 * 1024> data{};
  uint64_t offset = 0;
  while (offset < total_size_) {
    if (cancelled && cancelled->load()) return {};
    size_t count = static_cast<size_t>(std::min<uint64_t>(data.size(), total_size_ - offset));
#if defined(_WIN32)
    HANDLE handle = reinterpret_cast<HANDLE>(_get_osfhandle(fd_));
    LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return {};
    DWORD received = 0;
    if (!ReadFile(handle, data.data(), static_cast<DWORD>(count), &received, nullptr) || received == 0) return {};
    size_t count_read = received;
#else
    ssize_t received = pread(fd_, data.data(), count, static_cast<off_t>(offset));
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) return {};
    size_t count_read = static_cast<size_t>(received);
#endif
    if (!hash.Update(std::span(data).first(count_read))) return {};
    offset += count_read;
  }
  return hash.Finish();
}
bool StreamingFileWriter::Finalize(const std::string& target, bool allow_overwrite) {
  if (!IsValidUtf8Path(target)) return false;
  if (temp_path_.empty() || !IsRegularOrAbsent(target)) return false;
  if (fd_ >= 0 && !Flush()) return false;
#if !defined(_WIN32)
  struct stat source{}, opened{};
  if (lstat(NativePath(temp_path_).c_str(), &source) != 0 || !S_ISREG(source.st_mode)) return false;
  if (fd_ >= 0 && (fstat(fd_, &opened) != 0 || opened.st_dev != source.st_dev ||
                  opened.st_ino != source.st_ino)) return false;
#endif
  Close();
#if defined(_WIN32)
  if (!MoveFileExW(NativePath(temp_path_).c_str(), NativePath(target).c_str(),
                   (allow_overwrite ? MOVEFILE_REPLACE_EXISTING : 0) |
                       MOVEFILE_WRITE_THROUGH)) return false;
#else
  if (allow_overwrite) {
    if (rename(NativePath(temp_path_).c_str(), NativePath(target).c_str()) != 0) return false;
  } else {
#if defined(__APPLE__)
    if (renamex_np(NativePath(temp_path_).c_str(), NativePath(target).c_str(),
                   RENAME_EXCL) != 0) return false;
#elif defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, NativePath(temp_path_).c_str(),
                AT_FDCWD, NativePath(target).c_str(), RENAME_NOREPLACE) != 0) return false;
#else
    // Fail closed on platforms without an atomic no-replace rename.
    return false;
#endif
  }
#endif
  temp_path_.clear();
  return true;
}
void StreamingFileWriter::Close() {
  if (fd_ < 0) return;
#if defined(_WIN32)
  _close(fd_);
#else
  close(fd_);
#endif
  fd_ = -1;
}
}  // namespace cross_transfer
