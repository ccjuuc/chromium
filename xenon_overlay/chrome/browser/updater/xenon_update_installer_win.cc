// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#include <windows.h>

#include <algorithm>
#include <set>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/version.h"
#include "base/win/registry.h"
#include "base/win/scoped_handle.h"
#include "chrome/common/chrome_switches.h"

namespace xenon::updater {
namespace {

bool IsSafeRootPath(const base::FilePath& path) {
  if (path.empty() || path.IsAbsolute() || path.ReferencesParent() ||
      path.value().find(L':') != std::wstring::npos ||
      path.value().find(L'\0') != std::wstring::npos) {
    return false;
  }
  const auto components = path.GetComponents();
  for (const auto& component : components) {
    if (component.empty() || component == L"." || component == L".." ||
        component.back() == L'.' || component.back() == L' ' ||
        (component.size() >= 7 && base::FilePath::CompareEqualIgnoreCase(
                                      component.substr(0, 7), L".xenon-"))) {
      return false;
    }
  }
  // Root operations must never modify another side-by-side version tree.
  return !base::Version(base::FilePath(components.front()).MaybeAsASCII())
              .IsValid();
}

bool HasNoReparsePoints(const base::FilePath& root,
                        const base::FilePath& relative) {
  base::FilePath path = root;
  for (const auto& component : relative.GetComponents()) {
    path = path.Append(component);
    const DWORD attributes = ::GetFileAttributesW(path.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      const DWORD error = ::GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        return false;
      }
      continue;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
      return false;
    }
  }
  return true;
}

bool CopyVersionDirectory(const base::FilePath& source,
                          const base::FilePath& destination) {
  const DWORD attributes = ::GetFileAttributesW(source.value().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
      (attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
      !base::CreateDirectory(destination)) {
    return false;
  }
  // CopyDirectory's Windows enumerator ignores read errors. A version must be
  // copied completely before its launcher can be committed.
  base::FileEnumerator entries(
      source, true,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES,
      base::FilePath::StringType(),
      base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  for (base::FilePath item = entries.Next(); !item.empty();
       item = entries.Next()) {
    base::FilePath relative;
    if (!source.AppendRelativePath(item, &relative) ||
        relative.ReferencesParent() || !HasNoReparsePoints(source, relative)) {
      return false;
    }
    const base::FilePath output = destination.Append(relative);
    if (entries.GetInfo().IsDirectory()) {
      if (!base::CreateDirectory(output)) {
        return false;
      }
    } else if (!base::CreateDirectory(output.DirName()) ||
               !base::CopyFile(item, output)) {
      return false;
    }
  }
  return entries.GetError() == base::File::FILE_OK;
}

bool SameInstallDirectory(const base::FilePath& registered,
                          const base::FilePath& installation) {
  return registered.IsAbsolute() && base::FilePath::CompareEqualIgnoreCase(
                                        registered.NormalizePathSeparators()
                                            .StripTrailingSeparators()
                                            .value(),
                                        installation.NormalizePathSeparators()
                                            .StripTrailingSeparators()
                                            .value());
}

base::FilePath InstallationFromSetup(const base::FilePath& setup) {
  // The setup file may already have been removed by version cleanup. Validate
  // its registered layout without requiring the old executable to exist.
  if (!setup.IsAbsolute() ||
      !base::FilePath::CompareEqualIgnoreCase(setup.BaseName().value(),
                                              L"setup.exe") ||
      !base::FilePath::CompareEqualIgnoreCase(
          setup.DirName().BaseName().value(), L"Installer") ||
      !base::Version(setup.DirName().DirName().BaseName().MaybeAsASCII())
           .IsValid()) {
    return {};
  }
  return setup.DirName().DirName().DirName();
}

bool RegistrationMatches(base::win::RegKey& key,
                         const base::FilePath& installation,
                         bool uninstall_key) {
  std::wstring value;
  if (uninstall_key &&
      key.ReadValue(L"InstallLocation", &value) == ERROR_SUCCESS &&
      !value.empty()) {
    return SameInstallDirectory(base::FilePath(value), installation);
  }
  if (key.ReadValue(L"UninstallString", &value) == ERROR_SUCCESS &&
      !value.empty()) {
    // The product key contains a raw path (which may contain spaces), whereas
    // the uninstall key contains a quoted command with arguments.
    const base::FilePath setup =
        uninstall_key ? base::CommandLine::FromString(value).GetProgram()
                      : base::FilePath(value);
    const base::FilePath registered = InstallationFromSetup(setup);
    if (!registered.empty()) {
      return SameInstallDirectory(registered, installation);
    }
  }
  if (!uninstall_key &&
      key.ReadValue(L"InstallerSuccessLaunchCmdLine", &value) ==
          ERROR_SUCCESS &&
      !value.empty()) {
    return SameInstallDirectory(
        base::CommandLine::FromString(value).GetProgram().DirName(),
        installation);
  }
  return false;
}

void WriteRegistrationValue(base::win::RegKey& key,
                            const wchar_t* name,
                            const std::wstring& value) {
  const LONG result = key.WriteValue(name, value.c_str());
  if (result != ERROR_SUCCESS) {
    LOG(ERROR) << "Failed to update installed version registration: " << result;
  }
}

std::wstring UpdatedSetupCommand(base::win::RegKey& key,
                                 const wchar_t* name,
                                 const base::FilePath& setup,
                                 bool cleanup,
                                 bool system_install) {
  std::wstring value;
  base::CommandLine command(setup);
  if (key.ReadValue(name, &value) == ERROR_SUCCESS && !value.empty()) {
    command = base::CommandLine::FromString(value);
    command.SetProgram(setup);
  }
  // Retain mode/channel, MSI, and logging arguments registered by setup.
  if (cleanup) {
    if (!command.HasSwitch("cleanup-for-downgrade-version")) {
      command.AppendSwitchNative("cleanup-for-downgrade-version", L"$1");
    }
    if (!command.HasSwitch("cleanup-for-downgrade-operation")) {
      command.AppendSwitchNative("cleanup-for-downgrade-operation", L"$2");
    }
  } else if (!command.HasSwitch("uninstall")) {
    command.AppendSwitch("uninstall");
  }
  if (system_install && !command.HasSwitch("system-level")) {
    command.AppendSwitch("system-level");
  }
  return L"\"" + setup.value() + L"\" " + command.GetArgumentsString();
}

}  // namespace

void XenonUpdateInstaller::UpdateRegistryVersion(
    const base::FilePath& install_dir,
    const base::FilePath& executable,
    const std::string& version) {
  const std::wstring product =
      executable.BaseName().RemoveFinalExtension().value();
  if (product.empty() || !install_dir.IsAbsolute() ||
      !base::Version(version).IsValid() ||
      !base::DirectoryExists(install_dir.AppendASCII(version))) {
    return;
  }
  const std::wstring app_path = L"Software\\" + product;
  const std::wstring uninstall_path =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + product;
  const base::FilePath setup = install_dir.AppendASCII(version)
                                   .AppendASCII("Installer")
                                   .AppendASCII("setup.exe");
  const bool has_setup = base::PathExists(setup);
  const std::wstring version_w = base::ASCIIToWide(version);

  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    for (bool is_uninstall : {false, true}) {
      const std::wstring& key_path = is_uninstall ? uninstall_path : app_path;
      for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
        base::win::RegKey key;
        if (key.Open(root, key_path.c_str(),
                     KEY_QUERY_VALUE | KEY_SET_VALUE | view) != ERROR_SUCCESS ||
            !RegistrationMatches(key, install_dir, is_uninstall)) {
          continue;
        }
        if (is_uninstall) {
          WriteRegistrationValue(key, L"DisplayVersion", version_w);
          WriteRegistrationValue(key, L"Version", version_w);
          if (has_setup) {
            WriteRegistrationValue(
                key, L"UninstallString",
                UpdatedSetupCommand(key, L"UninstallString", setup,
                                    /*cleanup=*/false,
                                    root == HKEY_LOCAL_MACHINE));
          }
        } else {
          WriteRegistrationValue(key, L"pv", version_w);
          if (has_setup) {
            WriteRegistrationValue(key, L"UninstallString", setup.value());
            WriteRegistrationValue(
                key, L"DowngradeCleanupCommand",
                UpdatedSetupCommand(key, L"DowngradeCleanupCommand", setup,
                                    /*cleanup=*/true,
                                    root == HKEY_LOCAL_MACHINE));
          }
        }
      }
    }
  }
}

std::string XenonUpdateInstaller::ReadBundleShortVersion(
    const base::FilePath&) {
  return std::string();
}

bool XenonUpdateInstaller::InstallAndRelaunch(
    const base::FilePath& staged_path,
    const base::FilePath& target_install_dir,
    const base::FilePath& relaunch_executable,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!base::PathExists(staged_path)) {
    LOG(ERROR) << "[XenonUpdateInstaller] Staged path does not exist: "
               << staged_path.value();
    return false;
  }

  // 1. If staged_path is a standalone executable (e.g. full installer exe),
  // launch it directly.
  if (!base::DirectoryExists(staged_path) &&
      staged_path.MatchesExtension(FILE_PATH_LITERAL(".exe"))) {
    base::CommandLine installer_cmd(staged_path);
    installer_cmd.AppendSwitch("silent");
    base::LaunchOptions options;
    options.start_hidden = true;
    base::Process process = base::LaunchProcess(installer_cmd, options);
    if (!process.IsValid()) {
      LOG(ERROR)
          << "[XenonUpdateInstaller] Failed to launch installer executable: "
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

  if (!base::DirectoryExists(staged_path)) {
    LOG(ERROR) << "Unsupported full update package: " << staged_path.value();
    return false;
  }

  // Multiple browser profiles can update the same installation. Keep a single
  // transaction owner; the OS releases/removes the lock even on process death.
  const base::FilePath lock_path =
      target_install_dir.AppendASCII(".xenon-install.lock");
  base::win::ScopedHandle install_lock(
      ::CreateFileW(lock_path.value().c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                    nullptr, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE |
                        FILE_FLAG_OPEN_REPARSE_POINT,
                    nullptr));
  if (!install_lock.is_valid()) {
    LOG(ERROR) << "Installation is not writable or another update is running";
    return false;
  }

  const bool has_version_name =
      base::Version(staged_path.BaseName().MaybeAsASCII()).IsValid();
  const bool version_staged =
      has_version_name && staged_path.DirName() == target_install_dir;
  const bool version_package =
      version_staged ||
      (has_version_name &&
       base::DirectoryExists(staged_path.AppendASCII(".xenon-root")));
  base::FilePath root_files =
      version_package ? staged_path.AppendASCII(".xenon-root") : staged_path;
  base::FilePath incoming_version;
  base::FilePath installed_version;
  if (!version_staged) {
    if (version_package) {
      // A differential package is prepared privately outside the installation.
      // Copy it into this transaction before publishing its version directory.
      incoming_version = staged_path;
    } else {
      base::FileEnumerator dirs(staged_path, false,
                                base::FileEnumerator::DIRECTORIES);
      for (base::FilePath dir = dirs.Next(); !dir.empty(); dir = dirs.Next()) {
        if (base::Version(dir.BaseName().MaybeAsASCII()).IsValid()) {
          if (!incoming_version.empty()) {
            LOG(ERROR) << "Ambiguous full package version directories";
            return false;
          }
          incoming_version = dir;
        }
      }
    }
    if (incoming_version.empty()) {
      return false;
    }
    installed_version = target_install_dir.Append(incoming_version.BaseName());
    if (base::PathExists(installed_version)) {
      // Never delete an existing version: another process may be using it.
      return false;
    }
  }

  base::ScopedTempDir transaction;
  if (!transaction.CreateUniqueTempDirUnderPath(
          target_install_dir, FILE_PATH_LITERAL(".xenon-recovery-"))) {
    return false;
  }
  struct Replacement {
    base::FilePath ready;
    base::FilePath target;
    base::FilePath backup;
    bool delete_only = false;
    bool had_original = false;
    bool committed = false;
  };
  std::vector<Replacement> replacements;
  auto compare_paths = [](const base::FilePath& left,
                          const base::FilePath& right) {
    return base::FilePath::CompareLessIgnoreCase(left.value(), right.value());
  };
  std::set<base::FilePath, decltype(compare_paths)> targets(compare_paths);
  if (base::DirectoryExists(root_files)) {
    base::FileEnumerator files(
        root_files, true,
        base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES,
        base::FilePath::StringType(),
        base::FileEnumerator::FolderSearchPolicy::ALL,
        base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
    for (base::FilePath file = files.Next(); !file.empty();
         file = files.Next()) {
      if (!version_package && !incoming_version.empty() &&
          (incoming_version == file || incoming_version.IsParent(file))) {
        continue;
      }
      base::FilePath relative;
      if (!root_files.AppendRelativePath(file, &relative) ||
          !IsSafeRootPath(relative) ||
          !HasNoReparsePoints(root_files, relative) ||
          !HasNoReparsePoints(target_install_dir, relative)) {
        return false;
      }
      if (files.GetInfo().IsDirectory()) {
        continue;
      }
      if (!targets.insert(relative).second) {
        return false;
      }
      // Stage every file before modifying the running installation.
      Replacement replacement;
      replacement.ready =
          transaction.GetPath().AppendASCII("ready").Append(relative);
      replacement.target = target_install_dir.Append(relative);
      replacement.backup =
          transaction.GetPath().AppendASCII("backup").Append(relative);
      if (!base::CreateDirectory(replacement.ready.DirName()) ||
          !base::CreateDirectory(replacement.backup.DirName()) ||
          !base::CopyFile(file, replacement.ready)) {
        return false;
      }
      replacements.push_back(std::move(replacement));
    }
    if (files.GetError() != base::File::FILE_OK) {
      return false;
    }
  }
  const base::FilePath deletions_file =
      staged_path.AppendASCII(".xenon-deletions.json");
  if (version_package && base::PathExists(deletions_file)) {
    std::string json;
    if (!base::ReadFileToStringWithMaxSize(deletions_file, &json,
                                           1024 * 1024)) {
      return false;
    }
    auto deletions = base::JSONReader::ReadList(json, base::JSON_PARSE_RFC);
    if (!deletions) {
      return false;
    }
    for (const auto& value : *deletions) {
      if (!value.is_string()) {
        return false;
      }
      const base::FilePath relative =
          base::FilePath::FromUTF8Unsafe(value.GetString());
      if (!IsSafeRootPath(relative) ||
          !HasNoReparsePoints(target_install_dir, relative) ||
          !targets.insert(relative).second) {
        return false;
      }
      Replacement replacement;
      replacement.target = target_install_dir.Append(relative);
      replacement.backup =
          transaction.GetPath().AppendASCII("backup").Append(relative);
      replacement.delete_only = true;
      if (!base::PathExists(replacement.target)) {
        continue;
      }
      if (base::DirectoryExists(replacement.target) ||
          !base::CreateDirectory(replacement.backup.DirName())) {
        return false;
      }
      replacements.push_back(std::move(replacement));
    }
  }
  const bool has_launcher = std::any_of(
      replacements.begin(), replacements.end(), [&](const Replacement& item) {
        return !item.delete_only &&
               base::FilePath::CompareEqualIgnoreCase(item.target.value(),
                                                      target_exe.value());
      });
  if (!has_launcher) {
    LOG(ERROR) << "Update contains no launcher/root files";
    return false;
  }
  // Make the launcher visible last, after every other root file is in place.
  std::sort(replacements.begin(), replacements.end(),
            [&](const Replacement& left, const Replacement& right) {
              const bool left_launcher = base::FilePath::CompareEqualIgnoreCase(
                  left.target.value(), target_exe.value());
              const bool right_launcher =
                  base::FilePath::CompareEqualIgnoreCase(right.target.value(),
                                                         target_exe.value());
              if (left_launcher != right_launcher) {
                return !left_launcher;
              }
              return compare_paths(left.target, right.target);
            });
  bool installed_new_version = false;
  if (!incoming_version.empty()) {
    base::FilePath ready_version = transaction.GetPath().AppendASCII("version");
    if (!CopyVersionDirectory(incoming_version, ready_version)) {
      return false;
    }
    // Private root payloads have already been staged above. Keep transaction
    // metadata out of the published version tree and leave the input untouched.
    if (version_package && (!base::DeletePathRecursively(
                                ready_version.AppendASCII(".xenon-root")) ||
                            !base::DeleteFile(ready_version.AppendASCII(
                                ".xenon-deletions.json")))) {
      return false;
    }
    if (!::MoveFileExW(ready_version.value().c_str(),
                       installed_version.value().c_str(), 0)) {
      return false;
    }
    installed_new_version = true;
  }
  std::vector<base::FilePath> created_directories;
  auto rollback = [&]() {
    bool restored = true;
    for (auto it = replacements.rbegin(); it != replacements.rend(); ++it) {
      if (it->committed && !base::DeleteFile(it->target)) {
        restored = false;
      }
      if (it->had_original &&
          !::MoveFileExW(it->backup.value().c_str(), it->target.value().c_str(),
                         MOVEFILE_REPLACE_EXISTING)) {
        restored = false;
      }
    }
    for (auto it = created_directories.rbegin();
         it != created_directories.rend(); ++it) {
      // Remove only empty directories created by this transaction.
      base::DeleteFile(*it);
    }
    if (installed_new_version && restored) {
      base::DeletePathRecursively(installed_version);
    }
    if (!restored) {
      // Preserve all recovery material if an OS lock prevents rollback.
      LOG(ERROR) << "Update rollback needs recovery from "
                 << transaction.GetPath().value();
      transaction.Take();
    }
  };
  for (auto& replacement : replacements) {
    base::FilePath relative;
    if (!target_install_dir.AppendRelativePath(replacement.target, &relative) ||
        !HasNoReparsePoints(target_install_dir, relative)) {
      rollback();
      return false;
    }
    base::FilePath parent = target_install_dir;
    const auto components = relative.GetComponents();
    for (size_t index = 0; index + 1 < components.size(); ++index) {
      parent = parent.Append(components[index]);
      if (!base::DirectoryExists(parent)) {
        if (!base::CreateDirectory(parent)) {
          rollback();
          return false;
        }
        created_directories.push_back(parent);
      }
    }
    if (base::PathExists(replacement.target)) {
      if (base::DirectoryExists(replacement.target) ||
          !::MoveFileExW(replacement.target.value().c_str(),
                         replacement.backup.value().c_str(), 0)) {
        rollback();
        return false;
      }
      replacement.had_original = true;
    }
    if (replacement.delete_only) {
      continue;
    }
    if (!::MoveFileExW(replacement.ready.value().c_str(),
                       replacement.target.value().c_str(), 0)) {
      rollback();
      return false;
    }
    replacement.committed = true;
  }

  // 4. If relaunch was requested, launch target executable using Chromium's
  // native wait-for-parent mechanism.
  if (!relaunch_executable.empty()) {
    if (!base::PathExists(target_exe)) {
      rollback();
      return false;
    }
    base::CommandLine relaunch_cmd = *base::CommandLine::ForCurrentProcess();
    relaunch_cmd.SetProgram(target_exe);
    base::LaunchOptions launch_options;
    launch_options.current_directory = target_install_dir;
    launch_options.grant_foreground_privilege = true;

    // Pass parent process handle so the new process waits for this instance to
    // exit cleanly
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
        rollback();
        return false;
      }
    } else {
      LOG(ERROR) << "Failed to create parent wait handle";
      rollback();
      return false;
    }
  }

  // Publish registration only after the file transaction and any relaunch
  // handoff succeed. Older target releases may not synchronize it at startup.
  const base::FilePath committed_version =
      version_staged ? staged_path : installed_version;
  UpdateRegistryVersion(target_install_dir, target_exe,
                        committed_version.BaseName().MaybeAsASCII());

  // Locked backup images may remain until browser exit. Keep this private
  // recovery directory; it is safe to clean on a subsequent successful start.
  transaction.Take();
  return true;
}

}  // namespace xenon::updater
