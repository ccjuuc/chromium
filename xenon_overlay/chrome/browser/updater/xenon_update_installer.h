// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_INSTALLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_INSTALLER_H_

#include <string>

#include "base/files/file_path.h"

namespace xenon::updater {

class XenonUpdateInstaller {
 public:
  // Prepares the platform-specific updater script/process, launches it,
  // and requests the current application to exit so replacement can occur.
  // |staged_path| can be either a staged directory with new files, or an installer binary.
  // |target_install_dir| is the root directory of the currently running application.
  // |relaunch_executable| is the path to the main executable to restart.
  static bool InstallAndRelaunch(
      const base::FilePath& staged_path,
      const base::FilePath& target_install_dir,
      const base::FilePath& relaunch_executable);

  // CFBundleShortVersionString of a macOS app bundle, then CFBundleVersion.
  // Empty when |bundle_path| has no Info.plist. Only implemented on macOS.
  static std::string ReadBundleShortVersion(const base::FilePath& bundle_path);
};

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_INSTALLER_H_
