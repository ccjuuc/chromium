// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_ASAR_TEST_SUPPORT_H_
#define XENON_OVERLAY_COMMON_ASAR_TEST_SUPPORT_H_

#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"

namespace xenon::asar {

struct TestArchiveEntry {
  std::string path;
  std::string contents;
  bool encrypted = false;
  bool unpacked = false;
  std::string link = {};
};

// Generates an independent RSA/AES fixture for each instance. No production
// keys or vendor implementation are used by these tests.
class TestArchiveBuilder {
 public:
  TestArchiveBuilder();
  ~TestArchiveBuilder();
  const std::string& public_key_pem() const;
  // Encrypted contents are padded with spaces to the next 16-byte boundary,
  // matching the legacy format, which does not record unpadded length.
  bool Write(const base::FilePath& path,
             const std::vector<TestArchiveEntry>& entries,
             bool encrypted_header = true) const;

 private:
  struct Impl;
  const std::unique_ptr<Impl> impl_;
};

}  // namespace xenon::asar

#endif  // XENON_OVERLAY_COMMON_ASAR_TEST_SUPPORT_H_
