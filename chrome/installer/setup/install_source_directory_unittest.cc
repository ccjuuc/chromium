// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/installer/setup/install_source_directory.h"

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/gmock_expected_support.h"
#include "base/test/test_file_util.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/installer/setup/install_worker.h"
#include "chrome/installer/setup/setup_constants.h"
#include "chrome/installer/util/work_item_list.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace installer {
namespace {

class InstallSourceDirectoryTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    source_ = temp_.GetPath().AppendASCII("source");
    target_ = temp_.GetPath().AppendASCII("Application");
    unpack_ = temp_.GetPath().AppendASCII("unpack");
    ASSERT_TRUE(base::CreateDirectory(target_));
    ASSERT_TRUE(base::CreateDirectory(unpack_));
    ASSERT_TRUE(base::CreateDirectory(VersionPath().AppendASCII("Installer")));
    ASSERT_TRUE(base::CreateDirectory(source_.AppendASCII("app/plugins")));
    ASSERT_TRUE(base::WriteFile(source_.Append(kChromeExe), "launcher"));
    ASSERT_TRUE(
        base::WriteFile(VersionPath().Append(kChromeDll), "browser DLL"));
    ASSERT_TRUE(base::WriteFile(
        VersionPath().AppendASCII("Installer/setup.exe"), "setup input"));
    ASSERT_TRUE(base::WriteFile(source_.AppendASCII("app/plugins/main.js"),
                                "application payload"));
  }

  base::FilePath VersionPath() const {
    return source_.AppendASCII(chrome::kChromeVersion);
  }

  base::CommandLine CommandLine() const {
    base::CommandLine command(base::CommandLine::NO_PROGRAM);
    command.AppendSwitchPath(switches::kInstallSourceDir, source_);
    command.AppendSwitchPath(switches::kInstallDirectory, target_);
    return command;
  }

  void WriteDeletions(std::string_view json) {
    ASSERT_TRUE(
        base::WriteFile(source_.AppendASCII(".xenon-deletions.json"), json));
  }

  void ExpectContents(const base::FilePath& path, std::string_view expected) {
    std::string actual;
    ASSERT_TRUE(base::ReadFileToString(path, &actual));
    EXPECT_EQ(expected, actual);
  }

  base::ScopedTempDir temp_;
  base::FilePath source_;
  base::FilePath target_;
  base::FilePath unpack_;
};

TEST_F(InstallSourceDirectoryTest, CopiesApplicationAndPreservesInput) {
  auto prepared =
      PrepareInstallSourceDirectory(CommandLine(), target_, unpack_);
  ASSERT_THAT(prepared, base::test::HasValue());
  EXPECT_TRUE(prepared->empty());
  const auto copied = unpack_.Append(kInstallSourceChromeDir);
  ExpectContents(copied.Append(kChromeExe), "launcher");
  ExpectContents(copied.AppendASCII(chrome::kChromeVersion).Append(kChromeDll),
                 "browser DLL");
  ExpectContents(copied.AppendASCII("app/plugins/main.js"),
                 "application payload");
  ExpectContents(copied.AppendASCII(chrome::kChromeVersion)
                     .AppendASCII("Installer/setup.exe"),
                 "setup input");

  // The native installer consumes its private copy. The caller's verified
  // input, including setup.exe, must remain available after that happens.
  ASSERT_TRUE(base::DeletePathRecursively(copied));
  ExpectContents(source_.Append(kChromeExe), "launcher");
  ExpectContents(VersionPath().Append(kChromeDll), "browser DLL");
  ExpectContents(VersionPath().AppendASCII("Installer/setup.exe"),
                 "setup input");
  EXPECT_TRUE(base::IsDirectoryEmpty(target_));
}

TEST_F(InstallSourceDirectoryTest, RejectsArchiveAndUninstallSwitches) {
  for (const char* option :
       {switches::kInstallArchive, switches::kUncompressedArchive,
        switches::kUninstall}) {
    auto command = CommandLine();
    command.AppendSwitch(option);  // Empty archive switches also conflict.
    EXPECT_THAT(PrepareInstallSourceDirectory(command, target_, unpack_),
                base::test::ErrorIs(UNSUPPORTED_OPTION));
  }
  EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
}

TEST_F(InstallSourceDirectoryTest, RequiresExplicitMatchingInstallDirectory) {
  auto command = CommandLine();
  command.RemoveSwitch(switches::kInstallDirectory);
  EXPECT_THAT(PrepareInstallSourceDirectory(command, target_, unpack_),
              base::test::ErrorIs(UNSUPPORTED_OPTION));
  const auto another = temp_.GetPath().AppendASCII("AnotherApplication");
  ASSERT_TRUE(base::CreateDirectory(another));
  command.AppendSwitchPath(switches::kInstallDirectory, another);
  EXPECT_THAT(PrepareInstallSourceDirectory(command, target_, unpack_),
              base::test::ErrorIs(UNSUPPORTED_OPTION));
  EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
}

TEST_F(InstallSourceDirectoryTest, RejectsEmptyAndInvalidSourceOrTarget) {
  for (const auto& invalid :
       {base::FilePath(), base::FilePath(FILE_PATH_LITERAL("relative")),
        temp_.GetPath().AppendASCII("missing"),
        target_.AppendASCII("../Application")}) {
    auto command = CommandLine();
    command.RemoveSwitch(switches::kInstallSourceDir);
    command.AppendSwitchPath(switches::kInstallSourceDir, invalid);
    EXPECT_THAT(PrepareInstallSourceDirectory(command, target_, unpack_),
                base::test::ErrorIs(UNSUPPORTED_OPTION));
    command = CommandLine();
    command.RemoveSwitch(switches::kInstallDirectory);
    command.AppendSwitchPath(switches::kInstallDirectory, invalid);
    EXPECT_THAT(PrepareInstallSourceDirectory(command, invalid, unpack_),
                base::test::ErrorIs(UNSUPPORTED_OPTION));
  }
  EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
}

TEST_F(InstallSourceDirectoryTest, RejectsEmptySource) {
  const auto empty = temp_.GetPath().AppendASCII("empty");
  ASSERT_TRUE(base::CreateDirectory(empty));
  auto command = CommandLine();
  command.RemoveSwitch(switches::kInstallSourceDir);
  command.AppendSwitchPath(switches::kInstallSourceDir, empty);
  EXPECT_THAT(PrepareInstallSourceDirectory(command, target_, unpack_),
              base::test::ErrorIs(INVALID_ARCHIVE));
  EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
}

TEST_F(InstallSourceDirectoryTest, RejectsSourceTargetAndScratchOverlap) {
  const auto child = target_.AppendASCII("child");
  ASSERT_TRUE(base::CreateDirectory(child));
  for (const auto& overlapping : {target_, target_.DirName(), child}) {
    auto command = CommandLine();
    command.RemoveSwitch(switches::kInstallSourceDir);
    command.AppendSwitchPath(switches::kInstallSourceDir, overlapping);
    EXPECT_THAT(PrepareInstallSourceDirectory(command, target_, unpack_),
                base::test::ErrorIs(UNSUPPORTED_OPTION));
  }
  EXPECT_THAT(PrepareInstallSourceDirectory(CommandLine(), target_, source_),
              base::test::ErrorIs(UNSUPPORTED_OPTION));
  EXPECT_THAT(PrepareInstallSourceDirectory(CommandLine(), target_, target_),
              base::test::ErrorIs(UNSUPPORTED_OPTION));
  EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
}

TEST_F(InstallSourceDirectoryTest, RejectsWrongOrAmbiguousVersion) {
  const std::string other =
      std::string(chrome::kChromeVersion) == "0.0.0.0" ? "0.0.0.1" : "0.0.0.0";
  const auto other_version = source_.AppendASCII(other);
  ASSERT_TRUE(base::CreateDirectory(other_version));
  EXPECT_THAT(PrepareInstallSourceDirectory(CommandLine(), target_, unpack_),
              base::test::ErrorIs(INVALID_ARCHIVE));
  ASSERT_TRUE(base::DeleteFile(other_version));
  ASSERT_TRUE(base::Move(VersionPath(), other_version));
  EXPECT_THAT(PrepareInstallSourceDirectory(CommandLine(), target_, unpack_),
              base::test::ErrorIs(INVALID_ARCHIVE));
  EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
}

TEST_F(InstallSourceDirectoryTest, RejectsMissingLauncherOrDll) {
  ASSERT_TRUE(base::DeleteFile(source_.Append(kChromeExe)));
  EXPECT_THAT(PrepareInstallSourceDirectory(CommandLine(), target_, unpack_),
              base::test::ErrorIs(INVALID_ARCHIVE));
  ASSERT_TRUE(base::WriteFile(source_.Append(kChromeExe), "launcher"));
  ASSERT_TRUE(base::WriteFile(VersionPath().Append(kChromeDll), ""));
  EXPECT_THAT(PrepareInstallSourceDirectory(CommandLine(), target_, unpack_),
              base::test::ErrorIs(INVALID_ARCHIVE));
  EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
}

TEST_F(InstallSourceDirectoryTest, DoesNotOverwriteExistingPrivateCopy) {
  const auto copied = unpack_.Append(kInstallSourceChromeDir);
  ASSERT_TRUE(base::CreateDirectory(copied));
  ASSERT_TRUE(base::WriteFile(copied.AppendASCII("sentinel"), "keep"));
  EXPECT_THAT(PrepareInstallSourceDirectory(CommandLine(), target_, unpack_),
              base::test::ErrorIs(UNPACKING_FAILED));
  ExpectContents(copied.AppendASCII("sentinel"), "keep");
}

TEST_F(InstallSourceDirectoryTest, RejectsEnumerationFailure) {
  const auto blocked = source_.AppendASCII("blocked");
  ASSERT_TRUE(base::CreateDirectory(blocked));
  ASSERT_TRUE(base::WriteFile(blocked.AppendASCII("hidden"), "keep"));
  base::FilePermissionRestorer restore(blocked);
  ASSERT_TRUE(base::DenyFilePermission(blocked, FILE_LIST_DIRECTORY));
  EXPECT_THAT(PrepareInstallSourceDirectory(CommandLine(), target_, unpack_),
              base::test::ErrorIs(INVALID_ARCHIVE));
  EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
}

TEST_F(InstallSourceDirectoryTest, RejectsSourceReparsePoint) {
  const auto outside = temp_.GetPath().AppendASCII("outside");
  ASSERT_TRUE(base::CreateDirectory(outside));
  ASSERT_TRUE(base::WriteFile(outside.AppendASCII("keep"), "keep"));
  const auto link = source_.AppendASCII("linked");
  if (!::CreateSymbolicLinkW(
          link.value().c_str(), outside.value().c_str(),
          SYMBOLIC_LINK_FLAG_DIRECTORY |
              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    if (::GetLastError() == ERROR_PRIVILEGE_NOT_HELD) {
      GTEST_SKIP() << "Symbolic-link privilege is unavailable";
    }
    FAIL() << "CreateSymbolicLinkW failed: " << ::GetLastError();
  }
  const auto result =
      PrepareInstallSourceDirectory(CommandLine(), target_, unpack_);
  ASSERT_TRUE(base::DeleteFile(link));
  EXPECT_THAT(result, base::test::ErrorIs(INVALID_ARCHIVE));
  ExpectContents(outside.AppendASCII("keep"), "keep");
  EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
}

TEST_F(InstallSourceDirectoryTest, RejectsTargetPayloadReparsePoint) {
  const auto outside = temp_.GetPath().AppendASCII("outside");
  ASSERT_TRUE(base::CreateDirectory(outside));
  const auto link = target_.AppendASCII("app");
  if (!::CreateSymbolicLinkW(
          link.value().c_str(), outside.value().c_str(),
          SYMBOLIC_LINK_FLAG_DIRECTORY |
              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    if (::GetLastError() == ERROR_PRIVILEGE_NOT_HELD) {
      GTEST_SKIP() << "Symbolic-link privilege is unavailable";
    }
    FAIL() << "CreateSymbolicLinkW failed: " << ::GetLastError();
  }
  const auto result =
      PrepareInstallSourceDirectory(CommandLine(), target_, unpack_);
  ASSERT_TRUE(base::DeleteFile(link));
  EXPECT_THAT(result, base::test::ErrorIs(INVALID_ARCHIVE));
  EXPECT_TRUE(base::IsDirectoryEmpty(outside));
}

TEST_F(InstallSourceDirectoryTest, StripsDeletionMetadataAndRollsBackRemovals) {
  ASSERT_TRUE(base::CreateDirectory(target_.AppendASCII("app")));
  ASSERT_TRUE(base::WriteFile(target_.AppendASCII("app/obsolete.js"), "old"));
  WriteDeletions(R"(["app/obsolete.js", "already-missing.txt"])");
  auto prepared =
      PrepareInstallSourceDirectory(CommandLine(), target_, unpack_);
  ASSERT_THAT(prepared, base::test::HasValue());
  ASSERT_EQ(prepared->size(), 2u);
  EXPECT_FALSE(base::PathExists(unpack_.Append(kInstallSourceChromeDir)
                                    .AppendASCII(".xenon-deletions.json")));
  EXPECT_TRUE(base::PathExists(source_.AppendASCII(".xenon-deletions.json")));
  const auto backup = temp_.GetPath().AppendASCII("backup");
  ASSERT_TRUE(base::CreateDirectory(backup));
  std::unique_ptr<WorkItemList> items(WorkItem::CreateWorkItemList());
  AddRootDeletionWorkItems(target_, backup, prepared.value(), items.get());
  ASSERT_TRUE(items->Do());
  EXPECT_FALSE(base::PathExists(target_.AppendASCII("app/obsolete.js")));
  items->Rollback();
  ExpectContents(target_.AppendASCII("app/obsolete.js"), "old");
  ExpectContents(source_.AppendASCII("app/plugins/main.js"),
                 "application payload");
}

TEST_F(InstallSourceDirectoryTest, RejectsUnsafeAndOverlappingDeletions) {
  for (const char* json : {
           R"({"not":"a list"})",
           R"([17])",
           R"(["../outside"])",
           R"(["C:/outside"])",
           R"(["app/../outside"])",
           R"(["file:stream"])",
           R"(["nul"])",
           R"(["app/obsolete.js "])",
           R"(["file\u0000ignored"])",
           R"([".xenon-install.lock"])",
           R"(["app/.xenon-recovery-file"])",
           R"(["1.2.3.4/file"])",
           R"(["app"])",
           R"(["app/plugins/main.js"])",
           R"(["missing", "MISSING"])",
           R"(["missing", "missing/child"])",
       }) {
    SCOPED_TRACE(json);
    WriteDeletions(json);
    EXPECT_THAT(PrepareInstallSourceDirectory(CommandLine(), target_, unpack_),
                base::test::ErrorIs(INVALID_ARCHIVE));
    EXPECT_TRUE(base::IsDirectoryEmpty(unpack_));
  }
}

TEST_F(InstallSourceDirectoryTest, RejectsDirectoryDeletionAtExecutionTime) {
  ASSERT_TRUE(base::WriteFile(target_.AppendASCII("obsolete"), "restore"));
  ASSERT_TRUE(base::CreateDirectory(target_.AppendASCII("keep")));
  ASSERT_TRUE(base::WriteFile(target_.AppendASCII("keep/sentinel"), "keep"));
  const auto backup = temp_.GetPath().AppendASCII("backup");
  ASSERT_TRUE(base::CreateDirectory(backup));
  std::unique_ptr<WorkItemList> items(WorkItem::CreateWorkItemList());
  const base::FilePath deletions[] = {
      base::FilePath(FILE_PATH_LITERAL("obsolete")),
      base::FilePath(FILE_PATH_LITERAL("keep"))};
  AddRootDeletionWorkItems(target_, backup, deletions, items.get());
  EXPECT_FALSE(items->Do());
  items->Rollback();
  ExpectContents(target_.AppendASCII("obsolete"), "restore");
  ExpectContents(target_.AppendASCII("keep/sentinel"), "keep");
}

}  // namespace
}  // namespace installer
