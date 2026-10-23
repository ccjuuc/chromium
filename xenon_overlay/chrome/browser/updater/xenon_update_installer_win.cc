// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#include <windows.h>

#include <map>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/callback.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/version.h"
#include "base/win/access_token.h"
#include "base/win/registry.h"
#include "base/win/scoped_handle.h"
#include "chrome/common/chrome_switches.h"

namespace xenon::updater {
namespace {

bool IsSafeRelativePath(const base::FilePath& path) {
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
  return true;
}

bool IsSafeRootPath(const base::FilePath& path) {
  // Root payloads must never overwrite the normalized version directory.
  return IsSafeRelativePath(path) &&
         !base::Version(
              base::FilePath(path.GetComponents().front()).MaybeAsASCII())
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

bool IsPlainDirectory(const base::FilePath& path) {
  const DWORD attributes = ::GetFileAttributesW(path.value().c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
         !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

base::win::ScopedHandle AcquireInstallationLock(
    const base::FilePath& install_dir) {
  if (!install_dir.IsAbsolute() || install_dir.ReferencesParent() ||
      !IsPlainDirectory(install_dir)) {
    return {};
  }
  base::win::ScopedHandle lock(::CreateFileW(
      install_dir.AppendASCII(".xenon-install.lock").value().c_str(),
      GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
      FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE |
          FILE_FLAG_OPEN_REPARSE_POINT,
      nullptr));
  BY_HANDLE_FILE_INFORMATION info = {};
  if (!lock.is_valid() || !::GetFileInformationByHandle(lock.get(), &info) ||
      (info.dwFileAttributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    return {};
  }
  return lock;
}

bool WriteCommittedMarker(const base::FilePath& recovery,
                          const std::string& version) {
  if (!IsPlainDirectory(recovery)) {
    return false;
  }
  // CREATE_NEW never follows or overwrites an existing marker/reparse point.
  base::win::ScopedHandle marker(::CreateFileW(
      recovery.AppendASCII("committed").value().c_str(), GENERIC_WRITE,
      FILE_SHARE_READ, nullptr, CREATE_NEW,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  DWORD written = 0;
  return marker.is_valid() &&
         ::WriteFile(marker.get(), version.data(),
                     static_cast<DWORD>(version.size()), &written, nullptr) &&
         written == version.size();
}

// Caller owns .xenon-install.lock. Keep the commit marker until every backup
// has been removed, so files locked by the old process can be retried at
// startup.
void CleanupCommittedRecoveryDirectory(const base::FilePath& recovery,
                                       const base::Version& current_version) {
  if (!IsPlainDirectory(recovery)) {
    return;
  }
  const base::FilePath marker = recovery.AppendASCII("committed");
  const DWORD attributes = ::GetFileAttributesW(marker.value().c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES ||
      (attributes &
       (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
    return;
  }
  std::string version;
  if (!base::ReadFileToStringWithMaxSize(marker, &version, 256)) {
    return;
  }
  const base::Version committed_version(version);
  if (!committed_version.IsValid() || committed_version > current_version) {
    return;
  }

  // Validate the entire tree before removing anything. Non-recursive deletes
  // below cannot accidentally remove the marker first or traverse a junction.
  std::vector<base::FilePath> children;
  base::FileEnumerator entries(
      recovery, true,
      base::FileEnumerator::FILES | base::FileEnumerator::DIRECTORIES,
      base::FilePath::StringType(),
      base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  for (base::FilePath item = entries.Next(); !item.empty();
       item = entries.Next()) {
    base::FilePath relative;
    if (!recovery.AppendRelativePath(item, &relative) ||
        !HasNoReparsePoints(recovery, relative)) {
      return;
    }
    if (!base::FilePath::CompareEqualIgnoreCase(item.value(), marker.value())) {
      children.push_back(item);
    }
  }
  if (entries.GetError() != base::File::FILE_OK) {
    return;
  }
  bool removed = true;
  for (auto it = children.rbegin(); it != children.rend(); ++it) {
    base::FilePath relative;
    if (!IsPlainDirectory(recovery) ||
        !recovery.AppendRelativePath(*it, &relative) ||
        !HasNoReparsePoints(recovery, relative)) {
      return;
    }
    if (!base::DeleteFile(*it)) {
      removed = false;
    }
  }
  if (!removed || !IsPlainDirectory(recovery) ||
      !HasNoReparsePoints(recovery, marker.BaseName()) ||
      !base::DeleteFile(marker)) {
    return;
  }
  if (!base::DeleteFile(recovery) && IsPlainDirectory(recovery) &&
      !WriteCommittedMarker(recovery, version)) {
    LOG(WARNING) << "Failed to retain update cleanup marker in "
                 << recovery.value();
  }
}

bool IsPlainFile(const base::FilePath& path) {
  const DWORD attributes = ::GetFileAttributesW(path.value().c_str());
  return attributes != INVALID_FILE_ATTRIBUTES &&
         !(attributes &
           (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
}

bool SameInstallDirectory(const base::FilePath& registered,
                          const base::FilePath& installation) {
  return registered.IsAbsolute() && !registered.ReferencesParent() &&
         base::FilePath::CompareEqualIgnoreCase(
             registered.NormalizePathSeparators()
                 .StripTrailingSeparators()
                 .value(),
             installation.NormalizePathSeparators()
                 .StripTrailingSeparators()
                 .value());
}

bool PathsOverlap(const base::FilePath& first, const base::FilePath& second) {
  const auto left = first.NormalizePathSeparators().StripTrailingSeparators();
  const auto right = second.NormalizePathSeparators().StripTrailingSeparators();
  return SameInstallDirectory(left, right) || left.IsParent(right) ||
         right.IsParent(left);
}

bool HasPlainAncestors(base::FilePath path) {
  while (!path.empty()) {
    const DWORD attributes = ::GetFileAttributesW(path.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
      return false;
    }
    const base::FilePath parent = path.DirName();
    if (parent == path) {
      break;
    }
    path = parent;
  }
  return true;
}

// Copy into a private setup source, leaving the downloaded input intact for
// retries. Metadata is handed to setup separately, never installed as payload.
bool CopyPayload(const base::FilePath& source,
                 const base::FilePath& destination,
                 bool version_payload) {
  if (!IsPlainDirectory(source) || !base::CreateDirectory(destination)) {
    return false;
  }
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
        !HasNoReparsePoints(source, relative)) {
      return false;
    }
    const auto components = relative.GetComponents();
    if (version_payload &&
        (base::FilePath::CompareEqualIgnoreCase(components.front(),
                                                L".xenon-root") ||
         (components.size() == 1 &&
          base::FilePath::CompareEqualIgnoreCase(components.front(),
                                                 L".xenon-deletions.json")))) {
      continue;
    }
    if (!(version_payload ? IsSafeRelativePath(relative)
                          : IsSafeRootPath(relative))) {
      return false;
    }
    const base::FilePath output = destination.Append(relative);
    if (entries.GetInfo().IsDirectory()) {
      if (!base::CreateDirectory(output)) {
        return false;
      }
    } else if (!IsPlainFile(item) || !base::CreateDirectory(output.DirName()) ||
               !base::CopyFile(item, output)) {
      return false;
    }
  }
  return entries.GetError() == base::File::FILE_OK;
}

base::FilePath InstallationFromSetup(const base::FilePath& setup) {
  // Old setup may have been removed by version cleanup. Its registered layout
  // is still useful for identifying the installation scope.
  if (!setup.IsAbsolute() || setup.ReferencesParent() ||
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
    // Product registration stores a raw path; ARP stores a quoted command.
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

struct InstallationOptions {
  bool system_level = false;
  std::map<std::string, std::wstring> switches;
};

bool MergeInstallationArguments(const base::CommandLine& registered,
                                InstallationOptions* options) {
  if (registered.HasSwitch("system-level") && !options->system_level) {
    return false;
  }
  // Never replay arbitrary uninstall/cleanup actions from registration.
  for (const char* name :
       {"channel", "chrome-beta", "chrome-dev", "chrome-sxs", "msi"}) {
    if (!registered.HasSwitch(name)) {
      continue;
    }
    const std::wstring value = registered.GetSwitchValueNative(name);
    if (std::string_view(name) != "channel" && !value.empty()) {
      return false;
    }
    auto [it, inserted] = options->switches.emplace(name, value);
    if (!inserted && it->second != value) {
      return false;
    }
  }
  int mode_count = 0;
  for (const char* mode : {"chrome-beta", "chrome-dev", "chrome-sxs"}) {
    mode_count += options->switches.contains(mode);
  }
  if (mode_count > 1) {
    return false;
  }
  const auto channel = options->switches.find("channel");
  if (channel != options->switches.end()) {
    for (const auto& [mode, expected] :
         {std::pair{"chrome-beta", L"beta"}, std::pair{"chrome-dev", L"dev"},
          std::pair{"chrome-sxs", L"canary"}}) {
      if (options->switches.contains(mode) && channel->second != expected) {
        return false;
      }
    }
  }
  return true;
}

std::optional<InstallationOptions> ResolveInstallationOptions(
    const base::FilePath& installation,
    const base::FilePath& launcher_name) {
  const std::wstring product = launcher_name.RemoveFinalExtension().value();
  if (product.empty()) {
    return std::nullopt;
  }
  const std::wstring app_path = L"Software\\" + product;
  const std::wstring uninstall_path =
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" + product;
  std::optional<InstallationOptions> result;
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    InstallationOptions options;
    options.system_level = root == HKEY_LOCAL_MACHINE;
    bool matched = false;
    for (bool uninstall : {false, true}) {
      const auto& key_path = uninstall ? uninstall_path : app_path;
      for (REGSAM view : {KEY_WOW64_32KEY, KEY_WOW64_64KEY}) {
        base::win::RegKey key;
        if (key.Open(root, key_path.c_str(), KEY_QUERY_VALUE | view) !=
                ERROR_SUCCESS ||
            !RegistrationMatches(key, installation, uninstall)) {
          continue;
        }
        matched = true;
        std::wstring value;
        const wchar_t* argument_value =
            uninstall ? L"UninstallString" : L"UninstallArguments";
        if (key.ReadValue(argument_value, &value) == ERROR_SUCCESS &&
            !value.empty()) {
          const auto command = base::CommandLine::FromString(
              uninstall ? value : L"setup.exe " + value);
          if (!MergeInstallationArguments(command, &options)) {
            return std::nullopt;
          }
        }
      }
    }
    if (matched) {
      if (result) {
        // A single physical installation cannot be both per-user and system.
        return std::nullopt;
      }
      result = std::move(options);
    }
  }
  return result;
}

void AppendInstallationOptions(const InstallationOptions& options,
                               base::CommandLine* command) {
  if (options.system_level) {
    command->AppendSwitch("system-level");
  }
  for (const auto& [name, value] : options.switches) {
    command->AppendSwitchNative(name, value);
  }
}

base::RepeatingCallback<int(const base::CommandLine&)>& SetupRunner() {
  static base::NoDestructor<
      base::RepeatingCallback<int(const base::CommandLine&)>>
      runner;
  return *runner;
}

int RunSetup(const base::CommandLine& command) {
  if (SetupRunner()) {
    return SetupRunner().Run(command);
  }
  base::LaunchOptions options;
  options.current_directory = command.GetProgram().DirName();
  options.start_hidden = true;
  base::Process process = base::LaunchProcess(command, options);
  int exit_code = -1;
  if (!process.IsValid() || !process.WaitForExit(&exit_code)) {
    return -1;
  }
  return exit_code;
}

}  // namespace

void XenonUpdateInstaller::CleanupRecoveryDirectories(
    const base::FilePath& install_dir,
    const std::string& current_version) {
  const base::Version version(current_version);
  if (!version.IsValid() ||
      !IsPlainDirectory(install_dir.AppendASCII(current_version))) {
    return;
  }
  base::win::ScopedHandle install_lock = AcquireInstallationLock(install_dir);
  if (!install_lock.is_valid()) {
    return;
  }
  std::vector<base::FilePath> recoveries;
  base::FileEnumerator entries(
      install_dir, false, base::FileEnumerator::DIRECTORIES,
      FILE_PATH_LITERAL(".xenon-recovery-*"),
      base::FileEnumerator::FolderSearchPolicy::ALL,
      base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
  for (base::FilePath item = entries.Next(); !item.empty();
       item = entries.Next()) {
    recoveries.push_back(item);
  }
  if (entries.GetError() != base::File::FILE_OK) {
    return;
  }
  for (const auto& recovery : recoveries) {
    CleanupCommittedRecoveryDirectory(recovery, version);
  }
}

void XenonUpdateInstaller::SetSetupRunnerForTesting(
    base::RepeatingCallback<int(const base::CommandLine&)> runner) {
  SetupRunner() = std::move(runner);
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
  auto fail = [&](const std::string& message) {
    LOG(ERROR) << "[XenonUpdateInstaller] " << message;
    if (error) {
      *error = message;
    }
    return false;
  };
  // Standalone installers retain their existing asynchronous handoff contract.
  // The synchronous directory-source setup contract below is only for ZIP and
  // reconstructed differential packages.
  if (IsPlainFile(staged_path) &&
      staged_path.MatchesExtension(FILE_PATH_LITERAL(".exe"))) {
    base::CommandLine installer_command(staged_path);
    installer_command.AppendSwitch("silent");
    base::LaunchOptions launch_options;
    launch_options.start_hidden = true;
    base::Process process =
        base::LaunchProcess(installer_command, launch_options);
    return process.IsValid() || fail("Cannot launch the installer executable.");
  }
  if (!staged_path.IsAbsolute() || staged_path.ReferencesParent() ||
      !target_install_dir.IsAbsolute() ||
      target_install_dir.ReferencesParent() ||
      !IsPlainDirectory(target_install_dir) ||
      !HasPlainAncestors(staged_path) ||
      !HasPlainAncestors(target_install_dir) ||
      PathsOverlap(staged_path, target_install_dir)) {
    return fail(
        "Update source and installation must be separate existing "
        "paths without links.");
  }

  base::FilePath target_exe_name = relaunch_executable.BaseName();
  if (target_exe_name.empty()) {
    base::FilePath current_exe;
    if (!base::PathService::Get(base::FILE_EXE, &current_exe)) {
      return fail("Cannot identify the installed browser executable.");
    }
    target_exe_name = current_exe.BaseName();
  }
  if (!IsSafeRootPath(target_exe_name) ||
      !target_exe_name.MatchesExtension(FILE_PATH_LITERAL(".exe"))) {
    return fail("Invalid installed browser executable.");
  }
  const auto installation =
      ResolveInstallationOptions(target_install_dir, target_exe_name);
  if (!installation) {
    return fail("Cannot uniquely identify the registered installation.");
  }
  if (installation->system_level && relaunch_executable.empty()) {
    const auto token = base::win::AccessToken::FromCurrentProcess();
    if (!token || !token->IsElevated()) {
      return fail(
          "System installation requires an explicit restart to request "
          "administrator approval.");
    }
  }

  const base::FilePath target_exe = target_install_dir.Append(target_exe_name);
  base::ScopedTempDir temporary;
  base::FilePath setup_executable;
  base::FilePath source;
  base::FilePath version_name;
  const bool directory_source = IsPlainDirectory(staged_path);
  if (directory_source) {
    if (!temporary.CreateUniqueTempDir() ||
        PathsOverlap(temporary.GetPath(), target_install_dir) ||
        PathsOverlap(temporary.GetPath(), staged_path)) {
      return fail("Cannot create an external setup workspace.");
    }
    const bool differential_source =
        base::Version(staged_path.BaseName().MaybeAsASCII()).IsValid() &&
        IsPlainDirectory(staged_path.AppendASCII(".xenon-root"));
    if (differential_source) {
      source = temporary.GetPath().AppendASCII("Chrome-bin");
      version_name = staged_path.BaseName();
      if (!CopyPayload(staged_path, source.Append(version_name), true) ||
          !CopyPayload(staged_path.AppendASCII(".xenon-root"), source, false)) {
        return fail("Cannot prepare the differential package for setup.");
      }
      const base::FilePath deletions =
          staged_path.AppendASCII(".xenon-deletions.json");
      if (base::PathExists(deletions) &&
          (!IsPlainFile(deletions) ||
           !base::CopyFile(deletions,
                           source.AppendASCII(".xenon-deletions.json")))) {
        return fail("Cannot prepare the package deletion manifest.");
      }
    } else {
      source = staged_path;
      if (!IsPlainFile(source.Append(target_exe_name)) &&
          IsPlainDirectory(source.AppendASCII("Chrome-bin"))) {
        source = source.AppendASCII("Chrome-bin");
      }
      base::FileEnumerator versions(
          source, false, base::FileEnumerator::DIRECTORIES,
          base::FilePath::StringType(),
          base::FileEnumerator::FolderSearchPolicy::ALL,
          base::FileEnumerator::ErrorPolicy::STOP_ENUMERATION);
      for (base::FilePath version = versions.Next(); !version.empty();
           version = versions.Next()) {
        if (base::Version(version.BaseName().MaybeAsASCII()).IsValid()) {
          if (!version_name.empty() || !IsPlainDirectory(version)) {
            return fail("Full package contains ambiguous version directories.");
          }
          version_name = version.BaseName();
        }
      }
      if (versions.GetError() != base::File::FILE_OK || version_name.empty()) {
        return fail("Full package has no readable version directory.");
      }
    }
    const base::FilePath setup_relative =
        version_name.AppendASCII("Installer").AppendASCII("setup.exe");
    const base::FilePath packaged_setup = source.Append(setup_relative);
    if (!IsPlainFile(source.Append(target_exe_name)) ||
        !HasNoReparsePoints(source, setup_relative) ||
        !IsPlainFile(packaged_setup)) {
      return fail(
          "Update package must include its launcher and new setup.exe.");
    }
    // Setup moves its private payload and installs its own executable. Run a
    // separate copy so neither the source tree nor the installed setup is busy.
    setup_executable = temporary.GetPath().AppendASCII("setup.exe");
    if (!base::CopyFile(packaged_setup, setup_executable)) {
      return fail("Cannot prepare the new setup executable.");
    }
  } else {
    return fail("Unsupported Windows update package.");
  }

  base::CommandLine setup_command(setup_executable);
  AppendInstallationOptions(*installation, &setup_command);
  setup_command.AppendSwitchPath("install-directory", target_install_dir);
  setup_command.AppendSwitch("do-not-launch-chrome");
  setup_command.AppendSwitch("do-not-register-for-update-launch");
  setup_command.AppendSwitch("silent");
  if (directory_source) {
    setup_command.AppendSwitchPath("install-source-dir", source);
  }
  // Setup owns SetupSingleton, elevation, file rollback and OS registration.
  // In particular, do not acquire the legacy writable installation lock here.
  const int exit_code = RunSetup(setup_command);
  if (exit_code != 0) {
    return fail(exit_code == -1 ? "Cannot start or wait for setup.exe."
                                : "setup.exe failed with exit code " +
                                      base::NumberToString(exit_code) + ".");
  }
  if (!IsPlainFile(target_exe) ||
      base::PathExists(target_install_dir.AppendASCII("new_chrome.exe")) ||
      (directory_source &&
       (!IsPlainDirectory(target_install_dir.Append(version_name)) ||
        !HasNoReparsePoints(
            target_install_dir,
            version_name.AppendASCII("Installer").AppendASCII("setup.exe")) ||
        !IsPlainFile(target_install_dir.Append(version_name)
                         .AppendASCII("Installer")
                         .AppendASCII("setup.exe"))))) {
    return fail(
        "Setup exited successfully but the installed version or "
        "launcher is not ready.");
  }

  if (!relaunch_executable.empty()) {
    base::CommandLine relaunch_cmd = *base::CommandLine::ForCurrentProcess();
    relaunch_cmd.SetProgram(target_exe);
    base::LaunchOptions launch_options;
    launch_options.current_directory = target_install_dir;
    launch_options.grant_foreground_privilege = true;

    HANDLE raw_handle = nullptr;
    if (!::DuplicateHandle(::GetCurrentProcess(), ::GetCurrentProcess(),
                           ::GetCurrentProcess(), &raw_handle, SYNCHRONIZE,
                           /*bInheritHandle=*/TRUE, 0)) {
      return fail(
          "Update installed, but the restart handoff failed. Restart "
          "the browser manually.");
    }
    base::win::ScopedHandle parent_handle(raw_handle);
    relaunch_cmd.AppendSwitchASCII(
        switches::kWaitForParentHandle,
        base::NumberToString(base::win::HandleToUint32(parent_handle.get())));
    launch_options.handles_to_inherit.push_back(parent_handle.get());
    base::Process process = base::LaunchProcess(relaunch_cmd, launch_options);
    if (!process.IsValid()) {
      return fail(
          "Update installed, but the browser could not restart. Restart "
          "the browser manually.");
    }
  }
  return true;
}

}  // namespace xenon::updater
