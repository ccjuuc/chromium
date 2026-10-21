// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_INSTALLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_INSTALLER_H_

#include <memory>
#include <string>

#include "base/files/file_path.h"
#include "build/build_config.h"

namespace xenon::updater {

#if BUILDFLAG(IS_MAC)
// Carries a verified installation identity across a failed install and its full
// package retry. Never persisted to disk; access only from sequential workers.
class MacUpdateInstallContext {
 public:
  MacUpdateInstallContext();
  ~MacUpdateInstallContext();

 private:
  friend class XenonUpdateInstaller;
  struct State;
  std::unique_ptr<State> state_;
};
#endif

class XenonUpdateInstaller {
 public:
#if BUILDFLAG(IS_MAC)
  // Blocking operations: call on a MayBlock worker. The returned bundle lives
  // in a unique private directory beneath staging_root, owned by the caller.
  // Package scripts are never used. target_app_name must be a .app basename.
  static base::FilePath PrepareMacFullUpdate(
      const base::FilePath& downloaded_file,
      const base::FilePath& staging_root,
      const base::FilePath& target_app_name,
      std::string* error_detail);

  // Validates the installed and staged Developer ID signatures, identity and
  // version, then synchronously runs Chromium's trusted versioned installer.
  // An optional context retains the verified original identity after a failed
  // write so a full-package retry can repair an incomplete resource seal.
  // Returns true only after installation succeeds. Does not quit or relaunch.
  static bool InstallMacUpdate(
      const base::FilePath& staged_bundle,
      const base::FilePath& target_install_dir,
      const std::string& target_version,
      std::string* error_detail,
      MacUpdateInstallContext* retry_context = nullptr);
#else
  // Prepares the platform-specific updater script/process, launches it,
  // and requests the current application to exit so replacement can occur.
  // |staged_path| can be either a staged directory with new files, or an
  // installer binary. |target_install_dir| is the root directory of the
  // currently running application. |relaunch_executable| is the path to the
  // main executable to restart.
  static bool InstallAndRelaunch(const base::FilePath& staged_path,
                                 const base::FilePath& target_install_dir,
                                 const base::FilePath& relaunch_executable);
#endif
};

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_INSTALLER_H_
