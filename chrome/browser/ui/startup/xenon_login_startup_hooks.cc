// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/startup/xenon_login_startup_hooks.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/startup/startup_browser_creator.h"

namespace xenon_login_startup_hooks {
namespace {

const XenonLoginStartupHooks* g_hooks = nullptr;

}  // namespace

void SetXenonLoginStartupHooks(const XenonLoginStartupHooks* hooks) {
  g_hooks = hooks;
}

bool MaybeDeferLaunchBrowserForLastProfiles(
    StartupBrowserCreator* creator,
    const base::CommandLine& command_line,
    const base::FilePath& cur_dir,
    chrome::startup::IsProcessStartup process_startup,
    chrome::startup::IsFirstRun is_first_run,
    StartupProfileInfo profile_info,
    const std::vector<Profile*>& last_opened_profiles,
    bool restore_tabbed_browser) {
  if (!g_hooks || !g_hooks->maybe_defer_launch_browser_for_last_profiles) {
    return false;
  }
  return g_hooks->maybe_defer_launch_browser_for_last_profiles(
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
  if (!g_hooks || !g_hooks->maybe_defer_single_browser_launch) {
    return false;
  }
  return g_hooks->maybe_defer_single_browser_launch(
      creator, command_line, profile, cur_dir, process_startup, is_first_run,
      restore_tabbed_browser);
}

}  // namespace xenon_login_startup_hooks
