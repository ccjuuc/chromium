// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_launch_arguments.h"

#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"

namespace xenon::ipc {

std::vector<std::string> BuildElectronAppLaunchArgv(
    const base::CommandLine& browser_command_line,
    const base::FilePath& executable_path) {
  std::vector<std::string> result{executable_path.AsUTF8Unsafe()};
  bool found_separator = false;
  const auto& argv = browser_command_line.argv();
  for (size_t index = 1; index < argv.size(); ++index) {
    const auto& argument = argv[index];
    if (!found_separator) {
      found_separator = argument == FILE_PATH_LITERAL("--");
      continue;
    }
#if BUILDFLAG(IS_WIN)
    result.push_back(base::WideToUTF8(argument));
#else
    result.push_back(argument);
#endif
  }
  return result;
}

}  // namespace xenon::ipc
