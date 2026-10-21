// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#include <windows.h>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/win/scoped_handle.h"
#include "chrome/common/chrome_switches.h"

namespace xenon::updater {

std::string XenonUpdateInstaller::ReadBundleShortVersion(
    const base::FilePath&) {
  return std::string();
}

bool XenonUpdateInstaller::InstallAndRelaunch(
    const base::FilePath& staged_path,
    const base::FilePath& target_install_dir,
    const base::FilePath& relaunch_executable) {
  if (!base::PathExists(staged_path)) {
    LOG(ERROR) << "[XenonUpdateInstaller] Staged path does not exist: "
               << staged_path.value();
    return false;
  }

  // 1. If staged_path is a standalone executable (e.g. full installer exe), launch it directly.
  if (!base::DirectoryExists(staged_path) &&
      staged_path.Extension() == FILE_PATH_LITERAL(".exe")) {
    base::CommandLine installer_cmd(staged_path);
    installer_cmd.AppendSwitch("silent");
    base::LaunchOptions options;
    options.start_hidden = true;
    base::Process process = base::LaunchProcess(installer_cmd, options);
    if (!process.IsValid()) {
      LOG(ERROR) << "[XenonUpdateInstaller] Failed to launch installer executable: "
                 << staged_path.value();
      return false;
    }
    return true;
  }

  // 2. Resolve target launcher executable name
  base::FilePath target_exe_name = relaunch_executable.BaseName();
  if (target_exe_name.empty()) {
    base::FilePath current_exe;
    if (base::PathService::Get(base::FILE_EXE, &current_exe)) {
      target_exe_name = current_exe.BaseName();
    }
  }

  base::FilePath target_exe = target_install_dir.Append(target_exe_name);

  // 3. If staged_path is a version directory (either inside target_install_dir or external),
  // apply file updates using native Windows filesystem APIs without shell commands.
  if (base::DirectoryExists(staged_path)) {
    if (staged_path.DirName() == target_install_dir) {
      // The version directory is already placed inside target_install_dir.
      // 3a. Swap all new_* files in target_install_dir (e.g. new_xlb153.exe, new_chrome_proxy.exe)
      base::FileEnumerator enum_new(
          target_install_dir, /*recursive=*/false, base::FileEnumerator::FILES,
          FILE_PATH_LITERAL("new_*"));
      for (base::FilePath new_file = enum_new.Next(); !new_file.empty();
           new_file = enum_new.Next()) {
        base::FilePath::StringType new_base = new_file.BaseName().value();
        base::FilePath::StringType orig_base = new_base.substr(4);  // strip "new_"
        base::FilePath orig_file = target_install_dir.Append(orig_base);
        base::FilePath old_file =
            target_install_dir.Append(FILE_PATH_LITERAL("old_") + orig_base);

        if (base::PathExists(old_file)) {
          base::DeleteFile(old_file);
        }
        if (base::PathExists(orig_file)) {
          if (::MoveFileExW(orig_file.value().c_str(), old_file.value().c_str(),
                            MOVEFILE_REPLACE_EXISTING)) {
            if (base::CopyFile(new_file, orig_file)) {
              LOG(INFO) << "[XenonUpdateInstaller] Successfully swapped "
                        << orig_base;
              base::DeleteFile(new_file);
            } else {
              LOG(ERROR) << "[XenonUpdateInstaller] Failed to copy new file for "
                         << orig_base << ". Rolling back.";
              ::MoveFileExW(old_file.value().c_str(), orig_file.value().c_str(),
                            MOVEFILE_REPLACE_EXISTING);
            }
          } else {
            LOG(WARNING) << "[XenonUpdateInstaller] MoveFileEx on " << orig_base
                         << " failed: " << ::GetLastError();
          }
        } else {
          base::Move(new_file, orig_file);
        }
      }

      // 3b. Also check if launcher was staged inside staged_path and target_exe wasn't swapped yet
      base::FilePath new_exe_in_staged = staged_path.Append(target_exe_name);
      if (base::PathExists(new_exe_in_staged) && base::PathExists(target_exe)) {
        base::FilePath old_exe =
            target_install_dir.Append(FILE_PATH_LITERAL("old_") + target_exe_name.value());
        if (base::PathExists(old_exe)) {
          base::DeleteFile(old_exe);
        }
        if (::MoveFileExW(target_exe.value().c_str(), old_exe.value().c_str(),
                          MOVEFILE_REPLACE_EXISTING)) {
          if (base::CopyFile(new_exe_in_staged, target_exe)) {
            LOG(INFO) << "[XenonUpdateInstaller] Successfully swapped launcher from staged dir: "
                      << target_exe.value();
          } else {
            ::MoveFileExW(old_exe.value().c_str(), target_exe.value().c_str(),
                          MOVEFILE_REPLACE_EXISTING);
          }
        }
      }
    } else {
      // External staging directory: copy recursively into target_install_dir
      base::CopyDirectory(staged_path, target_install_dir, /*recursive=*/true);
      base::DeletePathRecursively(staged_path);
    }
  } else {
    // Single file replacement
    base::FilePath target_file = target_install_dir.Append(staged_path.BaseName());
    base::CopyFile(staged_path, target_file);
    base::DeleteFile(staged_path);
  }

  // 4. If relaunch was requested, launch target executable using Chromium's native wait-for-parent mechanism.
  if (!relaunch_executable.empty() && base::PathExists(target_exe)) {
    base::CommandLine relaunch_cmd = *base::CommandLine::ForCurrentProcess();
    relaunch_cmd.SetProgram(target_exe);
    base::LaunchOptions launch_options;
    launch_options.current_directory = target_install_dir;
    launch_options.grant_foreground_privilege = true;

    // Pass parent process handle so the new process waits for this instance to exit cleanly
    HANDLE raw_handle = nullptr;
    if (::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(),
                          ::GetCurrentProcess(), &raw_handle, SYNCHRONIZE,
                          /*bInheritHandle=*/TRUE, 0)) {
      base::win::ScopedHandle parent_handle(raw_handle);
      relaunch_cmd.AppendSwitchASCII(
          switches::kWaitForParentHandle,
          base::NumberToString(base::win::HandleToUint32(parent_handle.get())));
      launch_options.handles_to_inherit.push_back(parent_handle.get());
      base::Process proc = base::LaunchProcess(relaunch_cmd, launch_options);
      if (!proc.IsValid()) {
        LOG(ERROR) << "[XenonUpdateInstaller] Failed to relaunch application";
        return false;
      }
    } else {
      base::Process proc = base::LaunchProcess(relaunch_cmd, launch_options);
      if (!proc.IsValid()) {
        LOG(ERROR) << "[XenonUpdateInstaller] Failed to relaunch application";
        return false;
      }
    }
  }

  return true;
}

}  // namespace xenon::updater
