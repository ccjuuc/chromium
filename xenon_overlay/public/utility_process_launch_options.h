// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_PUBLIC_UTILITY_PROCESS_LAUNCH_OPTIONS_H_
#define XENON_OVERLAY_PUBLIC_UTILITY_PROCESS_LAUNCH_OPTIONS_H_

#include <array>
#include <string>

#include "base/environment.h"
#include "base/files/file.h"
#include "base/files/file_path.h"

namespace xenon {

// Owns only launch-scoped state for one dedicated Electron utilityProcess.
// Browser supplies all three files (including /dev/null or NUL for ignore),
// and the launcher retains them until the OS has inherited/remapped them.
// No application-specific options or sandbox exemptions belong here.
struct UtilityProcessLaunchOptions {
  base::EnvironmentMap environment;
  base::FilePath working_directory;
  std::array<base::File, 3> stdio;

  bool Validate(std::string* error) const {
    if (!working_directory.IsAbsolute()) {
      *error = "ERR_INVALID_ARG_VALUE: utilityProcess cwd must be absolute";
      return false;
    }
    for (const auto& [key, value] : environment) {
      if (key.empty() || key.find('=') != key.npos ||
          key.find(base::NativeEnvironmentString::value_type()) != key.npos ||
          value.find(base::NativeEnvironmentString::value_type()) !=
              value.npos) {
        *error = "ERR_INVALID_ARG_VALUE: Invalid utilityProcess environment";
        return false;
      }
    }
    for (const auto& file : stdio) {
      if (!file.IsValid()) {
        *error =
            "ERR_INVALID_ARG_VALUE: utilityProcess requires three valid "
            "standard stream files";
        return false;
      }
    }
    return true;
  }
};

}  // namespace xenon

#endif  // XENON_OVERLAY_PUBLIC_UTILITY_PROCESS_LAUNCH_OPTIONS_H_
