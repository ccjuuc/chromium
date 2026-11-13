// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_file_descriptors.h"

#include <string>
#include <string_view>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/asar/test_support.h"
#include "xenon_overlay/third_party/libuv/include/uv.h"

namespace xenon::ipc {
namespace {
class XenonFileDescriptorsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    files_ = base::MakeRefCounted<XenonFileDescriptors>(directory_.GetPath());
  }
  void TearDown() override {
    files_->CloseAll();
    asar::RemoveArchivePublicKeys("descriptor-test");
  }
  mojom::IpcResultPtr Call(const std::string& operation,
                           base::DictValue options = {},
                           XenonFileDescriptors* table = nullptr) {
    options.Set("operation", operation);
    return (table ? table : files_.get())
        ->Call(base::Value(base::ListValue().Append(std::move(options))));
  }
  int Open(const std::string& path, const std::string& flags = "r") {
    auto result = Call("fd_open",
                       base::DictValue().Set("path", path).Set("flags", flags));
    EXPECT_TRUE(result->success) << result->error;
    return result->success ? result->value.GetInt() : -1;
  }
  void Close(int fd) {
    auto result = Call("fd_close", base::DictValue().Set("fd", fd));
    EXPECT_TRUE(result->success) << result->error;
  }
  base::DictValue WriteOptions(int fd, std::string_view bytes) {
    return base::DictValue().Set("fd", fd).Set(
        "data",
        base::Value(base::Value::BlobStorage(bytes.begin(), bytes.end())));
  }
  void ExpectRead(int fd,
                  int length,
                  std::string_view expected,
                  base::Value position = base::Value()) {
    auto result = Call("fd_read", base::DictValue()
                                      .Set("fd", fd)
                                      .Set("length", length)
                                      .Set("position", std::move(position)));
    ASSERT_TRUE(result->success) << result->error;
    ASSERT_TRUE(result->value.is_blob());
    const auto& bytes = result->value.GetBlob();
    EXPECT_EQ(expected, std::string(bytes.begin(), bytes.end()));
  }
  void ExpectError(const mojom::IpcResultPtr& result, std::string_view code) {
    EXPECT_FALSE(result->success);
    EXPECT_TRUE(result->error.starts_with(std::string(code) + ":"))
        << result->error;
  }
  base::ScopedTempDir directory_;
  scoped_refptr<XenonFileDescriptors> files_;
};

TEST_F(XenonFileDescriptorsTest, PositionedWritesDoNotMoveSequentialCursor) {
  const int fd = Open("relative.bin", "w+");
  ASSERT_GE(fd, 0);
  auto first = Call("fd_write", WriteOptions(fd, "abcdef"));
  ASSERT_TRUE(first->success) << first->error;
  EXPECT_EQ(6, first->value.GetInt());
  auto positioned = WriteOptions(fd, "XY");
  positioned.Set("position", 2);
  ASSERT_TRUE(Call("fd_write", std::move(positioned))->success);
  ASSERT_TRUE(Call("fd_write", WriteOptions(fd, "!"))->success);
  ExpectRead(fd, 4, "bXYe", base::Value(1));
  ExpectRead(fd, 1, "");
  Close(fd);
  std::string disk;
  ASSERT_TRUE(base::ReadFileToString(
      directory_.GetPath().AppendASCII("relative.bin"), &disk));
  EXPECT_EQ("abXYef!", disk);
}

TEST_F(XenonFileDescriptorsTest, BigIntDescriptorStatsMatchPathIdentityAndTimes) {
  const auto path = directory_.GetPath().AppendASCII("identity.txt");
  const int fd = Open(path.AsUTF8Unsafe(), "w+");
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(Call("fd_write", WriteOptions(fd, "metadata"))->success);
  auto descriptor = Call("fd_fstat", base::DictValue().Set("fd", fd).Set("bigint", true));
  ASSERT_TRUE(descriptor->success) << descriptor->error;
  auto pathname = Call("lstat", base::DictValue().Set("path", path.AsUTF8Unsafe()).Set("bigint", true));
  ASSERT_TRUE(pathname->success) << pathname->error;
  for (const char* field : {"dev", "ino", "size", "mtimeNs", "birthtimeNs"}) {
    const auto* descriptor_value = descriptor->value.GetDict().FindString(field);
    const auto* path_value = pathname->value.GetDict().FindString(field);
    ASSERT_TRUE(descriptor_value) << field;
    ASSERT_TRUE(path_value) << field;
    EXPECT_EQ(*descriptor_value, *path_value) << field;
  }
  uv_fs_t native = {};
  ASSERT_EQ(0, uv_fs_fstat(nullptr, &native, fd, nullptr));
  EXPECT_EQ(base::NumberToString(native.statbuf.st_ino),
            *descriptor->value.GetDict().FindString("ino"));
  uv_fs_req_cleanup(&native);
  Close(fd);
}

TEST_F(XenonFileDescriptorsTest, PositionedReadsDoNotMoveSequentialCursor) {
  const auto path = directory_.GetPath().AppendASCII("bytes.bin");
  ASSERT_TRUE(base::WriteFile(path, "0123456789"));
  const int fd = Open(path.AsUTF8Unsafe());
  ASSERT_GE(fd, 0);
  ExpectRead(fd, 2, "01");
  ExpectRead(fd, 3, "678", base::Value(6));
  ExpectRead(fd, 3, "234");
  ExpectRead(fd, 20, "56789");
  ExpectRead(fd, 2, "");
}

TEST_F(XenonFileDescriptorsTest, NegativeOnePositionUsesCurrentCursor) {
  const int fd = Open("current.bin", "w+");
  ASSERT_GE(fd, 0);
  auto write = WriteOptions(fd, "abc");
  write.Set("position", -1);
  auto result = Call("fd_write", std::move(write));
  ASSERT_TRUE(result->success) << result->error;
  Close(fd);
  const int reader = Open("current.bin");
  ASSERT_GE(reader, 0);
  ExpectRead(reader, 1, "a", base::Value(-1));
  ExpectRead(reader, 1, "b", base::Value(-1));
  ExpectRead(reader, 1, "c");
}

TEST_F(XenonFileDescriptorsTest, ClosedAndOtherTableDescriptorsAreRejected) {
  const int fd = Open("owned.bin", "w+");
  ASSERT_GE(fd, 0);
  auto other = base::MakeRefCounted<XenonFileDescriptors>(directory_.GetPath());
  ExpectError(Call("fd_fstat", base::DictValue().Set("fd", fd), other.get()),
              "EBADF");
  ExpectError(Call("fd_close", base::DictValue().Set("fd", fd), other.get()),
              "EBADF");
  auto still_open = Call("fd_fstat", base::DictValue().Set("fd", fd));
  ASSERT_TRUE(still_open->success) << still_open->error;
  uv_fs_t stat = {};
  EXPECT_EQ(0, uv_fs_fstat(nullptr, &stat, fd, nullptr));
  uv_fs_req_cleanup(&stat);
  Close(fd);
  ExpectError(Call("fd_fstat", base::DictValue().Set("fd", fd)), "EBADF");
  ExpectError(Call("fd_close", base::DictValue().Set("fd", fd)), "EBADF");
  uv_fs_t closed = {};
  EXPECT_EQ(UV_EBADF, uv_fs_fstat(nullptr, &closed, fd, nullptr));
  uv_fs_req_cleanup(&closed);
}

TEST_F(XenonFileDescriptorsTest, WorkerCleanupUsesOwnershipNotAccessIsolation) {
  const int parent = Open("shared.bin", "w+");
  ASSERT_GE(parent, 0);
  auto owned = [&](uint64_t owner, const char* operation,
                   base::DictValue options) {
    options.Set("operation", operation);
    return files_->CallOwned(
        base::Value(base::ListValue().Append(std::move(options))), owner);
  };
  ASSERT_TRUE(owned(12, "fd_write", WriteOptions(parent, "parent"))->success);
  auto created =
      owned(12, "fd_open",
            base::DictValue().Set("path", "child.bin").Set("flags", "w+"));
  ASSERT_TRUE(created->success) << created->error;
  const int child = created->value.GetInt();
  ASSERT_TRUE(owned(13, "fd_write", WriteOptions(child, "shared"))->success);
  files_->CloseOwned(12);
  EXPECT_TRUE(Call("fd_fstat", base::DictValue().Set("fd", parent))->success);
  ExpectError(Call("fd_fstat", base::DictValue().Set("fd", child)), "EBADF");
  // An asynchronous open queued by the exited worker may not resurrect its
  // descriptors after cleanup has finished.
  ExpectError(
      owned(12, "fd_open",
            base::DictValue().Set("path", "late.bin").Set("flags", "w+")),
      "EBADF");
  EXPECT_FALSE(base::PathExists(directory_.GetPath().AppendASCII("late.bin")));
}

TEST_F(XenonFileDescriptorsTest, ExclusiveCreateDoesNotTruncateExistingFile) {
  const auto path = directory_.GetPath().AppendASCII("exclusive.bin");
  ASSERT_TRUE(base::WriteFile(path, "preserve"));
  for (const char* flags : {"wx", "wx+", "ax", "ax+"}) {
    ExpectError(Call("fd_open", base::DictValue()
                                    .Set("path", path.AsUTF8Unsafe())
                                    .Set("flags", flags)),
                "EEXIST");
  }
  std::string bytes;
  ASSERT_TRUE(base::ReadFileToString(path, &bytes));
  EXPECT_EQ("preserve", bytes);
  const int new_fd = Open("created.bin", "wx");
  ASSERT_GE(new_fd, 0);
  Close(new_fd);
  EXPECT_TRUE(
      base::PathExists(directory_.GetPath().AppendASCII("created.bin")));
}

TEST_F(XenonFileDescriptorsTest, AppendWritesAtEndAndReadOnlyCannotWrite) {
  const auto path = directory_.GetPath().AppendASCII("append.bin");
  ASSERT_TRUE(base::WriteFile(path, "old"));
  const int fd = Open("append.bin", "a");
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(Call("fd_write", WriteOptions(fd, "new"))->success);
  Close(fd);
  std::string bytes;
  ASSERT_TRUE(base::ReadFileToString(path, &bytes));
  EXPECT_EQ("oldnew", bytes);
  const int read_only = Open("append.bin");
  EXPECT_FALSE(Call("fd_write", WriteOptions(read_only, "bad"))->success);
  ExpectRead(read_only, 6, "oldnew");
}

TEST_F(XenonFileDescriptorsTest, FlushAndTruncateUpdateActualDisk) {
  const int fd = Open("truncate.bin", "w+");
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(Call("fd_write", WriteOptions(fd, "abcdef"))->success);
  ASSERT_TRUE(Call("fd_fsync", base::DictValue().Set("fd", fd))->success);
  ASSERT_TRUE(Call("fd_fdatasync", base::DictValue().Set("fd", fd))->success);
  ASSERT_TRUE(
      Call("fd_ftruncate", base::DictValue().Set("fd", fd).Set("length", 3))
          ->success);
  auto stat = Call("fd_fstat", base::DictValue().Set("fd", fd));
  ASSERT_TRUE(stat->success) << stat->error;
  EXPECT_EQ(3, stat->value.GetDict().FindDouble("size"));
  ExpectRead(fd, 6, "abc", base::Value(0));
}

TEST_F(XenonFileDescriptorsTest,
       CloseAllRevokesRetainedWorkerTableAndClosesFd) {
  const int fd = Open("shutdown.bin", "w+");
  ASSERT_GE(fd, 0);
  auto worker_reference = files_;
  files_->CloseAll();
  ExpectError(Call("fd_open",
                   base::DictValue().Set("path", "later.bin").Set("flags", "w"),
                   worker_reference.get()),
              "EBADF");
  ExpectError(
      Call("fd_write", WriteOptions(fd, "late"), worker_reference.get()),
      "EBADF");
  uv_fs_t request = {};
  EXPECT_EQ(UV_EBADF, uv_fs_fstat(nullptr, &request, fd, nullptr));
  uv_fs_req_cleanup(&request);
  EXPECT_FALSE(base::PathExists(directory_.GetPath().AppendASCII("later.bin")));
  files_->CloseAll();
}

TEST_F(XenonFileDescriptorsTest,
       AsarPackedAndUnpackedMembersUseRealReadOnlyFds) {
  const std::string packed(128 * 1024 + 17, 'p');
  asar::TestArchiveBuilder builder;
  const auto archive = directory_.GetPath().AppendASCII("app.asar");
  ASSERT_TRUE(builder.Write(
      archive, {{"packed.bin", packed}, {"unpacked.bin", "loose", false, true}},
      false));
  const auto member = archive.AppendASCII("packed.bin").AsUTF8Unsafe();
  const int packed_fd = Open(member);
  ASSERT_GE(packed_fd, 0);
  ExpectRead(packed_fd, 20, std::string(17, 'p'), base::Value(128 * 1024));
  auto stat = Call("fd_fstat", base::DictValue().Set("fd", packed_fd));
  ASSERT_TRUE(stat->success) << stat->error;
  EXPECT_EQ(static_cast<double>(packed.size()),
            stat->value.GetDict().FindDouble("size"));
  ExpectError(
      Call("fd_open", base::DictValue().Set("path", member).Set("flags", "r+")),
      "EROFS");
  const int unpacked_fd =
      Open(archive.AppendASCII("unpacked.bin").AsUTF8Unsafe());
  ASSERT_GE(unpacked_fd, 0);
  ExpectRead(unpacked_fd, 10, "loose");
  Close(packed_fd);
  uv_fs_t request = {};
  EXPECT_EQ(UV_EBADF, uv_fs_fstat(nullptr, &request, packed_fd, nullptr));
  uv_fs_req_cleanup(&request);
}

TEST_F(XenonFileDescriptorsTest,
       EncryptedAsarReadsDecodedMemberAndOriginalFsReadsArchive) {
  asar::TestArchiveBuilder builder;
  const auto archive = directory_.GetPath().AppendASCII("encrypted.asar");
  const std::string expected = "0123456789abcdef";
  ASSERT_TRUE(builder.Write(archive, {{"private.bin", expected, true}}, true));
  ASSERT_TRUE(asar::SetArchivePublicKeys(
      "descriptor-test", {{directory_.GetPath(), builder.public_key_pem()}}));
  const int fd = Open(archive.AppendASCII("private.bin").AsUTF8Unsafe());
  ASSERT_GE(fd, 0);
  ExpectRead(fd, 32, expected);
  auto raw = Call("fd_open", base::DictValue()
                                 .Set("path", archive.AsUTF8Unsafe())
                                 .Set("noAsar", true));
  ASSERT_TRUE(raw->success) << raw->error;
  std::string physical;
  ASSERT_TRUE(base::ReadFileToString(archive, &physical));
  ExpectRead(raw->value.GetInt(), 16, physical.substr(0, 16));
}

#if BUILDFLAG(IS_POSIX)
TEST_F(XenonFileDescriptorsTest, CreationModeAndChmodApplyToDisk) {
  auto opened = Call("fd_open", base::DictValue()
                                    .Set("path", "private.bin")
                                    .Set("flags", "w+")
                                    .Set("mode", 0600));
  ASSERT_TRUE(opened->success) << opened->error;
  const int fd = opened->value.GetInt();
  int permissions = 0;
  const auto path = directory_.GetPath().AppendASCII("private.bin");
  ASSERT_TRUE(base::GetPosixFilePermissions(path, &permissions));
  EXPECT_EQ(0, permissions & 077);
  ASSERT_TRUE(
      Call("fd_fchmod", base::DictValue().Set("fd", fd).Set("mode", 0640))
          ->success);
  ASSERT_TRUE(base::GetPosixFilePermissions(path, &permissions));
  EXPECT_EQ(0640, permissions & 0777);
}
#endif
}  // namespace
}  // namespace xenon::ipc
