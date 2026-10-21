// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_BUNDLE_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_BUNDLE_H_

#include <string>

#include "base/files/file_path.h"

namespace xenon::updater {

// Reconstructs a complete application from a mac schema_version=2 manifest.
// All paths must be absolute. |output_app| must not exist, its parent must
// exist, and it must not overlap either input tree. The caller must own the
// staging directory exclusively for the duration of this blocking operation.
// No installed files are modified. On failure the newly created output is
// removed on a best-effort basis and |error| describes the failure.
//
// Regular files require a final SHA-256, size, and mode. Directories must be
// listed explicitly; symbolic links are reconstructed after all regular files
// and must resolve inside the manifest's tree. POSIX modes and links are
// supported on POSIX; Windows supports regular-file reconstruction for tests.
bool ReconstructMacUpdateBundle(const base::FilePath& current_app,
                                const base::FilePath& unpacked_patch,
                                const base::FilePath& output_app,
                                const std::string& expected_base_version,
                                const std::string& expected_target_version,
                                std::string* error);

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_BUNDLE_H_
