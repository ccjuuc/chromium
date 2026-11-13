// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"

#include <sys/stat.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/pickle.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/values.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_stats.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/asar/test_support.h"

#if BUILDFLAG(IS_WIN)
#include <io.h>
#elif BUILDFLAG(IS_POSIX)
#include <unistd.h>
#endif

namespace xenon::ipc {
namespace {

TEST(XenonFileSystemStatTest, BigIntSerializationPreservesIntegersAndNanoseconds) {
  uv_stat_t stat = {};
  stat.st_ino = UINT64_C(9007199254740993);
  stat.st_size = UINT64_C(18446744073709551615);
  stat.st_mtim = {1794455573, 756347601};
  stat.st_atim = {-1, 999499999};
  stat.st_ctim = {-2, 0};
  const auto exact = FileSystemStatValue(stat, true);
  ASSERT_TRUE(exact.FindString("ino"));
  EXPECT_EQ("9007199254740993", *exact.FindString("ino"));
  EXPECT_EQ("18446744073709551615", *exact.FindString("size"));
  EXPECT_EQ("1794455573756347601", *exact.FindString("mtimeNs"));
  EXPECT_EQ("-500001", *exact.FindString("atimeNs"));
  EXPECT_EQ("-2000000000", *exact.FindString("ctimeNs"));
  EXPECT_EQ("0", *exact.FindString("birthtimeNs"));
  EXPECT_FALSE(exact.Find("mtimeMs"));
  const auto number = FileSystemStatValue(stat, false);
  EXPECT_TRUE(number.FindDouble("ino"));
  EXPECT_FALSE(number.Find("mtimeNs"));
}

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

  mojom::IpcResultPtr Access(const base::FilePath& path,
                             base::DictValue options = {}) {
    options.Set("operation", "access");
    options.Set("path", path.AsUTF8Unsafe());
    return PerformFileSystemCall(
        base::Value(base::ListValue().Append(std::move(options))));
  }

  mojom::IpcResultPtr Chmod(const base::FilePath& path,
                           base::DictValue options = {}) {
    options.Set("operation", "chmod");
    options.Set("path", path.AsUTF8Unsafe());
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

TEST_F(XenonFileSystemRangeTest, BigIntPathStatsPreserveNativeMetadata) {
  const auto path = temp_dir_.GetPath().AppendASCII("precise.txt");
  ASSERT_TRUE(base::WriteFile(path, "metadata"));
  uv_fs_t native = {};
  ASSERT_EQ(0, uv_fs_stat(nullptr, &native, path.AsUTF8Unsafe().c_str(), nullptr));
  const auto expected = FileSystemStatValue(native.statbuf, true);
  uv_fs_req_cleanup(&native);
  for (const char* operation : {"stat", "lstat"}) {
    auto result = PerformFileSystemCall(base::Value(base::ListValue().Append(
        base::DictValue().Set("operation", operation)
            .Set("path", path.AsUTF8Unsafe()).Set("bigint", true))));
    ASSERT_TRUE(result->success) << result->error;
    EXPECT_EQ(expected, result->value.GetDict());
  }
#if BUILDFLAG(IS_POSIX)
  const auto link = temp_dir_.GetPath().AppendASCII("link");
  ASSERT_TRUE(base::CreateSymbolicLink(path, link));
  auto result = PerformFileSystemCall(base::Value(base::ListValue().Append(
      base::DictValue().Set("operation", "lstat")
          .Set("path", link.AsUTF8Unsafe()).Set("bigint", true))));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(true, result->value.GetDict().FindBool("isSymbolicLink"));
#endif
}

TEST_F(XenonFileSystemRangeTest, BigIntAsarStatsPreserveKnownVirtualMetadata) {
  CreateArchive("packed-data", "unpacked-data");
  for (const char* operation : {"stat", "lstat"}) {
    auto result = PerformFileSystemCall(base::Value(base::ListValue().Append(
        base::DictValue().Set("operation", operation)
            .Set("path", archive_path_.AppendASCII("packed.bin").AsUTF8Unsafe())
            .Set("bigint", true))));
    ASSERT_TRUE(result->success) << result->error;
    const auto& stat = result->value.GetDict();
    ASSERT_TRUE(stat.FindString("size"));
    EXPECT_EQ("11", *stat.FindString("size"));
    EXPECT_EQ("0", *stat.FindString("mtimeNs"));
    EXPECT_EQ("0", *stat.FindString("birthtimeNs"));
    EXPECT_EQ(true, stat.FindBool("isFile"));
    EXPECT_FALSE(stat.Find("ino"));
    EXPECT_FALSE(stat.Find("mode"));
  }
  auto raw = PerformFileSystemCall(base::Value(base::ListValue().Append(
      base::DictValue().Set("operation", "stat")
          .Set("path", archive_path_.AsUTF8Unsafe())
          .Set("bigint", true).Set("noAsar", true))));
  ASSERT_TRUE(raw->success) << raw->error;
  EXPECT_EQ(true, raw->value.GetDict().FindBool("isFile"));
  EXPECT_TRUE(raw->value.GetDict().FindString("ino"));
  EXPECT_TRUE(raw->value.GetDict().FindString("birthtimeNs"));
}

TEST_F(XenonFileSystemRangeTest,
       AccessChecksRealFilesAndDoesNotCreateMissingFiles) {
  const auto file = temp_dir_.GetPath().AppendASCII("access.txt");
  ASSERT_TRUE(base::WriteFile(file, "unchanged"));
  for (int mode : {0, 2, 4, 6}) {
    const auto result = Access(file, base::DictValue().Set("mode", mode));
    EXPECT_TRUE(result->success) << result->error;
    EXPECT_TRUE(result->value.is_none());
  }
  EXPECT_TRUE(
      Access(temp_dir_.GetPath(), base::DictValue().Set("mode", 7))->success);
  const auto missing = temp_dir_.GetPath().AppendASCII("missing");
  const auto error = Access(missing, base::DictValue().Set("mode", 2));
  ASSERT_FALSE(error->success);
  EXPECT_TRUE(error->error.starts_with("ENOENT:"));
  EXPECT_FALSE(base::PathExists(missing));
  std::string unchanged;
  ASSERT_TRUE(base::ReadFileToString(file, &unchanged));
  EXPECT_EQ("unchanged", unchanged);
}

TEST_F(XenonFileSystemRangeTest,
       AccessRejectsMalformedRequestsBeforeFileLookup) {
  const auto missing = temp_dir_.GetPath().AppendASCII("missing");
  for (const auto& invalid :
       {base::Value(-1), base::Value(8), base::Value(1.5), base::Value("2"),
        base::Value(true), base::Value()}) {
    const auto result =
        Access(missing, base::DictValue().Set("mode", invalid.Clone()));
    ASSERT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with("EINVAL:"));
  }
  auto arguments = base::DictValue()
                       .Set("operation", "access")
                       .Set("path", std::string("present\0truncated", 17))
                       .Set("mode", 0);
  const auto nul = PerformFileSystemCall(
      base::Value(base::ListValue().Append(std::move(arguments))));
  EXPECT_FALSE(nul->success);
  EXPECT_TRUE(nul->error.starts_with("EINVAL:"));
}

TEST_F(XenonFileSystemRangeTest,
       AccessPreservesAsarAndPhysicalFilesystemViews) {
  ASSERT_NO_FATAL_FAILURE(CreateArchive("packed", "unpacked"));
  const auto packed = archive_path_.AppendASCII("packed.bin");
  const auto unpacked = archive_path_.AppendASCII("unpacked.bin");
  for (int mode : {0, 1, 4, 5}) {
    EXPECT_TRUE(Access(packed, base::DictValue().Set("mode", mode))->success);
  }
  for (int mode : {2, 3, 6, 7}) {
    const auto result = Access(packed, base::DictValue().Set("mode", mode));
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with("EACCES:"));
  }
  EXPECT_TRUE(Access(unpacked, base::DictValue().Set("mode", 6))->success);
  EXPECT_FALSE(
      Access(archive_path_, base::DictValue().Set("mode", 2))->success);
  EXPECT_TRUE(Access(archive_path_,
                     base::DictValue().Set("mode", 2).Set("noAsar", true))
                  ->success);
  const auto physical =
      archive_path_.AddExtension(FILE_PATH_LITERAL("unpacked"))
          .AppendASCII("unpacked.bin");
  ASSERT_TRUE(base::DeleteFile(physical));
  const auto missing = Access(unpacked);
  EXPECT_FALSE(missing->success);
  EXPECT_TRUE(missing->error.starts_with("ENOENT:"));
}

TEST_F(XenonFileSystemRangeTest,
       AccessChecksReadOnlyFileAndUnpackedPermissions) {
#if BUILDFLAG(IS_POSIX)
  if (geteuid() == 0) {
    GTEST_SKIP() << "Root bypasses ordinary write permission bits";
  }
#endif
  ASSERT_NO_FATAL_FAILURE(CreateArchive("packed", "unpacked"));
  const auto physical =
      archive_path_.AddExtension(FILE_PATH_LITERAL("unpacked"))
          .AppendASCII("unpacked.bin");
#if BUILDFLAG(IS_WIN)
  ASSERT_EQ(0, _wchmod(physical.value().c_str(), _S_IREAD));
#elif BUILDFLAG(IS_POSIX)
  ASSERT_TRUE(base::SetPosixFilePermissions(physical, 0400));
#else
  GTEST_SKIP() << "No test fixture for read-only permissions on this platform";
#endif
  for (const auto& path :
       {physical, archive_path_.AppendASCII("unpacked.bin")}) {
    const auto read = Access(path, base::DictValue().Set("mode", 4));
    EXPECT_TRUE(read->success) << read->error;
    const auto write = Access(path, base::DictValue().Set("mode", 2));
    EXPECT_FALSE(write->success);
    EXPECT_TRUE(write->error.starts_with("EACCES:") ||
                write->error.starts_with("EPERM:"))
        << write->error;
  }
#if BUILDFLAG(IS_WIN)
  EXPECT_EQ(0, _wchmod(physical.value().c_str(), _S_IREAD | _S_IWRITE));
#elif BUILDFLAG(IS_POSIX)
  EXPECT_TRUE(base::SetPosixFilePermissions(physical, 0600));
#endif
}

TEST_F(XenonFileSystemRangeTest, ChmodChangesRealPermissionsAndPreservesBytes) {
  const auto file = temp_dir_.GetPath().AppendASCII("permissions.txt");
  ASSERT_TRUE(base::WriteFile(file, "unchanged permissions fixture"));
  for (const int mode : {0400, 0600}) {
    const auto result = Chmod(file, base::DictValue().Set("mode", mode));
    EXPECT_TRUE(result->success) << result->error;
    EXPECT_TRUE(result->value.is_none());
#if BUILDFLAG(IS_WIN)
    // Windows chmod changes read-only, not Unix group/other permission bits.
    const auto writable = Access(file, base::DictValue().Set("mode", 2));
    EXPECT_EQ((mode & 0200) != 0, writable->success);
#elif BUILDFLAG(IS_POSIX)
    int actual = 0;
    EXPECT_TRUE(base::GetPosixFilePermissions(file, &actual));
    EXPECT_EQ(mode, actual);
#endif
  }
  std::string contents;
  EXPECT_TRUE(base::ReadFileToString(file, &contents));
  EXPECT_EQ("unchanged permissions fixture", contents);
  EXPECT_TRUE(Chmod(temp_dir_.GetPath(), base::DictValue().Set("mode", 0700))
                  ->success);
  const auto missing = Chmod(temp_dir_.GetPath().AppendASCII("missing"),
                              base::DictValue().Set("mode", 0600));
  EXPECT_FALSE(missing->success);
  EXPECT_TRUE(missing->error.starts_with("ENOENT:"));
  EXPECT_NE(std::string::npos, missing->error.find(", chmod '"));
}

TEST_F(XenonFileSystemRangeTest, ChmodRejectsInvalidRequestsWithoutMutation) {
  const auto file = temp_dir_.GetPath().AppendASCII("permissions.txt");
  ASSERT_TRUE(base::WriteFile(file, "unchanged"));
  for (const auto& mode : {base::Value(), base::Value(-1), base::Value(0.5),
                           base::Value(true), base::Value("644"),
                           base::Value(4294967296.0)}) {
    const auto result = Chmod(file, base::DictValue().Set("mode", mode.Clone()));
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with("EINVAL:"));
  }
  EXPECT_FALSE(Chmod(file)->success);
  const auto nul = PerformFileSystemCall(base::Value(base::ListValue().Append(
      base::DictValue()
          .Set("operation", "chmod")
          .Set("path", file.AsUTF8Unsafe() + std::string("\0suffix", 7))
          .Set("mode", 0400))));
  EXPECT_FALSE(nul->success);
  EXPECT_TRUE(nul->error.starts_with("EINVAL:"));
  EXPECT_TRUE(Access(file, base::DictValue().Set("mode", 2))->success);
  // Numeric transport can carry unsigned modes beyond signed-int range.
  EXPECT_TRUE(Chmod(file, base::DictValue().Set("mode", 4294967295.0))->success);
  EXPECT_TRUE(Chmod(file, base::DictValue().Set("mode", 0600))->success);
}

TEST_F(XenonFileSystemRangeTest, ChmodPreservesAsarReadOnlyAndOriginalFsViews) {
  ASSERT_NO_FATAL_FAILURE(CreateArchive("packed bytes", "unpacked bytes"));
  for (const auto& file : {archive_path_,
                            archive_path_.AppendASCII("packed.bin"),
                            archive_path_.AppendASCII("unpacked.bin")}) {
    const auto result = Chmod(file, base::DictValue().Set("mode", 0400));
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with("EACCES:"));
  }
  EXPECT_TRUE(Chmod(archive_path_, base::DictValue()
                                      .Set("mode", 0400)
                                      .Set("noAsar", true))
                  ->success);
  EXPECT_TRUE(Chmod(archive_path_, base::DictValue()
                                      .Set("mode", 0600)
                                      .Set("noAsar", true))
                  ->success);
  ExpectBytes(archive_path_.AppendASCII("packed.bin"), {}, "packed bytes");
}

#if BUILDFLAG(IS_POSIX)
TEST_F(XenonFileSystemRangeTest, ChmodFollowsRealSymbolicLinks) {
  const auto target = temp_dir_.GetPath().AppendASCII("target");
  const auto link = temp_dir_.GetPath().AppendASCII("link");
  ASSERT_TRUE(base::WriteFile(target, "target bytes"));
  ASSERT_TRUE(base::CreateSymbolicLink(target, link));
  EXPECT_TRUE(Chmod(link, base::DictValue().Set("mode", 0400))->success);
  int actual = 0;
  EXPECT_TRUE(base::GetPosixFilePermissions(target, &actual));
  EXPECT_EQ(0400, actual);
  EXPECT_TRUE(Chmod(link, base::DictValue().Set("mode", 0600))->success);
}
#endif

TEST_F(XenonFileSystemRangeTest,
       OriginalFsReadsPhysicalArchiveWithoutChangingFs) {
  CreateArchive("logical entry", "unpacked content");
  std::string physical;
  ASSERT_TRUE(base::ReadFileToString(archive_path_, &physical));
  ExpectBytes(archive_path_, base::DictValue().Set("noAsar", true), physical);
  ExpectBytes(
      archive_path_,
      base::DictValue().Set("noAsar", true).Set("start", 0).Set("end", 7),
      physical.substr(0, 8));
  auto virtual_root = Read(archive_path_);
  EXPECT_FALSE(virtual_root->success);
  EXPECT_TRUE(virtual_root->error.starts_with("EISDIR:"));
  const auto member = archive_path_.AppendASCII("packed.bin");
  auto raw_member = Read(member, base::DictValue().Set("noAsar", true));
  EXPECT_FALSE(raw_member->success);
  ExpectBytes(member, {}, "logical entry");
  auto malformed = Read(archive_path_, base::DictValue().Set("noAsar", "true"));
  EXPECT_FALSE(malformed->success);
  EXPECT_TRUE(malformed->error.starts_with("EINVAL:"));
}

TEST_F(XenonFileSystemRangeTest, OriginalFsReadsEncryptedBytesWithoutAKey) {
  asar::TestArchiveBuilder builder;
  const auto archive = temp_dir_.GetPath().AppendASCII("encrypted.asar");
  ASSERT_TRUE(
      builder.Write(archive, {{"private.bin", "encrypted bytes", true}}, true));
  std::string physical;
  ASSERT_TRUE(base::ReadFileToString(archive, &physical));
  ExpectBytes(archive, base::DictValue().Set("noAsar", true), physical);
  auto member = Read(archive.AppendASCII("private.bin"));
  EXPECT_FALSE(member->success);
}

class XenonFileSystemSymlinkTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_dir_.CreateUniqueTempDir()); }

  static mojom::IpcResultPtr Call(const std::string& operation,
                                  const base::FilePath& path,
                                  base::DictValue options = {}) {
    options.Set("operation", operation);
    options.Set("path", path.AsUTF8Unsafe());
    return PerformFileSystemCall(
        base::Value(base::ListValue().Append(std::move(options))));
  }

  base::ScopedTempDir temp_dir_;
};

#if BUILDFLAG(IS_POSIX)
TEST_F(XenonFileSystemSymlinkTest,
       DanglingPayloadRoundTripsAndExistingLinkCannotBeReplaced) {
  const auto link = temp_dir_.GetPath().AppendASCII("single-instance.lock");
  const std::string payload = "12345\tWed Oct 28 12:00:00 2026";
  auto created =
      Call("symlink", link, base::DictValue().Set("target", payload));
  ASSERT_TRUE(created->success) << created->error;
  EXPECT_FALSE(base::PathExists(link));
  EXPECT_TRUE(base::IsLink(link));
  auto read =
      Call("readlink", link, base::DictValue().Set("returnBytes", true));
  ASSERT_TRUE(read->success) << read->error;
  ASSERT_TRUE(read->value.is_blob());
  EXPECT_EQ(payload, std::string(read->value.GetBlob().begin(),
                                 read->value.GetBlob().end()));

  auto duplicate =
      Call("symlink", link, base::DictValue().Set("target", "replacement"));
  ASSERT_FALSE(duplicate->success);
  EXPECT_TRUE(duplicate->error.starts_with("EEXIST:")) << duplicate->error;
  base::FilePath actual;
  ASSERT_TRUE(base::ReadSymbolicLink(link, &actual));
  EXPECT_EQ(payload, actual.value());
  ASSERT_TRUE(Call("unlink", link)->success);
  EXPECT_FALSE(base::IsLink(link));
  auto missing = Call("readlink", link);
  ASSERT_FALSE(missing->success);
  EXPECT_TRUE(missing->error.starts_with("ENOENT:")) << missing->error;

  // Relative paths and parent components are also opaque target bytes.
  const std::string relative = "./missing/../peer-token";
  ASSERT_TRUE(Call("symlink", link, base::DictValue().Set("target", relative))
                  ->success);
  auto legacy = Call("readlink", link);
  ASSERT_TRUE(legacy->success) << legacy->error;
  EXPECT_EQ(base::Base64Encode(relative), legacy->value.GetString());
}

TEST_F(XenonFileSystemSymlinkTest, ConcurrentCreatorsHaveExactlyOneWinner) {
  base::test::TaskEnvironment task_environment;
  const auto link = temp_dir_.GetPath().AppendASCII("exclusive.lock");
  using Future = base::test::TestFuture<mojom::IpcResultPtr>;
  std::vector<std::unique_ptr<Future>> futures;
  constexpr int kCreators = 8;
  for (int i = 0; i < kCreators; ++i) {
    auto future = std::make_unique<Future>();
    ASSERT_TRUE(base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock()},
        base::BindOnce(
            &XenonFileSystemSymlinkTest::Call, "symlink", link,
            base::DictValue().Set("target", base::NumberToString(i))),
        future->GetCallback()));
    futures.push_back(std::move(future));
  }
  int winners = 0;
  for (int i = 0; i < kCreators; ++i) {
    auto result = futures[i]->Take();
    if (result->success) {
      ++winners;
      base::FilePath target;
      ASSERT_TRUE(base::ReadSymbolicLink(link, &target));
      EXPECT_EQ(base::NumberToString(i), target.value());
    } else {
      EXPECT_TRUE(result->error.starts_with("EEXIST:")) << result->error;
    }
  }
  EXPECT_EQ(winners, 1);
}

TEST_F(XenonFileSystemSymlinkTest, UnlinkNeverDeletesTheLinkTarget) {
  const auto file = temp_dir_.GetPath().AppendASCII("kept.txt");
  const auto directory = temp_dir_.GetPath().AppendASCII("kept-directory");
  const auto link = temp_dir_.GetPath().AppendASCII("link");
  ASSERT_TRUE(base::WriteFile(file, "keep me"));
  ASSERT_TRUE(base::CreateDirectory(directory));
  for (const auto& target : {file, directory}) {
    auto created = Call("symlink", link,
                        base::DictValue().Set("target", target.AsUTF8Unsafe()));
    ASSERT_TRUE(created->success) << created->error;
    auto removed = Call("unlink", link);
    ASSERT_TRUE(removed->success) << removed->error;
    EXPECT_FALSE(base::IsLink(link));
    EXPECT_TRUE(base::PathExists(target));
    auto duplicate = Call("symlink", target,
                          base::DictValue().Set("target", "must-not-replace"));
    ASSERT_FALSE(duplicate->success);
    EXPECT_TRUE(duplicate->error.starts_with("EEXIST:")) << duplicate->error;
  }
  std::string bytes;
  ASSERT_TRUE(base::ReadFileToString(file, &bytes));
  EXPECT_EQ(bytes, "keep me");
}

TEST_F(XenonFileSystemSymlinkTest, ErrorsAndEmbeddedNullsNeverCreateALink) {
  const auto file = temp_dir_.GetPath().AppendASCII("regular");
  const auto link = temp_dir_.GetPath().AppendASCII("lock");
  ASSERT_TRUE(base::WriteFile(file, "regular file"));
  auto regular = Call("readlink", file);
  ASSERT_FALSE(regular->success);
  EXPECT_TRUE(regular->error.starts_with("EINVAL:")) << regular->error;
  auto missing_parent =
      Call("symlink", temp_dir_.GetPath().AppendASCII("missing/lock"),
           base::DictValue().Set("target", "payload"));
  ASSERT_FALSE(missing_parent->success);
  EXPECT_TRUE(missing_parent->error.starts_with("ENOENT:"))
      << missing_parent->error;
  auto file_parent = Call("symlink", file.AppendASCII("lock"),
                          base::DictValue().Set("target", "payload"));
  ASSERT_FALSE(file_parent->success);
  EXPECT_TRUE(file_parent->error.starts_with("ENOTDIR:")) << file_parent->error;
  std::vector<base::DictValue> invalid_requests;
  invalid_requests.emplace_back();
  invalid_requests.push_back(
      base::DictValue().Set("target", std::string("token\0suffix", 12)));
  invalid_requests.push_back(
      base::DictValue().Set("target", "payload").Set("type", "invalid"));
  for (auto& options : invalid_requests) {
    auto invalid = Call("symlink", link, std::move(options));
    ASSERT_FALSE(invalid->success);
    EXPECT_TRUE(invalid->error.starts_with("EINVAL:")) << invalid->error;
    EXPECT_FALSE(base::IsLink(link));
  }
  base::DictValue request;
  request.Set("operation", "symlink");
  request.Set("path", link.AsUTF8Unsafe() + std::string("\0suffix", 7));
  request.Set("target", "payload");
  auto invalid_path = PerformFileSystemCall(
      base::Value(base::ListValue().Append(std::move(request))));
  ASSERT_FALSE(invalid_path->success);
  EXPECT_TRUE(invalid_path->error.starts_with("EINVAL:"))
      << invalid_path->error;
  EXPECT_FALSE(base::IsLink(link));
}

TEST_F(XenonFileSystemSymlinkTest, LinkCallsRetainTheReadOnlyAsarPolicy) {
  xenon::asar::TestArchiveBuilder builder;
  const auto archive = temp_dir_.GetPath().AppendASCII("readonly.asar");
  ASSERT_TRUE(builder.Write(archive, {{"packed.txt", "original"}}, false));
  const auto entry = archive.AppendASCII("packed.txt");
  for (const char* operation : {"symlink", "readlink"}) {
    auto denied = Call(operation, entry,
                       base::DictValue().Set("target", "must-not-replace"));
    ASSERT_FALSE(denied->success);
    EXPECT_TRUE(denied->error.starts_with("EACCES:")) << denied->error;
  }
  auto packed = Call("read_file", entry);
  ASSERT_TRUE(packed->success) << packed->error;
  EXPECT_EQ(base::Base64Encode("original"), packed->value.GetString());
}
#else
TEST_F(XenonFileSystemSymlinkTest, UnsupportedPlatformsDoNotCreateFakeLocks) {
  const auto link = temp_dir_.GetPath().AppendASCII("lock");
  for (const char* operation : {"symlink", "readlink"}) {
    auto result =
        Call(operation, link, base::DictValue().Set("target", "payload"));
    ASSERT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with("ENOSYS:")) << result->error;
    EXPECT_FALSE(base::PathExists(link));
  }
}
#endif

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
