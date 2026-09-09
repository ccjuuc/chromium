// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_INSTALLER_MINI_INSTALLER_MINI_INSTALLER_UI_H_
#define CHROME_INSTALLER_MINI_INSTALLER_MINI_INSTALLER_UI_H_

#include <windows.h>

#include "chrome/installer/mini_installer/mini_installer.h"

namespace mini_installer {

class Configuration;

struct InstallerUiOptions {
  wchar_t install_path[MAX_PATH] = {};
  bool apply_options = false;
  bool system_level = false;
  bool create_desktop_shortcut = true;
  bool launch_after_install = true;
  bool make_default_browser = false;
  bool privacy_accepted = false;
};

// Progress stages for the installer UI status text.
enum InstallerUiStage {
  kInstallerUiStageExtractArchive = 1,  // Writing chrome.7z / packed.7z
  kInstallerUiStageExtractSetup = 2,    // Writing / expanding setup.exe
  kInstallerUiStageInstall = 3,         // Running setup.exe
  kInstallerUiStageFinishing = 4,       // Almost done
  kInstallerUiStageFailed = 5,          // Failed
};

#if defined(MINI_INSTALLER_HAS_DUILIB_UI) && MINI_INSTALLER_HAS_DUILIB_UI

// Uses the same localized display name as installed shortcuts.
// Safe to call more than once; subsequent calls are no-ops once initialized.
void InitInstallerBrandName(HMODULE module, const Configuration& configuration);

// Returns the brand name from InitInstallerBrandName (never null/empty).
const wchar_t* InstallerBrandName();

// Returns false for silent/quiet installs (no splash UI).
bool ShouldShowInstallerUi(const Configuration& configuration);

// Fills |options| for silent/quiet installs (and UI skin fallback):
// reuse the registry install path for the requested scope. Explicit
// --system-level is never downgraded; if it is omitted and only a machine
// install exists, that path/scope is reused. Never auto-launches.
// Returns false if a usable path cannot be determined.
bool PrepareSilentInstallerOptions(const Configuration& configuration,
                                   InstallerUiOptions* options);

// Posts a progress update to the installer UI (thread-safe).
void PostInstallerUiProgress(HWND hwnd, int percent, InstallerUiStage stage);

// Shows the DuiLib installer window. |work| starts after the user clicks
// Install and may report progress via PostInstallerUiProgress. The window
// stays open through unpack + setup, then shows a Finish button.
ProcessExitResult RunWithInstallerUi(
    HMODULE module,
    const Configuration& configuration,
    InstallerUiOptions* options,
    ProcessExitResult (*work)(void* ctx, HWND progress_hwnd),
    void* ctx);

#else

inline void InitInstallerBrandName(HMODULE, const Configuration&) {}

inline const wchar_t* InstallerBrandName() {
  return L"Chromium";
}

inline bool ShouldShowInstallerUi(const Configuration&) {
  return false;
}

inline bool PrepareSilentInstallerOptions(const Configuration&,
                                          InstallerUiOptions*) {
  return false;
}

inline void PostInstallerUiProgress(HWND, int, InstallerUiStage) {}

inline ProcessExitResult RunWithInstallerUi(
    HMODULE,
    const Configuration&,
    InstallerUiOptions*,
    ProcessExitResult (*work)(void*, HWND),
    void* ctx) {
  return work(ctx, nullptr);
}

#endif  // MINI_INSTALLER_HAS_DUILIB_UI

}  // namespace mini_installer

#endif  // CHROME_INSTALLER_MINI_INSTALLER_MINI_INSTALLER_UI_H_
