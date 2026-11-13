// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_stats.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/third_party/libuv/include/uv.h"

#if BUILDFLAG(IS_POSIX)
#include <unistd.h>

#include "base/posix/safe_strerror.h"
#endif

namespace xenon::ipc {

namespace {

xenon::ipc::mojom::IpcResultPtr Success(base::Value value = base::Value()) {
  auto result = xenon::ipc::mojom::IpcResult::New();
  result->success = true;
  result->value = std::move(value);
  return result;
}

xenon::ipc::mojom::IpcResultPtr Failure(const std::string& error) {
  auto result = xenon::ipc::mojom::IpcResult::New();
  result->success = false;
  result->error = error;
  return result;
}

#if BUILDFLAG(IS_POSIX)
const char* PosixFileSystemErrorCode(int error) {
  switch (error) {
    case EACCES:
      return "EACCES";
    case EEXIST:
      return "EEXIST";
    case EFAULT:
      return "EFAULT";
    case EINTR:
      return "EINTR";
    case EINVAL:
      return "EINVAL";
    case EIO:
      return "EIO";
    case ELOOP:
      return "ELOOP";
    case ENAMETOOLONG:
      return "ENAMETOOLONG";
    case ENOENT:
      return "ENOENT";
    case ENOMEM:
      return "ENOMEM";
    case ENOSPC:
      return "ENOSPC";
    case ENOTDIR:
      return "ENOTDIR";
    case EPERM:
      return "EPERM";
    case EROFS:
      return "EROFS";
    default:
      return "EIO";
  }
}
#endif

xenon::ipc::mojom::IpcResultPtr FileContentsResult(
    const base::DictValue& request,
    const std::string& contents) {
  if (request.FindBool("returnBytes").value_or(false)) {
    return Success(base::Value(
        base::Value::BlobStorage(contents.begin(), contents.end())));
  }
  return Success(base::Value(base::Base64Encode(contents)));
}

struct FileReadRange {
  uint64_t start = 0;
  std::optional<uint64_t> end;
};

bool ReadRangeIndex(const base::Value& value, uint64_t* index) {
  if (!value.is_int() && !value.is_double()) {
    return false;
  }
  const double number = value.GetDouble();
  constexpr double kMaxSafeInteger = 9007199254740991.0;
  if (!std::isfinite(number) || number < 0 || number > kMaxSafeInteger ||
      std::floor(number) != number) {
    return false;
  }
  *index = static_cast<uint64_t>(number);
  return true;
}

bool ParseReadRange(const base::DictValue& request,
                    std::optional<FileReadRange>* range) {
  const base::Value* start = request.Find("start");
  const base::Value* end = request.Find("end");
  if (!start && !end) {
    return true;
  }
  range->emplace();
  if (start && !ReadRangeIndex(*start, &range->value().start)) {
    return false;
  }
  if (end) {
    uint64_t end_index = 0;
    if (!ReadRangeIndex(*end, &end_index) || end_index < range->value().start) {
      return false;
    }
    range->value().end = end_index;
  }
  return true;
}

// Ordinary files use their actual EOF, including readable pseudo-files whose
// stat size is zero. The request owns this handle and its seek position.
bool ReadFileRange(base::File file,
                   const FileReadRange& range,
                   std::string* contents) {
  if (!file.IsValid()) {
    return false;
  }
  contents->clear();
  constexpr uint64_t kMaxOffset = std::numeric_limits<int64_t>::max();
  if (range.start > kMaxOffset) {
    return false;
  }
  const uint64_t start = range.start;
  uint64_t remaining = kMaxOffset - start;
  if (range.end) {
    remaining = std::min(remaining, *range.end - range.start + 1);
  }
  if (start &&
      file.Seek(base::File::FROM_BEGIN, static_cast<int64_t>(start)) < 0) {
    return false;
  }
  // Probe EOF without growing |contents| first. Otherwise an exact-capacity
  // file can double its retained allocation just to discover that EOF follows.
  std::vector<uint8_t> buffer(static_cast<size_t>(
      std::min(remaining, static_cast<uint64_t>(64 * 1024))));
  while (remaining) {
    // base::File::Read(span) accepts an int-sized request internally. Bounded
    // increments avoid allocating a huge requested end on a short file. File
    // length is not an EOF oracle: /proc-style files can report a zero length.
    const size_t count = static_cast<size_t>(
        std::min(remaining, static_cast<uint64_t>(buffer.size())));
    const size_t total = contents->size();
    const auto destination = base::span(buffer).first(count);
    // An initial sequential read also preserves readable non-seekable files.
    const auto read = file.ReadAtCurrentPos(destination);
    if (!read || *read > contents->max_size() - total) {
      contents->clear();
      return false;
    }
    contents->append(reinterpret_cast<const char*>(buffer.data()), *read);
    remaining -= *read;
    if (*read == 0) {
      break;
    }
  }
  return true;
}

bool ReadAsarRange(const xenon::asar::Archive::EntryReader& reader,
                   const FileReadRange& range,
                   std::string* contents) {
  contents->clear();
  // Check the logical boundary before offset arithmetic, including very large
  // starts. Encrypted readers translate only the requested blocks to disk I/O.
  if (range.start >= reader.size()) {
    return true;
  }
  uint64_t remaining = reader.size() - range.start;
  if (range.end) {
    remaining = std::min(remaining, *range.end - range.start + 1);
  }
  std::vector<uint8_t> buffer(
      static_cast<size_t>(std::min<uint64_t>(remaining, 64 * 1024)));
  while (remaining) {
    const size_t count =
        static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
    const size_t total = contents->size();
    const auto read =
        reader.Read(range.start + total, base::span(buffer).first(count));
    if (!read || *read > contents->max_size() - total) {
      contents->clear();
      return false;
    }
    contents->append(reinterpret_cast<const char*>(buffer.data()), *read);
    remaining -= *read;
    if (*read == 0) {
      break;
    }
  }
  return true;
}

bool ResolveAsarEntry(const base::FilePath& path,
                      std::shared_ptr<xenon::asar::Archive>* archive,
                      base::FilePath* relative_path,
                      xenon::asar::Archive::FileInfo* info,
                      bool* is_directory,
                      bool* is_asar_path) {
  base::FilePath archive_path;
  if (!xenon::asar::GetAsarArchivePath(path, &archive_path, relative_path,
                                       /*allow_root=*/true)) {
    *is_asar_path = false;
    return false;
  }
  *is_asar_path = true;
  *archive = xenon::asar::GetOrCreateAsarArchive(archive_path);
  if (!*archive) {
    return false;
  }
  return (*archive)->StatPath(*relative_path, info, is_directory);
}

std::string StatNanoseconds(const uv_timespec_t& time) {
  // Decimal concatenation keeps the full seconds range without overflowing
  // int64 or requiring a platform-specific 128-bit integer type.
  uint64_t seconds = time.tv_sec >= 0
                         ? static_cast<uint64_t>(time.tv_sec)
                         : static_cast<uint64_t>(-(time.tv_sec + 1));
  uint64_t nanos = time.tv_nsec;
  if (time.tv_sec < 0) {
    nanos = 1000000000 - nanos;
    if (nanos == 1000000000) {
      ++seconds;
      nanos = 0;
    }
  }
  std::string value = base::NumberToString(nanos);
  if (seconds != 0) {
    value = base::NumberToString(seconds) +
            std::string(9 - value.size(), '0') + value;
  }
  return time.tv_sec < 0 ? "-" + value : value;
}

base::DictValue MakeArchiveStat(bool is_file,
                               bool is_directory,
                               uint64_t size,
                               bool bigint) {
  base::DictValue stat;
  stat.Set("isFile", is_file);
  stat.Set("isDirectory", is_directory);
  stat.Set("isSymbolicLink", false);
  if (bigint) {
    stat.Set("size", base::NumberToString(size));
    stat.Set("mtimeNs", "0");
    stat.Set("birthtimeNs", "0");
  } else {
    stat.Set("size", static_cast<double>(size));
    stat.Set("mtimeMs", 0.0);
    stat.Set("birthtimeMs", 0.0);
  }
  return stat;
}

}  // namespace

base::DictValue FileSystemStatValue(const uv_stat_t& stat, bool bigint) {
  base::DictValue result;
  auto integer = [&](const char* name, uint64_t value) {
    if (bigint) {
      result.Set(name, base::NumberToString(value));
    } else {
      result.Set(name, static_cast<double>(value));
    }
  };
  integer("dev", stat.st_dev);
  integer("mode", stat.st_mode);
  integer("nlink", stat.st_nlink);
  integer("uid", stat.st_uid);
  integer("gid", stat.st_gid);
  integer("rdev", stat.st_rdev);
  integer("blksize", stat.st_blksize);
  integer("ino", stat.st_ino);
  integer("size", stat.st_size);
  integer("blocks", stat.st_blocks);
  auto time = [&](const char* name, const uv_timespec_t& value) {
    if (bigint) {
      result.Set(std::string(name) + "Ns", StatNanoseconds(value));
    } else {
      result.Set(std::string(name) + "Ms",
                 static_cast<double>(value.tv_sec) * 1000 +
                     value.tv_nsec / 1000000.0);
    }
  };
  time("atime", stat.st_atim);
  time("mtime", stat.st_mtim);
  time("ctime", stat.st_ctim);
  time("birthtime", stat.st_birthtim);
  // libuv normalizes these Node/POSIX type bits on every supported platform.
  const uint64_t type = stat.st_mode & 0170000;
  result.Set("isFile", type == 0100000);
  result.Set("isDirectory", type == 0040000);
  result.Set("isSymbolicLink", type == 0120000);
  return result;
}

xenon::ipc::mojom::IpcResultPtr PerformFileSystemCall(base::Value arguments) {
  if (!arguments.is_list() || arguments.GetList().empty() ||
      !arguments.GetList().front().is_dict()) {
    return Failure("EINVAL: invalid argument, fs request");
  }
  const base::DictValue& request = arguments.GetList().front().GetDict();
  const std::string* operation = request.FindString("operation");
  const std::string* path_string = request.FindString("path");
  if (!operation || !path_string) {
    return Failure("EINVAL: fs request requires operation and path");
  }
  const auto* no_asar_value = request.Find("noAsar");
  if (no_asar_value && !no_asar_value->is_bool()) {
    return Failure("EINVAL: noAsar must be a boolean");
  }
  const bool no_asar = request.FindBool("noAsar").value_or(false);
  const bool bigint = request.FindBool("bigint").value_or(false);

  const base::FilePath path = base::FilePath::FromUTF8Unsafe(*path_string);
  auto error = [&](const char* code, const char* description,
                   const char* syscall) {
    return Failure(std::string(code) + ": " + description + ", " + syscall +
                   " '" + *path_string + "'");
  };
#if BUILDFLAG(IS_POSIX)
  auto posix_error = [&](int error_number, const char* syscall) {
    const std::string description = base::safe_strerror(error_number);
    return error(PosixFileSystemErrorCode(error_number), description.c_str(),
                 syscall);
  };
#endif

  if ((*operation == "symlink" || *operation == "readlink" ||
       *operation == "access" || *operation == "mkdtemp" ||
       *operation == "chmod" || *operation == "stat" ||
       *operation == "lstat") &&
      path_string->find('\0') != std::string::npos) {
    return error("EINVAL", "path must not contain null bytes",
                 operation->c_str());
  }

  int access_mode = 0;
  if (*operation == "access") {
    const base::Value* mode = request.Find("mode");
    if (mode && (!mode->is_int() || mode->GetInt() < 0 || mode->GetInt() > 7)) {
      return error("EINVAL", "invalid access mode", "access");
    }
    access_mode = mode ? mode->GetInt() : 0;
  }
  auto access_file = [&](const base::FilePath& target) {
    uv_fs_t access_request = {};
    const int status =
        uv_fs_access(nullptr, &access_request, target.AsUTF8Unsafe().c_str(),
                     access_mode, nullptr);
    uv_fs_req_cleanup(&access_request);
    return status == 0
               ? Success()
               : error(uv_err_name(status), uv_strerror(status), "access");
  };

  int chmod_mode = 0;
  if (*operation == "chmod") {
    const base::Value* mode = request.Find("mode");
    if (!mode || (!mode->is_int() && !mode->is_double()) ||
        !std::isfinite(mode->GetDouble()) || mode->GetDouble() < 0 ||
        mode->GetDouble() > std::numeric_limits<uint32_t>::max() ||
        std::floor(mode->GetDouble()) != mode->GetDouble()) {
      return error("EINVAL", "invalid permission mode", "chmod");
    }
    chmod_mode = static_cast<uint32_t>(mode->GetDouble()) & 07777;
  }

  std::optional<FileReadRange> read_range;
  if (*operation == "read_file" && !ParseReadRange(request, &read_range)) {
    return error("EINVAL", "invalid file read range", "read");
  }

  std::shared_ptr<xenon::asar::Archive> asar_archive;
  base::FilePath asar_relative_path;
  xenon::asar::Archive::FileInfo asar_info;
  bool asar_is_directory = false;
  bool is_asar_path = false;
  const bool asar_found =
      !no_asar &&
      ResolveAsarEntry(path, &asar_archive, &asar_relative_path, &asar_info,
                       &asar_is_directory, &is_asar_path);
  if (is_asar_path) {
    if (*operation == "exists") {
      return Success(base::Value(asar_found));
    }
    if (*operation == "access") {
      if (!asar_found) {
        return error("ENOENT", "no such file or directory", "access");
      }
      if (asar_info.unpacked) {
        base::FilePath unpacked_path;
        if (!asar_archive->GetUnpackedPath(asar_relative_path,
                                           &unpacked_path)) {
          return error("ENOENT", "no such file or directory", "access");
        }
        return access_file(unpacked_path);
      }
      // Packed members are immutable even when the physical archive is
      // writable. Unpacked members above use the real host permissions.
      return (access_mode & 2) ? error("EACCES", "permission denied", "access")
                               : Success();
    }
    if (!asar_found) {
      return error("ENOENT", "no such file or directory", operation->c_str());
    }
    if (*operation == "stat" || *operation == "lstat") {
      return Success(base::Value(MakeArchiveStat(
          !asar_is_directory, asar_is_directory,
          asar_is_directory ? 0 : asar_info.size, bigint)));
    }
    if (*operation == "read_file") {
      if (asar_is_directory) {
        return error("EISDIR", "illegal operation on a directory", "read");
      }
      std::string contents;
      bool read = false;
      if (!read_range) {
        read = asar_archive->ReadFile(asar_relative_path, &contents);
      } else {
        auto reader = asar_archive->CreateReader(asar_relative_path);
        read = reader && ReadAsarRange(*reader, *read_range, &contents);
      }
      if (!read) {
        return error("ENOENT", "no such file or directory", "open");
      }
      return FileContentsResult(request, contents);
    }
    if (*operation == "realpath") {
      return Success(base::Value(*path_string));
    }
    return error("EACCES", "operation not permitted on asar archive",
                 operation->c_str());
  }

  if (*operation == "exists") {
    return Success(base::Value(base::PathExists(path)));
  }
  if (*operation == "mkdtemp") {
    // libuv uses the platform's atomic exclusive directory creation. Append
    // exactly six template characters without normalizing the supplied prefix.
    const std::string pattern = *path_string + "XXXXXX";
    uv_fs_t temporary_request = {};
    const int status = uv_fs_mkdtemp(nullptr, &temporary_request,
                                    pattern.c_str(), nullptr);
    const std::string created =
        status == 0 ? temporary_request.path : std::string();
    uv_fs_req_cleanup(&temporary_request);
    if (status != 0) {
      return Failure(std::string(uv_err_name(status)) + ": " +
                     uv_strerror(status) + ", mkdtemp '" + pattern + "'");
    }
    return FileContentsResult(request, created);
  }
  if (*operation == "symlink" || *operation == "readlink") {
#if BUILDFLAG(IS_POSIX)
    if (path.empty()) {
      return error("ENOENT", "no such file or directory", operation->c_str());
    }
    if (*operation == "symlink") {
      const std::string* target = request.FindString("target");
      const base::Value* type = request.Find("type");
      if (!target || target->find('\0') != std::string::npos ||
          (type && (!type->is_string() || (type->GetString() != "file" &&
                                           type->GetString() != "dir" &&
                                           type->GetString() != "junction")))) {
        return error("EINVAL", "invalid symlink target or type", "symlink");
      }
      if (target->empty()) {
        return error("ENOENT", "no such file or directory", "symlink");
      }
      // The target is opaque link content, including TH's pid/start-token
      // payload. Do not resolve it, require it to exist, or preflight the link
      // with PathExists: symlink(2) atomically rejects an existing dangling
      // link.
      if (!base::CreateSymbolicLink(base::FilePath::FromUTF8Unsafe(*target),
                                    path)) {
        const int os_error = errno;
        return posix_error(os_error, "symlink");
      }
      return Success();
    }
    base::FilePath target;
    if (!base::ReadSymbolicLink(path, &target)) {
      const int os_error = errno;
      return posix_error(os_error, "readlink");
    }
    return FileContentsResult(request, target.value());
#else
    // Windows link creation needs separate reparse-point/privilege handling.
    // Never substitute a regular file or claim that the lock was acquired.
    return error("ENOSYS", "operation not supported", operation->c_str());
#endif
  }
  if (*operation == "stat" || *operation == "lstat") {
    uv_fs_t stat_request = {};
    const int status = *operation == "lstat"
                           ? uv_fs_lstat(nullptr, &stat_request,
                                         path.AsUTF8Unsafe().c_str(), nullptr)
                           : uv_fs_stat(nullptr, &stat_request,
                                        path.AsUTF8Unsafe().c_str(), nullptr);
    auto result = status == 0
                      ? Success(base::Value(
                            FileSystemStatValue(stat_request.statbuf, bigint)))
                      : error(uv_err_name(status), uv_strerror(status),
                              operation->c_str());
    uv_fs_req_cleanup(&stat_request);
    return result;
  }
  if (*operation == "read_file") {
    if (base::DirectoryExists(path)) {
      return error("EISDIR", "illegal operation on a directory", "read");
    }
    std::string contents;
    const bool read =
        read_range ? ReadFileRange(base::File(path, base::File::FLAG_OPEN |
                                                        base::File::FLAG_READ),
                                   *read_range, &contents)
                   : base::ReadFileToString(path, &contents);
    if (!read) {
      return error("ENOENT", "no such file or directory", "open");
    }
    return FileContentsResult(request, contents);
  }
  if (*operation == "write_file" || *operation == "append_file") {
    const base::Value* bytes = request.Find("data");
    std::string decoded;
    std::string_view data;
    if (bytes && bytes->is_blob()) {
      const auto& blob = bytes->GetBlob();
      data = std::string_view(reinterpret_cast<const char*>(blob.data()),
                              blob.size());
    } else if (const std::string* encoded = request.FindString("dataBase64");
               encoded && base::Base64Decode(*encoded, &decoded)) {
      data = decoded;
    } else {
      return error("EINVAL", "invalid file data", "write");
    }
    if (!base::DirectoryExists(path.DirName())) {
      return error("ENOENT", "no such file or directory", "open");
    }
    if (base::DirectoryExists(path)) {
      return error("EISDIR", "illegal operation on a directory", "open");
    }
    bool ok;
    if (*operation == "append_file") {
      // Node appendFile and WriteStream(flags: 'a') create a missing file.
      // Open/create with append semantics atomically; exists-then-write would
      // race another writer and could truncate its data.
      base::File file(path,
                      base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_APPEND);
      ok = file.IsValid() &&
           file.WriteAtCurrentPosAndCheck(base::as_byte_span(data));
    } else {
      ok = base::WriteFile(path, data);
    }
    return ok ? Success() : error("EACCES", "permission denied", "open");
  }
  if (*operation == "readdir") {
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory", "scandir");
    }
    if (!base::DirectoryExists(path)) {
      return error("ENOTDIR", "not a directory", "scandir");
    }
    base::ListValue names;
    base::FileEnumerator enumerator(
        path, false,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES);
    for (base::FilePath child = enumerator.Next(); !child.empty();
         child = enumerator.Next()) {
      names.Append(child.BaseName().AsUTF8Unsafe());
    }
    return Success(base::Value(std::move(names)));
  }
  if (*operation == "mkdir") {
    const bool recursive = request.FindBool("recursive").value_or(false);
    if (base::PathExists(path)) {
      return recursive && base::DirectoryExists(path)
                 ? Success()
                 : error("EEXIST", "file already exists", "mkdir");
    }
    if (!recursive && !base::DirectoryExists(path.DirName())) {
      return error("ENOENT", "no such file or directory", "mkdir");
    }
    return base::CreateDirectory(path)
               ? Success()
               : error("EACCES", "permission denied", "mkdir");
  }
  if (*operation == "unlink") {
#if BUILDFLAG(IS_POSIX)
    // PathExists follows links and misses dangling lock links. unlink(2)
    // removes only the link itself, including links to directories.
    return ::unlink(path.value().c_str()) == 0 ? Success()
                                               : posix_error(errno, "unlink");
#else
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory", "unlink");
    }
    if (base::DirectoryExists(path)) {
      return error("EISDIR", "illegal operation on a directory", "unlink");
    }
    return base::DeleteFile(path)
               ? Success()
               : error("EACCES", "permission denied", "unlink");
#endif
  }
  if (*operation == "rm" || *operation == "rmdir") {
    const bool force = request.FindBool("force").value_or(false);
    const bool recursive = request.FindBool("recursive").value_or(false);
    if (!base::PathExists(path)) {
      return force ? Success()
                   : error("ENOENT", "no such file or directory",
                           operation->c_str());
    }
    if (*operation == "rmdir" && !base::DirectoryExists(path)) {
      return error("ENOTDIR", "not a directory", "rmdir");
    }
    const bool ok =
        recursive ? base::DeletePathRecursively(path) : base::DeleteFile(path);
    return ok ? Success()
              : error("ENOTEMPTY", "directory not empty", operation->c_str());
  }
  if (*operation == "access") {
    return access_file(path);
  }
  if (*operation == "rename" || *operation == "copy_file") {
    const std::string* destination_string = request.FindString("destination");
    if (!destination_string) {
      return error("EINVAL", "missing destination", operation->c_str());
    }
    const base::FilePath destination =
        base::FilePath::FromUTF8Unsafe(*destination_string);
    if (!base::PathExists(path) ||
        !base::DirectoryExists(destination.DirName())) {
      return error("ENOENT", "no such file or directory", operation->c_str());
    }
    const bool ok = *operation == "rename" ? base::Move(path, destination)
                                           : base::CopyFile(path, destination);
    return ok ? Success()
              : error("EACCES", "permission denied", operation->c_str());
  }
  if (*operation == "realpath") {
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory", "realpath");
    }
    base::FilePath normalized;
    if (!base::NormalizeFilePath(path, &normalized)) {
      normalized = path;
    }
    return Success(base::Value(normalized.AsUTF8Unsafe()));
  }
  if (*operation == "chmod") {
    // Use the same platform implementation as Node. Windows applies its real
    // read-only attribute; POSIX applies permission bits and follows symlinks.
    uv_fs_t chmod_request = {};
    const int status = uv_fs_chmod(nullptr, &chmod_request,
                                   path.AsUTF8Unsafe().c_str(), chmod_mode,
                                   nullptr);
    uv_fs_req_cleanup(&chmod_request);
    return status == 0
               ? Success()
               : error(uv_err_name(status), uv_strerror(status), "chmod");
  }
  return error("ENOSYS", "operation not supported", operation->c_str());
}

}  // namespace xenon::ipc
