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

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_APP_RUNTIME_H_
