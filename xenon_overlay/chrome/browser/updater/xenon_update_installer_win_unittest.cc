// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#include <memory>
#include <string>
#include <string_view>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_path_override.h"
#include "base/test/test_reg_util_win.h"
#include "base/win/registry.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::updater {
namespace {

constexpr wchar_t kProductKey[] = L"Software\\browser";
constexpr wchar_t kUninstallKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\browser";

class XenonUpdateInstallerWinTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(
        registry_overrides_.OverrideRegistry(HKEY_LOCAL_MACHINE));
    ASSERT_NO_FATAL_FAILURE(
        registry_overrides_.OverrideRegistry(HKEY_CURRENT_USER));
    ASSERT_TRUE(temporary_.CreateUniqueTempDir());
    installation_ = temporary_.GetPath().AppendASCII("installed browser");
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

  void RegisterInstallation(HKEY hive,
                            const base::FilePath& installation,
                            std::string_view version) {
    const base::FilePath setup = installation.AppendASCII(version)
                                     .AppendASCII("Installer")
                                     .AppendASCII("setup.exe");
    Write(setup, "old setup");
    const std::wstring version_w = base::ASCIIToWide(version);
    const std::wstring system_level =
        hive == HKEY_LOCAL_MACHINE ? L" --system-level" : L"";
    const std::wstring uninstall_command =
        L"\"" + setup.value() + L"\" --uninstall" + system_level;
    const std::wstring cleanup_command =
        L"\"" + setup.value() +
        L"\" --cleanup-for-downgrade-version=$1 "
        L"--cleanup-for-downgrade-operation=$2" +
        system_level;
    base::win::RegKey product;
    ASSERT_EQ(ERROR_SUCCESS,
              product.Create(hive, kProductKey,
                             KEY_READ | KEY_WRITE | KEY_WOW64_32KEY));
    ASSERT_EQ(ERROR_SUCCESS, product.WriteValue(L"pv", version_w.c_str()));
    ASSERT_EQ(ERROR_SUCCESS,
              product.WriteValue(L"UninstallString", setup.value().c_str()));
    ASSERT_EQ(ERROR_SUCCESS, product.WriteValue(L"DowngradeCleanupCommand",
                                                cleanup_command.c_str()));
    base::win::RegKey uninstall;
    ASSERT_EQ(ERROR_SUCCESS,
              uninstall.Create(hive, kUninstallKey,
                               KEY_READ | KEY_WRITE | KEY_WOW64_32KEY));
    ASSERT_EQ(
        ERROR_SUCCESS,
        uninstall.WriteValue(L"InstallLocation", installation.value().c_str()));
    ASSERT_EQ(ERROR_SUCCESS,
              uninstall.WriteValue(L"Version", version_w.c_str()));
    ASSERT_EQ(ERROR_SUCCESS,
              uninstall.WriteValue(L"DisplayVersion", version_w.c_str()));
    ASSERT_EQ(ERROR_SUCCESS, uninstall.WriteValue(L"UninstallString",
                                                  uninstall_command.c_str()));
  }

  void ExpectRegistryString(HKEY hive,
                            const wchar_t* key_path,
                            const wchar_t* name,
                            const std::wstring& expected) {
    base::win::RegKey key;
    ASSERT_EQ(ERROR_SUCCESS,
              key.Open(hive, key_path, KEY_READ | KEY_WOW64_32KEY));
    std::wstring value;
    ASSERT_EQ(ERROR_SUCCESS, key.ReadValue(name, &value));
    EXPECT_EQ(value, expected);
  }

  void ExpectRegistration(HKEY hive,
                          const base::FilePath& installation,
                          std::string_view version) {
    const std::wstring version_w = base::ASCIIToWide(version);
    const std::wstring setup = installation.AppendASCII(version)
                                   .AppendASCII("Installer")
                                   .AppendASCII("setup.exe")
                                   .value();
    const std::wstring system_level =
        hive == HKEY_LOCAL_MACHINE ? L" --system-level" : L"";
    ExpectRegistryString(hive, kProductKey, L"pv", version_w);
    ExpectRegistryString(hive, kProductKey, L"UninstallString", setup);
    ExpectRegistryString(hive, kProductKey, L"DowngradeCleanupCommand",
                         L"\"" + setup +
                             L"\" --cleanup-for-downgrade-version=$1 "
                             L"--cleanup-for-downgrade-operation=$2" +
                             system_level);
    ExpectRegistryString(hive, kUninstallKey, L"Version", version_w);
    ExpectRegistryString(hive, kUninstallKey, L"DisplayVersion", version_w);
    ExpectRegistryString(hive, kUninstallKey, L"UninstallString",
                         L"\"" + setup + L"\" --uninstall" + system_level);
    ExpectRegistryString(hive, kUninstallKey, L"InstallLocation",
                         installation.value());
  }

  registry_util::RegistryOverrideManager registry_overrides_;
  base::ScopedTempDir temporary_;
  base::FilePath installation_;
  base::FilePath staged_;
  base::FilePath root_files_;
  base::FilePath launcher_;
  std::unique_ptr<base::ScopedPathOverride> file_exe_override_;
};

TEST_F(XenonUpdateInstallerWinTest, CommitsRootChangesAndDeletesObsoleteFiles) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageLauncher();
  Write(staged_.AppendASCII("Installer/setup.exe"), "new setup");
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
  ExpectRegistration(HKEY_CURRENT_USER, installation_, "2.0.0.0");
}

TEST_F(XenonUpdateInstallerWinTest, LockedLauncherRollsBackOtherRootChanges) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageLauncher();
  Write(staged_.AppendASCII("Installer/setup.exe"), "new setup");
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
  ExpectRegistration(HKEY_CURRENT_USER, installation_, "1.0.0.0");
}

TEST_F(XenonUpdateInstallerWinTest, FailedRelaunchRollsBackCommittedFiles) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageLauncher();  // Deliberately not a PE image; CreateProcess must fail.
  Write(staged_.AppendASCII("Installer/setup.exe"), "new setup");
  Write(installation_.AppendASCII("obsolete.dat"), "old obsolete");
  Write(staged_.AppendASCII(".xenon-deletions.json"), "[\"obsolete.dat\"]");

  EXPECT_FALSE(XenonUpdateInstaller::InstallAndRelaunch(staged_, installation_,
                                                        launcher_));
  EXPECT_EQ(Read(launcher_), "old launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("obsolete.dat")), "old obsolete");
  ExpectRegistration(HKEY_CURRENT_USER, installation_, "1.0.0.0");
}

TEST_F(XenonUpdateInstallerWinTest, FullPackageRollbackRemovesOnlyNewVersion) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  const base::FilePath full = temporary_.GetPath().AppendASCII("full");
  Write(full.AppendASCII("3.0.0.0/browser.dll"), "new DLL");
  Write(full.AppendASCII("3.0.0.0/Installer/setup.exe"), "new setup");
  Write(full.AppendASCII("browser.exe"), "invalid PE image");
  Write(installation_.AppendASCII("1.0.0.0/browser.dll"), "old DLL");

  EXPECT_FALSE(
      XenonUpdateInstaller::InstallAndRelaunch(full, installation_, launcher_));
  EXPECT_EQ(Read(launcher_), "old launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("1.0.0.0/browser.dll")), "old DLL");
  EXPECT_FALSE(base::PathExists(installation_.AppendASCII("3.0.0.0")));
  ExpectRegistration(HKEY_CURRENT_USER, installation_, "1.0.0.0");
}

TEST_F(XenonUpdateInstallerWinTest,
       ExternalVersionPackageCommitsWithoutChangingInput) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_LOCAL_MACHINE, installation_, "1.0.0.0"));
  const base::FilePath external =
      temporary_.GetPath().AppendASCII("download/versions/3.0.0.0");
  Write(external.AppendASCII("browser.dll"), "new DLL");
  Write(external.AppendASCII("Installer/setup.exe"), "new setup");
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
  EXPECT_EQ(Read(external.AppendASCII("Installer/setup.exe")), "new setup");
  ExpectRegistration(HKEY_LOCAL_MACHINE, installation_, "3.0.0.0");
}

TEST_F(XenonUpdateInstallerWinTest,
       FullPackageCommitUpdatesOnlyMatchingInstallation) {
  const base::FilePath other_installation =
      temporary_.GetPath().AppendASCII("other-installation");
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_LOCAL_MACHINE, other_installation, "4.0.0.0"));
  const base::FilePath full = temporary_.GetPath().AppendASCII("full");
  Write(full.AppendASCII("3.0.0.0/browser.dll"), "new DLL");
  Write(full.AppendASCII("3.0.0.0/Installer/setup.exe"), "new setup");
  Write(full.AppendASCII("browser.exe"), "new launcher");

  ASSERT_TRUE(XenonUpdateInstaller::InstallAndRelaunch(full, installation_,
                                                       base::FilePath()));
  EXPECT_EQ(Read(launcher_), "new launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("3.0.0.0/Installer/setup.exe")),
            "new setup");
  ExpectRegistration(HKEY_CURRENT_USER, installation_, "3.0.0.0");
  ExpectRegistration(HKEY_LOCAL_MACHINE, other_installation, "4.0.0.0");
}

TEST_F(XenonUpdateInstallerWinTest,
       MachineInstallationPreservesUninstallAndCleanupArguments) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_LOCAL_MACHINE, installation_, "1.0.0.0"));
  const base::FilePath old_setup = installation_.AppendASCII("1.0.0.0")
                                       .AppendASCII("Installer")
                                       .AppendASCII("setup.exe");
  const base::FilePath new_setup =
      staged_.AppendASCII("Installer").AppendASCII("setup.exe");
  constexpr wchar_t kUninstallArguments[] =
      L" --uninstall --system-level --msi --verbose-logging --chrome-beta "
      L"--channel=beta";
  const std::wstring uninstall_command =
      L"\"" + old_setup.value() + L"\"" + kUninstallArguments;
  const std::wstring cleanup_command =
      L"\"" + old_setup.value() +
      L"\" --cleanup-for-downgrade-version=$1 "
      L"--cleanup-for-downgrade-operation=$2 --system-level "
      L"--verbose-logging --chrome-beta --channel=beta";
  base::win::RegKey product;
  ASSERT_EQ(ERROR_SUCCESS,
            product.Open(HKEY_LOCAL_MACHINE, kProductKey,
                         KEY_READ | KEY_WRITE | KEY_WOW64_32KEY));
  ASSERT_EQ(ERROR_SUCCESS,
            product.WriteValue(L"UninstallArguments", kUninstallArguments));
  ASSERT_EQ(ERROR_SUCCESS, product.WriteValue(L"DowngradeCleanupCommand",
                                              cleanup_command.c_str()));
  base::win::RegKey uninstall;
  ASSERT_EQ(ERROR_SUCCESS,
            uninstall.Open(HKEY_LOCAL_MACHINE, kUninstallKey,
                           KEY_READ | KEY_WRITE | KEY_WOW64_32KEY));
  ASSERT_EQ(ERROR_SUCCESS, uninstall.WriteValue(L"UninstallString",
                                                uninstall_command.c_str()));
  StageLauncher();
  Write(new_setup, "new setup");

  ASSERT_TRUE(InstallWithoutRelaunch());
  ExpectRegistryString(HKEY_LOCAL_MACHINE, kProductKey, L"pv", L"2.0.0.0");
  ExpectRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"Version",
                       L"2.0.0.0");
  ExpectRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"DisplayVersion",
                       L"2.0.0.0");
  ExpectRegistryString(HKEY_LOCAL_MACHINE, kProductKey, L"UninstallString",
                       new_setup.value());
  ExpectRegistryString(HKEY_LOCAL_MACHINE, kProductKey, L"UninstallArguments",
                       kUninstallArguments);

  std::wstring updated_uninstall;
  ASSERT_EQ(ERROR_SUCCESS,
            uninstall.ReadValue(L"UninstallString", &updated_uninstall));
  const base::CommandLine parsed_uninstall =
      base::CommandLine::FromString(updated_uninstall);
  EXPECT_EQ(parsed_uninstall.GetProgram(), new_setup);
  EXPECT_EQ(parsed_uninstall.GetSwitches(),
            base::CommandLine::FromString(uninstall_command).GetSwitches());

  std::wstring updated_cleanup;
  ASSERT_EQ(ERROR_SUCCESS,
            product.ReadValue(L"DowngradeCleanupCommand", &updated_cleanup));
  const base::CommandLine parsed_cleanup =
      base::CommandLine::FromString(updated_cleanup);
  EXPECT_EQ(parsed_cleanup.GetProgram(), new_setup);
  EXPECT_EQ(parsed_cleanup.GetSwitches(),
            base::CommandLine::FromString(cleanup_command).GetSwitches());
}

TEST_F(XenonUpdateInstallerWinTest,
       UnregisteredInstallationDoesNotCreateProductKeys) {
  StageLauncher();
  Write(staged_.AppendASCII("Installer/setup.exe"), "new setup");

  ASSERT_TRUE(InstallWithoutRelaunch());
  for (HKEY hive : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
      base::win::RegKey key;
      EXPECT_EQ(ERROR_FILE_NOT_FOUND,
                key.Open(hive, kProductKey, KEY_READ | view));
      EXPECT_EQ(ERROR_FILE_NOT_FOUND,
                key.Open(hive, kUninstallKey, KEY_READ | view));
    }
  }
}

TEST_F(XenonUpdateInstallerWinTest,
       RegistrationWithoutInstallationPathIsNotOverwritten) {
  base::win::RegKey product;
  ASSERT_EQ(ERROR_SUCCESS,
            product.Create(HKEY_CURRENT_USER, kProductKey,
                           KEY_READ | KEY_WRITE | KEY_WOW64_32KEY));
  ASSERT_EQ(ERROR_SUCCESS, product.WriteValue(L"pv", L"4.0.0.0"));
  base::win::RegKey uninstall;
  ASSERT_EQ(ERROR_SUCCESS,
            uninstall.Create(HKEY_CURRENT_USER, kUninstallKey,
                             KEY_READ | KEY_WRITE | KEY_WOW64_32KEY));
  ASSERT_EQ(ERROR_SUCCESS, uninstall.WriteValue(L"Version", L"4.0.0.0"));
  ASSERT_EQ(ERROR_SUCCESS, uninstall.WriteValue(L"DisplayVersion", L"4.0.0.0"));
  StageLauncher();
  Write(staged_.AppendASCII("Installer/setup.exe"), "new setup");

  ASSERT_TRUE(InstallWithoutRelaunch());
  ExpectRegistryString(HKEY_CURRENT_USER, kProductKey, L"pv", L"4.0.0.0");
  ExpectRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"Version",
                       L"4.0.0.0");
  ExpectRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion",
                       L"4.0.0.0");
  EXPECT_FALSE(product.HasValue(L"UninstallString"));
  EXPECT_FALSE(product.HasValue(L"DowngradeCleanupCommand"));
  EXPECT_FALSE(uninstall.HasValue(L"UninstallString"));
}

TEST_F(XenonUpdateInstallerWinTest,
       UninstallEntryForAnotherInstallationIsNotOverwritten) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  const base::FilePath other_installation =
      temporary_.GetPath().AppendASCII("other browser");
  const std::wstring other_uninstall =
      L"\"" +
      other_installation.AppendASCII("4.0.0.0/Installer/setup.exe").value() +
      L"\" --uninstall";
  base::win::RegKey uninstall;
  ASSERT_EQ(ERROR_SUCCESS,
            uninstall.Open(HKEY_CURRENT_USER, kUninstallKey,
                           KEY_READ | KEY_WRITE | KEY_WOW64_32KEY));
  ASSERT_EQ(ERROR_SUCCESS,
            uninstall.WriteValue(L"InstallLocation",
                                 other_installation.value().c_str()));
  ASSERT_EQ(ERROR_SUCCESS,
            uninstall.WriteValue(L"UninstallString", other_uninstall.c_str()));
  ASSERT_EQ(ERROR_SUCCESS, uninstall.WriteValue(L"Version", L"4.0.0.0"));
  ASSERT_EQ(ERROR_SUCCESS, uninstall.WriteValue(L"DisplayVersion", L"4.0.0.0"));
  StageLauncher();
  Write(staged_.AppendASCII("Installer/setup.exe"), "new setup");

  ASSERT_TRUE(InstallWithoutRelaunch());
  ExpectRegistryString(HKEY_CURRENT_USER, kProductKey, L"pv", L"2.0.0.0");
  ExpectRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"Version",
                       L"4.0.0.0");
  ExpectRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"DisplayVersion",
                       L"4.0.0.0");
  ExpectRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"UninstallString",
                       other_uninstall);
  ExpectRegistryString(HKEY_CURRENT_USER, kUninstallKey, L"InstallLocation",
                       other_installation.value());
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
