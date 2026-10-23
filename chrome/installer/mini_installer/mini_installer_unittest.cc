// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/installer/mini_installer/mini_installer.h"

#include <string>

#include "base/base_paths.h"
#include "base/base_paths_win.h"
#include "base/compiler_specific.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/scoped_environment_variable_override.h"
#include "base/scoped_native_library.h"
#include "base/test/test_reg_util_win.h"
#include "base/win/access_token.h"
#include "base/win/registry.h"
#include "chrome/install_static/install_details.h"
#include "chrome/installer/mini_installer/configuration.h"
#include "chrome/installer/mini_installer/mini_installer_constants.h"
#include "chrome/installer/mini_installer/path_string.h"
#include "chrome/installer/util/util_constants.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace mini_installer {

namespace {

base::FilePath GetTestFileRootPath() {
  base::FilePath test_data_root;
  base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &test_data_root);
  return test_data_root.Append(FILE_PATH_LITERAL("chrome"))
      .Append(FILE_PATH_LITERAL("test"))
      .Append(FILE_PATH_LITERAL("data"))
      .Append(FILE_PATH_LITERAL("installer"));
}

}  // namespace

TEST(MiniInstallerTest, AppendCommandLineFlags) {
  static constexpr struct {
    const wchar_t* command_line;
    const wchar_t* args;
  } kData[] = {
      {L"", L"foo.exe"},
      {L"mini_installer.exe", L"foo.exe"},
      {L"mini_installer.exe --verbose-logging", L"foo.exe --verbose-logging"},
      {L"C:\\Temp\\mini_installer.exe --verbose-logging",
       L"foo.exe --verbose-logging"},
      {L"C:\\Temp\\mini_installer --verbose-logging",
       L"foo.exe --verbose-logging"},
      {L"\"C:\\Temp\\mini_installer (1).exe\" --verbose-logging",
       L"foo.exe --verbose-logging"},
      // mini_installer-only switches are not forwarded to setup.exe.
      {L"mini_installer.exe --silent --verbose-logging",
       L"foo.exe --verbose-logging"},
      {L"mini_installer.exe --quiet /S --system-level",
       L"foo.exe --system-level"},
  };

  CommandString buffer;

  for (const auto& data : kData) {
    buffer.assign(L"foo.exe");
    AppendCommandLineFlags(data.command_line, &buffer);
    EXPECT_STREQ(data.args, buffer.get()) << data.command_line;
  }
}

TEST(MiniInstallerTest, GetModuleDir) {
  PathString directory;

  ASSERT_TRUE(GetModuleDir(/*module=*/nullptr, &directory));
  ASSERT_NE(directory.length(), 0U);
  EXPECT_LT(directory.length(), directory.capacity());
  EXPECT_EQ(UNSAFE_TODO(directory.get()[directory.length() - 1]), L'\\');
}

TEST(MiniInstallerTest, WorkDirectoryUsesTemporaryRoot) {
  base::ScopedTempDir temporary_root;
  ASSERT_TRUE(temporary_root.CreateUniqueTempDir());
  const base::FilePath user_temp = temporary_root.GetPath().Append(
      FILE_PATH_LITERAL("installer staging \u66F4\u65B0"));
  ASSERT_TRUE(base::CreateDirectory(user_temp));
  base::ScopedEnvironmentVariableOverride tmp_override(
      "TMP", user_temp.AsUTF8Unsafe());
  ASSERT_TRUE(tmp_override.IsOverridden());

  PathString first_path;
  PathString second_path;
  ProcessExitResult result(GENERIC_ERROR);
  ASSERT_TRUE(GetWorkDir(&first_path, &result));
  EXPECT_TRUE(result.IsSuccess());
  ASSERT_TRUE(base::DirectoryExists(base::FilePath(first_path.get())));
  base::ScopedTempDir first;
  ASSERT_TRUE(
      first.Set(base::FilePath(first_path.get()).StripTrailingSeparators()));
  ASSERT_TRUE(GetWorkDir(&second_path, &result));
  EXPECT_TRUE(result.IsSuccess());
  ASSERT_TRUE(base::DirectoryExists(base::FilePath(second_path.get())));
  base::ScopedTempDir second;
  ASSERT_TRUE(
      second.Set(base::FilePath(second_path.get()).StripTrailingSeparators()));

  EXPECT_TRUE(StrEndsWith(first_path.get(), L"\\"));
  EXPECT_TRUE(StrEndsWith(second_path.get(), L"\\"));
  EXPECT_NE(first.GetPath(), second.GetPath());
  const base::FilePath work_root = first.GetPath().DirName();
  EXPECT_EQ(work_root, second.GetPath().DirName());
  EXPECT_NE(work_root, base::PathService::CheckedGet(base::DIR_EXE));

  auto token = base::win::AccessToken::FromCurrentProcess();
  ASSERT_TRUE(token);
  if (token->IsElevated()) {
    // An elevated installer must ignore the user-controlled TMP directory.
    const base::FilePath system_temp =
        base::PathService::CheckedGet(base::DIR_WINDOWS).AppendASCII("SystemTemp");
    const base::FilePath program_files =
        base::PathService::CheckedGet(base::DIR_PROGRAM_FILES);
    EXPECT_TRUE(work_root == system_temp || work_root == program_files);
    EXPECT_TRUE(base::IsDirectoryEmpty(user_temp));
  } else {
    EXPECT_EQ(work_root, user_temp);
  }
}

TEST(MiniInstallerTest, WorkDirectoryFailsWithoutUsableParent) {
  base::ScopedTempDir parent;
  ASSERT_TRUE(parent.CreateUniqueTempDir());
  const std::wstring missing_parent =
      parent.GetPath().AppendASCII("missing").value() + L"\\";
  PathString work_path;
  ASSERT_TRUE(work_path.assign(L"previous path"));
  ProcessExitResult result(SUCCESS_EXIT_CODE);
  EXPECT_FALSE(CreateWorkDir(missing_parent.c_str(), &work_path, &result));
  EXPECT_EQ(result.exit_code, UNABLE_TO_GET_WORK_DIRECTORY);
  EXPECT_EQ(result.windows_error, static_cast<DWORD>(ERROR_PATH_NOT_FOUND));
  EXPECT_TRUE(work_path.empty());
  EXPECT_TRUE(base::IsDirectoryEmpty(parent.GetPath()));
}

TEST(MiniInstallerTest, WorkDirectoryRejectsOverlongPath) {
  PathString work_path;
  ASSERT_TRUE(work_path.assign(L"previous path"));
  ProcessExitResult result(SUCCESS_EXIT_CODE);
  const std::wstring overlong_path(MAX_PATH, L'x');
  EXPECT_FALSE(CreateWorkDir(overlong_path.c_str(), &work_path, &result));
  EXPECT_EQ(result.exit_code, PATH_STRING_OVERFLOW);
  EXPECT_TRUE(work_path.empty());
}

struct UnpackParams {
  UnpackParams(base::FilePath mini_installer_file_path,
               std::wstring expected_unpacked_archive_file_name,
               base::FilePath expected_archive_file_path,
               const wchar_t* expected_setup_resource_type,
               const wchar_t* expected_archive_resource_type,
               bool is_compressed)
      : mini_installer_file_path(mini_installer_file_path),
        expected_unpacked_archive_file_name(
            expected_unpacked_archive_file_name),
        expected_archive_file_path(expected_archive_file_path),
        expected_setup_resource_type(expected_setup_resource_type),
        expected_archive_resource_type(expected_archive_resource_type),
        is_compressed(is_compressed) {}

  base::FilePath mini_installer_file_path;
  std::wstring expected_unpacked_archive_file_name;
  base::FilePath expected_archive_file_path;
  const wchar_t* expected_setup_resource_type;
  const wchar_t* expected_archive_resource_type;
  bool is_compressed;
};

class MiniInstallerTest : public ::testing::TestWithParam<UnpackParams> {};

INSTANTIATE_TEST_SUITE_P(CompressedArchive,
                         MiniInstallerTest,
                         ::testing::Values(UnpackParams(
                             GetTestFileRootPath().Append(
                                 FILE_PATH_LITERAL("mini_installer.exe.test")),
                             std::wstring(L"CHROME.PACKED.7Z"),
                             GetTestFileRootPath().Append(
                                 FILE_PATH_LITERAL("test_chrome.packed.7z")),
                             kLZCResourceType,
                             kLZMAResourceType,
                             true)));

INSTANTIATE_TEST_SUITE_P(
    UncompressedArchive,
    MiniInstallerTest,
    ::testing::Values(UnpackParams(
        GetTestFileRootPath().Append(
            FILE_PATH_LITERAL("mini_installer_uncompressed.exe.test")),
        std::wstring(L"CHROME.7Z"),
        GetTestFileRootPath().Append(FILE_PATH_LITERAL("test_chrome.7z")),
        kBinResourceType,
        kBinResourceType,
        false)));

// Tests unpacking the compressed chrome.packed.7z and setup.ex_ from a test
// mini_installer.
TEST_P(MiniInstallerTest, UnpackMiniInstaller) {
  PathString work_path;
  ProcessExitResult work_result(GENERIC_ERROR);
  ASSERT_TRUE(GetWorkDir(&work_path, &work_result));
  ASSERT_TRUE(work_result.IsSuccess());
  ASSERT_TRUE(base::DirectoryExists(base::FilePath(work_path.get())));
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(
      temp_dir.Set(base::FilePath(work_path.get()).StripTrailingSeparators()));

  int max_delete_attempts = 0;
  PathString setup_path;
  PathString archive_path;
  ResourceTypeString archive_type;

  base::ScopedNativeLibrary loaded_module(
      LoadLibraryExW(GetParam().mini_installer_file_path.value().c_str(), NULL,
                     LOAD_LIBRARY_AS_IMAGE_RESOURCE));
  ASSERT_TRUE(loaded_module.is_valid());

  ProcessExitResult exit_code =
      UnpackBinaryResources(loaded_module.get(), work_path.get(), setup_path,
                            archive_path, archive_type, max_delete_attempts);
  EXPECT_EQ(exit_code.exit_code, SUCCESS_EXIT_CODE);

  base::FilePath expected_setup_path =
      temp_dir.GetPath().Append(FILE_PATH_LITERAL("setup.exe"));
  EXPECT_STREQ(setup_path.get(), expected_setup_path.value().c_str());

  std::string actual_setup_data;
  EXPECT_TRUE(base::ReadFileToString(expected_setup_path, &actual_setup_data));
  EXPECT_STREQ(actual_setup_data.c_str(), "fakesetupdata");
  base::FilePath expected_unpacked_archive_file_path =
      temp_dir.GetPath().Append(GetParam().expected_unpacked_archive_file_name);
  ASSERT_TRUE(base::FilePath::CompareEqualIgnoreCase(
      archive_path.get(), expected_unpacked_archive_file_path.value()));
  EXPECT_STREQ(archive_type.get(), GetParam().expected_archive_resource_type);

  EXPECT_TRUE(base::ContentsEqual(base::FilePath(archive_path.get()),
                                  GetParam().expected_archive_file_path));

  if (GetParam().is_compressed) {
    EXPECT_TRUE(!base::PathExists(
        temp_dir.GetPath().Append(FILE_PATH_LITERAL("SETUP.EX_"))));
  } else {
    EXPECT_EQ(max_delete_attempts, 0);
  }
}

}  // namespace mini_installer
