// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "xenon_overlay/chrome/browser/xenon_login_startup_registration.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/startup/startup_browser_creator.h"
#include "chrome/browser/ui/startup/startup_types.h"
#include "chrome/browser/ui/startup/xenon_login_startup_hooks.h"
#include "xenon_overlay/chrome/browser/xenon_login_controller.h"

namespace xenon {
namespace {

bool MaybeDeferLaunchBrowserForLastProfiles(
    StartupBrowserCreator* creator,
    const base::CommandLine& command_line,
    const base::FilePath& cur_dir,
    chrome::startup::IsProcessStartup process_startup,
    chrome::startup::IsFirstRun is_first_run,
    StartupProfileInfo profile_info,
    const std::vector<Profile*>& last_opened_profiles,
    bool restore_tabbed_browser) {
  return XenonLoginController::GetInstance()
      ->MaybeDeferLaunchBrowserForLastProfiles(
          creator, command_line, cur_dir, process_startup, is_first_run,
          profile_info, last_opened_profiles, restore_tabbed_browser);
}

bool MaybeDeferSingleBrowserLaunch(
    StartupBrowserCreator* creator,
    const base::CommandLine& command_line,
    Profile* profile,
    const base::FilePath& cur_dir,
    chrome::startup::IsProcessStartup process_startup,
    chrome::startup::IsFirstRun is_first_run,
    bool restore_tabbed_browser) {
  return XenonLoginController::GetInstance()->MaybeDeferSingleBrowserLaunch(
      creator, command_line, profile, cur_dir, process_startup, is_first_run,
      restore_tabbed_browser);
}

const xenon_login_startup_hooks::XenonLoginStartupHooks k_login_startup_hooks =
    {
        &MaybeDeferLaunchBrowserForLastProfiles,
        &MaybeDeferSingleBrowserLaunch,
};

}  // namespace

void RegisterXenonLoginStartupHooks() {
  xenon_login_startup_hooks::SetXenonLoginStartupHooks(&k_login_startup_hooks);
}

}  // namespace xenon
