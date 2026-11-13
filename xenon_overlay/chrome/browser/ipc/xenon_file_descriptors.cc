// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_file_descriptors.h"

#include <fcntl.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_file.h"
#include "base/strings/string_view_util.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_stats.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/third_party/libuv/include/uv.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMaxTransfer = 32 * 1024 * 1024;
constexpr size_t kMaxDescriptors = 1024;
mojom::IpcResultPtr Success(base::Value value = base::Value()) {
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = std::move(value);
  return result;
}
mojom::IpcResultPtr Error(int code, std::string_view operation) {
  auto result = mojom::IpcResult::New();
  result->error = std::string(uv_err_name(code)) + ": " + uv_strerror(code) +
                  ", " + std::string(operation);
  return result;
}
struct Request {
  uv_fs_t value = {};
  ~Request() { uv_fs_req_cleanup(&value); }
};
std::optional<int64_t> Integer(const base::Value* value,
                               int64_t fallback,
                               int64_t maximum = 9007199254740991LL) {
  if (!value || value->is_none()) {
    return fallback;
  }
  if (!value->is_int() && !value->is_double()) {
    return std::nullopt;
  }
  const double number = value->GetDouble();
  if (!std::isfinite(number) || number < 0 || number > maximum ||
      std::floor(number) != number) {
    return std::nullopt;
  }
  return static_cast<int64_t>(number);
}
std::optional<int> OpenFlags(const base::Value* value) {
  if (!value) {
    return UV_FS_O_RDONLY;
  }
  if (value->is_int() && value->GetInt() >= 0) {
    return value->GetInt();
  }
  if (!value->is_string()) {
    return std::nullopt;
  }
  const std::string& flag = value->GetString();
  if (flag == "r") {
    return UV_FS_O_RDONLY;
  }
  if (flag == "r+") {
    return UV_FS_O_RDWR;
  }
  if (flag == "rs" || flag == "sr") {
    return UV_FS_O_RDONLY | UV_FS_O_SYNC;
  }
  if (flag == "rs+" || flag == "sr+") {
    return UV_FS_O_RDWR | UV_FS_O_SYNC;
  }
  if (flag == "w") {
    return UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC;
  }
  if (flag == "w+") {
    return UV_FS_O_RDWR | UV_FS_O_CREAT | UV_FS_O_TRUNC;
  }
  if (flag == "wx" || flag == "xw") {
    return UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_EXCL;
  }
  if (flag == "wx+" || flag == "xw+") {
    return UV_FS_O_RDWR | UV_FS_O_CREAT | UV_FS_O_TRUNC | UV_FS_O_EXCL;
  }
  if (flag == "a") {
    return UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_APPEND;
  }
  if (flag == "a+") {
    return UV_FS_O_RDWR | UV_FS_O_CREAT | UV_FS_O_APPEND;
  }
  if (flag == "ax" || flag == "xa") {
    return UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_APPEND | UV_FS_O_EXCL;
  }
  if (flag == "ax+" || flag == "xa+") {
    return UV_FS_O_RDWR | UV_FS_O_CREAT | UV_FS_O_APPEND | UV_FS_O_EXCL;
  }
  if (flag == "as" || flag == "sa") {
    return UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_APPEND | UV_FS_O_SYNC;
  }
  if (flag == "as+" || flag == "sa+") {
    return UV_FS_O_RDWR | UV_FS_O_CREAT | UV_FS_O_APPEND | UV_FS_O_SYNC;
  }
  return std::nullopt;
}
}  // namespace

struct XenonFileDescriptors::Entry {
  base::ScopedTempFile extracted;
  uint64_t owner = 0;
};
XenonFileDescriptors::XenonFileDescriptors(base::FilePath working_directory,
                                           bool owns_process_directory)
    : working_directory_(std::move(working_directory)),
      owns_process_directory_(owns_process_directory) {}

int XenonFileDescriptors::GetWorkingDirectory(base::FilePath* directory) const {
  if (!owns_process_directory_) {
    *directory = working_directory_;
    return 0;
  }
  std::vector<char> buffer(1024);
  size_t length = buffer.size();
  int status = uv_cwd(buffer.data(), &length);
  if (status == UV_ENOBUFS) {
    buffer.resize(length);
    status = uv_cwd(buffer.data(), &length);
  }
  if (status == 0) {
    *directory = base::FilePath::FromUTF8Unsafe(
        base::as_string_view(base::span(buffer).first(length)));
  }
  return status;
}

int XenonFileDescriptors::ChangeWorkingDirectory(const std::string& directory) {
  if (!owns_process_directory_) {
    return UV_ENOTSUP;
  }
  if (directory.find('\0') != std::string::npos) {
    return UV_EINVAL;
  }
  return uv_chdir(directory.c_str());
}
XenonFileDescriptors::~XenonFileDescriptors() {
  CloseAll();
}
base::DictValue XenonFileDescriptors::Constants() {
  base::DictValue value;
#define XENON_FS_CONSTANT(name) value.Set(#name, name)
  XENON_FS_CONSTANT(O_RDONLY);
  XENON_FS_CONSTANT(O_WRONLY);
  XENON_FS_CONSTANT(O_RDWR);
  XENON_FS_CONSTANT(O_CREAT);
  XENON_FS_CONSTANT(O_EXCL);
  XENON_FS_CONSTANT(O_TRUNC);
  XENON_FS_CONSTANT(O_APPEND);
#if defined(O_NOFOLLOW)
  XENON_FS_CONSTANT(O_NOFOLLOW);
#endif
#if defined(O_DIRECTORY)
  XENON_FS_CONSTANT(O_DIRECTORY);
#endif
#if defined(O_SYNC)
  XENON_FS_CONSTANT(O_SYNC);
#endif
#if defined(O_DSYNC)
  XENON_FS_CONSTANT(O_DSYNC);
#endif
#if defined(O_NONBLOCK)
  XENON_FS_CONSTANT(O_NONBLOCK);
#endif
#if defined(O_NOCTTY)
  XENON_FS_CONSTANT(O_NOCTTY);
#endif
#if defined(O_DIRECT)
  XENON_FS_CONSTANT(O_DIRECT);
#endif
#undef XENON_FS_CONSTANT
  return value;
}
void XenonFileDescriptors::CloseAll() {
  base::AutoLock guard(lock_);
  closed_ = true;
  for (const auto& [fd, entry] : files_) {
    Request request;
    uv_fs_close(nullptr, &request.value, fd, nullptr);
  }
  files_.clear();
}

mojom::IpcResultPtr XenonFileDescriptors::Call(base::Value arguments) {
  return CallOwned(std::move(arguments), 0);
}
void XenonFileDescriptors::CloseOwned(uint64_t owner) {
  base::AutoLock guard(lock_);
  closed_owners_.insert(owner);
  for (auto it = files_.begin(); it != files_.end();) {
    if (it->second->owner != owner) {
      ++it;
      continue;
    }
    Request request;
    uv_fs_close(nullptr, &request.value, it->first, nullptr);
    it = files_.erase(it);
  }
}
mojom::IpcResultPtr XenonFileDescriptors::CallOwned(base::Value arguments,
                                                    uint64_t owner) {
  if (!arguments.is_list() || arguments.GetList().empty() ||
      !arguments.GetList().front().is_dict()) {
    return Error(UV_EINVAL, "fs request");
  }
  auto& options = arguments.GetList().front().GetDict();
  const auto* operation = options.FindString("operation");
  if (!operation) {
    return Error(UV_EINVAL, "fs operation");
  }
  if (!operation->starts_with("fd_")) {
    return PerformFileSystemCall(std::move(arguments));
  }
  base::AutoLock guard(lock_);
  if (closed_ || closed_owners_.contains(owner)) {
    return Error(UV_EBADF, *operation);
  }
  Request request;
  if (*operation == "fd_open") {
    const auto* name = options.FindString("path");
    const auto flags = OpenFlags(options.Find("flags"));
    const auto mode = Integer(options.Find("mode"), 0666, 0xffffffffLL);
    if (!name || name->find('\0') != std::string::npos || !flags || !mode) {
      return Error(UV_EINVAL, "open");
    }
    if (name->empty()) {
      return Error(UV_ENOENT, "open");
    }
    if (const auto* no_asar = options.Find("noAsar");
        no_asar && !no_asar->is_bool()) {
      return Error(UV_EINVAL, "open");
    }
    if (files_.size() >= kMaxDescriptors) {
      return Error(UV_EMFILE, "open");
    }
    auto path = base::FilePath::FromUTF8Unsafe(*name);
    if (!path.IsAbsolute()) {
      base::FilePath directory;
      const int status = GetWorkingDirectory(&directory);
      if (status != 0) {
        return Error(status, "open");
      }
      path = directory.Append(path);
    }
    auto entry = std::make_unique<Entry>();
    entry->owner = owner;
    base::FilePath archive_path, relative;
    if (!options.FindBool("noAsar").value_or(false) &&
        asar::GetAsarArchivePath(path, &archive_path, &relative, true)) {
      auto archive = asar::GetOrCreateAsarArchive(archive_path);
      asar::Archive::FileInfo info;
      bool directory = false;
      if (!archive || !archive->StatPath(relative, &info, &directory)) {
        return Error(UV_ENOENT, "open");
      }
      if (directory) {
        return Error(UV_EISDIR, "open");
      }
      if (*flags & (UV_FS_O_WRONLY | UV_FS_O_RDWR | UV_FS_O_CREAT |
                    UV_FS_O_TRUNC | UV_FS_O_APPEND)) {
        return Error(UV_EROFS, "open");
      }
      if (info.unpacked) {
        if (!archive->GetUnpackedPath(relative, &path)) {
          return Error(UV_ENOENT, "open");
        }
      } else {
        // Electron's open API needs an actual descriptor, so packed members
        // are extracted to a private temporary file for this handle's lifetime.
        auto reader = archive->CreateReader(relative);
        if (!reader || !entry->extracted.Create()) {
          return Error(UV_EIO, "open");
        }
        path = entry->extracted.path();
        base::File output(path, base::File::FLAG_OPEN | base::File::FLAG_WRITE);
        if (!output.IsValid()) {
          return Error(UV_EIO, "open");
        }
        std::vector<uint8_t> buffer(64 * 1024);
        for (uint64_t offset = 0; offset < reader->size();) {
          const auto length =
              std::min<uint64_t>(buffer.size(), reader->size() - offset);
          auto bytes = base::span(buffer).first(static_cast<size_t>(length));
          auto count = reader->Read(offset, bytes);
          if (!count || *count == 0 ||
              !output.WriteAtCurrentPosAndCheck(bytes.first(*count))) {
            return Error(UV_EIO, "open");
          }
          offset += *count;
        }
      }
    }
    const int fd =
        uv_fs_open(nullptr, &request.value, path.AsUTF8Unsafe().c_str(), *flags,
                   static_cast<int>(*mode & 07777), nullptr);
    if (fd < 0) {
      return Error(fd, "open");
    }
    files_.emplace(fd, std::move(entry));
    return Success(base::Value(fd));
  }
  const auto fd = options.FindInt("fd");
  if (!fd || !files_.contains(*fd)) {
    return Error(UV_EBADF, *operation);
  }
  int result = 0;
  if (*operation == "fd_close") {
    result = uv_fs_close(nullptr, &request.value, *fd, nullptr);
    files_.erase(*fd);
  } else if (*operation == "fd_fstat") {
    result = uv_fs_fstat(nullptr, &request.value, *fd, nullptr);
    if (result == 0) {
      return Success(base::Value(FileSystemStatValue(
          request.value.statbuf, options.FindBool("bigint").value_or(false))));
    }
  } else if (*operation == "fd_fsync") {
    result = uv_fs_fsync(nullptr, &request.value, *fd, nullptr);
  } else if (*operation == "fd_fdatasync") {
    result = uv_fs_fdatasync(nullptr, &request.value, *fd, nullptr);
  } else if (*operation == "fd_ftruncate") {
    const auto length = Integer(options.Find("length"), 0);
    if (!length) {
      return Error(UV_EINVAL, "ftruncate");
    }
    result = uv_fs_ftruncate(nullptr, &request.value, *fd, *length, nullptr);
  } else if (*operation == "fd_fchmod") {
    const auto mode = Integer(options.Find("mode"), -1, 0xffffffffLL);
    if (!mode || *mode < 0) {
      return Error(UV_EINVAL, "fchmod");
    }
    result = uv_fs_fchmod(nullptr, &request.value, *fd,
                          static_cast<int>(*mode & 07777), nullptr);
  } else if (*operation == "fd_read" || *operation == "fd_write") {
    const auto* raw_position = options.Find("position");
    const bool current_position =
        raw_position && (raw_position->is_int() || raw_position->is_double()) &&
        raw_position->GetDouble() == -1;
    const auto position = current_position ? std::optional<int64_t>(-1)
                                           : Integer(raw_position, -1);
    if (!position) {
      return Error(UV_EINVAL, *operation);
    }
    std::vector<char> bytes;
    if (*operation == "fd_read") {
      const auto length = Integer(options.Find("length"), 0, kMaxTransfer);
      if (!length) {
        return Error(UV_EINVAL, "read");
      }
      bytes.resize(static_cast<size_t>(*length));
    } else {
      const auto* data = options.FindBlob("data");
      if (!data || data->size() > kMaxTransfer) {
        return Error(UV_EINVAL, "write");
      }
      bytes.assign(data->begin(), data->end());
    }
    uv_buf_t buffer =
        uv_buf_init(bytes.data(), static_cast<unsigned int>(bytes.size()));
    result = *operation == "fd_read"
                 ? uv_fs_read(nullptr, &request.value, *fd, &buffer, 1,
                              *position, nullptr)
                 : uv_fs_write(nullptr, &request.value, *fd, &buffer, 1,
                               *position, nullptr);
    if (result >= 0) {
      if (*operation == "fd_write") {
        return Success(base::Value(result));
      }
      return Success(base::Value(
          base::Value::BlobStorage(bytes.begin(), bytes.begin() + result)));
    }
  } else {
    return Error(UV_ENOSYS, *operation);
  }
  return result < 0 ? Error(result, *operation) : Success();
}
}  // namespace xenon::ipc
