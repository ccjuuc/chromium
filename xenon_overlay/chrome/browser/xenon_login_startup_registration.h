// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_LOGIN_STARTUP_REGISTRATION_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_LOGIN_STARTUP_REGISTRATION_H_

namespace xenon {

// Installs StartupBrowserCreator hooks that defer main-window launch until
// Xenon login completes. Must run from PostProfileInit for the initial profile
// before browser_creator_->Start (normal Chrome startup order).
void RegisterXenonLoginStartupHooks();

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_LOGIN_STARTUP_REGISTRATION_H_
