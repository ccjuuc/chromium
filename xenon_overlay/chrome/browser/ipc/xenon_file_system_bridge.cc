// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"

#include <memory>
#include <string>
#include <utility>

#include "base/base64.h"
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
      if (!asar_archive->ReadFile(asar_relative_path, &contents)) {
        return error("ENOENT", "no such file or directory", "open");
      }
      return Success(base::Value(base::Base64Encode(contents)));
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
    if (!base::ReadFileToString(path, &contents)) {
      return error("ENOENT", "no such file or directory", "open");
    }
    return Success(base::Value(base::Base64Encode(contents)));
  }
  if (*operation == "write_file" || *operation == "append_file") {
    const std::string* encoded = request.FindString("dataBase64");
    std::string data;
    if (!encoded || !base::Base64Decode(*encoded, &data)) {
      return error("EINVAL", "invalid file data", "write");
    }
    if (!base::DirectoryExists(path.DirName())) {
      return error("ENOENT", "no such file or directory", "open");
    }
    if (base::DirectoryExists(path)) {
      return error("EISDIR", "illegal operation on a directory", "open");
    }
    const bool ok = *operation == "append_file"
                        ? base::AppendToFile(path, data)
                        : base::WriteFile(path, data);
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
