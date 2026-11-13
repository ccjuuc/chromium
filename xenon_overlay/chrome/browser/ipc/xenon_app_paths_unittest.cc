// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_paths.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/path_service.h"
#include "base/strings/string_split.h"
#include "base/test/scoped_path_override.h"
#include "base/test/bind.h"
#include "build/build_config.h"
#include "chrome/common/chrome_paths.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

class XenonAppPathsTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(root_.CreateUniqueTempDir());
    Override(base::DIR_HOME, "home");
    Override(base::DIR_TEMP, "temp");
    Override(base::DIR_USER_DESKTOP, "desktop");
    Override(chrome::DIR_USER_DATA, "host-data");
    Override(chrome::DIR_USER_DOCUMENTS, "documents");
    Override(chrome::DIR_DEFAULT_DOWNLOADS, "downloads");
    Override(chrome::DIR_USER_MUSIC, "music");
    Override(chrome::DIR_USER_PICTURES, "pictures");
    Override(chrome::DIR_USER_VIDEOS, "videos");
    Override(base::FILE_MODULE, "runtime-module");
#if BUILDFLAG(IS_WIN)
    Override(base::DIR_ROAMING_APP_DATA, "os-app-data");
#elif BUILDFLAG(IS_MAC)
    Override(base::DIR_APP_DATA, "os-app-data");
#endif
#if BUILDFLAG(IS_POSIX)
    Override(base::DIR_CACHE, "os-cache");
#endif
#if !BUILDFLAG(IS_MAC)
    Override(base::DIR_ASSETS, "runtime-assets");
#endif
  }

  base::FilePath Path(std::string_view name) const {
    auto result = root_.GetPath();
    for (auto component : base::SplitStringPiece(
             name, "/", base::KEEP_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
      result = result.AppendASCII(component);
    }
    return result;
  }

  XenonAppPaths Paths(std::string name = "Example") const {
    return XenonAppPaths(Path("release/app.asar"), Path("runtime"),
                         std::move(name), Path("legacy-data"));
  }

  void Override(int key, std::string_view name) {
    overrides_.push_back(std::make_unique<base::ScopedPathOverride>(
        key, Path(name), true, false));
  }

  base::ScopedTempDir root_;
  std::vector<std::unique_ptr<base::ScopedPathOverride>> overrides_;
};

TEST_F(XenonAppPathsTest, ReadsNativeSystemDirectoriesAndRealRuntimeModule) {
  auto paths = Paths();
  constexpr std::pair<const char*, int> kNativePaths[] = {
      {"home", base::DIR_HOME},
      {"temp", base::DIR_TEMP},
      {"desktop", base::DIR_USER_DESKTOP},
      {"userDesktop", base::DIR_USER_DESKTOP},
      {"documents", chrome::DIR_USER_DOCUMENTS},
      {"downloads", chrome::DIR_DEFAULT_DOWNLOADS},
      {"music", chrome::DIR_USER_MUSIC},
      {"pictures", chrome::DIR_USER_PICTURES},
      {"videos", chrome::DIR_USER_VIDEOS},
      {"module", base::FILE_MODULE},
  };
  for (const auto& [name, key] : kNativePaths) {
    SCOPED_TRACE(name);
    base::FilePath native;
    ASSERT_TRUE(base::PathService::Get(key, &native));
    auto actual = paths.Get(name);
    ASSERT_TRUE(actual.has_value()) << actual.error().message;
    EXPECT_EQ(native, *actual);
  }
  EXPECT_EQ(Path("runtime"), paths.Get("exe").value());
  EXPECT_EQ(Path("release/app.asar"), paths.Get("app").value());
  EXPECT_EQ(paths.Get("app").value(), paths.Get("appPath").value());
}

TEST_F(XenonAppPathsTest, OverridesStayWithinOneContainerAndShareDesktopAlias) {
  auto first = Paths("First");
  auto second = Paths("Second");
  const auto custom = Path("first-desktop");
  ASSERT_TRUE(first.Set("userDesktop", custom).has_value());
  EXPECT_EQ(custom, first.Get("desktop").value());
  EXPECT_EQ(custom, first.Get("userDesktop").value());
  EXPECT_EQ(Path("desktop"), second.Get("desktop").value());
  base::FilePath native;
  ASSERT_TRUE(base::PathService::Get(base::DIR_USER_DESKTOP, &native));
  EXPECT_EQ(Path("desktop"), native);
  EXPECT_FALSE(base::PathExists(custom));
}

TEST_F(XenonAppPathsTest, SetPathAcceptsNonexistentPathsWithoutCreatingThem) {
  auto paths = Paths();
  for (const char* name : {"music", "logs", "userCache"}) {
    SCOPED_TRACE(name);
    const auto custom = Path(name).Append(FILE_PATH_LITERAL("not-created"));
    ASSERT_TRUE(paths.Set(name, custom).has_value());
    EXPECT_EQ(custom, paths.Get(name).value());
    EXPECT_FALSE(base::PathExists(custom));
  }
  // Setting an override must not create a default location either.
  EXPECT_FALSE(base::PathExists(Path("legacy-data/logs")));
}

TEST_F(XenonAppPathsTest,
       RelativeAndUnknownPathsFailWithoutChangingPriorValue) {
  auto paths = Paths();
  ASSERT_TRUE(paths.Set("music", Path("accepted")).has_value());
  auto rejected = paths.Set("music", base::FilePath(FILE_PATH_LITERAL("rel")));
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ("ERR_INVALID_ARG_VALUE", rejected.error().code);
  EXPECT_EQ(Path("accepted"), paths.Get("music").value());
  for (const char* name : {"", "USERDATA", "not-an-electron-path"}) {
    auto read = paths.Get(name);
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ("ERR_INVALID_ARG_VALUE", read.error().code);
    rejected = paths.Set(name, Path("unknown"));
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ("ERR_INVALID_ARG_VALUE", rejected.error().code);
  }
}

TEST_F(XenonAppPathsTest, PreservesManagedUserDataAndInjectedCrashLocation) {
  auto paths = Paths();
  EXPECT_EQ(Path("legacy-data"), paths.Get("userData").value());
  XenonAppPaths managed(Path("app"), Path("runtime"), "Example",
                        Path("legacy-data"), Path("actual-crash-database"));
  for (const char* name : {"userData", "crashDumps"}) {
    SCOPED_TRACE(name);
    const auto original = managed.Get(name).value();
    ASSERT_TRUE(managed.Set(name, original).has_value());
    const auto changed = managed.Set(name, Path("new-storage"));
    ASSERT_FALSE(changed.has_value());
    EXPECT_EQ("ERR_NOT_SUPPORTED", changed.error().code);
    EXPECT_EQ(original, managed.Get(name).value());
  }
  XenonAppPaths host_defaults(Path("app"), Path("runtime"), "Example");
  EXPECT_EQ(Path("host-data"), host_defaults.Get("userData").value());
  EXPECT_EQ(Path("actual-crash-database"), managed.Get("crashDumps").value());
  auto unknown_crash_path = host_defaults.Get("crashDumps");
  ASSERT_FALSE(unknown_crash_path.has_value());
  EXPECT_EQ("ERR_NOT_SUPPORTED", unknown_crash_path.error().code);
  const auto unknown_crash_set =
      host_defaults.Set("crashDumps", Path("actual-crash-database"));
  ASSERT_FALSE(unknown_crash_set.has_value());
  EXPECT_EQ("ERR_NOT_SUPPORTED", unknown_crash_set.error().code);
  EXPECT_FALSE(base::PathExists(Path("new-storage")));
}

TEST_F(XenonAppPathsTest, DoesNotGuessSessionStorageOrPermitLegacyAliasWrites) {
  auto paths = Paths();
  auto session = paths.Get("sessionData");
  ASSERT_FALSE(session.has_value());
  EXPECT_EQ("ERR_NOT_SUPPORTED", session.error().code);
  for (const char* name : {"sessionData", "app", "appPath"}) {
    auto changed = paths.Set(name, Path("legacy-data"));
    ASSERT_FALSE(changed.has_value());
    EXPECT_EQ("ERR_NOT_SUPPORTED", changed.error().code);
  }
}

TEST_F(XenonAppPathsTest, CreatesDefaultLogsOnFirstRead) {
  auto paths = Paths();
#if BUILDFLAG(IS_MAC)
  const auto expected = Path("home/Library/Logs/Example");
#else
  const auto expected = Path("legacy-data/logs");
#endif
  EXPECT_FALSE(base::PathExists(expected));
  auto actual = paths.Get("logs");
  ASSERT_TRUE(actual.has_value()) << actual.error().message;
  EXPECT_EQ(expected, *actual);
  EXPECT_TRUE(base::DirectoryExists(expected));
}

TEST_F(XenonAppPathsTest, SetLogsPathCreatesExplicitAndDefaultDirectories) {
  auto paths = Paths();
  auto second = Paths("Other");
  const auto custom = Path("nested/custom-logs");
  ASSERT_TRUE(paths.SetLogsPath(custom).has_value());
  EXPECT_EQ(custom, paths.Get("logs").value());
  EXPECT_TRUE(base::DirectoryExists(custom));
  const auto default_logs = second.Get("logs").value();
  EXPECT_NE(custom, default_logs);
  ASSERT_TRUE(paths.SetLogsPath().has_value());
#if BUILDFLAG(IS_MAC)
  EXPECT_EQ(Path("home/Library/Logs/Example"), paths.Get("logs").value());
#else
  EXPECT_EQ(default_logs, paths.Get("logs").value());
#endif
  EXPECT_TRUE(base::DirectoryExists(paths.Get("logs").value()));
}

TEST_F(XenonAppPathsTest, FailedLogDirectoryCreationPreservesPreviousSetting) {
  auto paths = Paths();
  const auto initial = Path("initial-logs");
  ASSERT_TRUE(paths.SetLogsPath(initial).has_value());
  ASSERT_TRUE(base::WriteFile(Path("regular-file"), "not a directory"));
  const auto failed =
      paths.SetLogsPath(Path("regular-file").Append(FILE_PATH_LITERAL("logs")));
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ("ERR_FS_OPERATION_FAILED", failed.error().code);
  EXPECT_EQ(initial, paths.Get("logs").value());
  const auto relative =
      paths.SetLogsPath(base::FilePath(FILE_PATH_LITERAL("relative-logs")));
  ASSERT_FALSE(relative.has_value());
  EXPECT_EQ("ERR_INVALID_ARG_VALUE", relative.error().code);
  EXPECT_EQ(initial, paths.Get("logs").value());
}

TEST_F(XenonAppPathsTest, CachePathsUseTheOsRootAndCreateOnlyUserCache) {
  auto paths = Paths();
#if BUILDFLAG(IS_WIN)
  const auto native_cache = Path("os-app-data");
#else
  const auto native_cache = Path("os-cache");
#endif
  EXPECT_EQ(native_cache, paths.Get("cache").value());
  EXPECT_FALSE(base::PathExists(native_cache));
  const auto user_cache = paths.Get("userCache");
  ASSERT_TRUE(user_cache.has_value()) << user_cache.error().message;
  EXPECT_EQ(native_cache.Append(FILE_PATH_LITERAL("Example")), *user_cache);
  EXPECT_TRUE(base::DirectoryExists(*user_cache));
  EXPECT_NE(paths.Get("userData").value(), *user_cache);
}

TEST_F(XenonAppPathsTest,
       RejectsUnsafeApplicationNamesBeforeCreatingDirectories) {
  for (const char* name : {"", ".", "..", "../escape", "a/b", "a\\b",
                           "C:escape", "trailing.", ".. "}) {
    SCOPED_TRACE(name);
    auto paths = Paths(name);
    auto result = paths.Get("userCache");
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ("ERR_INVALID_ARG_VALUE", result.error().code);
#if BUILDFLAG(IS_MAC)
    EXPECT_FALSE(paths.SetLogsPath().has_value());
#endif
  }
  auto nul_name = Paths(std::string("safe\0hidden", 11));
  EXPECT_FALSE(nul_name.Get("userCache").has_value());
  EXPECT_FALSE(base::PathExists(Path("os-app-data")));
  EXPECT_FALSE(base::PathExists(Path("os-cache")));
}

TEST_F(XenonAppPathsTest, PlatformSpecificNamesFollowElectronAvailability) {
  auto paths = Paths();
#if BUILDFLAG(IS_MAC)
  EXPECT_FALSE(paths.Get("assets").has_value());
  EXPECT_FALSE(paths.Set("assets", Path("custom-assets")).has_value());
#else
  EXPECT_EQ(Path("runtime-assets"), paths.Get("assets").value());
#endif
#if BUILDFLAG(IS_WIN)
  auto recent = paths.Get("recent");
  ASSERT_TRUE(recent.has_value()) << recent.error().message;
  EXPECT_TRUE(recent->IsAbsolute());
  // Both names are aliases for DIR_ROAMING_APP_DATA in upstream Electron.
  ASSERT_TRUE(paths.Set("cache", Path("custom-app-data")).has_value());
  EXPECT_EQ(Path("custom-app-data"), paths.Get("appData").value());
#else
  EXPECT_FALSE(paths.Get("recent").has_value());
  EXPECT_FALSE(paths.Set("recent", Path("recent")).has_value());
#endif
}

TEST_F(XenonAppPathsTest, StoragePathsUseBrowserAuthorityAndDoNotCacheFailedSets) {
  auto user_data = Path("browser-user-data");
  auto session_data = Path("browser-session-data");
  bool reject = false;
  XenonAppPaths paths(
      Path("release"), Path("runtime"), "Example", Path("legacy"), {},
      base::BindLambdaForTesting(
          [&](std::string name, std::optional<base::FilePath> value)
              -> base::expected<base::FilePath, XenonAppPaths::Error> {
            auto& selected = name == "userData" ? user_data : session_data;
            if (value) {
              if (reject) {
                return base::unexpected(XenonAppPaths::Error{
                    "ERR_INVALID_STATE", "Session is already initialized"});
              }
              selected = *value;
            }
            return selected;
          }));
  EXPECT_EQ(paths.Get("userData").value(), user_data);
  EXPECT_EQ(paths.Get("sessionData").value(), session_data);
  ASSERT_TRUE(paths.Set("userData", Path("changed")).has_value());
  EXPECT_EQ(paths.Get("userData").value(), Path("changed"));
  reject = true;
  auto failed = paths.Set("userData", Path("rejected"));
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code, "ERR_INVALID_STATE");
  EXPECT_EQ(paths.Get("userData").value(), Path("changed"));
  EXPECT_EQ(paths.Get("sessionData").value(), session_data);
  // Browser remains authoritative when another native operation updates it.
  user_data = Path("host-updated");
  EXPECT_EQ(paths.Get("userData").value(), user_data);
#if !BUILDFLAG(IS_MAC)
  EXPECT_EQ(paths.Get("logs").value(), user_data.AppendASCII("logs"));
#endif
}

}  // namespace
}  // namespace xenon::ipc
