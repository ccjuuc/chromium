// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import <Foundation/Foundation.h>
#include <sys/stat.h>

#include <string>

#include "base/apple/foundation_util.h"
#include "base/command_line.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/launch.h"
#include "base/strings/sys_string_conversions.h"
#include "base/version.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

namespace xenon::updater {
namespace {

// These tests deliberately require release-signed fixtures. Ad-hoc or mocked
// signatures cannot exercise the installer's production trust checks.
constexpr char kOldAppEnvironment[] = "XENON_MAC_UPDATE_OLD_APP";
constexpr char kNewAppEnvironment[] = "XENON_MAC_UPDATE_NEW_APP";
constexpr char kDeveloperIdRequirement[] =
    "anchor apple generic and certificate "
    "1[field.1.2.840.113635.100.6.2.6] exists and certificate "
    "leaf[field.1.2.840.113635.100.6.1.13] exists";

testing::AssertionResult Run(const base::CommandLine& command) {
  std::string output;
  if (!base::GetAppOutputAndError(command, &output)) {
    return testing::AssertionFailure() << command.GetCommandLineString() << "\n"
                                       << output;
  }
  return testing::AssertionSuccess();
}

testing::AssertionResult VerifySignature(const base::FilePath& bundle) {
  base::CommandLine command(base::FilePath("/usr/bin/codesign"));
  command.AppendArg("--verify");
  command.AppendArg("--deep");
  command.AppendArg("--strict");
  command.AppendArg("--all-architectures");
  command.AppendArg("-R");
  command.AppendArg(kDeveloperIdRequirement);
  command.AppendArgPath(bundle);
  return Run(command);
}

testing::AssertionResult CopyBundle(const base::FilePath& from,
                                    const base::FilePath& to) {
  base::CommandLine command(base::FilePath("/usr/bin/ditto"));
  command.AppendArgPath(from);
  command.AppendArgPath(to);
  return Run(command);
}

std::string ReadBundleString(const base::FilePath& bundle, NSString* key) {
  @autoreleasepool {
    NSDictionary* plist = [NSDictionary
        dictionaryWithContentsOfURL:base::apple::FilePathToNSURL(
                                        bundle.AppendASCII(
                                            "Contents/Info.plist"))
                              error:nil];
    NSString* value = base::apple::ObjCCast<NSString>(plist[key]);
    return value ? base::SysNSStringToUTF8(value) : std::string();
  }
}

enum class FullPackage { kZip, kDmg };

class ScopedReadOnlyDirectory {
 public:
  explicit ScopedReadOnlyDirectory(const base::FilePath& path) : path_(path) {}
  ~ScopedReadOnlyDirectory() {
    if (restore_) {
      base::SetPosixFilePermissions(path_, original_mode_);
    }
  }

  bool Protect() {
    if (!base::GetPosixFilePermissions(path_, &original_mode_)) {
      return false;
    }
    restore_ = true;
    return base::SetPosixFilePermissions(path_, 0555);
  }

 private:
  const base::FilePath path_;
  int original_mode_ = 0;
  bool restore_ = false;
};

// Protect a private fixture file without modifying its signed contents. This
// makes the real outer-app rsync fail after the versioned framework was copied.
// Always restore flags before ScopedTempDir tries to delete the test directory.
class ScopedImmutableFile {
 public:
  explicit ScopedImmutableFile(const base::FilePath& path) : path_(path) {}
  ~ScopedImmutableFile() { Restore(); }

  bool Protect() {
    struct stat info;
    if (lstat(path_.value().c_str(), &info) != 0 || !S_ISREG(info.st_mode)) {
      return false;
    }
    original_flags_ = info.st_flags;
    protected_ =
        chflags(path_.value().c_str(), original_flags_ | UF_IMMUTABLE) == 0;
    return protected_;
  }

  bool Restore() {
    if (protected_ && chflags(path_.value().c_str(), original_flags_) != 0) {
      return false;
    }
    protected_ = false;
    return true;
  }

 private:
  const base::FilePath path_;
  unsigned long original_flags_ = 0;
  bool protected_ = false;
};

class XenonMacInstallerIntegrationTest
    : public testing::TestWithParam<FullPackage> {
 protected:
  void SetUp() override {
    base::Environment environment;
    const auto old_app = environment.GetVar(kOldAppEnvironment);
    const auto new_app = environment.GetVar(kNewAppEnvironment);
    if (!old_app && !new_app) {
      GTEST_SKIP()
          << "Native signed-update integration was not run. Set "
          << kOldAppEnvironment << " and " << kNewAppEnvironment
          << " to compatible Developer ID signed release .app bundles; "
             "see mac_install_integration_tests.md.";
    }
    ASSERT_TRUE(old_app && !old_app->empty()) << kOldAppEnvironment;
    ASSERT_TRUE(new_app && !new_app->empty()) << kNewAppEnvironment;
    old_source_ = base::FilePath::FromUTF8Unsafe(*old_app);
    new_source_ = base::FilePath::FromUTF8Unsafe(*new_app);
    ASSERT_TRUE(old_source_.IsAbsolute());
    ASSERT_TRUE(new_source_.IsAbsolute());
    ASSERT_FALSE(base::IsLink(old_source_));
    ASSERT_FALSE(base::IsLink(new_source_));
    ASSERT_TRUE(base::DirectoryExists(old_source_));
    ASSERT_TRUE(base::DirectoryExists(new_source_));
    ASSERT_EQ(".app", old_source_.Extension());
    ASSERT_EQ(old_source_.BaseName(), new_source_.BaseName());
    ASSERT_TRUE(VerifySignature(old_source_));
    ASSERT_TRUE(VerifySignature(new_source_));

    old_version_ = ReadBundleString(old_source_, @"CFBundleShortVersionString");
    new_version_ = ReadBundleString(new_source_, @"CFBundleShortVersionString");
    ASSERT_TRUE(base::Version(old_version_).IsValid());
    ASSERT_TRUE(base::Version(new_version_).IsValid());
    ASSERT_GT(
        base::Version(new_version_).CompareTo(base::Version(old_version_)), 0);
    const std::string product =
        ReadBundleString(old_source_, @"CFBundleDisplayName");
    ASSERT_FALSE(product.empty());
    ASSERT_EQ(product, ReadBundleString(new_source_, @"CFBundleDisplayName"));
    versions_path_ = base::FilePath("Contents/Frameworks")
                         .Append(base::FilePath::FromUTF8Unsafe(
                             product + " Framework.framework"))
                         .AppendASCII("Versions");
    framework_binary_ = base::FilePath::FromUTF8Unsafe(product + " Framework");
    ASSERT_TRUE(base::PathExists(old_source_.Append(versions_path_)
                                     .AppendASCII(old_version_)
                                     .Append(framework_binary_)));
    ASSERT_TRUE(base::PathExists(new_source_.Append(versions_path_)
                                     .AppendASCII(new_version_)
                                     .Append(framework_binary_)));

    // Every write and every installer target stays beneath this private root.
    // The supplied fixtures are only read, never installed over or resigned.
    ASSERT_TRUE(root_.CreateUniqueTempDir());
    ASSERT_TRUE(base::SetPosixFilePermissions(root_.GetPath(), 0700));
    // Include spaces to exercise subprocess argument handling with real paths.
    const base::FilePath install_parent =
        root_.GetPath().AppendASCII("installed application");
    const base::FilePath package_parent =
        root_.GetPath().AppendASCII("package contents");
    staging_ = root_.GetPath().AppendASCII("private staging");
    ASSERT_TRUE(base::CreateDirectory(install_parent));
    ASSERT_TRUE(base::CreateDirectory(package_parent));
    ASSERT_TRUE(base::CreateDirectory(staging_));
    // Users may rename an installed app. Full package preparation must locate
    // the signed app and stage it under the installed name without resigning.
    installed_ =
        install_parent.Append("Renamed " + old_source_.BaseName().value());
    packaged_ = package_parent.Append(new_source_.BaseName());
    ASSERT_TRUE(CopyBundle(old_source_, installed_));
    ASSERT_TRUE(CopyBundle(new_source_, packaged_));
    ASSERT_TRUE(VerifySignature(installed_));
    ASSERT_TRUE(VerifySignature(packaged_));
  }

  void MakePackage(base::FilePath* archive) {
    if (GetParam() == FullPackage::kZip) {
      *archive = root_.GetPath().AppendASCII("full update.zip");
      base::CommandLine command(base::FilePath("/usr/bin/ditto"));
      command.AppendArg("-c");
      command.AppendArg("-k");
      command.AppendArg("--keepParent");
      command.AppendArgPath(packaged_);
      command.AppendArgPath(*archive);
      ASSERT_TRUE(Run(command));
    } else {
      *archive = root_.GetPath().AppendASCII("full update.dmg");
      base::CommandLine command(base::FilePath("/usr/bin/hdiutil"));
      command.AppendArg("create");
      command.AppendArg("-srcfolder");
      command.AppendArgPath(packaged_.DirName());
      command.AppendArg("-format");
      command.AppendArg("UDZO");
      command.AppendArg("-fs");
      command.AppendArg("HFS+");
      command.AppendArg("-volname");
      command.AppendArg("Xenon updater integration");
      command.AppendArgPath(*archive);
      ASSERT_TRUE(Run(command));
    }
  }

  void VerifyInstalledUpdate() {
    ASSERT_TRUE(VerifySignature(installed_));
    EXPECT_EQ(new_version_,
              ReadBundleString(installed_, @"CFBundleShortVersionString"));
    const base::FilePath installed_versions = installed_.Append(versions_path_);
    const base::FilePath previous =
        installed_versions.AppendASCII(old_version_);
    const base::FilePath updated = installed_versions.AppendASCII(new_version_);
    ASSERT_TRUE(base::DirectoryExists(previous));
    ASSERT_TRUE(base::DirectoryExists(updated));
    EXPECT_TRUE(base::ContentsEqual(previous.Append(framework_binary_),
                                    old_source_.Append(versions_path_)
                                        .AppendASCII(old_version_)
                                        .Append(framework_binary_)));
    EXPECT_TRUE(base::ContentsEqual(updated.Append(framework_binary_),
                                    new_source_.Append(versions_path_)
                                        .AppendASCII(new_version_)
                                        .Append(framework_binary_)));
    const base::FilePath current = installed_versions.AppendASCII("Current");
    ASSERT_TRUE(base::IsLink(current));
    EXPECT_EQ(base::MakeAbsoluteFilePath(updated),
              base::MakeAbsoluteFilePath(current));
  }

  base::ScopedTempDir root_;
  base::FilePath old_source_;
  base::FilePath new_source_;
  base::FilePath installed_;
  base::FilePath packaged_;
  base::FilePath staging_;
  base::FilePath versions_path_;
  base::FilePath framework_binary_;
  std::string old_version_;
  std::string new_version_;
};

TEST_P(XenonMacInstallerIntegrationTest,
       InstallsSignedReleaseAndPreservesPreviousFramework) {
  base::FilePath archive;
  ASSERT_NO_FATAL_FAILURE(MakePackage(&archive));
  std::string error;
  const base::FilePath prepared = XenonUpdateInstaller::PrepareMacFullUpdate(
      archive, staging_, installed_.BaseName(), &error);
  ASSERT_FALSE(prepared.empty()) << error;
  ASSERT_TRUE(staging_.IsParent(prepared));
  ASSERT_EQ(installed_.BaseName(), prepared.BaseName());
  ASSERT_TRUE(VerifySignature(prepared));
  // Chromium preserves the previous version without writing into it. This
  // directory must not be required to be writable before installation starts.
  ScopedReadOnlyDirectory previous_version(
      installed_.Append(versions_path_).AppendASCII(old_version_));
  ASSERT_TRUE(previous_version.Protect());
  ASSERT_TRUE(XenonUpdateInstaller::InstallMacUpdate(prepared, installed_,
                                                     new_version_, &error))
      << error;
  ASSERT_NO_FATAL_FAILURE(VerifyInstalledUpdate());

  // The JSON feed currently permits upgrades only. Repeating the same package
  // must be rejected without damaging the successfully installed application.
  EXPECT_FALSE(XenonUpdateInstaller::InstallMacUpdate(prepared, installed_,
                                                      new_version_, &error));
  EXPECT_FALSE(error.empty());
  ASSERT_NO_FATAL_FAILURE(VerifyInstalledUpdate());
  EXPECT_TRUE(VerifySignature(old_source_));
  EXPECT_TRUE(VerifySignature(new_source_));
  EXPECT_EQ(old_version_,
            ReadBundleString(old_source_, @"CFBundleShortVersionString"));
  EXPECT_EQ(new_version_,
            ReadBundleString(new_source_, @"CFBundleShortVersionString"));
}

TEST_P(XenonMacInstallerIntegrationTest,
       RetriesFullPackageAfterPartialNativeInstallation) {
  base::FilePath archive;
  ASSERT_NO_FATAL_FAILURE(MakePackage(&archive));
  std::string error;
  const base::FilePath prepared = XenonUpdateInstaller::PrepareMacFullUpdate(
      archive, staging_, installed_.BaseName(), &error);
  ASSERT_FALSE(prepared.empty()) << error;
  ASSERT_TRUE(VerifySignature(prepared));

  const base::FilePath executable = base::FilePath::FromUTF8Unsafe(
      ReadBundleString(installed_, @"CFBundleExecutable"));
  ASSERT_FALSE(executable.empty());
  ASSERT_EQ(executable.BaseName(), executable);
  ScopedImmutableFile protection(
      installed_.AppendASCII("Contents/MacOS").Append(executable));
  ASSERT_TRUE(protection.Protect());
  ASSERT_TRUE(VerifySignature(installed_));
  MacUpdateInstallContext context;
  ASSERT_FALSE(XenonUpdateInstaller::InstallMacUpdate(
      prepared, installed_, new_version_, &error, &context));
  ASSERT_NE(std::string::npos,
            error.find("Chromium versioned application installation"))
      << error;
  // Prove this was a partially applied real install, not a validation refusal.
  ASSERT_TRUE(base::DirectoryExists(
      installed_.Append(versions_path_).AppendASCII(new_version_)));
  ASSERT_TRUE(protection.Restore());

  // Another process can finish a newer update without replacing the outer
  // directory inode. A retained repair identity must not authorize downgrade.
  const base::FilePath plist = installed_.AppendASCII("Contents/Info.plist");
  std::string partial_plist;
  ASSERT_TRUE(base::ReadFileToString(plist, &partial_plist));
  ASSERT_TRUE(base::WriteFile(
      plist, "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
             "<key>CFBundleShortVersionString</key><string>" +
                 new_version_ + ".1</string></dict></plist>"));
  EXPECT_FALSE(XenonUpdateInstaller::InstallMacUpdate(
      prepared, installed_, new_version_, &error, &context));
  EXPECT_NE(std::string::npos, error.find("newer application version"));
  ASSERT_TRUE(base::WriteFile(plist, partial_plist));

  // Prepare a fresh full payload as the manager's fallback does. The same
  // in-memory context retains the identity verified before the partial write.
  const base::FilePath retry_prepared =
      XenonUpdateInstaller::PrepareMacFullUpdate(archive, staging_,
                                                 installed_.BaseName(), &error);
  ASSERT_FALSE(retry_prepared.empty()) << error;
  ASSERT_NE(prepared, retry_prepared);
  ASSERT_TRUE(XenonUpdateInstaller::InstallMacUpdate(
      retry_prepared, installed_, new_version_, &error, &context))
      << error;
  ASSERT_NO_FATAL_FAILURE(VerifyInstalledUpdate());

  // A successful retry must clear the exception for the interrupted update.
  EXPECT_FALSE(XenonUpdateInstaller::InstallMacUpdate(
      retry_prepared, installed_, new_version_, &error, &context));
  EXPECT_FALSE(error.empty());
  ASSERT_NO_FATAL_FAILURE(VerifyInstalledUpdate());
}

INSTANTIATE_TEST_SUITE_P(SignedRelease,
                         XenonMacInstallerIntegrationTest,
                         testing::Values(FullPackage::kZip, FullPackage::kDmg),
                         [](const testing::TestParamInfo<FullPackage>& info) {
                           return info.param == FullPackage::kZip ? "Zip"
                                                                  : "Dmg";
                         });

}  // namespace
}  // namespace xenon::updater
