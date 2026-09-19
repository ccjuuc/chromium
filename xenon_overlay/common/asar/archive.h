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
#include <vector>

#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/synchronization/lock.h"
#include "base/values.h"

namespace xenon::asar {

struct ArchiveDecryptionKey;

// Trusted application configuration. Roots must be absolute; overlapping roots
// may share a public key, but may not select different keys implicitly.
struct ArchiveKeyConfig {
  base::FilePath root_path;
  std::string public_key_pem;
};

bool SetArchivePublicKeys(const std::string& owner_id,
                          const std::vector<ArchiveKeyConfig>& configs);
void RemoveArchivePublicKeys(const std::string& owner_id);

// Read-only view of a standard or configured encrypted ASAR archive. Keeps its
// file handle open so offsets parsed from the header always refer to the same
// file contents.
class Archive {
 public:
  struct FileInfo {
    bool unpacked = false;
    bool encrypted = false;
    uint64_t size = 0;
    uint64_t offset = 0;
  };

  // Logical plaintext reader. Owns its file handle and immutable per-archive
  // decryption context, and remains valid after the archive cache is evicted.
  class EntryReader {
   public:
    ~EntryReader();
    uint64_t size() const { return size_; }
    // Returns zero at EOF, and nullopt for an I/O or decoding failure.
    std::optional<size_t> Read(uint64_t offset,
                               base::span<uint8_t> output) const;

   private:
    friend class Archive;
    EntryReader(base::File file,
                uint64_t offset,
                uint64_t size,
                std::shared_ptr<const ArchiveDecryptionKey> key);
    mutable base::Lock lock_;
    mutable base::File file_;
    const uint64_t offset_;
    const uint64_t size_;
    const std::shared_ptr<const ArchiveDecryptionKey> key_;
  };

  explicit Archive(const base::FilePath& path);
  ~Archive();

  Archive(const Archive&) = delete;
  Archive& operator=(const Archive&) = delete;

  bool Init();
  bool GetFileInfo(const base::FilePath& path, FileInfo* info) const;
  bool GetUnpackedPath(const base::FilePath& path,
                       base::FilePath* unpacked_path) const;
  // Resolves archive file/directory links to a canonical archive-relative
  // path. Empty path denotes the archive root and resolves to an empty path.
  bool ResolvePath(const base::FilePath& path,
                   base::FilePath* canonical_path) const;
  // `path` is relative to the archive root. Empty path is the archive root
  // directory. Directories set |is_directory| and leave |info| zeroed.
  bool StatPath(const base::FilePath& path,
                FileInfo* info,
                bool* is_directory) const;
  bool ReadFile(const base::FilePath& path, std::string* contents) const;
  std::unique_ptr<EntryReader> CreateReader(const base::FilePath& path) const;
  base::File DuplicateFile() const;

 private:
  friend std::shared_ptr<Archive> GetOrCreateAsarArchive(
      const base::FilePath& path);
  bool InitWithPublicKey(const std::string& public_key_pem);

  const base::FilePath path_;
  base::File file_;
  uint64_t archive_size_ = 0;
  uint32_t header_size_ = 0;
  std::optional<base::DictValue> header_;
  std::shared_ptr<const ArchiveDecryptionKey> key_;
  bool initialized_ = false;
};

// Splits ".../app.asar/path/in/archive" into the archive and relative path.
// A path ending at the archive itself is ignored unless |allow_root| is true.
bool GetAsarArchivePath(const base::FilePath& full_path,
                        base::FilePath* asar_path,
                        base::FilePath* relative_path,
                        bool allow_root = false);

// Returns a cached, initialized archive. Failed opens are not cached.
std::shared_ptr<Archive> GetOrCreateAsarArchive(const base::FilePath& path);

}  // namespace xenon::asar

#endif  // XENON_OVERLAY_COMMON_ASAR_ARCHIVE_H_
