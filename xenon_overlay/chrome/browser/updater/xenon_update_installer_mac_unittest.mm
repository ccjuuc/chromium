// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <initializer_list>
#include <string_view>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_file.h"
#include "base/files/scoped_temp_dir.h"
#include "base/process/launch.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/zlib/contrib/minizip/zip.h"

namespace xenon::updater {
namespace {

struct ZipEntry {
  const char* path;
  mode_t mode;
  std::string_view contents;
};

class XenonMacInstallerTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(root_.CreateUniqueTempDir());
    staging_ = root_.GetPath().AppendASCII("staging");
    ASSERT_TRUE(base::CreateDirectory(staging_));
  }

  base::FilePath MakeBundle(const char* directory) {
    base::FilePath bundle =
        root_.GetPath().AppendASCII(directory).AppendASCII("Xenon.app");
    base::FilePath executable = bundle.AppendASCII("Contents/MacOS/Xenon");
    EXPECT_TRUE(base::CreateDirectory(executable.DirName()));
    EXPECT_TRUE(base::CopyFile(base::FilePath("/bin/echo"), executable));
    EXPECT_TRUE(base::SetPosixFilePermissions(executable, 0755));
    EXPECT_TRUE(
        base::WriteFile(bundle.AppendASCII("Contents/Info.plist"),
                        "<?xml version=\"1.0\"?><plist version=\"1.0\">"
                        "<dict><key>CFBundleIdentifier</key>"
                        "<string>org.example.Xenon</string>"
                        "<key>CFBundleExecutable</key><string>Xenon</string>"
                        "<key>CFBundlePackageType</key><string>APPL</string>"
                        "<key>CFBundleShortVersionString</key>"
                        "<string>1.0.0.1</string></dict></plist>"));
    return bundle;
  }

  base::FilePath ZipBundle(const base::FilePath& bundle) {
    const base::FilePath archive = root_.GetPath().AppendASCII("update.zip");
    base::CommandLine command(base::FilePath("/usr/bin/ditto"));
    command.AppendArg("-c");
    command.AppendArg("-k");
    command.AppendArg("--keepParent");
    command.AppendArgPath(bundle);
    command.AppendArgPath(archive);
    std::string output;
    EXPECT_TRUE(base::GetAppOutputAndError(command, &output)) << output;
    return archive;
  }

  base::FilePath MakeZip(std::initializer_list<ZipEntry> entries,
                         unsigned host = 3) {
    const base::FilePath archive = root_.GetPath().AppendASCII("update.zip");
    zipFile file = zipOpen64(archive.value().c_str(), APPEND_STATUS_CREATE);
    EXPECT_TRUE(file);
    if (!file) {
      return {};
    }
    for (const auto& entry : entries) {
      zip_fileinfo info = {};
      info.external_fa = static_cast<uLong>(entry.mode) << 16;
      EXPECT_EQ(ZIP_OK,
                zipOpenNewFileInZip4(file, entry.path, &info, nullptr, 0,
                                     nullptr, 0, nullptr, 0, 0, 0, -MAX_WBITS,
                                     DEF_MEM_LEVEL, Z_DEFAULT_STRATEGY, nullptr,
                                     0, (host << 8) | 20, 0));
      EXPECT_EQ(ZIP_OK, zipWriteInFileInZip(
                            file, entry.contents.data(),
                            static_cast<unsigned>(entry.contents.size())));
      EXPECT_EQ(ZIP_OK, zipCloseFileInZip(file));
    }
    EXPECT_EQ(ZIP_OK, zipClose(file, nullptr));
    return archive;
  }

  base::FilePath DmgBundle(const base::FilePath& bundle) {
    const base::FilePath image = root_.GetPath().AppendASCII("update.dmg");
    base::CommandLine command(base::FilePath("/usr/bin/hdiutil"));
    command.AppendArg("create");
    command.AppendArg("-srcfolder");
    command.AppendArgPath(bundle.DirName());
    command.AppendArg("-format");
    command.AppendArg("UDZO");
    command.AppendArg("-volname");
    command.AppendArg("XenonUpdate");
    command.AppendArgPath(image);
    std::string output;
    EXPECT_TRUE(base::GetAppOutputAndError(command, &output)) << output;
    return image;
  }

  base::ScopedTempDir root_;
  base::FilePath staging_;
};

TEST_F(XenonMacInstallerTest, FullZipPreservesExecutableAndInternalSymlink) {
  const base::FilePath original = MakeBundle("package");
  ASSERT_TRUE(base::CreateSymbolicLink(base::FilePath("MacOS/Xenon"),
                                       original.AppendASCII("Contents/link")));
  std::string error;
  const base::FilePath extracted = XenonUpdateInstaller::PrepareMacFullUpdate(
      ZipBundle(original), staging_, base::FilePath("Xenon.app"), &error);
  ASSERT_FALSE(extracted.empty()) << error;
  EXPECT_TRUE(staging_.IsParent(extracted));
  EXPECT_TRUE(base::IsLink(extracted.AppendASCII("Contents/link")));
  base::FilePath target;
  ASSERT_TRUE(
      base::ReadSymbolicLink(extracted.AppendASCII("Contents/link"), &target));
  EXPECT_EQ(base::FilePath("MacOS/Xenon"), target);
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(
      extracted.AppendASCII("Contents/MacOS/Xenon"), &mode));
  EXPECT_NE(0, mode & 0100);
  ASSERT_TRUE(base::GetPosixFilePermissions(extracted.DirName(), &mode));
  EXPECT_EQ(0700, mode);
}

TEST_F(XenonMacInstallerTest, FullZipPreservesExactModesWithoutFollowingLinks) {
  const base::FilePath archive = MakeZip({
      {"Xenon.app/", S_IFDIR | 0700, ""},
      {"Xenon.app/Contents/", S_IFDIR | 0550, ""},
      {"Xenon.app/Contents/private", S_IFREG | 0600, "private data"},
      {"Xenon.app/Contents/program", S_IFREG | 0711, "executable data"},
      {"Xenon.app/Contents/link", S_IFLNK | 0777, "private"},
  });
  std::string error;
  const base::FilePath extracted = XenonUpdateInstaller::PrepareMacFullUpdate(
      archive, staging_, base::FilePath("Xenon.app"), &error);
  ASSERT_FALSE(extracted.empty()) << error;
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(extracted, &mode));
  EXPECT_EQ(0700, mode);
  ASSERT_TRUE(
      base::GetPosixFilePermissions(extracted.AppendASCII("Contents"), &mode));
  EXPECT_EQ(0550, mode);
  ASSERT_TRUE(base::GetPosixFilePermissions(
      extracted.AppendASCII("Contents/private"), &mode));
  EXPECT_EQ(0600, mode);
  ASSERT_TRUE(base::GetPosixFilePermissions(
      extracted.AppendASCII("Contents/program"), &mode));
  EXPECT_EQ(0711, mode);
  EXPECT_TRUE(base::IsLink(extracted.AppendASCII("Contents/link")));
  // The successful staging tree is owned by the caller. Restore its directory
  // permission so the fixture can remove it after checking the exact mode.
  EXPECT_TRUE(
      base::SetPosixFilePermissions(extracted.AppendASCII("Contents"), 0700));
}

TEST_F(XenonMacInstallerTest, FullZipRejectsDosAttributesAsUnixPermissions) {
  const base::FilePath archive =
      MakeZip({{"Xenon.app/", S_IFDIR | 0755, ""}}, /*host=*/0);
  std::string error;
  EXPECT_TRUE(XenonUpdateInstaller::PrepareMacFullUpdate(
                  archive, staging_, base::FilePath("Xenon.app"), &error)
                  .empty());
  EXPECT_NE(std::string::npos, error.find("Unix file types and permissions"));
  EXPECT_TRUE(base::IsDirectoryEmpty(staging_));
}

TEST_F(XenonMacInstallerTest, FullZipCleansReadOnlyDirectoriesAfterFailure) {
  const base::FilePath archive = MakeZip({
      {"Xenon.app/", S_IFDIR | 0555, ""},
      {"Xenon.app/Contents/", S_IFDIR | 0555, ""},
      {"Xenon.app/Contents/data", S_IFREG | 0444, "read only"},
      {"Xenon.app/Contents/broken", S_IFLNK | 0777, "missing"},
  });
  std::string error;
  EXPECT_TRUE(XenonUpdateInstaller::PrepareMacFullUpdate(
                  archive, staging_, base::FilePath("Xenon.app"), &error)
                  .empty());
  EXPECT_NE(std::string::npos, error.find("broken symbolic link"));
  EXPECT_TRUE(base::IsDirectoryEmpty(staging_));
}

TEST_F(XenonMacInstallerTest, FullZipWithoutFileExtensionIsDetected) {
  const base::FilePath archive = root_.GetPath().AppendASCII("download.bin");
  ASSERT_TRUE(base::CopyFile(ZipBundle(MakeBundle("package")), archive));
  std::string error;
  EXPECT_FALSE(XenonUpdateInstaller::PrepareMacFullUpdate(
                   archive, staging_, base::FilePath("Xenon.app"), &error)
                   .empty())
      << error;
}

TEST_F(XenonMacInstallerTest, FullZipRejectsExternalSymlinkAndCleansStaging) {
  const base::FilePath original = MakeBundle("package");
  ASSERT_TRUE(base::CreateSymbolicLink(
      base::FilePath("/bin/echo"), original.AppendASCII("Contents/external")));
  std::string error;
  EXPECT_TRUE(
      XenonUpdateInstaller::PrepareMacFullUpdate(
          ZipBundle(original), staging_, base::FilePath("Xenon.app"), &error)
          .empty());
  EXPECT_FALSE(error.empty());
  EXPECT_TRUE(base::IsDirectoryEmpty(staging_));
}

TEST_F(XenonMacInstallerTest, FullZipPreservesARenamedInstallationName) {
  const base::FilePath original = MakeBundle("package");
  std::string error;
  const base::FilePath extracted = XenonUpdateInstaller::PrepareMacFullUpdate(
      ZipBundle(original), staging_, base::FilePath("Other.app"), &error);
  ASSERT_FALSE(extracted.empty()) << error;
  EXPECT_EQ(base::FilePath("Other.app"), extracted.BaseName());
  EXPECT_TRUE(base::PathExists(extracted.AppendASCII("Contents/Info.plist")));
}

TEST_F(XenonMacInstallerTest, FullZipRejectsAmbiguousApplicationNames) {
  const base::FilePath archive = MakeZip({
      {"Xenon.app/", S_IFDIR | 0755, ""},
      {"Another.app/", S_IFDIR | 0755, ""},
  });
  std::string error;
  EXPECT_TRUE(XenonUpdateInstaller::PrepareMacFullUpdate(
                  archive, staging_, base::FilePath("Renamed.app"), &error)
                  .empty());
  EXPECT_NE(std::string::npos, error.find("multiple application bundles"));
  EXPECT_TRUE(base::IsDirectoryEmpty(staging_));
}

TEST_F(XenonMacInstallerTest, FullDmgWithoutExtensionPreservesRenamedBundle) {
  const base::FilePath original = MakeBundle("package");
  ASSERT_TRUE(
      base::WriteFile(original.AppendASCII("Contents/private"), "data"));
  ASSERT_TRUE(base::SetPosixFilePermissions(
      original.AppendASCII("Contents/private"), 0600));
  ASSERT_TRUE(base::CreateSymbolicLink(base::FilePath("MacOS/Xenon"),
                                       original.AppendASCII("Contents/link")));
  const base::FilePath image = root_.GetPath().AppendASCII("download.bin");
  ASSERT_TRUE(base::CopyFile(DmgBundle(original), image));
  std::string error;
  const base::FilePath extracted = XenonUpdateInstaller::PrepareMacFullUpdate(
      image, staging_, base::FilePath("Renamed.app"), &error);
  ASSERT_FALSE(extracted.empty()) << error;
  EXPECT_EQ(base::FilePath("Renamed.app"), extracted.BaseName());
  EXPECT_TRUE(base::IsLink(extracted.AppendASCII("Contents/link")));
  int mode = 0;
  ASSERT_TRUE(base::GetPosixFilePermissions(
      extracted.AppendASCII("Contents/private"), &mode));
  EXPECT_EQ(0600, mode);
}

TEST_F(XenonMacInstallerTest, RejectsUnknownFormatAndParentTraversal) {
  const base::FilePath archive = root_.GetPath().AppendASCII("package.bin");
  ASSERT_TRUE(base::WriteFile(archive, "untrusted package"));
  std::string error;
  EXPECT_TRUE(XenonUpdateInstaller::PrepareMacFullUpdate(
                  archive, staging_, base::FilePath("Xenon.app"), &error)
                  .empty());
  EXPECT_NE(std::string::npos, error.find("Unsupported full update format"));
  EXPECT_TRUE(XenonUpdateInstaller::PrepareMacFullUpdate(
                  archive, staging_, base::FilePath("../Xenon.app"), &error)
                  .empty());
  EXPECT_TRUE(base::IsDirectoryEmpty(staging_));
}

TEST_F(XenonMacInstallerTest, RefusesUnsignedInstallationWithoutChangingIt) {
  const base::FilePath installed = MakeBundle("installed");
  const base::FilePath update = MakeBundle("update");
  ASSERT_TRUE(base::WriteFile(installed.AppendASCII("sentinel"), "keep"));
  std::string error;
  EXPECT_FALSE(XenonUpdateInstaller::InstallMacUpdate(update, installed,
                                                      "1.0.0.2", &error));
  EXPECT_NE(std::string::npos, error.find("Developer ID signature"));
  std::string sentinel;
  ASSERT_TRUE(
      base::ReadFileToString(installed.AppendASCII("sentinel"), &sentinel));
  EXPECT_EQ("keep", sentinel);
}

TEST_F(XenonMacInstallerTest, RefusesAdHocSigning) {
  const base::FilePath installed = MakeBundle("installed");
  const base::FilePath update = MakeBundle("update");
  // Both bundles must be signed, otherwise the first update signature check
  // only exercises the unsigned case and never reaches ad-hoc rejection.
  for (const auto& bundle : {installed, update}) {
    base::CommandLine command(base::FilePath("/usr/bin/codesign"));
    command.AppendArg("--force");
    command.AppendArg("--sign");
    command.AppendArg("-");
    command.AppendArgPath(bundle);
    std::string output;
    ASSERT_TRUE(base::GetAppOutputAndError(command, &output)) << output;
  }
  std::string error;
  EXPECT_FALSE(XenonUpdateInstaller::InstallMacUpdate(update, installed,
                                                      "1.0.0.2", &error));
  EXPECT_NE(std::string::npos, error.find("Developer ID signature"));
}

TEST_F(XenonMacInstallerTest, RefusesAnInstallationWhoseLockIsHeld) {
  const base::FilePath installed = MakeBundle("installed");
  const base::FilePath update = MakeBundle("update");
  const base::FilePath lock_path =
      installed.DirName().AppendASCII(".Xenon.app.xenon-update.lock");
  base::ScopedFD lock(open(lock_path.value().c_str(), O_CREAT | O_RDWR, 0600));
  ASSERT_TRUE(lock.is_valid());
  ASSERT_EQ(0, flock(lock.get(), LOCK_EX | LOCK_NB));
  std::string error;
  EXPECT_FALSE(XenonUpdateInstaller::InstallMacUpdate(update, installed,
                                                      "1.0.0.2", &error));
  EXPECT_NE(std::string::npos, error.find("Another process is updating"));
}

}  // namespace
}  // namespace xenon::updater
