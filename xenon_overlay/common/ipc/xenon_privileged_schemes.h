// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_IPC_XENON_PRIVILEGED_SCHEMES_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_PRIVILEGED_SCHEMES_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"

namespace base {
class CommandLine;
}

namespace xenon::ipc {

struct PrivilegedScheme {
  std::string scheme;
  bool standard = false;
  bool secure = false;
  bool bypass_csp = false;
  bool support_fetch_api = false;
  bool cors_enabled = false;

  bool operator==(const PrivilegedScheme&) const = default;
};
using PrivilegedSchemes = std::vector<PrivilegedScheme>;

// Parses Electron's [{scheme, privileges}] form without using GURL. This is
// safe before Chromium has initialized its URL scheme registry. Output is
// sorted and scheme names are canonical lowercase ASCII. Unsupported enabled
// privileges, reserved names, and conflicting duplicates fail atomically.
bool ParsePrivilegedSchemes(const base::Value& value,
                            PrivilegedSchemes* schemes,
                            std::string* error);

// Reads host-level descriptors and an explicitly selected JSON descriptor.
// Directories are ordered: the first copy of a descriptor basename wins.
// A release directory passed as explicit_application has no implicit metadata.
bool LoadPrivilegedSchemeDeclarations(
    const std::vector<base::FilePath>& host_directories,
    const base::FilePath& explicit_application,
    PrivilegedSchemes* schemes,
    std::string* error);

// Called exclusively by ContentClient::AddAdditionalSchemes, before URL scheme
// registries are locked. Browser reads descriptors; children consume the frozen
// browser snapshot and never read descriptors independently.
void InitializeStartupPrivilegedSchemes();
const PrivilegedSchemes& GetStartupPrivilegedSchemes();

// These functions do not mutate the snapshot or URL registries. Runtime main
// registration is validation of real startup declarations, never late mutation.
bool ValidatePrivilegedSchemesAgainstStartup(const base::ListValue& schemes,
                                             std::string* error);
void AppendStartupPrivilegedSchemes(base::CommandLine* command_line);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_COMMON_IPC_XENON_PRIVILEGED_SCHEMES_H_
