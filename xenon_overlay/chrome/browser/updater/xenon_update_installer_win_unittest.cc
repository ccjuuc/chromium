// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#include <windows.h>

#include <memory>
#include <string>
#include <string_view>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/bind.h"
#include "base/test/scoped_path_override.h"
#include "base/test/test_reg_util_win.h"
#include "base/win/access_token.h"
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
    staged_ = temporary_.GetPath().AppendASCII("download/versions/2.0.0.0");
    root_files_ = staged_.AppendASCII(".xenon-root");
    launcher_ = installation_.AppendASCII("browser.exe");
    ASSERT_TRUE(base::CreateDirectory(root_files_));
    Write(launcher_, "old launcher");
    file_exe_override_ = std::make_unique<base::ScopedPathOverride>(
        base::FILE_EXE, launcher_, /*is_absolute=*/true, /*create=*/false);
    XenonUpdateInstaller::SetSetupRunnerForTesting(base::BindRepeating(
        &XenonUpdateInstallerWinTest::RunSetup, base::Unretained(this)));
  }

  void TearDown() override {
    XenonUpdateInstaller::SetSetupRunnerForTesting({});
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

  void StageUpdate() {
    StageLauncher();
    Write(staged_.AppendASCII("browser.dll"), "new DLL");
    Write(staged_.AppendASCII("Installer/setup.exe"), "new setup");
  }

  // Deliberately write distinguishable results: this fake models setup's
  // completion contract, not Chromium's real installation transaction.
  int RunSetup(const base::CommandLine& command) {
    ++setup_runs_;
    if (setup_observer_) {
      setup_observer_.Run(command);
    }
    if (setup_result_ != 0 || !produce_setup_output_) {
      return setup_result_;
    }
    Write(installation_.AppendASCII("2.0.0.0/browser.dll"),
          "setup-installed DLL");
    Write(launcher_, "setup-installed launcher");
    if (!omit_installed_setup_) {
      Write(installation_.AppendASCII("2.0.0.0/Installer/setup.exe"),
            "setup-installed setup");
    }
    if (leave_pending_launcher_) {
      Write(installation_.AppendASCII("new_chrome.exe"), "pending launcher");
    }
    return 0;
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
  base::RepeatingCallback<void(const base::CommandLine&)> setup_observer_;
  int setup_runs_ = 0;
  int setup_result_ = 0;
  bool produce_setup_output_ = true;
  bool omit_installed_setup_ = false;
  bool leave_pending_launcher_ = false;
};

TEST_F(XenonUpdateInstallerWinTest,
       DifferentialHandoffUsesNewSetupAndLeavesRegistrationToSetup) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageUpdate();
  Write(root_files_.AppendASCII("nested/resource.dat"), "new resource");
  Write(installation_.AppendASCII("obsolete.dat"), "old obsolete");
  Write(staged_.AppendASCII(".xenon-deletions.json"), "[\"obsolete.dat\"]");
  base::FilePath source;
  base::FilePath setup;
  setup_observer_ = base::BindLambdaForTesting([&](const base::CommandLine&
                                                       command) {
    source = command.GetSwitchValuePath("install-source-dir");
    setup = command.GetProgram();
    EXPECT_EQ(source.BaseName().MaybeAsASCII(), "Chrome-bin");
    EXPECT_NE(source, staged_);
    EXPECT_FALSE(installation_.IsParent(source));
    EXPECT_EQ(command.GetSwitchValuePath("install-directory"), installation_);
    EXPECT_TRUE(command.HasSwitch("silent"));
    EXPECT_TRUE(command.HasSwitch("do-not-launch-chrome"));
    EXPECT_TRUE(command.HasSwitch("do-not-register-for-update-launch"));
    EXPECT_FALSE(command.HasSwitch("system-level"));
    EXPECT_FALSE(command.HasSwitch("uninstall"));
    EXPECT_FALSE(source.IsParent(setup));
    EXPECT_EQ(Read(setup), "new setup");
    EXPECT_EQ(Read(source.AppendASCII("browser.exe")), "new launcher");
    EXPECT_EQ(Read(source.AppendASCII("2.0.0.0/browser.dll")), "new DLL");
    EXPECT_EQ(Read(source.AppendASCII("2.0.0.0/Installer/setup.exe")),
              "new setup");
    EXPECT_EQ(Read(source.AppendASCII("nested/resource.dat")), "new resource");
    EXPECT_EQ(Read(source.AppendASCII(".xenon-deletions.json")),
              "[\"obsolete.dat\"]");
    EXPECT_FALSE(base::PathExists(source.AppendASCII("2.0.0.0/.xenon-root")));
    EXPECT_EQ(Read(launcher_), "old launcher");
    EXPECT_EQ(Read(installation_.AppendASCII("obsolete.dat")), "old obsolete");
    EXPECT_FALSE(base::PathExists(installation_.AppendASCII("2.0.0.0")));
  });

  ASSERT_TRUE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 1);
  EXPECT_EQ(Read(launcher_), "setup-installed launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("2.0.0.0/browser.dll")),
            "setup-installed DLL");
  // The updater must not perform setup's deletion or registration work itself.
  EXPECT_EQ(Read(installation_.AppendASCII("obsolete.dat")), "old obsolete");
  ExpectRegistration(HKEY_CURRENT_USER, installation_, "1.0.0.0");
  EXPECT_EQ(Read(staged_.AppendASCII("browser.dll")), "new DLL");
  EXPECT_EQ(Read(staged_.AppendASCII("Installer/setup.exe")), "new setup");
  EXPECT_EQ(Read(root_files_.AppendASCII("nested/resource.dat")),
            "new resource");
  EXPECT_EQ(Read(staged_.AppendASCII(".xenon-deletions.json")),
            "[\"obsolete.dat\"]");
  EXPECT_FALSE(base::PathExists(source));
  EXPECT_FALSE(base::PathExists(setup));
  base::FileEnumerator recoveries(installation_, false,
                                  base::FileEnumerator::DIRECTORIES,
                                  FILE_PATH_LITERAL(".xenon-recovery-*"));
  EXPECT_TRUE(recoveries.Next().empty());
}

TEST_F(XenonUpdateInstallerWinTest,
       FullPackageHandoffUsesCompleteApplicationTree) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  const base::FilePath full = temporary_.GetPath().AppendASCII("full");
  Write(full.AppendASCII("browser.exe"), "full launcher");
  Write(full.AppendASCII("2.0.0.0/browser.dll"), "full DLL");
  Write(full.AppendASCII("2.0.0.0/Installer/setup.exe"), "full setup");
  Write(full.AppendASCII("assets/plugin.js"), "full plugin");
  setup_observer_ =
      base::BindLambdaForTesting([&](const base::CommandLine& command) {
        const auto source = command.GetSwitchValuePath("install-source-dir");
        EXPECT_EQ(Read(command.GetProgram()), "full setup");
        EXPECT_EQ(Read(source.AppendASCII("browser.exe")), "full launcher");
        EXPECT_EQ(Read(source.AppendASCII("2.0.0.0/browser.dll")), "full DLL");
        EXPECT_EQ(Read(source.AppendASCII("assets/plugin.js")), "full plugin");
      });

  EXPECT_TRUE(
      XenonUpdateInstaller::InstallAndRelaunch(full, installation_, {}));
  EXPECT_EQ(setup_runs_, 1);
  EXPECT_EQ(Read(full.AppendASCII("assets/plugin.js")), "full plugin");
  ExpectRegistration(HKEY_CURRENT_USER, installation_, "1.0.0.0");
}

TEST_F(XenonUpdateInstallerWinTest, FullPackageAcceptsChromeBinContainer) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  const base::FilePath full = temporary_.GetPath().AppendASCII("full");
  Write(full.AppendASCII("Chrome-bin/browser.exe"), "full launcher");
  Write(full.AppendASCII("Chrome-bin/2.0.0.0/browser.dll"), "full DLL");
  Write(full.AppendASCII("Chrome-bin/2.0.0.0/Installer/setup.exe"),
        "full setup");
  setup_observer_ =
      base::BindLambdaForTesting([&](const base::CommandLine& command) {
        const auto source = command.GetSwitchValuePath("install-source-dir");
        EXPECT_EQ(Read(source.AppendASCII("browser.exe")), "full launcher");
        EXPECT_FALSE(base::PathExists(source.AppendASCII("Chrome-bin")));
      });

  EXPECT_TRUE(
      XenonUpdateInstaller::InstallAndRelaunch(full, installation_, {}));
  EXPECT_EQ(setup_runs_, 1);
}

TEST_F(XenonUpdateInstallerWinTest, RejectsInPlaceStagingWithoutChangingInput) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  staged_ = installation_.AppendASCII("2.0.0.0");
  root_files_ = staged_.AppendASCII(".xenon-root");
  StageUpdate();

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 0);
  EXPECT_EQ(Read(staged_.AppendASCII("browser.dll")), "new DLL");
  EXPECT_EQ(Read(staged_.AppendASCII("Installer/setup.exe")), "new setup");
  EXPECT_EQ(Read(root_files_.AppendASCII("browser.exe")), "new launcher");
  EXPECT_EQ(Read(launcher_), "old launcher");
  ExpectRegistration(HKEY_CURRENT_USER, installation_, "1.0.0.0");
}

TEST_F(XenonUpdateInstallerWinTest,
       MachineHandoffPreservesOnlySupportedInstallationArguments) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_LOCAL_MACHINE, installation_, "1.0.0.0"));
  StageUpdate();
  const std::wstring uninstall_command =
      L"\"" + installation_.AppendASCII("1.0.0.0/Installer/setup.exe").value() +
      L"\" --uninstall --system-level --msi --chrome-beta --channel=beta "
      L"--delete-profile --arbitrary-option=ignored";
  base::win::RegKey uninstall(HKEY_LOCAL_MACHINE, kUninstallKey,
                              KEY_READ | KEY_WRITE | KEY_WOW64_32KEY);
  ASSERT_EQ(ERROR_SUCCESS, uninstall.WriteValue(L"UninstallString",
                                                uninstall_command.c_str()));
  const auto token = base::win::AccessToken::FromCurrentProcess();
  ASSERT_TRUE(token);
  if (!token->IsElevated()) {
    // Exit-time installation cannot show a UAC prompt. It must refuse before
    // handing a machine installation to even the injected setup runner.
    std::string error;
    EXPECT_FALSE(XenonUpdateInstaller::InstallAndRelaunch(
        staged_, installation_, {}, &error));
    EXPECT_EQ(setup_runs_, 0);
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Read(launcher_), "old launcher");
    ExpectRegistryString(HKEY_LOCAL_MACHINE, kProductKey, L"pv", L"1.0.0.0");
    return;
  }
  setup_observer_ =
      base::BindLambdaForTesting([&](const base::CommandLine& command) {
        EXPECT_TRUE(command.HasSwitch("system-level"));
        EXPECT_TRUE(command.HasSwitch("msi"));
        EXPECT_TRUE(command.HasSwitch("chrome-beta"));
        EXPECT_EQ(command.GetSwitchValueASCII("channel"), "beta");
        EXPECT_FALSE(command.HasSwitch("uninstall"));
        EXPECT_FALSE(command.HasSwitch("delete-profile"));
        EXPECT_FALSE(command.HasSwitch("arbitrary-option"));
      });

  EXPECT_TRUE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 1);
  ExpectRegistryString(HKEY_LOCAL_MACHINE, kProductKey, L"pv", L"1.0.0.0");
  ExpectRegistryString(HKEY_LOCAL_MACHINE, kUninstallKey, L"UninstallString",
                       uninstall_command);
}

TEST_F(XenonUpdateInstallerWinTest, PreservesDevAndSxsInstallModes) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageUpdate();
  setup_result_ = 7;
  for (const char* mode : {"chrome-dev", "chrome-sxs"}) {
    const std::wstring uninstall_command =
        L"\"" +
        installation_.AppendASCII("1.0.0.0/Installer/setup.exe").value() +
        L"\" --uninstall --" + base::ASCIIToWide(mode);
    base::win::RegKey uninstall(HKEY_CURRENT_USER, kUninstallKey,
                                KEY_READ | KEY_WRITE | KEY_WOW64_32KEY);
    ASSERT_EQ(ERROR_SUCCESS, uninstall.WriteValue(L"UninstallString",
                                                  uninstall_command.c_str()));
    setup_observer_ =
        base::BindLambdaForTesting([&](const base::CommandLine& command) {
          EXPECT_TRUE(command.HasSwitch(mode));
          EXPECT_FALSE(command.HasSwitch("system-level"));
        });
    EXPECT_FALSE(InstallWithoutRelaunch());
  }
  EXPECT_EQ(setup_runs_, 2);
}

TEST_F(XenonUpdateInstallerWinTest,
       UserHandoffIgnoresOtherMachineInstallation) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  ASSERT_NO_FATAL_FAILURE(RegisterInstallation(
      HKEY_LOCAL_MACHINE, temporary_.GetPath().AppendASCII("other"),
      "4.0.0.0"));
  StageUpdate();
  setup_observer_ =
      base::BindLambdaForTesting([&](const base::CommandLine& command) {
        EXPECT_FALSE(command.HasSwitch("system-level"));
      });

  EXPECT_TRUE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 1);
}

TEST_F(XenonUpdateInstallerWinTest, RequiresMatchingInstallationRegistration) {
  ASSERT_NO_FATAL_FAILURE(RegisterInstallation(
      HKEY_CURRENT_USER, temporary_.GetPath().AppendASCII("other"), "1.0.0.0"));
  StageUpdate();

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 0);
  EXPECT_EQ(Read(launcher_), "old launcher");
}

TEST_F(XenonUpdateInstallerWinTest, RejectsAmbiguousUserAndMachineScopes) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_LOCAL_MACHINE, installation_, "1.0.0.0"));
  StageUpdate();

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 0);
}

TEST_F(XenonUpdateInstallerWinTest, MissingNewSetupNeverRunsOldSetup) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageUpdate();
  ASSERT_TRUE(base::DeleteFile(staged_.AppendASCII("Installer/setup.exe")));

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 0);
  EXPECT_EQ(Read(launcher_), "old launcher");
}

TEST_F(XenonUpdateInstallerWinTest,
       MissingSourceLauncherDoesNotUseInstalledOne) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageUpdate();
  ASSERT_TRUE(base::DeleteFile(root_files_.AppendASCII("browser.exe")));

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 0);
  EXPECT_EQ(Read(launcher_), "old launcher");
}

TEST_F(XenonUpdateInstallerWinTest, SetupFailuresAreRetryableAndPreserveInput) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageUpdate();
  for (const int result : {-1, 7}) {
    setup_result_ = result;
    std::string error;
    EXPECT_FALSE(XenonUpdateInstaller::InstallAndRelaunch(
        staged_, installation_, {}, &error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Read(launcher_), "old launcher");
    EXPECT_EQ(Read(root_files_.AppendASCII("browser.exe")), "new launcher");
    EXPECT_EQ(Read(staged_.AppendASCII("Installer/setup.exe")), "new setup");
    ExpectRegistration(HKEY_CURRENT_USER, installation_, "1.0.0.0");
  }
  EXPECT_EQ(setup_runs_, 2);
  setup_result_ = 0;
  EXPECT_TRUE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 3);
}

TEST_F(XenonUpdateInstallerWinTest, ZeroSetupExitRequiresInstalledVersion) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageUpdate();
  produce_setup_output_ = false;

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 1);
  EXPECT_EQ(Read(launcher_), "old launcher");
}

TEST_F(XenonUpdateInstallerWinTest, ZeroSetupExitRequiresInstalledSetup) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageUpdate();
  omit_installed_setup_ = true;

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 1);
  EXPECT_TRUE(base::DirectoryExists(installation_.AppendASCII("2.0.0.0")));
}

TEST_F(XenonUpdateInstallerWinTest, PendingLauncherAfterSetupPreventsRelaunch) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageUpdate();
  leave_pending_launcher_ = true;

  EXPECT_FALSE(InstallWithoutRelaunch());
  EXPECT_EQ(setup_runs_, 1);
  EXPECT_TRUE(base::PathExists(installation_.AppendASCII("new_chrome.exe")));
}

TEST_F(XenonUpdateInstallerWinTest,
       FailedRelaunchDoesNotUndoSetupInstallation) {
  ASSERT_NO_FATAL_FAILURE(
      RegisterInstallation(HKEY_CURRENT_USER, installation_, "1.0.0.0"));
  StageUpdate();
  std::string error;
  // Fake setup's launcher is not a PE image, so CreateProcess must fail.
  EXPECT_FALSE(XenonUpdateInstaller::InstallAndRelaunch(staged_, installation_,
                                                        launcher_, &error));
  EXPECT_EQ(setup_runs_, 1);
  EXPECT_EQ(Read(launcher_), "setup-installed launcher");
  EXPECT_EQ(Read(installation_.AppendASCII("2.0.0.0/browser.dll")),
            "setup-installed DLL");
  EXPECT_FALSE(error.empty());
}

TEST_F(XenonUpdateInstallerWinTest,
       RecoveryCleanupKeepsCommitMarkerUntilLockedBackupCanBeDeleted) {
  ASSERT_TRUE(base::CreateDirectory(installation_.AppendASCII("2.0.0.0")));
  const base::FilePath recovery =
      installation_.AppendASCII(".xenon-recovery-locked");
  const base::FilePath marker = recovery.AppendASCII("committed");
  const base::FilePath backup = recovery.AppendASCII("backup/browser.exe");
  Write(marker, "2.0.0.0");
  Write(backup, "old launcher");
  Write(recovery.AppendASCII("backup/resource.dat"), "old resource");
  // Model an image still held by the old browser: cleanup must be retryable
  // even if other files in this transaction can already be removed.
  base::File held_backup(backup, base::File::FLAG_OPEN | base::File::FLAG_READ);
  ASSERT_TRUE(held_backup.IsValid());

  for (int attempt = 0; attempt < 2; ++attempt) {
    XenonUpdateInstaller::CleanupRecoveryDirectories(installation_, "2.0.0.0");
    EXPECT_EQ(Read(marker), "2.0.0.0");
    EXPECT_EQ(Read(backup), "old launcher");
  }

  held_backup.Close();
  XenonUpdateInstaller::CleanupRecoveryDirectories(installation_, "2.0.0.0");
  EXPECT_FALSE(base::PathExists(recovery));
  EXPECT_EQ(Read(launcher_), "old launcher");
}

TEST_F(XenonUpdateInstallerWinTest,
       RecoveryCleanupPreservesUncommittedInvalidAndFutureTransactions) {
  ASSERT_TRUE(base::CreateDirectory(installation_.AppendASCII("2.0.0.0")));
  const base::FilePath uncommitted =
      installation_.AppendASCII(".xenon-recovery-uncommitted");
  const base::FilePath invalid =
      installation_.AppendASCII(".xenon-recovery-invalid");
  const base::FilePath future =
      installation_.AppendASCII(".xenon-recovery-future");
  const base::FilePath older =
      installation_.AppendASCII(".xenon-recovery-older");
  const base::FilePath unrelated =
      installation_.AppendASCII("unrelated-backup");
  // Failed rollback and interrupted installation deliberately have no marker.
  Write(uncommitted.AppendASCII("backup/browser.exe"), "recovery launcher");
  Write(invalid.AppendASCII("committed"), "not a version");
  Write(invalid.AppendASCII("backup/browser.exe"), "invalid launcher");
  Write(future.AppendASCII("committed"), "3.0.0.0");
  Write(future.AppendASCII("backup/browser.exe"), "future launcher");
  Write(older.AppendASCII("committed"), "1.0.0.0");
  Write(older.AppendASCII("backup/browser.exe"), "older launcher");
  Write(unrelated.AppendASCII("committed"), "1.0.0.0");
  Write(unrelated.AppendASCII("backup/browser.exe"), "unrelated launcher");

  XenonUpdateInstaller::CleanupRecoveryDirectories(installation_, "2.0.0.0");

  EXPECT_EQ(Read(uncommitted.AppendASCII("backup/browser.exe")),
            "recovery launcher");
  EXPECT_EQ(Read(invalid.AppendASCII("committed")), "not a version");
  EXPECT_EQ(Read(invalid.AppendASCII("backup/browser.exe")),
            "invalid launcher");
  EXPECT_EQ(Read(future.AppendASCII("committed")), "3.0.0.0");
  EXPECT_EQ(Read(future.AppendASCII("backup/browser.exe")), "future launcher");
  EXPECT_FALSE(base::PathExists(older));
  EXPECT_EQ(Read(unrelated.AppendASCII("backup/browser.exe")),
            "unrelated launcher");
}

TEST_F(XenonUpdateInstallerWinTest,
       RecoveryCleanupWaitsForActiveInstallationTransaction) {
  ASSERT_TRUE(base::CreateDirectory(installation_.AppendASCII("2.0.0.0")));
  const base::FilePath recovery =
      installation_.AppendASCII(".xenon-recovery-committed");
  Write(recovery.AppendASCII("committed"), "2.0.0.0");
  Write(recovery.AppendASCII("backup/browser.exe"), "old launcher");
  base::File held_lock(installation_.AppendASCII(".xenon-install.lock"),
                       base::File::FLAG_CREATE_ALWAYS | base::File::FLAG_READ |
                           base::File::FLAG_WRITE |
                           base::File::FLAG_WIN_EXCLUSIVE_READ |
                           base::File::FLAG_WIN_EXCLUSIVE_WRITE);
  ASSERT_TRUE(held_lock.IsValid());

  XenonUpdateInstaller::CleanupRecoveryDirectories(installation_, "2.0.0.0");
  EXPECT_EQ(Read(recovery.AppendASCII("committed")), "2.0.0.0");
  EXPECT_EQ(Read(recovery.AppendASCII("backup/browser.exe")), "old launcher");

  held_lock.Close();
  XenonUpdateInstaller::CleanupRecoveryDirectories(installation_, "2.0.0.0");
  EXPECT_FALSE(base::PathExists(recovery));
}

TEST_F(XenonUpdateInstallerWinTest,
       RecoveryCleanupRequiresValidRunningVersion) {
  ASSERT_TRUE(base::CreateDirectory(installation_.AppendASCII("2.0.0.0")));
  const base::FilePath recovery =
      installation_.AppendASCII(".xenon-recovery-committed");
  Write(recovery.AppendASCII("committed"), "2.0.0.0");
  Write(recovery.AppendASCII("backup/browser.exe"), "old launcher");

  XenonUpdateInstaller::CleanupRecoveryDirectories(installation_, "");
  XenonUpdateInstaller::CleanupRecoveryDirectories(installation_, "invalid");

  EXPECT_EQ(Read(recovery.AppendASCII("committed")), "2.0.0.0");
  EXPECT_EQ(Read(recovery.AppendASCII("backup/browser.exe")), "old launcher");
}

TEST_F(XenonUpdateInstallerWinTest,
       RecoveryCleanupDoesNotFollowRecoveryDirectoryLink) {
  ASSERT_TRUE(base::CreateDirectory(installation_.AppendASCII("2.0.0.0")));
  const base::FilePath outside = temporary_.GetPath().AppendASCII("outside");
  const base::FilePath link =
      installation_.AppendASCII(".xenon-recovery-linked");
  Write(outside.AppendASCII("committed"), "2.0.0.0");
  Write(outside.AppendASCII("backup/browser.exe"), "outside launcher");
  if (!::CreateSymbolicLinkW(
          link.value().c_str(), outside.value().c_str(),
          SYMBOLIC_LINK_FLAG_DIRECTORY |
              SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD) {
      GTEST_SKIP() << "Creating symbolic links requires developer mode or "
                      "SeCreateSymbolicLinkPrivilege";
    }
    FAIL() << "CreateSymbolicLinkW failed: " << error;
  }

  XenonUpdateInstaller::CleanupRecoveryDirectories(installation_, "2.0.0.0");

  EXPECT_TRUE(base::DirectoryExists(link));
  EXPECT_EQ(Read(outside.AppendASCII("committed")), "2.0.0.0");
  EXPECT_EQ(Read(outside.AppendASCII("backup/browser.exe")),
            "outside launcher");
  EXPECT_TRUE(::RemoveDirectoryW(link.value().c_str()));
}

TEST_F(XenonUpdateInstallerWinTest,
       RecoveryCleanupDoesNotFollowCommitMarkerLink) {
  ASSERT_TRUE(base::CreateDirectory(installation_.AppendASCII("2.0.0.0")));
  const base::FilePath outside =
      temporary_.GetPath().AppendASCII("outside-marker");
  const base::FilePath recovery =
      installation_.AppendASCII(".xenon-recovery-linked-marker");
  const base::FilePath marker = recovery.AppendASCII("committed");
  Write(outside, "2.0.0.0");
  Write(recovery.AppendASCII("backup/browser.exe"), "old launcher");
  if (!::CreateSymbolicLinkW(marker.value().c_str(), outside.value().c_str(),
                             SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_PRIVILEGE_NOT_HELD) {
      GTEST_SKIP() << "Creating symbolic links requires developer mode or "
                      "SeCreateSymbolicLinkPrivilege";
    }
    FAIL() << "CreateSymbolicLinkW failed: " << error;
  }

  XenonUpdateInstaller::CleanupRecoveryDirectories(installation_, "2.0.0.0");

  EXPECT_TRUE(base::PathExists(marker));
  EXPECT_EQ(Read(outside), "2.0.0.0");
  EXPECT_EQ(Read(recovery.AppendASCII("backup/browser.exe")), "old launcher");
  EXPECT_TRUE(::DeleteFileW(marker.value().c_str()));
}

}  // namespace
}  // namespace xenon::updater
