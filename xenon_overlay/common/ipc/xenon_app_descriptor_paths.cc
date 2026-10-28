// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_app_descriptor_paths.h"

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "build/build_config.h"

#if BUILDFLAG(IS_MAC)
#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#endif

namespace xenon::ipc {

std::vector<base::FilePath> GetElectronAppDescriptorDirectories() {
  std::vector<base::FilePath> directories;
#if BUILDFLAG(IS_MAC)
  if (base::apple::AmIBundled()) {
    const auto bundle = base::apple::OuterBundlePath();
    if (!bundle.empty()) {
      // JSON belongs in the resource envelope, not Contents/MacOS code slots.
      directories.push_back(bundle.AppendASCII("Contents")
                                .AppendASCII("Resources")
                                .AppendASCII("xenon-apps"));
    }
  }
#endif
  base::FilePath executable_directory;
  if (base::PathService::Get(base::DIR_EXE, &executable_directory) &&
      (directories.empty() || directories.front() != executable_directory)) {
    directories.push_back(executable_directory);
  }
  return directories;
}

base::FilePath FindElectronAppDescriptor(std::string_view basename) {
  const auto filename = base::FilePath::FromUTF8Unsafe(basename);
  if (filename.empty() || filename.IsAbsolute() ||
      filename.ReferencesParent() || filename != filename.BaseName()) {
    return {};
  }
  for (const auto& directory : GetElectronAppDescriptorDirectories()) {
    const auto descriptor = directory.Append(filename);
    if (base::PathExists(descriptor)) {
      return descriptor;
    }
  }
  return {};
}

}  // namespace xenon::ipc
