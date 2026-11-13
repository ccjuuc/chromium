// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <string>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"
#include "xenon_overlay/common/asar/test_support.h"

namespace xenon::ipc {
namespace {

class XenonFileSystemMkdtempTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(directory_.CreateUniqueTempDir()); }
  mojom::IpcResultPtr Create(std::string prefix, bool no_asar = false) {
    return PerformFileSystemCall(
        base::Value(base::ListValue().Append(base::DictValue()
                                                 .Set("operation", "mkdtemp")
                                                 .Set("path", prefix)
                                                 .Set("returnBytes", true)
                                                 .Set("noAsar", no_asar))));
  }
  std::string CreatedPath(const mojom::IpcResultPtr& result) {
    const auto& bytes = result->value.GetBlob();
    return std::string(bytes.begin(), bytes.end());
  }
  base::ScopedTempDir directory_;
};

TEST_F(XenonFileSystemMkdtempTest,
       AtomicallyCreatesUniqueUnicodePrefixDirectories) {
  const auto prefix =
      directory_.GetPath()
          .Append(base::FilePath::FromUTF8Unsafe("temporary-中文-"))
          .AsUTF8Unsafe();
  auto first = Create(prefix);
  auto second = Create(prefix);
  ASSERT_TRUE(first->success) << first->error;
  ASSERT_TRUE(second->success) << second->error;
  ASSERT_TRUE(first->value.is_blob());
  ASSERT_TRUE(second->value.is_blob());
  const auto a = CreatedPath(first), b = CreatedPath(second);
  EXPECT_NE(a, b);
  EXPECT_EQ(a.substr(0, a.size() - 6), prefix);
  EXPECT_EQ(b.substr(0, b.size() - 6), prefix);
  EXPECT_TRUE(base::DirectoryExists(base::FilePath::FromUTF8Unsafe(a)));
  EXPECT_TRUE(base::DirectoryExists(base::FilePath::FromUTF8Unsafe(b)));
}

TEST_F(XenonFileSystemMkdtempTest,
       PrefixWithTrailingSeparatorCreatesInsideDirectory) {
  const auto prefix =
      directory_.GetPath().AsEndingWithSeparator().AsUTF8Unsafe();
  auto result = Create(prefix);
  ASSERT_TRUE(result->success) << result->error;
  const auto created = base::FilePath::FromUTF8Unsafe(CreatedPath(result));
  EXPECT_EQ(created.DirName(), directory_.GetPath());
  EXPECT_EQ(created.BaseName().value().size(), 6u);
}

TEST_F(XenonFileSystemMkdtempTest,
       MissingParentAndNullBytesFailWithoutCreatingDirectories) {
  const auto parent = directory_.GetPath().AppendASCII("missing");
  const auto prefix = parent.AppendASCII("prefix-").AsUTF8Unsafe();
  auto missing = Create(prefix);
  EXPECT_FALSE(missing->success);
  EXPECT_TRUE(missing->error.starts_with("ENOENT:"));
  EXPECT_NE(missing->error.find(prefix + "XXXXXX"), std::string::npos);
  EXPECT_FALSE(base::PathExists(parent));
  auto nul = Create(prefix + std::string("\0suffix", 7));
  EXPECT_FALSE(nul->success);
  EXPECT_TRUE(nul->error.starts_with("EINVAL:"));
  EXPECT_FALSE(base::PathExists(parent));
}

TEST_F(XenonFileSystemMkdtempTest,
       PhysicalDirectoryWithAsarSuffixRemainsWritable) {
  const auto physical = directory_.GetPath().AppendASCII("physical.asar");
  ASSERT_TRUE(base::CreateDirectory(physical));
  const auto prefix = physical.AppendASCII("prefix-").AsUTF8Unsafe();
  auto normal = Create(prefix);
  ASSERT_TRUE(normal->success) << normal->error;
  EXPECT_TRUE(base::DirectoryExists(
      base::FilePath::FromUTF8Unsafe(CreatedPath(normal))));
  auto original = Create(prefix, true);
  ASSERT_TRUE(original->success) << original->error;
  EXPECT_TRUE(base::DirectoryExists(
      base::FilePath::FromUTF8Unsafe(CreatedPath(original))));
}

TEST_F(XenonFileSystemMkdtempTest,
       NormalFsRejectsArchiveWritesAndOriginalFsSeesThePhysicalFile) {
  const auto archive = directory_.GetPath().AppendASCII("packed.asar");
  asar::TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(archive, {{"prefix-", "immutable member"}}, false));
  std::string before;
  ASSERT_TRUE(base::ReadFileToString(archive, &before));
  const auto prefix = archive.AppendASCII("prefix-").AsUTF8Unsafe();
  auto normal = Create(prefix);
  EXPECT_FALSE(normal->success);
  EXPECT_TRUE(normal->error.starts_with("EACCES:"));
  auto original = Create(prefix, true);
  EXPECT_FALSE(original->success);
  EXPECT_TRUE(original->error.starts_with("ENOTDIR:") ||
              original->error.starts_with("ENOENT:"))
      << original->error;
  std::string after;
  ASSERT_TRUE(base::ReadFileToString(archive, &after));
  EXPECT_EQ(after, before);
}

}  // namespace
}  // namespace xenon::ipc
