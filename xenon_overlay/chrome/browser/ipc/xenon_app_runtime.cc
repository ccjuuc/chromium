// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_WIN)
#include "base/file_version_info_win.h"
#elif BUILDFLAG(IS_APPLE)
#include "base/file_version_info.h"
#include "base/strings/utf_string_conversions.h"
#endif

namespace xenon::ipc {

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
