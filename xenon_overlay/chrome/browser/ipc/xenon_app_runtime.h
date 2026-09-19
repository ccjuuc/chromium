// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_RUNTIME_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_RUNTIME_H_

#include <string>

#include "base/files/file_path.h"

namespace xenon::ipc {

// An embedder can retain the original packaged executable's identity without
// executing it. Empty selects the actual host; invalid explicit paths fail.
bool ResolveAppExecutable(const base::FilePath& requested,
                          base::FilePath* executable,
                          std::string* error);
std::string GetAppExecutableVersion(const base::FilePath& executable);

struct ElectronApplicationInfo {
  base::FilePath app_path;
  base::FilePath resources_directory;
  base::FilePath runtime_directory;
  // Absolute candidate from package.main (or index.js). CommonJS resolution
  // subsequently adds extensions or resolves a directory's package/index.
  base::FilePath main_script_path;
  std::string name;
  std::string version;
  bool is_packaged = false;
};

// Accepts a release directory, macOS .app bundle, resources directory, source
// application directory or ASAR. Encrypted archive keys must already be
// registered by the caller. Does not change process state or extract files.
bool ResolveElectronApplication(const base::FilePath& requested,
                                ElectronApplicationInfo* application,
                                std::string* error);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_RUNTIME_H_
