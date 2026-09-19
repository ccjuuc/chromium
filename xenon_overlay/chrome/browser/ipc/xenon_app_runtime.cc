// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"

#include <utility>

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "build/build_config.h"
#include "xenon_overlay/common/asar/archive.h"

#if BUILDFLAG(IS_WIN)
#include "base/file_version_info_win.h"
#elif BUILDFLAG(IS_APPLE)
#include "base/file_version_info.h"
#include "base/strings/utf_string_conversions.h"
#endif

namespace xenon::ipc {

namespace {

bool IsResourcesDirectory(const base::FilePath& path) {
  return base::FilePath::CompareEqualIgnoreCase(path.BaseName().value(),
                                                FILE_PATH_LITERAL("resources"));
}

base::FilePath ApplicationUnderResources(const base::FilePath& resources) {
  const auto archive = resources.AppendASCII("app.asar");
  if (base::PathExists(archive)) {
    return archive;
  }
  const auto directory = resources.AppendASCII("app");
  return base::DirectoryExists(directory) ? directory : base::FilePath();
}

}  // namespace

bool ResolveElectronApplication(const base::FilePath& requested,
                                ElectronApplicationInfo* application,
                                std::string* error) {
  if (!application || !error) {
    return false;
  }
  if (!requested.IsAbsolute()) {
    *error = "Electron application path must be absolute";
    return false;
  }
  base::FilePath normalized;
  if (requested.empty() || !base::NormalizeFilePath(requested, &normalized)) {
    *error = "Electron application does not exist: " + requested.AsUTF8Unsafe();
    return false;
  }

  ElectronApplicationInfo result;
  result.app_path = normalized;
  if (base::DirectoryExists(normalized)) {
    // A release's packaged application wins over unrelated package.json files
    // beside the executable. Native resources keep their original layout.
    for (const auto& resources :
         {normalized.AppendASCII("Contents").AppendASCII("Resources"),
          normalized.AppendASCII("resources"),
          IsResourcesDirectory(normalized) ? normalized : base::FilePath()}) {
      if (resources.empty()) {
        continue;
      }
      auto app = ApplicationUnderResources(resources);
      if (!app.empty()) {
        result.app_path = std::move(app);
        result.resources_directory = resources;
        result.is_packaged = true;
        break;
      }
    }
  } else if (!normalized.MatchesExtension(FILE_PATH_LITERAL(".asar"))) {
    *error = "Electron application must be a directory or ASAR archive";
    return false;
  } else {
    result.is_packaged = true;
  }
  if (result.resources_directory.empty() &&
      IsResourcesDirectory(result.app_path.DirName())) {
    result.resources_directory = result.app_path.DirName();
    result.is_packaged = true;
  }
  if (!result.resources_directory.empty()) {
    const auto parent = result.resources_directory.DirName();
    result.runtime_directory =
        parent.BaseName().value() == FILE_PATH_LITERAL("Contents")
            ? parent.AppendASCII("MacOS")
            : parent;
  } else {
    result.runtime_directory = base::DirectoryExists(result.app_path)
                                   ? result.app_path
                                   : result.app_path.DirName();
    result.resources_directory =
        result.app_path.MatchesExtension(FILE_PATH_LITERAL(".asar"))
            ? result.app_path.DirName()
            : result.runtime_directory.AppendASCII("resources");
  }
  base::FilePath canonical_app;
  if (!base::NormalizeFilePath(result.app_path, &canonical_app)) {
    *error = "Electron packaged application cannot be resolved";
    return false;
  }
  result.app_path = std::move(canonical_app);

  std::string package_text;
  bool read = false;
  if (base::DirectoryExists(result.app_path)) {
    read = base::ReadFileToString(result.app_path.AppendASCII("package.json"),
                                  &package_text);
  } else {
    auto archive = asar::GetOrCreateAsarArchive(result.app_path);
    read = archive &&
           archive->ReadFile(base::FilePath(FILE_PATH_LITERAL("package.json")),
                             &package_text);
  }
  if (!read) {
    *error = "Electron application has no readable package.json: " +
             result.app_path.AsUTF8Unsafe();
    return false;
  }
  auto package = base::JSONReader::Read(package_text, base::JSON_PARSE_RFC);
  if (!package || !package->is_dict()) {
    *error = "Electron application package.json is invalid";
    return false;
  }
  const auto& manifest = package->GetDict();
  const auto* name = manifest.FindString("productName");
  if (!name || name->empty()) {
    name = manifest.FindString("name");
  }
  if (name) {
    result.name = *name;
  }
  if (const auto* version = manifest.FindString("version")) {
    result.version = *version;
  }
  const auto* main = manifest.FindString("main");
  if (manifest.Find("main") && !main) {
    *error = "Electron application package.main must be a string";
    return false;
  }
  const auto relative_main = base::FilePath::FromUTF8Unsafe(
      main && !main->empty() ? *main : "index.js");
  if (relative_main.IsAbsolute()) {
    *error = "Electron application package.main must be relative";
    return false;
  }
  result.main_script_path = result.app_path.Append(relative_main);
  *application = std::move(result);
  return true;
}

bool ResolveAppExecutable(const base::FilePath& requested,
                          base::FilePath* executable,
                          std::string* error) {
  base::FilePath path = requested;
  if (path.empty() && !base::PathService::Get(base::FILE_EXE, &path)) {
    *error = "Host executable path is unavailable";
    return false;
  }
  base::File::Info info;
  if (!path.IsAbsolute() || !base::GetFileInfo(path, &info) ||
      info.is_directory || !base::NormalizeFilePath(path, executable)) {
    *error = "Application executable must be an existing absolute file: " +
             path.AsUTF8Unsafe();
    return false;
  }
  return true;
}

std::string GetAppExecutableVersion(const base::FilePath& executable) {
#if BUILDFLAG(IS_WIN)
  const auto info = FileVersionInfoWin::CreateFileVersionInfoWin(executable);
  return info ? info->GetFileVersion().GetString() : std::string();
#elif BUILDFLAG(IS_APPLE)
  const auto info = FileVersionInfo::CreateFileVersionInfo(executable);
  return info ? base::UTF16ToUTF8(info->file_version()) : std::string();
#else
  return {};
#endif
}

}  // namespace xenon::ipc
