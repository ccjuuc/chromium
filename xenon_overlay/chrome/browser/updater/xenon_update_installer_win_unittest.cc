// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#include <memory>
#include <string_view>

#include "base/base_paths.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/scoped_path_override.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::updater {
namespace {

class XenonUpdateInstallerWinTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temporary_.CreateUniqueTempDir());
    installation_ = temporary_.GetPath().AppendASCII("installed");
    staged_ = installation_.AppendASCII("2.0.0.0");
    root_files_ = staged_.AppendASCII(".xenon-root");
    launcher_ = installation_.AppendASCII("browser.exe");
    ASSERT_TRUE(base::CreateDirectory(root_files_));
    Write(launcher_, "old launcher");
    file_exe_override_ = std::make_unique<base::ScopedPathOverride>(
        base::FILE_EXE, launcher_, /*is_absolute=*/true, /*create=*/false);
  }

  void Write(const base::FilePath& path, std::string_view contents) {
    ASSERT_TRUE(base::CreateDirectory(path.DirName()));
    ASSERT_TRUE(base::WriteFile(path, contents));
  }

  std::string Read(const base::FilePath& path) {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(path, &contents));
    return contents;
  }

  void StageLauncher() {
    Write(root_files_.AppendASCII("browser.exe"), "new launcher");
  }

  bool InstallWithoutRelaunch() {
    return XenonUpdateInstaller::InstallAndRelaunch(staged_, installation_,
                                                    base::FilePath());
  }

  base::ScopedTempDir temporary_;
  base::FilePath installation_;
  base::FilePath staged_;
  base::FilePath root_files_;
  base::FilePath launcher_;
  std::unique_ptr<base::ScopedPathOverride> file_exe_override_;
};

TEST_F(XenonUpdateInstallerWinTest, CommitsRootChangesAndDeletesObsoleteFiles) {
  StageLauncher();
  Write(installation_.AppendASCII("resource.dat"), "old resource");
  Write(installation_.AppendASCII("obsolete.dat"), "old obsolete");
  Write(root_files_.AppendASCII("resource.dat"), "new resource");
  Write(root_files_.AppendASCII("nested/addition.dat"), "new addition");
  Write(staged_.AppendASCII(".xenon-deletions.json"), "[\"obsolete.dat\"]");

  ASSERT_TRUE(InstallWithoutRelaunch());
  EXPECT_EQ(Read(launcher_), "new launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("resource.dat")), "new resource");
  EXPECT_EQ(Read(installation_.AppendASCII("nested/addition.dat")),
            "new addition");
  EXPECT_FALSE(base::PathExists(installation_.AppendASCII("obsolete.dat")));
}

TEST_F(XenonUpdateInstallerWinTest, LockedLauncherRollsBackOtherRootChanges) {
  StageLauncher();
  Write(installation_.AppendASCII("resource.dat"), "old resource");
  Write(installation_.AppendASCII("obsolete.dat"), "old obsolete");
  Write(root_files_.AppendASCII("resource.dat"), "new resource");
  Write(root_files_.AppendASCII("nested/addition.dat"), "new addition");
  Write(staged_.AppendASCII(".xenon-deletions.json"), "[\"obsolete.dat\"]");
  // No FLAG_WIN_SHARE_DELETE: moving this final commit item must fail after
  // earlier replacements, additions and deletions have already happened.
  base::File held_launcher(launcher_,
                           base::File::FLAG_OPEN | base::File::FLAG_READ);
  ASSERT_TRUE(held_launcher.IsValid());

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(Read(launcher_), "old launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("resource.dat")), "old resource");
  EXPECT_EQ(Read(installation_.AppendASCII("obsolete.dat")), "old obsolete");
  EXPECT_FALSE(base::PathExists(installation_.AppendASCII("nested")));
}

TEST_F(XenonUpdateInstallerWinTest, FailedRelaunchRollsBackCommittedFiles) {
  StageLauncher();  // Deliberately not a PE image; CreateProcess must fail.
  Write(installation_.AppendASCII("obsolete.dat"), "old obsolete");
  Write(staged_.AppendASCII(".xenon-deletions.json"), "[\"obsolete.dat\"]");

  EXPECT_FALSE(XenonUpdateInstaller::InstallAndRelaunch(staged_, installation_,
                                                        launcher_));
  EXPECT_EQ(Read(launcher_), "old launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("obsolete.dat")), "old obsolete");
}

TEST_F(XenonUpdateInstallerWinTest, FullPackageRollbackRemovesOnlyNewVersion) {
  const base::FilePath full = temporary_.GetPath().AppendASCII("full");
  Write(full.AppendASCII("3.0.0.0/browser.dll"), "new DLL");
  Write(full.AppendASCII("browser.exe"), "invalid PE image");
  Write(installation_.AppendASCII("1.0.0.0/browser.dll"), "old DLL");

  EXPECT_FALSE(
      XenonUpdateInstaller::InstallAndRelaunch(full, installation_, launcher_));
  EXPECT_EQ(Read(launcher_), "old launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("1.0.0.0/browser.dll")), "old DLL");
  EXPECT_FALSE(base::PathExists(installation_.AppendASCII("3.0.0.0")));
}

TEST_F(XenonUpdateInstallerWinTest,
       ExternalVersionPackageCommitsWithoutChangingInput) {
  const base::FilePath external =
      temporary_.GetPath().AppendASCII("download/versions/3.0.0.0");
  Write(external.AppendASCII("browser.dll"), "new DLL");
  Write(external.AppendASCII(".xenon-root/browser.exe"), "new launcher");
  Write(external.AppendASCII(".xenon-root/resource.dat"), "new resource");
  Write(external.AppendASCII(".xenon-deletions.json"), "[\"obsolete.dat\"]");
  Write(installation_.AppendASCII("obsolete.dat"), "old obsolete");

  ASSERT_TRUE(XenonUpdateInstaller::InstallAndRelaunch(external, installation_,
                                                       base::FilePath()));
  EXPECT_EQ(Read(launcher_), "new launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("resource.dat")), "new resource");
  EXPECT_EQ(Read(installation_.AppendASCII("3.0.0.0/browser.dll")), "new DLL");
  EXPECT_FALSE(base::PathExists(installation_.AppendASCII("obsolete.dat")));
  EXPECT_FALSE(
      base::PathExists(installation_.AppendASCII("3.0.0.0/.xenon-root")));
  EXPECT_FALSE(base::PathExists(
      installation_.AppendASCII("3.0.0.0/.xenon-deletions.json")));
  EXPECT_EQ(Read(external.AppendASCII("browser.dll")), "new DLL");
  EXPECT_EQ(Read(external.AppendASCII(".xenon-root/browser.exe")),
            "new launcher");
  EXPECT_EQ(Read(external.AppendASCII(".xenon-root/resource.dat")),
            "new resource");
  EXPECT_EQ(Read(external.AppendASCII(".xenon-deletions.json")),
            "[\"obsolete.dat\"]");
}

TEST_F(XenonUpdateInstallerWinTest,
       ExternalVersionFailurePreservesInputForRetry) {
  const base::FilePath external =
      temporary_.GetPath().AppendASCII("download/versions/3.0.0.0");
  Write(external.AppendASCII("browser.dll"), "new DLL");
  Write(external.AppendASCII(".xenon-root/browser.exe"), "new launcher");
  Write(external.AppendASCII(".xenon-root/nested/addition.dat"),
        "new addition");
  Write(external.AppendASCII(".xenon-deletions.json"), "[\"obsolete.dat\"]");
  Write(installation_.AppendASCII("obsolete.dat"), "old obsolete");
  base::File held_launcher(launcher_,
                           base::File::FLAG_OPEN | base::File::FLAG_READ);
  ASSERT_TRUE(held_launcher.IsValid());

  EXPECT_FALSE(XenonUpdateInstaller::InstallAndRelaunch(external, installation_,
                                                        base::FilePath()));
  EXPECT_EQ(Read(launcher_), "old launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("obsolete.dat")), "old obsolete");
  EXPECT_FALSE(base::PathExists(installation_.AppendASCII("3.0.0.0")));
  EXPECT_FALSE(base::PathExists(installation_.AppendASCII("nested")));
  EXPECT_EQ(Read(external.AppendASCII("browser.dll")), "new DLL");
  EXPECT_EQ(Read(external.AppendASCII(".xenon-root/browser.exe")),
            "new launcher");
  EXPECT_EQ(Read(external.AppendASCII(".xenon-root/nested/addition.dat")),
            "new addition");
  EXPECT_EQ(Read(external.AppendASCII(".xenon-deletions.json")),
            "[\"obsolete.dat\"]");
  held_launcher.Close();
  EXPECT_TRUE(XenonUpdateInstaller::InstallAndRelaunch(external, installation_,
                                                       base::FilePath()));
  EXPECT_EQ(Read(installation_.AppendASCII("3.0.0.0/browser.dll")), "new DLL");
}

TEST_F(XenonUpdateInstallerWinTest,
       ExternalVersionNeverOverwritesExistingVersion) {
  const base::FilePath external =
      temporary_.GetPath().AppendASCII("download/versions/3.0.0.0");
  Write(external.AppendASCII("browser.dll"), "new DLL");
  Write(external.AppendASCII(".xenon-root/browser.exe"), "new launcher");
  Write(installation_.AppendASCII("3.0.0.0/browser.dll"), "existing DLL");

  EXPECT_FALSE(XenonUpdateInstaller::InstallAndRelaunch(external, installation_,
                                                        base::FilePath()));
  EXPECT_EQ(Read(launcher_), "old launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("3.0.0.0/browser.dll")),
            "existing DLL");
  EXPECT_EQ(Read(external.AppendASCII("browser.dll")), "new DLL");
  EXPECT_EQ(Read(external.AppendASCII(".xenon-root/browser.exe")),
            "new launcher");
}

TEST_F(XenonUpdateInstallerWinTest,
       MissingLauncherDoesNotConsumeStaleNewFiles) {
  Write(installation_.AppendASCII("new_browser.exe"), "stale launcher");
  Write(root_files_.AppendASCII("resource.dat"), "new resource");

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(Read(launcher_), "old launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("new_browser.exe")),
            "stale launcher");
  EXPECT_FALSE(base::PathExists(installation_.AppendASCII("resource.dat")));
}

TEST_F(XenonUpdateInstallerWinTest, RejectsDeletionOutsideInstallation) {
  StageLauncher();
  Write(temporary_.GetPath().AppendASCII("outside.dat"), "keep outside");
  Write(staged_.AppendASCII(".xenon-deletions.json"), "[\"../outside.dat\"]");

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(Read(launcher_), "old launcher");
  EXPECT_EQ(Read(temporary_.GetPath().AppendASCII("outside.dat")),
            "keep outside");
}

TEST_F(XenonUpdateInstallerWinTest, RejectsDeletionOfVersionTreeOrDirectory) {
  StageLauncher();
  Write(installation_.AppendASCII("1.0.0.0/browser.dll"), "old DLL");
  Write(staged_.AppendASCII(".xenon-deletions.json"),
        "[\"1.0.0.0/browser.dll\"]");
  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(Read(installation_.AppendASCII("1.0.0.0/browser.dll")), "old DLL");

  Write(installation_.AppendASCII("assets/keep.dat"), "keep");
  Write(staged_.AppendASCII(".xenon-deletions.json"), "[\"assets\"]");
  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(Read(installation_.AppendASCII("assets/keep.dat")), "keep");
}

TEST_F(XenonUpdateInstallerWinTest,
       RejectsConflictingDeletionCaseInsensitively) {
  StageLauncher();
  Write(staged_.AppendASCII(".xenon-deletions.json"), "[\"BROWSER.EXE\"]");
  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(Read(launcher_), "old launcher");
}

TEST_F(XenonUpdateInstallerWinTest, AnotherTransactionHoldsExclusiveLock) {
  StageLauncher();
  base::File held_lock(installation_.AppendASCII(".xenon-install.lock"),
                       base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_READ |
                           base::File::FLAG_WRITE |
                           base::File::FLAG_WIN_EXCLUSIVE_READ |
                           base::File::FLAG_WIN_EXCLUSIVE_WRITE);
  ASSERT_TRUE(held_lock.IsValid());

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(Read(launcher_), "old launcher");
}

}  // namespace
}  // namespace xenon::updater
