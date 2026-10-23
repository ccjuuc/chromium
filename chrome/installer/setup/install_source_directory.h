// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_INSTALLER_SETUP_INSTALL_SOURCE_DIRECTORY_H_
#define CHROME_INSTALLER_SETUP_INSTALL_SOURCE_DIRECTORY_H_

#include <vector>

#include "base/files/file_path.h"
#include "base/types/expected.h"
#include "chrome/installer/util/util_constants.h"

namespace base {
class CommandLine;
}

namespace installer {

namespace switches {
inline constexpr char kInstallSourceDir[] = "install-source-dir";
}

// Directory input requires an explicit matching installation directory and
// cannot be combined with an archive or uninstall operation. Both directories
// must exist, be disjoint, and contain no reparse points in their ancestors.
base::expected<void, InstallStatus> ValidateInstallSourceDirectoryCommandLine(
    const base::CommandLine& command_line,
    const base::FilePath& target_path);

// Copies an independently staged application into unpack_path/Chrome-bin. The
// caller owns the empty unpack_path and its cleanup. Input is never modified.
// Requires exactly this setup's compiled version, a launcher and its DLL.
// Removes the optional .xenon-deletions.json metadata from the private copy and
// returns validated relative root-file deletions for the install transaction.
base::expected<std::vector<base::FilePath>, InstallStatus>
PrepareInstallSourceDirectory(const base::CommandLine& command_line,
                              const base::FilePath& target_path,
                              const base::FilePath& unpack_path);

// Rechecks a previously validated deletion immediately before its work item.
// Missing files are harmless; directories and reparse points are rejected.
bool IsSafeInstallSourceDeletion(const base::FilePath& target_path,
                                 const base::FilePath& relative_path);

}  // namespace installer

#endif  // CHROME_INSTALLER_SETUP_INSTALL_SOURCE_DIRECTORY_H_
