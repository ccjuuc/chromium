// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_UI_STARTUP_XENON_LOGIN_STARTUP_HOOKS_H_
#define CHROME_BROWSER_UI_STARTUP_XENON_LOGIN_STARTUP_HOOKS_H_

#include <vector>

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "chrome/browser/ui/startup/startup_types.h"

class Profile;
class StartupBrowserCreator;
struct StartupProfileInfo;

namespace xenon_login_startup_hooks {

// Optional callbacks installed by the Xenon overlay. nullptr entries are
// ignored. The struct pointer must remain valid for the process lifetime.
struct XenonLoginStartupHooks {
  bool (*maybe_defer_launch_browser_for_last_profiles)(
      StartupBrowserCreator* creator,
      const base::CommandLine& command_line,
      const base::FilePath& cur_dir,
      chrome::startup::IsProcessStartup process_startup,
      chrome::startup::IsFirstRun is_first_run,
      StartupProfileInfo profile_info,
      const std::vector<Profile*>& last_opened_profiles,
      bool restore_tabbed_browser) = nullptr;

  bool (*maybe_defer_single_browser_launch)(
      StartupBrowserCreator* creator,
      const base::CommandLine& command_line,
      Profile* profile,
      const base::FilePath& cur_dir,
      chrome::startup::IsProcessStartup process_startup,
      chrome::startup::IsFirstRun is_first_run,
      bool restore_tabbed_browser) = nullptr;
};

// Pass nullptr to clear all hooks (tests / teardown only).
void SetXenonLoginStartupHooks(const XenonLoginStartupHooks* hooks);

bool MaybeDeferLaunchBrowserForLastProfiles(
    StartupBrowserCreator* creator,
    const base::CommandLine& command_line,
    const base::FilePath& cur_dir,
    chrome::startup::IsProcessStartup process_startup,
    chrome::startup::IsFirstRun is_first_run,
    StartupProfileInfo profile_info,
    const std::vector<Profile*>& last_opened_profiles,
    bool restore_tabbed_browser);

bool MaybeDeferSingleBrowserLaunch(
    StartupBrowserCreator* creator,
    const base::CommandLine& command_line,
    Profile* profile,
    const base::FilePath& cur_dir,
    chrome::startup::IsProcessStartup process_startup,
    chrome::startup::IsFirstRun is_first_run,
    bool restore_tabbed_browser);

}  // namespace xenon_login_startup_hooks

#endif  // CHROME_BROWSER_UI_STARTUP_XENON_LOGIN_STARTUP_HOOKS_H_
