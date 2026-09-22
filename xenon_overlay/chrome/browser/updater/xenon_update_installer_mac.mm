// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_installer.h"

#include <sys/stat.h>
#include <unistd.h>

#include "base/apple/bundle_locations.h"
#include "base/apple/foundation_util.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/logging.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/process/process_handle.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/unguessable_token.h"

namespace xenon::updater {

std::string XenonUpdateInstaller::ReadBundleShortVersion(
    const base::FilePath& bundle_path) {
  if (bundle_path.empty()) {
    return std::string();
  }
  base::FilePath plist_path =
      bundle_path.Append("Contents").Append("Info.plist");
  @autoreleasepool {
    NSDictionary* info = [NSDictionary dictionaryWithContentsOfFile:
        base::apple::FilePathToNSString(plist_path)];
    NSString* version = base::apple::ObjCCast<NSString>(
        info[@"CFBundleShortVersionString"]);
    if (version.length == 0) {
      version = base::apple::ObjCCast<NSString>(info[@"CFBundleVersion"]);
    }
    return base::SysNSStringToUTF8(version);
  }
}

bool XenonUpdateInstaller::InstallAndRelaunch(
    const base::FilePath& staged_path,
    const base::FilePath& target_install_dir,
    const base::FilePath& relaunch_executable) {
  if (!base::PathExists(staged_path)) {
    LOG(ERROR) << "[XenonUpdateInstaller] Staged path does not exist: "
               << staged_path.value();
    return false;
  }

  // 1. Resolve effective target application bundle path
  base::FilePath target_app = target_install_dir;
  if (target_app.empty() || target_app.Extension() != FILE_PATH_LITERAL(".app")) {
    base::FilePath outer_bundle = base::apple::OuterBundlePath();
    if (!outer_bundle.empty()) {
      target_app = outer_bundle;
    }
  }

  // 2. Identify staged bundle (.app directory)
  base::FilePath staged_app = staged_path;
  if (staged_path.Extension() != FILE_PATH_LITERAL(".app") &&
      base::DirectoryExists(staged_path)) {
    // If staged_path is a directory containing an .app bundle, find it
    base::FileEnumerator enumerator(staged_path, false,
                                    base::FileEnumerator::DIRECTORIES,
                                    FILE_PATH_LITERAL("*.app"));
    base::FilePath found = enumerator.Next();
    if (!found.empty()) {
      staged_app = found;
    }
  }

  if (!base::DirectoryExists(staged_app)) {
    LOG(ERROR) << "[XenonUpdateInstaller] Staged update is not an app bundle: "
               << staged_app.value();
    return false;
  }

  // 3. Codesign verification. Unsigned local builds continue; a bundle that
  // already has a signature must verify before it replaces the installed app.
  base::FilePath codesign_bin("/usr/bin/codesign");
  base::FilePath signature_dir =
      staged_app.Append("Contents").Append("_CodeSignature");
  if (base::PathExists(codesign_bin) && base::DirectoryExists(signature_dir)) {
    base::CommandLine verify_cmd(codesign_bin);
    verify_cmd.AppendArg("--verify");
    verify_cmd.AppendArg("--deep");
    verify_cmd.AppendArg("--strict");
    verify_cmd.AppendArgPath(staged_app);

    std::string verify_output;
    int exit_code = -1;
    if (!base::GetAppOutputWithExitCode(verify_cmd, &verify_output, &exit_code) ||
        exit_code != 0) {
      LOG(ERROR) << "[XenonUpdateInstaller] Codesign verification failed ("
                 << exit_code << "): " << verify_output;
      return false;
    }
    VLOG(1) << "[XenonUpdateInstaller] Codesign verification passed for "
            << staged_app.value();
  }

  // 4. Generate transient atomic replacement helper script
  base::FilePath temp_dir;
  if (!base::GetTempDir(&temp_dir)) {
    temp_dir = base::FilePath("/tmp");
  }

  int current_pid = base::GetCurrentProcId();
  base::FilePath script_path = temp_dir.Append(
      "xenon_mac_update_" + base::NumberToString(current_pid) + "_" +
      base::UnguessableToken::Create().ToString() + ".sh");

  std::string script_content = "#!/bin/bash\n";
  script_content += "# Xenon macOS Atomic Swap & Relaunch Helper\n";
  script_content += "PID=" + base::NumberToString(current_pid) + "\n";
  script_content += "TARGET=\"" + target_app.value() + "\"\n";
  script_content += "STAGED=\"" + staged_app.value() + "\"\n";

  // Wait cooperatively for parent browser process to exit completely
  script_content += "while kill -0 \"$PID\" 2>/dev/null; do\n";
  script_content += "  sleep 0.1\n";
  script_content += "done\n";

  // Perform atomic swap via POSIX mv
  script_content += "if [ -d \"$STAGED\" ] && [ -n \"$TARGET\" ]; then\n";
  script_content += "  rm -rf \"$TARGET.old\"\n";
  script_content += "  mv \"$TARGET\" \"$TARGET.old\" 2>/dev/null\n";
  script_content += "  mv \"$STAGED\" \"$TARGET\"\n";
  script_content += "  xattr -d -r com.apple.quarantine \"$TARGET\" 2>/dev/null || true\n";
  script_content += "  touch \"$TARGET\"\n";
  script_content += "  rm -rf \"$TARGET.old\"\n";
  script_content += "fi\n";

  // Relaunch the upgraded application if requested
  if (!relaunch_executable.empty()) {
    script_content += "/usr/bin/open -n \"$TARGET\"\n";
  }

  // Self-remove the script file
  script_content += "rm -f \"$0\"\n";

  if (!base::WriteFile(script_path, script_content)) {
    LOG(ERROR) << "[XenonUpdateInstaller] Failed to write macOS update script: "
               << script_path.value();
    return false;
  }

  chmod(script_path.value().c_str(), 0755);

  base::CommandLine launch_cmd(script_path);
  base::LaunchOptions launch_options;
  launch_options.new_process_group = true;
  base::Process process = base::LaunchProcess(launch_cmd, launch_options);
  if (!process.IsValid()) {
    LOG(ERROR) << "[XenonUpdateInstaller] Failed to launch macOS update script";
    base::DeleteFile(script_path);
    return false;
  }

  LOG(INFO) << "[XenonUpdateInstaller] Launched macOS atomic swap script (PID "
            << process.Pid() << ") for target " << target_app.value();
  return true;
}

}  // namespace xenon::updater
