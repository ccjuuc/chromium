// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"

#include <algorithm>
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
#include "base/values.h"
#include "xenon_overlay/common/asar/archive.h"

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

// The file is already owned by this request. Packed ASAR entries add a base
// offset and a logical size; ordinary and unpacked files use their actual EOF.
bool ReadFileRange(base::File file,
                   const FileReadRange& range,
                   uint64_t base_offset,
                   std::optional<uint64_t> entry_size,
                   std::string* contents) {
  if (!file.IsValid()) {
    return false;
  }
  contents->clear();
  // Check the entry boundary before adding its offset, including when the
  // requested start is MAX_SAFE_INTEGER. Unpacked files use physical EOF.
  if (entry_size && range.start >= *entry_size) {
    return true;
  }
  constexpr uint64_t kMaxOffset = std::numeric_limits<int64_t>::max();
  if (base_offset > kMaxOffset || range.start > kMaxOffset - base_offset) {
    return false;
  }
  const uint64_t start = base_offset + range.start;
  uint64_t remaining = kMaxOffset - start;
  if (range.end) {
    remaining = std::min(remaining, *range.end - range.start + 1);
  }
  if (entry_size) {
    remaining = std::min(remaining, *entry_size - range.start);
  } else if (start && file.Seek(base::File::FROM_BEGIN,
                                static_cast<int64_t>(start)) < 0) {
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
    // ASAR duplicates may share an OS seek pointer, so their reads must carry
    // an explicit position. Ordinary/unpacked handles are request-local; an
    // initial sequential read also preserves readable non-seekable files.
    const auto read =
        entry_size ? file.Read(static_cast<int64_t>(start + total), destination)
                   : file.ReadAtCurrentPos(destination);
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
  if (!xenon::asar::GetAsarArchivePath(path, &archive_path, relative_path)) {
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

base::DictValue MakeStatDict(bool is_file,
                             bool is_directory,
                             double size,
                             double mtime_ms) {
  base::DictValue stat;
  stat.Set("isFile", is_file);
  stat.Set("isDirectory", is_directory);
  stat.Set("isSymbolicLink", false);
  stat.Set("size", size);
  stat.Set("mtimeMs", mtime_ms);
  stat.Set("birthtimeMs", mtime_ms);
  return stat;
}

}  // namespace

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

  const base::FilePath path = base::FilePath::FromUTF8Unsafe(*path_string);
  auto error = [&](const char* code, const char* description,
                   const char* syscall) {
    return Failure(std::string(code) + ": " + description + ", " + syscall +
                   " '" + *path_string + "'");
  };

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
      ResolveAsarEntry(path, &asar_archive, &asar_relative_path, &asar_info,
                       &asar_is_directory, &is_asar_path);
  if (is_asar_path) {
    if (*operation == "exists") {
      return Success(base::Value(asar_found));
    }
    if (*operation == "access") {
      return asar_found ? Success()
                        : error("ENOENT", "no such file or directory",
                                "access");
    }
    if (!asar_found) {
      return error("ENOENT", "no such file or directory", operation->c_str());
    }
    if (*operation == "stat" || *operation == "lstat") {
      return Success(base::Value(MakeStatDict(
          !asar_is_directory, asar_is_directory,
          asar_is_directory ? 0.0 : static_cast<double>(asar_info.size), 0.0)));
    }
    if (*operation == "read_file") {
      if (asar_is_directory) {
        return error("EISDIR", "illegal operation on a directory", "read");
      }
      std::string contents;
      bool read = false;
      if (!read_range) {
        read = asar_archive->ReadFile(asar_relative_path, &contents);
      } else if (asar_info.unpacked) {
        base::FilePath unpacked_path;
        if (asar_archive->GetUnpackedPath(asar_relative_path, &unpacked_path)) {
          read = ReadFileRange(
              base::File(unpacked_path,
                         base::File::FLAG_OPEN | base::File::FLAG_READ),
              *read_range, 0, std::nullopt, &contents);
        }
      } else {
        read = ReadFileRange(asar_archive->DuplicateFile(), *read_range,
                             asar_info.offset, asar_info.size, &contents);
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
  if (*operation == "stat" || *operation == "lstat") {
    base::File::Info info;
    if (!base::GetFileInfo(path, &info)) {
      return error("ENOENT", "no such file or directory", "stat");
    }
    base::DictValue stat;
    stat.Set("isFile", !info.is_directory);
    stat.Set("isDirectory", info.is_directory);
    stat.Set("isSymbolicLink", false);
    stat.Set("size", static_cast<double>(info.size));
    stat.Set("mtimeMs", info.last_modified.InMillisecondsFSinceUnixEpoch());
    stat.Set("birthtimeMs", info.creation_time.InMillisecondsFSinceUnixEpoch());
    return Success(base::Value(std::move(stat)));
  }
  if (*operation == "read_file") {
    if (base::DirectoryExists(path)) {
      return error("EISDIR", "illegal operation on a directory", "read");
    }
    std::string contents;
    const bool read =
        read_range ? ReadFileRange(base::File(path, base::File::FLAG_OPEN |
                                                        base::File::FLAG_READ),
                                   *read_range, 0, std::nullopt, &contents)
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
    if (!base::PathExists(path)) {
      return error("ENOENT", "no such file or directory", "unlink");
    }
    if (base::DirectoryExists(path)) {
      return error("EISDIR", "illegal operation on a directory", "unlink");
    }
    return base::DeleteFile(path)
               ? Success()
               : error("EACCES", "permission denied", "unlink");
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
    const bool ok = recursive ? base::DeletePathRecursively(path)
                              : base::DeleteFile(path);
    return ok ? Success()
              : error("ENOTEMPTY", "directory not empty",
                      operation->c_str());
  }
  if (*operation == "access") {
    return base::PathExists(path)
               ? Success()
               : error("ENOENT", "no such file or directory", "access");
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
      return error("ENOENT", "no such file or directory",
                   operation->c_str());
    }
    const bool ok = *operation == "rename"
                        ? base::Move(path, destination)
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
    return base::PathExists(path)
               ? Success()
               : error("ENOENT", "no such file or directory", "chmod");
  }
  return error("ENOSYS", "operation not supported", operation->c_str());
}

}  // namespace xenon::ipc
