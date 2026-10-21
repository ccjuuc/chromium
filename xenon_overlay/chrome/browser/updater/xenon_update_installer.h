// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_INSTALLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_INSTALLER_H_

#include <string>

#include "base/files/file_path.h"
#include "base/functional/callback_forward.h"

namespace xenon::updater {

class XenonUpdateInstaller {
 public:
  struct InstallerPackagePreparation {
    base::FilePath path;
    std::string error;
  };

  enum class PrivilegedInstallStatus {
    kReadyToQuit,
    kRequiresApproval,
    kFailed
  };
  struct PrivilegedInstallResult {
    PrivilegedInstallStatus status = PrivilegedInstallStatus::kFailed;
    std::string error;
  };

  // macOS only. Read-only MayBlock capability checks. Differential updates
  // need direct ownership or the formally signed privileged service capability.
  static bool CanInstallDifferentially(const base::FilePath& target,
                                       std::string* error);
  static bool RequiresElevation(const base::FilePath& target);

  // Explicit UI installation only. Approval leaves the staged bundle intact.
  // Ready means a user helper is watching this browser, not that it installed.
  static void BeginPrivilegedInstallAndRelaunch(
      const base::FilePath& staged,
      const base::FilePath& target,
      const base::FilePath& relaunch,
      base::OnceCallback<void(PrivilegedInstallResult)> completion);

  // macOS only; MayBlock. Copies and re-verifies a downloaded flat PKG into a
  // private persistent user cache, independent of browser download cleanup.
  static InstallerPackagePreparation PrepareInstallerPackage(
      const base::FilePath& source,
      const std::string& expected_sha256,
      const base::FilePath& cache_root_override = {});

  // macOS only; call on the UI sequence. Opens the package in Apple's Installer
  // and replies on this sequence. true means handed off, never installed. The
  // browser remains running and the cached package remains available for retry.
  static void OpenInstallerPackage(
      const base::FilePath& package,
      base::OnceCallback<void(bool, std::string)> completion);

  // Prepares the platform-specific installation and returns whether the caller
  // can request normal application shutdown. On macOS the native helper waits
  // for this process to exit before atomically replacing the application.
  // |staged_path| can be either a staged directory with new files, or an
  // installer binary. |target_install_dir| is the root directory of the
  // currently running application. |relaunch_executable| is the path to the
  // main executable to restart.
  static bool InstallAndRelaunch(const base::FilePath& staged_path,
                                 const base::FilePath& target_install_dir,
                                 const base::FilePath& relaunch_executable,
                                 std::string* error = nullptr);

  // Windows only. Updates existing registrations belonging to |install_dir|
  // after a directory installation commits, or when that version starts.
  // Does not create registrations or finalize an asynchronous installer EXE.
  static void UpdateRegistryVersion(const base::FilePath& install_dir,
                                    const base::FilePath& executable,
                                    const std::string& version);

  // CFBundleShortVersionString of a macOS app bundle, then CFBundleVersion.
  // Empty when |bundle_path| has no Info.plist. Only implemented on macOS.
  static std::string ReadBundleShortVersion(const base::FilePath& bundle_path);

  // Restores the last failure at browser startup and confirms a pending launch
  // when the running version matches its target. Excludes superseded results.
  // Called on a MayBlock worker; only implemented on Mac.
  static std::string ReadLastInstallError(const base::FilePath& bundle_path,
                                          const std::string& current_version);
};

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_INSTALLER_H_
