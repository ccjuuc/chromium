// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/asar/archive.h"

#include <array>
#include <thread>
#include <vector>

#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/pickle.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/common/asar/test_support.h"

namespace xenon::asar {
namespace {

class ArchiveTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_.CreateUniqueTempDir()); }
  void TearDown() override {
    RemoveArchivePublicKeys("archive-test");
    RemoveArchivePublicKeys("archive-test-second");
  }
  base::FilePath Path(const char* name = "test.asar") const {
    return temp_.GetPath().AppendASCII(name);
  }
  bool Configure(const TestArchiveBuilder& builder) {
    return SetArchivePublicKeys("archive-test",
                                {{temp_.GetPath(), builder.public_key_pem()}});
  }
  void ExpectContents(const std::shared_ptr<Archive>& archive,
                      const char* path,
                      const std::string& expected) {
    ASSERT_TRUE(archive);
    std::string actual;
    ASSERT_TRUE(archive->ReadFile(base::FilePath::FromASCII(path), &actual));
    EXPECT_EQ(actual, expected);
  }
  bool WriteStandardHeader(std::string_view json,
                           std::string_view payload = {}) {
    base::Pickle index;
    index.WriteString(json);
    base::Pickle size;
    size.WriteUInt32(index.size());
    std::string bytes(reinterpret_cast<const char*>(size.data()), size.size());
    bytes.append(reinterpret_cast<const char*>(index.data()), index.size());
    bytes.append(payload);
    return base::WriteFile(Path(), bytes);
  }
  base::ScopedTempDir temp_;
};

TEST_F(ArchiveTest, StandardAndEncryptedMembersHaveSameLogicalContents) {
  TestArchiveBuilder builder;
  const std::string content(4096, 'z');
  ASSERT_TRUE(builder.Write(Path("plain.asar"),
                            {{"dir/code.js", content}, {"empty", ""}}, false));
  ASSERT_TRUE(builder.Write(Path(), {{"dir/code.js", content, true},
                                     {"plain", "ordinary"},
                                     {"empty", "", true},
                                     {"addon.node", "unpacked", false, true}}));
  ASSERT_TRUE(Configure(builder));
  ExpectContents(GetOrCreateAsarArchive(Path("plain.asar")), "dir/code.js",
                 content);
  auto encrypted = GetOrCreateAsarArchive(Path());
  ExpectContents(encrypted, "dir/code.js", content);
  ExpectContents(encrypted, "plain", "ordinary");
  ExpectContents(encrypted, "empty", "");
  ExpectContents(encrypted, "addon.node", "unpacked");
}

TEST_F(ArchiveTest, SpacePaddingIsPreservedAndUnpackedSizeUsesDisk) {
  TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(
      Path(), {{"padded", "data", true}, {"external", "old", false, true}}));
  ASSERT_TRUE(Configure(builder));
  ASSERT_TRUE(base::WriteFile(Path()
                                  .AddExtension(FILE_PATH_LITERAL("unpacked"))
                                  .AppendASCII("external"),
                              "updated external data"));
  auto archive = GetOrCreateAsarArchive(Path());
  ExpectContents(archive, "padded", "data            ");
  ExpectContents(archive, "external", "updated external data");
}

TEST_F(ArchiveTest, RangeReadsOnlyAlignedBlocksAndReportsShortReads) {
  TestArchiveBuilder builder;
  std::string contents(16 * 1024 * 1024, '\0');
  for (size_t index = 0; index < contents.size(); ++index) {
    contents[index] = static_cast<char>(index % 251);
  }
  ASSERT_TRUE(builder.Write(Path(), {{"large", contents, true}}));
  ASSERT_TRUE(Configure(builder));
  auto archive = GetOrCreateAsarArchive(Path());
  ASSERT_TRUE(archive);
  auto reader = archive->CreateReader(base::FilePath::FromASCII("large"));
  ASSERT_TRUE(reader);
  std::vector<uint8_t> bytes(65536);
  ASSERT_EQ(reader->Read(3, bytes), bytes.size());
  EXPECT_EQ(std::string(bytes.begin(), bytes.end()), contents.substr(3, 65536));
  std::array<uint8_t, 19> tail;
  ASSERT_EQ(reader->Read(contents.size() - tail.size(), tail), tail.size());
  EXPECT_EQ(std::string(tail.begin(), tail.end()),
            contents.substr(contents.size() - 19));
  EXPECT_EQ(reader->Read(contents.size(), bytes), 0u);
  EXPECT_EQ(reader->Read(UINT64_MAX, bytes), 0u);
  Archive::FileInfo info;
  ASSERT_TRUE(archive->GetFileInfo(base::FilePath::FromASCII("large"), &info));
  base::File truncate(Path(), base::File::FLAG_OPEN | base::File::FLAG_WRITE);
  ASSERT_TRUE(truncate.SetLength(info.offset + 65552));
  // A whole-member read, or even one block beyond the required 65,552 bytes,
  // would now fail. The requested 64 KiB remains readable and identical.
  ASSERT_EQ(reader->Read(3, bytes), bytes.size());
  EXPECT_EQ(std::string(bytes.begin(), bytes.end()), contents.substr(3, 65536));
  EXPECT_FALSE(reader->Read(65536, bytes));
}

TEST_F(ArchiveTest, PackedEncryptedAndUnpackedReadsCrossPhysicalIoChunks) {
  TestArchiveBuilder builder;
  std::string contents(3 * 1024 * 1024 + 64, '\0');
  for (size_t index = 0; index < contents.size(); ++index) {
    contents[index] = static_cast<char>(index % 251);
  }
  ASSERT_TRUE(builder.Write(Path(), {{"packed", contents},
                                     {"encrypted", contents, true},
                                     {"unpacked", contents, false, true}}));
  ASSERT_TRUE(Configure(builder));
  auto archive = GetOrCreateAsarArchive(Path());
  ASSERT_TRUE(archive);
  for (const char* name : {"packed", "encrypted", "unpacked"}) {
    ExpectContents(archive, name, contents);
    auto reader = archive->CreateReader(base::FilePath::FromASCII(name));
    ASSERT_TRUE(reader);
    std::vector<uint8_t> bytes(2 * 1024 * 1024 + 7);
    ASSERT_EQ(reader->Read(3, bytes), bytes.size());
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()),
              contents.substr(3, bytes.size()));
  }
  auto reader = archive->CreateReader(base::FilePath::FromASCII("unpacked"));
  ASSERT_TRUE(reader);
  base::File truncated(Path()
                           .AddExtension(FILE_PATH_LITERAL("unpacked"))
                           .AppendASCII("unpacked"),
                       base::File::FLAG_OPEN | base::File::FLAG_WRITE);
  ASSERT_TRUE(truncated.SetLength(1024 * 1024 + 4));
  std::vector<uint8_t> bytes(contents.size());
  // A later physical I/O failure must fail the complete logical request.
  EXPECT_FALSE(reader->Read(0, bytes));
}

TEST_F(ArchiveTest, DifferentArchiveKeysSurviveAlternatingAndConcurrentReads) {
  TestArchiveBuilder first;
  TestArchiveBuilder second;
  const std::string first_data(1024 * 1024, 'a');
  const std::string second_data(1024 * 1024, 'b');
  ASSERT_TRUE(first.Write(Path("a.asar"), {{"data", first_data, true}}));
  ASSERT_TRUE(second.Write(Path("b.asar"), {{"data", second_data, true}}));
  ASSERT_TRUE(SetArchivePublicKeys(
      "archive-test", {{Path("a.asar"), first.public_key_pem()},
                       {Path("b.asar"), second.public_key_pem()}}));
  auto a = GetOrCreateAsarArchive(Path("a.asar"));
  auto b = GetOrCreateAsarArchive(Path("b.asar"));
  for (int index = 0; index < 3; ++index) {
    ExpectContents(a, "data", first_data);
    ExpectContents(b, "data", second_data);
  }
  ASSERT_TRUE(a);
  auto reader = a->CreateReader(base::FilePath::FromASCII("data"));
  ASSERT_TRUE(reader);
  auto read = [&] {
    std::vector<uint8_t> bytes(65536);
    EXPECT_EQ(reader->Read(3, bytes), bytes.size());
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()),
              first_data.substr(3, 65536));
  };
  std::thread worker(read);
  read();
  ExpectContents(b, "data", second_data);
  worker.join();
}

TEST_F(ArchiveTest, MissingWrongRevokedAndReplacedKeysFailClosed) {
  TestArchiveBuilder first;
  TestArchiveBuilder second;
  ASSERT_TRUE(first.Write(Path(), {{"data", "plaintext", true}}));
  EXPECT_FALSE(GetOrCreateAsarArchive(Path()));
  ASSERT_TRUE(Configure(second));
  EXPECT_FALSE(GetOrCreateAsarArchive(Path()));
  ASSERT_TRUE(Configure(first));
  auto old = GetOrCreateAsarArchive(Path());
  ASSERT_TRUE(old);
  EXPECT_EQ(old, GetOrCreateAsarArchive(Path()));
  RemoveArchivePublicKeys("archive-test");
  EXPECT_FALSE(GetOrCreateAsarArchive(Path()));
  // Existing readers keep their original open-file/decryption snapshot.
  ExpectContents(old, "data", "plaintext       ");
  ASSERT_TRUE(Configure(second));
  EXPECT_FALSE(GetOrCreateAsarArchive(Path()));
  ASSERT_TRUE(Configure(first));
  auto current = GetOrCreateAsarArchive(Path());
  ASSERT_TRUE(current);
  EXPECT_NE(old, current);
}

TEST_F(ArchiveTest, ConflictingOwnersRejectedWithoutReplacingExistingConfig) {
  TestArchiveBuilder first;
  TestArchiveBuilder second;
  ASSERT_TRUE(first.Write(Path(), {{"data", "x", true}}));
  ASSERT_TRUE(Configure(first));
  EXPECT_FALSE(SetArchivePublicKeys("archive-test-second",
                                    {{Path(), second.public_key_pem()}}));
  EXPECT_TRUE(GetOrCreateAsarArchive(Path()));
  EXPECT_FALSE(
      SetArchivePublicKeys("archive-test", {{temp_.GetPath(), "bad"}}));
  EXPECT_TRUE(GetOrCreateAsarArchive(Path()));
  EXPECT_FALSE(SetArchivePublicKeys("archive-test",
                                    {{temp_.GetPath(), first.public_key_pem()},
                                     {Path(), second.public_key_pem()}}));
  EXPECT_TRUE(GetOrCreateAsarArchive(Path()));
  ASSERT_TRUE(SetArchivePublicKeys(
      "archive-test-second", {{temp_.GetPath(), first.public_key_pem()}}));
  RemoveArchivePublicKeys("archive-test");
  EXPECT_TRUE(GetOrCreateAsarArchive(Path()));
  RemoveArchivePublicKeys("archive-test-second");
  EXPECT_FALSE(GetOrCreateAsarArchive(Path()));
}

TEST_F(ArchiveTest, KeyRootDoesNotAuthorizeSiblingPaths) {
  TestArchiveBuilder builder;
  base::ScopedTempDir sibling;
  ASSERT_TRUE(sibling.CreateUniqueTempDir());
  const auto other = sibling.GetPath().AppendASCII("test.asar");
  ASSERT_TRUE(builder.Write(Path(), {{"data", "x", true}}));
  ASSERT_TRUE(builder.Write(other, {{"data", "x", true}}));
  ASSERT_TRUE(Configure(builder));
  EXPECT_TRUE(GetOrCreateAsarArchive(Path()));
  EXPECT_FALSE(GetOrCreateAsarArchive(other));
}

TEST_F(ArchiveTest, IdenticalAndUnknownEmptyConfigurationPreserveCache) {
  TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(Path(), {{"data", "x", true}}));
  ASSERT_TRUE(Configure(builder));
  auto archive = GetOrCreateAsarArchive(Path());
  ASSERT_TRUE(archive);
  ASSERT_TRUE(Configure(builder));
  EXPECT_EQ(archive, GetOrCreateAsarArchive(Path()));
  ASSERT_TRUE(SetArchivePublicKeys(
      "archive-test", {{temp_.GetPath().AsEndingWithSeparator(),
                        "  \n" + builder.public_key_pem() + "\n "}}));
  EXPECT_EQ(archive, GetOrCreateAsarArchive(Path()));
  ASSERT_TRUE(SetArchivePublicKeys("archive-test-second", {}));
  EXPECT_EQ(archive, GetOrCreateAsarArchive(Path()));
  ASSERT_TRUE(SetArchivePublicKeys(
      "archive-test", {{temp_.GetPath(), builder.public_key_pem()},
                       {temp_.GetPath(), builder.public_key_pem()}}));
  EXPECT_EQ(archive, GetOrCreateAsarArchive(Path()));
  ASSERT_TRUE(SetArchivePublicKeys("archive-test", {}));
  EXPECT_FALSE(GetOrCreateAsarArchive(Path()));
}

TEST_F(ArchiveTest, ConfigurationRejectsPrivateOrMultiplePemBlocks) {
  TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(Path(), {{"data", "x", true}}));
  ASSERT_TRUE(Configure(builder));
  auto archive = GetOrCreateAsarArchive(Path());
  ASSERT_TRUE(archive);
  for (const auto& malformed :
       {builder.public_key_pem() + builder.public_key_pem(),
        "-----BEGIN PRIVATE KEY-----\ninvalid\n-----END PRIVATE KEY-----\n" +
            builder.public_key_pem(),
        builder.public_key_pem() + "unexpected trailing data"}) {
    EXPECT_FALSE(
        SetArchivePublicKeys("archive-test", {{temp_.GetPath(), malformed}}));
    EXPECT_EQ(archive, GetOrCreateAsarArchive(Path()));
  }
}

TEST_F(ArchiveTest, ReadersKeepTheOriginalFileWhenPathIsReplaced) {
  TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(Path(), {{"data", "original", true}}));
  ASSERT_TRUE(
      builder.Write(Path("replacement.asar"), {{"data", "replacement", true}}));
  ASSERT_TRUE(Configure(builder));
  auto archive = GetOrCreateAsarArchive(Path());
  ASSERT_TRUE(archive);
  auto reader = archive->CreateReader(base::FilePath::FromASCII("data"));
  ASSERT_TRUE(reader);
  ASSERT_TRUE(base::ReplaceFile(Path("replacement.asar"), Path(), nullptr));
  std::array<uint8_t, 8> bytes;
  ASSERT_EQ(reader->Read(0, bytes), bytes.size());
  EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "original");
  EXPECT_EQ(archive, GetOrCreateAsarArchive(Path()));
  RemoveArchivePublicKeys("archive-test");
  ASSERT_TRUE(Configure(builder));
  ExpectContents(GetOrCreateAsarArchive(Path()), "data", "replacement     ");
  EXPECT_EQ(reader->Read(0, bytes), bytes.size());
  EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "original");
}

TEST_F(ArchiveTest, CorruptEnvelopeIndexAndTruncatedPayloadAreRejected) {
  TestArchiveBuilder builder;
  ASSERT_TRUE(Configure(builder));
  for (const size_t position : {size_t{9}, size_t{270}}) {
    ASSERT_TRUE(builder.Write(Path(), {{"data", "hello", true}}));
    std::string bytes;
    ASSERT_TRUE(base::ReadFileToString(Path(), &bytes));
    ASSERT_LT(position, bytes.size());
    bytes[position] ^= 1;
    ASSERT_TRUE(base::WriteFile(Path(), bytes));
    EXPECT_FALSE(GetOrCreateAsarArchive(Path()));
  }
  ASSERT_TRUE(builder.Write(Path(), {{"data", "hello", true}}));
  base::File truncated(Path(), base::File::FLAG_OPEN | base::File::FLAG_WRITE);
  ASSERT_TRUE(truncated.SetLength(truncated.GetLength() - 1));
  EXPECT_FALSE(GetOrCreateAsarArchive(Path()));
}

TEST_F(ArchiveTest, InvalidJsonMetadataAndOutOfBoundsEntriesAreRejected) {
  for (const char* json :
       {"not json", R"({"files":{"bad":{"size":-1,"offset":"0"}}})",
        R"({"files":{"bad":{"size":1,"offset":"18446744073709551615"}}})",
        R"({"files":{"bad":{"size":100,"offset":"0"}}})",
        R"({"files":{"bad":{"size":0,"offset":"0","encrypted":true}}})",
        R"({"files":{"../escape":{"size":0,"offset":"0"}}})",
        R"({"files":{"bad":{"size":0,"offset":"0","unpacked":"true"}}})"}) {
    ASSERT_TRUE(WriteStandardHeader(json));
    EXPECT_FALSE(GetOrCreateAsarArchive(Path()));
  }
}

TEST_F(ArchiveTest, StandardPackedLinksResolveAndCyclesFail) {
  ASSERT_TRUE(WriteStandardHeader(
      R"({"files":{"data":{"size":4,"offset":"0"},"alias":{"link":"data"},"cycle":{"link":"cycle"}}})",
      "test"));
  auto archive = GetOrCreateAsarArchive(Path());
  ExpectContents(archive, "alias", "test");
  EXPECT_FALSE(archive->CreateReader(base::FilePath::FromASCII("cycle")));
  EXPECT_FALSE(archive->CreateReader(base::FilePath::FromASCII("../data")));
}

TEST_F(ArchiveTest, UnpackedFileAndDirectoryLinksResolveToTheirActualPath) {
  ASSERT_TRUE(WriteStandardHeader(
      R"({"files":{"dir":{"files":{"data":{"size":4,"unpacked":true}}},"alias":{"link":"dir/data"},"diralias":{"link":"dir"}}})"));
  const auto directory =
      Path().AddExtension(FILE_PATH_LITERAL("unpacked")).AppendASCII("dir");
  ASSERT_TRUE(base::CreateDirectory(directory));
  ASSERT_TRUE(base::WriteFile(directory.AppendASCII("data"), "test"));
  auto archive = GetOrCreateAsarArchive(Path());
  ExpectContents(archive, "alias", "test");
  ExpectContents(archive, "diralias/data", "test");
  base::FilePath actual;
  ASSERT_TRUE(
      archive->GetUnpackedPath(base::FilePath::FromASCII("alias"), &actual));
  EXPECT_EQ(actual.NormalizePathSeparators(),
            directory.AppendASCII("data").NormalizePathSeparators());
}

TEST_F(ArchiveTest, ResolvePathCanonicalizesLinksAndRejectsInvalidTargets) {
  TestArchiveBuilder builder;
  ASSERT_TRUE(builder.Write(Path(),
                            {{"dir/data", "test"},
                             {"alias", "", false, false, "dir/data"},
                             {"chain", "", false, false, "alias"},
                             {"diralias", "", false, false, "dir"},
                             {"dirchain", "", false, false, "diralias"},
                             {"cycle", "", false, false, "cycle"},
                             {"broken", "", false, false, "missing"}},
                            false));
  auto archive = GetOrCreateAsarArchive(Path());
  ASSERT_TRUE(archive);
  base::FilePath actual;
  const auto expected = base::FilePath::FromASCII("dir").AppendASCII("data");
  for (const char* name :
       {"dir/data", "alias", "chain", "diralias/data", "dirchain/data"}) {
    ASSERT_TRUE(archive->ResolvePath(base::FilePath::FromASCII(name), &actual));
    EXPECT_EQ(actual, expected);
  }
  ASSERT_TRUE(
      archive->ResolvePath(base::FilePath::FromASCII("diralias"), &actual));
  EXPECT_EQ(actual, base::FilePath::FromASCII("dir"));
  ASSERT_TRUE(archive->ResolvePath(base::FilePath(), &actual));
  EXPECT_TRUE(actual.empty());
  for (const char* name : {"cycle", "broken", "missing", "../dir/data"}) {
    EXPECT_FALSE(
        archive->ResolvePath(base::FilePath::FromASCII(name), &actual));
  }
  EXPECT_FALSE(archive->ResolvePath(Path(), &actual));
}

}  // namespace
}  // namespace xenon::asar
