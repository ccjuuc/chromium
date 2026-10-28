// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_IPC_XENON_APP_DESCRIPTOR_PATHS_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_APP_DESCRIPTOR_PATHS_H_

#include <string_view>
#include <vector>

#include "base/files/file_path.h"

namespace xenon::ipc {

// Returns host descriptor directories in priority order. Bundled macOS hosts
// use outer Contents/Resources/xenon-apps before the legacy executable
// directory. Other hosts retain the executable directory. Call on the main
// thread.
std::vector<base::FilePath> GetElectronAppDescriptorDirectories();

// Returns the first existing descriptor with this basename, or an empty path.
// Explicit application/configuration paths bypass this default discovery.
base::FilePath FindElectronAppDescriptor(std::string_view basename);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_COMMON_IPC_XENON_APP_DESCRIPTOR_PATHS_H_
