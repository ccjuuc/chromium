// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Portions copyright (c) 2014 GitHub, Inc. under the MIT license.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/asar/archive.h"

#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/pickle.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/lock.h"

namespace xenon::asar {

namespace {

#if BUILDFLAG(IS_WIN)
constexpr char kSeparators[] = "\\/";
#else
constexpr char kSeparators[] = "/";
#endif

constexpr int kMaxLinkDepth = 40;
constexpr uint32_t kMaxHeaderSize = 64 * 1024 * 1024;
constexpr base::FilePath::CharType kAsarExtension[] =
    FILE_PATH_LITERAL(".asar");

using ArchiveMap = std::map<base::FilePath, std::shared_ptr<Archive>>;

ArchiveMap& GetArchiveCache() {
  static base::NoDestructor<ArchiveMap> cache;
  return *cache;
}

base::Lock& GetArchiveCacheLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

const base::DictValue* GetNodeFromPath(std::string path,
                                       const base::DictValue& root,
                                       int depth);

const base::DictValue* GetFilesNode(const base::DictValue& root,
                                    const base::DictValue& directory,
                                    int depth) {
  const std::string* link = directory.FindString("link");
  if (!link) {
    return directory.FindDict("files");
  }
  if (depth >= kMaxLinkDepth) {
    return nullptr;
  }
  const base::DictValue* linked_node =
      GetNodeFromPath(*link, root, depth + 1);
  return linked_node ? linked_node->FindDict("files") : nullptr;
}

const base::DictValue* GetChildNode(const base::DictValue& root,
                                    const std::string& name,
                                    const base::DictValue& directory,
                                    int depth) {
  if (name.empty()) {
    return &root;
  }
  const base::DictValue* files = GetFilesNode(root, directory, depth);
  return files ? files->FindDict(name) : nullptr;
}

const base::DictValue* GetNodeFromPath(std::string path,
                                       const base::DictValue& root,
                                       int depth) {
  if (path.empty()) {
    return &root;
  }

  const base::DictValue* directory = &root;
  for (size_t separator = path.find_first_of(kSeparators);
       separator != std::string::npos;
       separator = path.find_first_of(kSeparators)) {
    directory = GetChildNode(root, path.substr(0, separator), *directory,
                             depth);
    if (!directory) {
      return nullptr;
    }
    path.erase(0, separator + 1);
  }
  return GetChildNode(root, path, *directory, depth);
}

bool ReadNonNegativeInteger(const base::DictValue& node,
                            std::string_view key,
                            uint64_t* result) {
  const base::Value* value = node.Find(key);
  if (!value) {
    return false;
  }
  if (value->is_int()) {
    const int integer = value->GetInt();
    if (integer < 0) {
      return false;
    }
    *result = static_cast<uint64_t>(integer);
    return true;
  }
  if (value->is_double()) {
    const double number = value->GetDouble();
    constexpr double kMaxExactJsonInteger = 9007199254740991.0;
    if (number < 0 || number > kMaxExactJsonInteger ||
        number != static_cast<uint64_t>(number)) {
      return false;
    }
    *result = static_cast<uint64_t>(number);
    return true;
  }
  return false;
}

bool FillFileInfo(const base::DictValue& node,
                  uint32_t header_size,
                  uint64_t archive_size,
                  Archive::FileInfo* info) {
  if (!ReadNonNegativeInteger(node, "size", &info->size)) {
    return false;
  }

  info->unpacked = node.FindBool("unpacked").value_or(false);
  if (info->unpacked) {
    return true;
  }

  const std::string* offset = node.FindString("offset");
  uint64_t relative_offset = 0;
  if (!offset || !base::StringToUint64(*offset, &relative_offset) ||
      relative_offset > std::numeric_limits<uint64_t>::max() - header_size) {
    return false;
  }

  info->offset = relative_offset + header_size;
  return info->offset <= archive_size &&
         info->size <= archive_size - info->offset;
}

bool IsSafeRelativePath(const base::FilePath& path) {
  return !path.empty() && !path.IsAbsolute() && !path.ReferencesParent();
}

}  // namespace

Archive::Archive(const base::FilePath& path)
    : path_(path),
      file_(path, base::File::FLAG_OPEN | base::File::FLAG_READ |
                      base::File::FLAG_WIN_SHARE_DELETE) {}

Archive::~Archive() = default;

bool Archive::Init() {
  CHECK(!initialized_);
  initialized_ = true;
  if (!file_.IsValid()) {
    LOG(WARNING) << "Unable to open standard ASAR archive " << path_;
    return false;
  }

  const int64_t length = file_.GetLength();
  if (length < 8) {
    return false;
  }
  archive_size_ = static_cast<uint64_t>(length);

  std::vector<uint8_t> size_buffer(8);
  if (!file_.ReadAndCheck(0, size_buffer)) {
    return false;
  }

  uint32_t serialized_header_size = 0;
  if (!base::PickleIterator(base::Pickle::WithData(size_buffer))
           .ReadUInt32(&serialized_header_size) ||
      serialized_header_size < 8 || serialized_header_size > kMaxHeaderSize ||
      serialized_header_size > archive_size_ - 8) {
    return false;
  }

  std::vector<uint8_t> header_buffer(serialized_header_size);
  if (!file_.ReadAndCheck(8, header_buffer)) {
    return false;
  }

  std::string json_header;
  if (!base::PickleIterator(base::Pickle::WithData(header_buffer))
           .ReadString(&json_header)) {
    return false;
  }

  std::optional<base::Value> value =
      base::JSONReader::Read(json_header, base::JSON_PARSE_CHROMIUM_EXTENSIONS);
  if (!value || !value->is_dict() || !value->GetDict().FindDict("files")) {
    return false;
  }

  header_size_ = 8 + serialized_header_size;
  header_ = std::move(*value).TakeDict();
  return true;
}

bool Archive::GetFileInfo(const base::FilePath& path, FileInfo* info) const {
  return GetFileInfo(path, info, 0);
}

bool Archive::GetFileInfo(const base::FilePath& path,
                          FileInfo* info,
                          int link_depth) const {
  if (!header_ || !info || !IsSafeRelativePath(path)) {
    return false;
  }

  const base::DictValue* node =
      GetNodeFromPath(path.AsUTF8Unsafe(), *header_, 0);
  if (!node) {
    return false;
  }

  if (const std::string* link = node->FindString("link")) {
    if (link_depth >= kMaxLinkDepth) {
      return false;
    }
    const base::FilePath link_path = base::FilePath::FromUTF8Unsafe(*link);
    if (!IsSafeRelativePath(link_path)) {
      return false;
    }
    return GetFileInfo(link_path, info, link_depth + 1);
  }
  return FillFileInfo(*node, header_size_, archive_size_, info);
}

bool Archive::GetUnpackedPath(const base::FilePath& path,
                              base::FilePath* unpacked_path) const {
  if (!unpacked_path || !IsSafeRelativePath(path)) {
    return false;
  }
  *unpacked_path =
      path_.AddExtension(FILE_PATH_LITERAL("unpacked")).Append(path);
  return true;
}

bool Archive::StatPath(const base::FilePath& path,
                       FileInfo* info,
                       bool* is_directory) const {
  if (!header_ || !is_directory) {
    return false;
  }
  if (path.empty()) {
    *is_directory = true;
    if (info) {
      *info = FileInfo();
    }
    return true;
  }
  if (!IsSafeRelativePath(path)) {
    return false;
  }

  const base::DictValue* node =
      GetNodeFromPath(path.AsUTF8Unsafe(), *header_, 0);
  if (!node) {
    return false;
  }
  if (node->FindDict("files")) {
    *is_directory = true;
    if (info) {
      *info = FileInfo();
    }
    return true;
  }

  *is_directory = false;
  FileInfo local_info;
  FileInfo* out = info ? info : &local_info;
  return FillFileInfo(*node, header_size_, archive_size_, out);
}

bool Archive::ReadFile(const base::FilePath& path,
                       std::string* contents) const {
  if (!contents) {
    return false;
  }
  FileInfo info;
  bool is_directory = false;
  if (!StatPath(path, &info, &is_directory) || is_directory) {
    return false;
  }
  if (info.unpacked) {
    base::FilePath unpacked_path;
    return GetUnpackedPath(path, &unpacked_path) &&
           base::ReadFileToString(unpacked_path, contents);
  }
  if (info.size == 0) {
    contents->clear();
    return true;
  }
  if (info.size > std::numeric_limits<size_t>::max() ||
      info.offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return false;
  }
  contents->assign(static_cast<size_t>(info.size), '\0');
  base::File file = DuplicateFile();
  return file.IsValid() &&
         file.ReadAndCheck(static_cast<int64_t>(info.offset),
                           base::as_writable_byte_span(*contents));
}

base::File Archive::DuplicateFile() const {
  return file_.Duplicate();
}

bool GetAsarArchivePath(const base::FilePath& full_path,
                        base::FilePath* asar_path,
                        base::FilePath* relative_path,
                        bool allow_root) {
  if (!asar_path || !relative_path) {
    return false;
  }

  using StringType = base::FilePath::StringType;
  const StringType& value = full_path.value();
  size_t end = value.size();
  while (end > 0 && base::FilePath::IsSeparator(value[end - 1])) {
    --end;
  }

  size_t component_end = end;
  bool leaf = true;
  while (component_end > 0) {
    size_t start = component_end;
    while (start > 0 && !base::FilePath::IsSeparator(value[start - 1])) {
      --start;
    }
    const base::FilePath::StringViewType component =
        base::FilePath::StringViewType{value}.substr(start,
                                                     component_end - start);
    if (component.size() >= 5 &&
        base::FilePath(component).MatchesExtension(kAsarExtension)) {
      base::FilePath candidate =
          leaf ? full_path : base::FilePath(value.substr(0, component_end));
      if (!base::DirectoryExists(candidate)) {
        if (leaf && !allow_root) {
          return false;
        }
        base::FilePath tail;
        size_t position = component_end;
        while (position < end) {
          while (position < end &&
                 base::FilePath::IsSeparator(value[position])) {
            ++position;
          }
          size_t next = position;
          while (next < end && !base::FilePath::IsSeparator(value[next])) {
            ++next;
          }
          if (next > position) {
            tail = tail.Append(base::FilePath::StringViewType{value}.substr(
                position, next - position));
          }
          position = next;
        }
        if (!allow_root && !IsSafeRelativePath(tail)) {
          return false;
        }
        *asar_path = std::move(candidate);
        *relative_path = std::move(tail);
        return true;
      }
    }

    if (start == 0) {
      return false;
    }
    leaf = false;
    component_end = start;
    while (component_end > 0 &&
           base::FilePath::IsSeparator(value[component_end - 1])) {
      --component_end;
    }
  }
  return false;
}

std::shared_ptr<Archive> GetOrCreateAsarArchive(
    const base::FilePath& path) {
  base::AutoLock lock(GetArchiveCacheLock());
  ArchiveMap& cache = GetArchiveCache();
  const auto lower = cache.lower_bound(path);
  if (lower != cache.end() && !cache.key_comp()(path, lower->first)) {
    return lower->second;
  }

  auto archive = std::make_shared<Archive>(path);
  if (!archive->Init()) {
    return nullptr;
  }
  cache.try_emplace(lower, path, archive);
  return archive;
}

}  // namespace xenon::asar
