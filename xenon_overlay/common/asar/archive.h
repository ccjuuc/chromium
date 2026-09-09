// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Portions copyright (c) 2014 GitHub, Inc. under the MIT license.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_ASAR_ARCHIVE_H_
#define XENON_OVERLAY_COMMON_ASAR_ARCHIVE_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/values.h"

namespace xenon::asar {

// Read-only view of a standard Electron ASAR archive. The archive keeps its
// file handle open so offsets parsed from the header always refer to the same
// file contents.
class Archive {
 public:
  struct FileInfo {
    bool unpacked = false;
    uint64_t size = 0;
    uint64_t offset = 0;
  };

  explicit Archive(const base::FilePath& path);
  ~Archive();

  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

  bool Init();
  bool GetFileInfo(const base::FilePath& path, FileInfo* info) const;
  bool GetUnpackedPath(const base::FilePath& path,
                       base::FilePath* unpacked_path) const;
  // `path` is relative to the archive root. Empty path is the archive root
  // directory. Directories set |is_directory| and leave |info| zeroed.
  bool StatPath(const base::FilePath& path,
                FileInfo* info,
                bool* is_directory) const;
  bool ReadFile(const base::FilePath& path, std::string* contents) const;
  base::File DuplicateFile() const;

 private:
  bool GetFileInfo(const base::FilePath& path,
                   FileInfo* info,
                   int link_depth) const;

  const base::FilePath path_;
  base::File file_;
  uint64_t archive_size_ = 0;
  uint32_t header_size_ = 0;
  std::optional<base::DictValue> header_;
  bool initialized_ = false;
};

// Splits ".../app.asar/path/in/archive" into the archive and relative path.
// A path ending at the archive itself is ignored unless |allow_root| is true.
bool GetAsarArchivePath(const base::FilePath& full_path,
                        base::FilePath* asar_path,
                        base::FilePath* relative_path,
                        bool allow_root = false);

// Returns a cached, initialized archive. Failed opens are not cached.
std::shared_ptr<Archive> GetOrCreateAsarArchive(
    const base::FilePath& path);

}  // namespace xenon::asar

#endif  // XENON_OVERLAY_COMMON_ASAR_ARCHIVE_H_
