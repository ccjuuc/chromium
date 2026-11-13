// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_paths.h"

#include <utility>

#include "base/base_paths.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/strings/strcat.h"
#include "build/build_config.h"
#include "chrome/common/chrome_paths.h"

#if BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
#include "base/environment.h"
#include "base/nix/xdg_util.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <shlobj.h>

#include "base/win/scoped_co_mem.h"
#endif

namespace xenon::ipc {
namespace {

using PathResult = base::expected<base::FilePath, XenonAppPaths::Error>;

std::string_view CanonicalName(std::string_view name) {
#if BUILDFLAG(IS_WIN)
  // Electron uses the same native path key for these two names on Windows.
  if (name == "cache") {
    return "appData";
  }
#endif
  return name == "userDesktop" ? "desktop" : name;
}

bool IsKnownName(std::string_view name) {
  if (name == "home" || name == "appData" || name == "userData" ||
      name == "sessionData" || name == "temp" || name == "exe" ||
      name == "module" || name == "desktop" || name == "documents" ||
      name == "downloads" || name == "music" || name == "pictures" ||
      name == "videos" || name == "logs" || name == "crashDumps" ||
      name == "cache" || name == "userCache" || name == "app" ||
      name == "appPath") {
    return true;
  }
#if BUILDFLAG(IS_WIN)
  if (name == "recent") {
    return true;
  }
#endif
#if BUILDFLAG(IS_WIN) || BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
  if (name == "assets") {
    return true;
  }
#endif
  return false;
}

XenonAppPaths::Error UnknownPath(std::string_view name) {
  return {"ERR_INVALID_ARG_VALUE",
          base::StrCat({"Unknown or unavailable app path: ", name})};
}

XenonAppPaths::Error ManagedPath(std::string_view name) {
  return {"ERR_NOT_SUPPORTED",
          base::StrCat({"The host manages app path '", name,
                        "'; changing its storage location is not supported"})};
}

XenonAppPaths::Error AbsolutePathRequired() {
  return {"ERR_INVALID_ARG_VALUE", "Path must be absolute"};
}

PathResult ReadNativePath(int key, std::string_view name) {
  base::FilePath result;
  if (!base::PathService::Get(key, &result) || !result.IsAbsolute()) {
    return base::unexpected(XenonAppPaths::Error{
        "ERR_PATH_UNAVAILABLE",
        base::StrCat({"Failed to get '", name, "' path"})});
  }
  return result;
}

base::expected<void, XenonAppPaths::Error> EnsureDirectory(
    const base::FilePath& path) {
  base::File::Error error;
  if (!base::CreateDirectoryAndGetError(path, &error)) {
    return base::unexpected(
        XenonAppPaths::Error{"ERR_FS_OPERATION_FAILED",
                             base::StrCat({"Failed to create app directory: ",
                                           base::File::ErrorToString(error)})});
  }
  return {};
}

bool IsSamePath(const base::FilePath& a, const base::FilePath& b) {
  return a.NormalizePathSeparators().StripTrailingSeparators() ==
         b.NormalizePathSeparators().StripTrailingSeparators();
}

}  // namespace

XenonAppPaths::XenonAppPaths(base::FilePath app_path,
                             base::FilePath executable_path,
                             std::string app_name,
                             base::FilePath legacy_user_data_path,
                             base::FilePath host_crash_dumps_path,
                             StoragePathCallback storage_path)
    : app_path_(std::move(app_path)),
      executable_path_(std::move(executable_path)),
      app_name_(std::move(app_name)),
      legacy_user_data_path_(std::move(legacy_user_data_path)),
      host_crash_dumps_path_(std::move(host_crash_dumps_path)),
      storage_path_(std::move(storage_path)) {}

XenonAppPaths::~XenonAppPaths() = default;

PathResult XenonAppPaths::Get(std::string_view name) {
  name = CanonicalName(name);
  if (!IsKnownName(name)) {
    return base::unexpected(UnknownPath(name));
  }
  if (storage_path_ && (name == "userData" || name == "sessionData")) {
    return storage_path_.Run(std::string(name), std::nullopt);
  }
  if (auto it = overrides_.find(name); it != overrides_.end()) {
    return it->second;
  }
  return GetDefault(name);
}

base::expected<void, XenonAppPaths::Error> XenonAppPaths::Set(
    std::string_view name,
    const base::FilePath& path) {
  if (!path.IsAbsolute()) {
    return base::unexpected(AbsolutePathRequired());
  }
  name = CanonicalName(name);
  if (!IsKnownName(name)) {
    return base::unexpected(UnknownPath(name));
  }
  if (storage_path_ && (name == "userData" || name == "sessionData")) {
    auto result = storage_path_.Run(std::string(name), path);
    return result.has_value() ? base::expected<void, Error>()
                              : base::unexpected(std::move(result.error()));
  }
  if (name == "sessionData" || name == "app" || name == "appPath") {
    return base::unexpected(ManagedPath(name));
  }
  if (name == "userData" || name == "crashDumps") {
    auto current = Get(name);
    if (!current.has_value()) {
      return base::unexpected(std::move(current.error()));
    }
    if (IsSamePath(*current, path)) {
      return {};
    }
    return base::unexpected(ManagedPath(name));
  }
  // Like Electron's OverrideAndCreateIfNeeded(..., true, false), an absolute
  // path need not exist and setPath must not create it. Consumers use Get().
  overrides_.insert_or_assign(std::string(name), path);
  return {};
}

base::expected<void, XenonAppPaths::Error> XenonAppPaths::SetLogsPath(
    std::optional<base::FilePath> path) {
  if (!path.has_value()) {
    auto default_path = DefaultLogsPath();
    if (!default_path.has_value()) {
      return base::unexpected(std::move(default_path.error()));
    }
    path = std::move(*default_path);
  }
  if (!path->IsAbsolute()) {
    return base::unexpected(AbsolutePathRequired());
  }
  auto created = EnsureDirectory(*path);
  if (!created.has_value()) {
    return created;
  }
  overrides_.insert_or_assign("logs", std::move(*path));
  return {};
}

PathResult XenonAppPaths::AppendApplicationName(const base::FilePath& parent) {
  // The package's display name is untrusted. It must remain one portable path
  // component; in particular, Windows also normalizes trailing dots/spaces.
  if (app_name_.empty() || app_name_ == "." || app_name_ == ".." ||
      app_name_.find_first_of("/\\:<>\"|?*") != std::string::npos ||
      app_name_.find('\0') != std::string::npos || app_name_.back() == '.' ||
      app_name_.back() == ' ') {
    return base::unexpected(
        Error{"ERR_INVALID_ARG_VALUE",
              "Application name must be a non-empty, safe path component"});
  }
  return parent.Append(base::FilePath::FromUTF8Unsafe(app_name_));
}

PathResult XenonAppPaths::DefaultLogsPath() {
#if BUILDFLAG(IS_MAC)
  auto home = Get("home");
  if (!home.has_value()) {
    return base::unexpected(std::move(home.error()));
  }
  return AppendApplicationName(home->Append(FILE_PATH_LITERAL("Library"))
                                   .Append(FILE_PATH_LITERAL("Logs")));
#else
  auto user_data = Get("userData");
  if (!user_data.has_value()) {
    return base::unexpected(std::move(user_data.error()));
  }
  return user_data->Append(FILE_PATH_LITERAL("logs"));
#endif
}

PathResult XenonAppPaths::GetDefault(std::string_view name) {
  if (name == "sessionData") {
    return base::unexpected(
        Error{"ERR_NOT_SUPPORTED",
              "The actual session storage path is not exposed by this host"});
  }
  if (name == "crashDumps") {
    if (host_crash_dumps_path_.empty()) {
      return base::unexpected(
          Error{"ERR_NOT_SUPPORTED",
                "The host has not exposed its actual crash report directory"});
    }
    if (!host_crash_dumps_path_.IsAbsolute()) {
      return base::unexpected(AbsolutePathRequired());
    }
    return host_crash_dumps_path_;
  }
  if (name == "app" || name == "appPath") {
    // Legacy, get-only aliases. Electron's standard API is app.getAppPath().
    if (!app_path_.IsAbsolute()) {
      return base::unexpected(AbsolutePathRequired());
    }
    return app_path_;
  }
  if (name == "userData" && !legacy_user_data_path_.empty()) {
    if (!legacy_user_data_path_.IsAbsolute()) {
      return base::unexpected(AbsolutePathRequired());
    }
    return legacy_user_data_path_;
  }
  if (name == "exe" && !executable_path_.empty()) {
    if (!executable_path_.IsAbsolute()) {
      return base::unexpected(AbsolutePathRequired());
    }
    return executable_path_;
  }
  if (name == "logs" || name == "userCache") {
    auto path = name == "logs" ? DefaultLogsPath() : Get("cache");
    if (!path.has_value()) {
      return path;
    }
    if (name == "userCache") {
      path = AppendApplicationName(*path);
      if (!path.has_value()) {
        return path;
      }
    }
    auto created = EnsureDirectory(*path);
    if (!created.has_value()) {
      return base::unexpected(std::move(created.error()));
    }
    return path;
  }
  if (name == "appData") {
#if BUILDFLAG(IS_WIN)
    return ReadNativePath(base::DIR_ROAMING_APP_DATA, name);
#elif BUILDFLAG(IS_MAC)
    return ReadNativePath(base::DIR_APP_DATA, name);
#elif BUILDFLAG(IS_LINUX) || BUILDFLAG(IS_CHROMEOS)
    auto environment = base::Environment::Create();
    auto path = base::nix::GetXDGDirectory(environment.get(),
                                           base::nix::kXdgConfigHomeEnvVar,
                                           base::nix::kDotConfigDir);
    if (!path.IsAbsolute()) {
      return base::unexpected(AbsolutePathRequired());
    }
    return path;
#else
    return base::unexpected(UnknownPath(name));
#endif
  }
  if (name == "cache") {
#if BUILDFLAG(IS_WIN)
    return ReadNativePath(base::DIR_ROAMING_APP_DATA, name);
#elif BUILDFLAG(IS_POSIX)
    return ReadNativePath(base::DIR_CACHE, name);
#else
    return base::unexpected(UnknownPath(name));
#endif
  }
#if BUILDFLAG(IS_WIN)
  if (name == "recent") {
    base::win::ScopedCoMem<wchar_t> path;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Recent, KF_FLAG_DONT_VERIFY,
                                    nullptr, &path))) {
      return base::unexpected(
          Error{"ERR_PATH_UNAVAILABLE", "Failed to get 'recent' path"});
    }
    return base::FilePath(path.get());
  }
#endif
  int key = -1;
  if (name == "home") {
    key = base::DIR_HOME;
  } else if (name == "temp") {
    key = base::DIR_TEMP;
  } else if (name == "userData") {
    key = chrome::DIR_USER_DATA;
  } else if (name == "exe") {
    key = base::FILE_EXE;
  } else if (name == "module") {
    key = base::FILE_MODULE;
  } else if (name == "assets") {
    key = base::DIR_ASSETS;
  } else if (name == "desktop") {
    key = base::DIR_USER_DESKTOP;
  } else if (name == "documents") {
    key = chrome::DIR_USER_DOCUMENTS;
  } else if (name == "downloads") {
    key = chrome::DIR_DEFAULT_DOWNLOADS;
  } else if (name == "music") {
    key = chrome::DIR_USER_MUSIC;
  } else if (name == "pictures") {
    key = chrome::DIR_USER_PICTURES;
  } else if (name == "videos") {
    key = chrome::DIR_USER_VIDEOS;
  }
  if (key < 0) {
    return base::unexpected(UnknownPath(name));
  }
  return ReadNativePath(key, name);
}

}  // namespace xenon::ipc
