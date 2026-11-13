// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_LAUNCH_ARGUMENTS_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_LAUNCH_ARGUMENTS_H_

#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"

namespace xenon::ipc {

// Builds packaged Electron process.argv from the Browser's explicitly
// separated launch arguments. Only tokens after the first standalone "--"
// are application arguments. Call only for the command-line app's container;
// sidebar applications must not inherit this Browser-wide launch request.
// Values are as parsed by Chromium's CommandLine; no reparsing or trimming is
// performed here. argv[0] is the trusted executable identity supplied by the
// Browser, not a utility executable or a fabricated app-specific filename.
std::vector<std::string> BuildElectronAppLaunchArgv(
    const base::CommandLine& browser_command_line,
    const base::FilePath& executable_path);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_LAUNCH_ARGUMENTS_H_
