// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
#include "base/pickle.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/asar/test_support.h"

namespace xenon::ipc {
namespace {

constexpr double kMaxSafeInteger = 9007199254740991.0;
constexpr size_t kLargeFileSize = 16 * 1024 * 1024;
constexpr size_t kRangeSize = 64 * 1024;

std::string TestBytes(size_t size) {
  std::string bytes(size, '\0');
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<char>(i % 251);
  }
  return bytes;
}

class XenonFileSystemRangeTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  void TearDown() override {
    xenon::asar::RemoveArchivePublicKeys("fs-range-test");
  }

  mojom::IpcResultPtr Read(const base::FilePath& path,
                           base::DictValue options = {},
                           bool return_bytes = true) {
    options.Set("operation", "read_file");
    options.Set("path", path.AsUTF8Unsafe());
    options.Set("returnBytes", return_bytes);
    return PerformFileSystemCall(
        base::Value(base::ListValue().Append(std::move(options))));
  }

  void ExpectBytes(const base::FilePath& path,
                   base::DictValue options,
                   std::string_view expected) {
    auto result = Read(path, std::move(options));
    ASSERT_TRUE(result->success) << result->error;
    ASSERT_TRUE(result->value.is_blob());
    const auto& bytes = result->value.GetBlob();
    EXPECT_EQ(expected.size(), bytes.size());
    EXPECT_EQ(expected, std::string(bytes.begin(), bytes.end()));
  }

  void CreateArchive(const std::string& packed, const std::string& unpacked) {
    const std::string next = "must-not-cross-entry";
    base::DictValue files;
    files.Set("packed.bin", base::DictValue()
                                .Set("size", static_cast<double>(packed.size()))
                                .Set("offset", "0"));
    files.Set("next.bin",
              base::DictValue()
                  .Set("size", static_cast<int>(next.size()))
                  .Set("offset", base::NumberToString(packed.size())));
    files.Set("empty.bin",
              base::DictValue().Set("size", 0).Set(
                  "offset", base::NumberToString(packed.size() + next.size())));
    // Existing unpacked reads use the actual file length. Deliberately stale
    // metadata detects accidental truncation to the header's advertised size.
    files.Set("unpacked.bin",
              base::DictValue().Set("size", 1).Set("unpacked", true));
    files.Set("empty-unpacked.bin",
              base::DictValue().Set("size", 99).Set("unpacked", true));
    auto json =
        base::WriteJson(base::DictValue().Set("files", std::move(files)));
    ASSERT_TRUE(json);
    base::Pickle header;
    header.WriteString(*json);
    base::Pickle size;
    size.WriteUInt32(static_cast<uint32_t>(header.AsBytes().size()));
    std::string archive(size.AsStringView());
    archive.append(header.AsStringView());
    archive.append(packed);
    archive.append(next);
    archive_path_ = temp_dir_.GetPath().AppendASCII("range.asar");
    ASSERT_TRUE(base::WriteFile(archive_path_, archive));
    const auto unpacked_dir =
        archive_path_.AddExtension(FILE_PATH_LITERAL("unpacked"));
    ASSERT_TRUE(base::CreateDirectory(unpacked_dir));
    ASSERT_TRUE(
        base::WriteFile(unpacked_dir.AppendASCII("unpacked.bin"), unpacked));
    ASSERT_TRUE(
        base::WriteFile(unpacked_dir.AppendASCII("empty-unpacked.bin"), ""));
  }

  base::ScopedTempDir temp_dir_;
  base::FilePath archive_path_;
};

TEST_F(XenonFileSystemRangeTest, LargeFileReturnsOnlyRequested64KiB) {
  const auto path = temp_dir_.GetPath().AppendASCII("large.bin");
  const std::string bytes = TestBytes(kLargeFileSize);
  ASSERT_TRUE(base::WriteFile(path, bytes));
  constexpr int kStart = 4 * 1024 * 1024 + 123;
  ExpectBytes(path,
              base::DictValue()
                  .Set("start", kStart)
                  .Set("end", kStart + static_cast<int>(kRangeSize) - 1),
              std::string_view(bytes).substr(kStart, kRangeSize));
  ExpectBytes(path,
              base::DictValue()
                  .Set("start", kStart)
                  .Set("end", kStart + static_cast<int>(2 * kRangeSize) + 16),
              std::string_view(bytes).substr(kStart, 2 * kRangeSize + 17));
  ExpectBytes(path, base::DictValue().Set("start", 0), bytes);
}

TEST_F(XenonFileSystemRangeTest, InclusiveDefaultsAndEofKeepExactBytes) {
  const auto path = temp_dir_.GetPath().AppendASCII("bytes.bin");
  const std::string bytes("a\0\xffz12", 6);
  ASSERT_TRUE(base::WriteFile(path, bytes));
  ExpectBytes(path, base::DictValue().Set("end", 0), bytes.substr(0, 1));
  ExpectBytes(path, base::DictValue().Set("end", 2), bytes.substr(0, 3));
  ExpectBytes(path, base::DictValue().Set("start", 2), bytes.substr(2));
  ExpectBytes(path, base::DictValue().Set("start", 2).Set("end", 2),
              bytes.substr(2, 1));
  ExpectBytes(path, base::DictValue().Set("start", 4).Set("end", 100),
              bytes.substr(4));
  ExpectBytes(path, base::DictValue().Set("start", 6), "");
  ExpectBytes(path, base::DictValue().Set("start", 7).Set("end", 8), "");
  ExpectBytes(path, base::DictValue().Set("start", 4294967296.0), "");
  ExpectBytes(path, base::DictValue().Set("start", kMaxSafeInteger), "");
  ExpectBytes(path, base::DictValue().Set("end", kMaxSafeInteger), bytes);
  const auto empty = temp_dir_.GetPath().AppendASCII("empty.bin");
  ASSERT_TRUE(base::WriteFile(empty, ""));
  ExpectBytes(empty, base::DictValue().Set("start", 0).Set("end", 0), "");
  ExpectBytes(empty, base::DictValue().Set("start", 1), "");
}

TEST_F(XenonFileSystemRangeTest, InvalidRangesFailBeforeOpeningFile) {
  const auto missing = temp_dir_.GetPath().AppendASCII("missing.bin");
  std::vector<base::Value> invalid;
  invalid.emplace_back(-1);
  invalid.emplace_back(-0.5);
  invalid.emplace_back(0.5);
  invalid.emplace_back(kMaxSafeInteger + 1);
  invalid.emplace_back("0");
  invalid.emplace_back(true);
  invalid.emplace_back();
  invalid.emplace_back(base::DictValue());
  invalid.emplace_back(base::ListValue());
  for (const auto& value : invalid) {
    for (const char* field : {"start", "end"}) {
      SCOPED_TRACE(field);
      auto result = Read(missing, base::DictValue().Set(field, value.Clone()));
      ASSERT_FALSE(result->success);
      EXPECT_TRUE(result->error.starts_with("EINVAL:")) << result->error;
    }
  }
  // base::Value normalizes nonfinite doubles to zero before this layer.
  // Public ReadStream validation rejects them before native serialization.
  auto reversed =
      Read(missing, base::DictValue().Set("start", 4).Set("end", 3));
  ASSERT_FALSE(reversed->success);
  EXPECT_TRUE(reversed->error.starts_with("EINVAL:"));
}

TEST_F(XenonFileSystemRangeTest, PackedAsarRangeCannotCrossEntryBoundaries) {
  const std::string bytes = TestBytes(kLargeFileSize);
  ASSERT_NO_FATAL_FAILURE(CreateArchive(bytes, "unpacked contents"));
  const auto packed = archive_path_.AppendASCII("packed.bin");
  constexpr int kStart = 2 * 1024 * 1024 + 29;
  ExpectBytes(packed,
              base::DictValue()
                  .Set("start", kStart)
                  .Set("end", kStart + static_cast<int>(kRangeSize) - 1),
              std::string_view(bytes).substr(kStart, kRangeSize));
  ExpectBytes(packed,
              base::DictValue()
                  .Set("start", static_cast<int>(bytes.size()) - 7)
                  .Set("end", kMaxSafeInteger),
              std::string_view(bytes).substr(bytes.size() - 7));
  ExpectBytes(packed,
              base::DictValue().Set("start", static_cast<int>(bytes.size())),
              "");
  ExpectBytes(packed, base::DictValue().Set("start", kMaxSafeInteger), "");
  ExpectBytes(archive_path_.AppendASCII("empty.bin"),
              base::DictValue().Set("start", 0).Set("end", 99), "");
  ExpectBytes(archive_path_.AppendASCII("next.bin"),
              base::DictValue().Set("end", 3), "must");
}

TEST_F(XenonFileSystemRangeTest, UnpackedAsarRangesUseActualFileLength) {
  const std::string bytes("unpacked\0\xff", 10);
  ASSERT_NO_FATAL_FAILURE(CreateArchive("packed", bytes));
  const auto path = archive_path_.AppendASCII("unpacked.bin");
  ExpectBytes(path, base::DictValue().Set("start", 3).Set("end", 5),
              bytes.substr(3, 3));
  ExpectBytes(path, base::DictValue().Set("start", 3), bytes.substr(3));
  ExpectBytes(path, base::DictValue().Set("end", kMaxSafeInteger), bytes);
  ExpectBytes(
      path, base::DictValue().Set("start", static_cast<int>(bytes.size())), "");
  ExpectBytes(archive_path_.AppendASCII("empty-unpacked.bin"),
              base::DictValue().Set("start", 0), "");
}

TEST_F(XenonFileSystemRangeTest, EncryptedAsarRangesReturnLogicalBytes) {
  const std::string bytes = TestBytes(kLargeFileSize);
  const std::string plain = "plain member after encrypted member";
  const std::string unpacked("unpacked\0\xff", 10);
  xenon::asar::TestArchiveBuilder builder;
  archive_path_ = temp_dir_.GetPath().AppendASCII("encrypted.asar");
  ASSERT_TRUE(
      builder.Write(archive_path_, {{"encrypted.bin", bytes, true},
                                    {"plain.bin", plain},
                                    {"empty.bin", "", true},
                                    {"unpacked.bin", unpacked, false, true}}));
  ASSERT_TRUE(xenon::asar::SetArchivePublicKeys(
      "fs-range-test", {{temp_dir_.GetPath(), builder.public_key_pem()}}));

  const auto path = archive_path_.AppendASCII("encrypted.bin");
  constexpr int kStart = 2 * 1024 * 1024 + 3;
  ExpectBytes(path,
              base::DictValue()
                  .Set("start", kStart)
                  .Set("end", kStart + static_cast<int>(kRangeSize) - 1),
              std::string_view(bytes).substr(kStart, kRangeSize));
  ExpectBytes(path, base::DictValue().Set("end", 0), bytes.substr(0, 1));
  ExpectBytes(path,
              base::DictValue()
                  .Set("start", static_cast<int>(bytes.size()) - 19)
                  .Set("end", kMaxSafeInteger),
              std::string_view(bytes).substr(bytes.size() - 19));
  ExpectBytes(
      path, base::DictValue().Set("start", static_cast<int>(bytes.size())), "");
  ExpectBytes(path, base::DictValue().Set("start", kMaxSafeInteger), "");
  ExpectBytes(archive_path_.AppendASCII("empty.bin"),
              base::DictValue().Set("start", 0).Set("end", 99), "");
  ExpectBytes(archive_path_.AppendASCII("plain.bin"),
              base::DictValue().Set("start", 3).Set("end", 16),
              plain.substr(3, 14));
  ExpectBytes(archive_path_.AppendASCII("unpacked.bin"),
              base::DictValue().Set("start", 3).Set("end", kMaxSafeInteger),
              unpacked.substr(3));
  auto legacy =
      Read(path, base::DictValue().Set("start", 3).Set("end", 18), false);
  ASSERT_TRUE(legacy->success) << legacy->error;
  EXPECT_EQ(base::Base64Encode(bytes.substr(3, 16)), legacy->value.GetString());
}

TEST_F(XenonFileSystemRangeTest, EncryptedAsarWholeReadPreservesPadding) {
  xenon::asar::TestArchiveBuilder builder;
  archive_path_ = temp_dir_.GetPath().AppendASCII("encrypted.asar");
  ASSERT_TRUE(builder.Write(archive_path_, {{"script.js", "one", true}}));
  ASSERT_TRUE(xenon::asar::SetArchivePublicKeys(
      "fs-range-test", {{temp_dir_.GetPath(), builder.public_key_pem()}}));
  const std::string padded = "one" + std::string(13, ' ');
  const auto path = archive_path_.AppendASCII("script.js");
  ExpectBytes(path, {}, padded);
  ExpectBytes(path, base::DictValue().Set("start", 1), padded.substr(1));

  // A cached archive must not keep serving decrypted content after its trusted
  // key configuration is withdrawn.
  xenon::asar::RemoveArchivePublicKeys("fs-range-test");
  auto result = Read(path, base::DictValue().Set("end", 0));
  EXPECT_FALSE(result->success);
}

TEST_F(XenonFileSystemRangeTest, NoRangeAndLegacyBase64ReadsRemainCompatible) {
  const std::string bytes("normal\0\xff", 8);
  const auto path = temp_dir_.GetPath().AppendASCII("normal.bin");
  ASSERT_TRUE(base::WriteFile(path, bytes));
  ASSERT_NO_FATAL_FAILURE(CreateArchive(bytes, bytes));
  for (const auto& target : {path, archive_path_.AppendASCII("packed.bin"),
                             archive_path_.AppendASCII("unpacked.bin")}) {
    ExpectBytes(target, {}, bytes);
    auto legacy = Read(target, {}, false);
    ASSERT_TRUE(legacy->success) << legacy->error;
    ASSERT_TRUE(legacy->value.is_string());
    EXPECT_EQ(base::Base64Encode(bytes), legacy->value.GetString());
    auto range =
        Read(target, base::DictValue().Set("start", 2).Set("end", 4), false);
    ASSERT_TRUE(range->success) << range->error;
    EXPECT_EQ(base::Base64Encode(bytes.substr(2, 3)), range->value.GetString());
  }
}

TEST_F(XenonFileSystemRangeTest, MissingFilesAndDirectoriesRetainErrors) {
  auto missing = Read(temp_dir_.GetPath().AppendASCII("missing.bin"),
                      base::DictValue().Set("start", 0));
  ASSERT_FALSE(missing->success);
  EXPECT_TRUE(missing->error.starts_with("ENOENT:"));
  auto directory = Read(temp_dir_.GetPath(), base::DictValue().Set("start", 0));
  ASSERT_FALSE(directory->success);
  EXPECT_TRUE(directory->error.starts_with("EISDIR:"));
}

#if BUILDFLAG(IS_POSIX)
TEST_F(XenonFileSystemRangeTest, SparseFileRangesKeepOffsetsAboveFourGiB) {
  const auto path = temp_dir_.GetPath().AppendASCII("sparse.bin");
  constexpr int64_t kOffset = (int64_t{1} << 32) + 17;
  const std::string bytes("large-offset\0\xff", 14);
  {
    base::File file(path, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
    ASSERT_TRUE(file.IsValid());
    ASSERT_TRUE(file.WriteAndCheck(kOffset, base::as_byte_span(bytes)));
  }
  ExpectBytes(path,
              base::DictValue()
                  .Set("start", static_cast<double>(kOffset))
                  .Set("end", static_cast<double>(kOffset + bytes.size() - 1)),
              bytes);
}
#endif

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
TEST_F(XenonFileSystemRangeTest,
       ZeroStatLengthDoesNotHideReadableProcContents) {
  const base::FilePath path(FILE_PATH_LITERAL("/proc/version"));
  if (!base::PathExists(path)) {
    GTEST_SKIP() << "procfs is not mounted";
  }
  std::string bytes;
  ASSERT_TRUE(base::ReadFileToString(path, &bytes));
  ASSERT_GT(bytes.size(), 8u);
  ExpectBytes(path, base::DictValue().Set("start", 0).Set("end", 7),
              bytes.substr(0, 8));
  ExpectBytes(path, base::DictValue().Set("start", 0), bytes);
}
#endif

}  // namespace
}  // namespace xenon::ipc
