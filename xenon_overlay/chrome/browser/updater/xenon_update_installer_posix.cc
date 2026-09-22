// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#include "base/logging.h"

namespace xenon::updater {

bool XenonUpdateInstaller::InstallAndRelaunch(
    const base::FilePath& staged_path,
    const base::FilePath& target_install_dir,
    const base::FilePath& relaunch_executable) {
  LOG(WARNING) << "[XenonUpdateInstaller] InstallAndRelaunch not implemented on this POSIX platform";
  return false;
}

}  // namespace xenon::updater
