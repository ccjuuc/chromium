// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NAVIGATION_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NAVIGATION_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"
#include "content/public/browser/navigation_controller.h"

namespace content {
class WebContents;
}

namespace xenon::ipc {
using ElectronNavigationReply =
    base::OnceCallback<void(base::Value, std::string)>;

// Completion belongs to the navigation and its committed document, never to
// the next arbitrary did-finish-load event on the same WebContents.
void LoadElectronURL(content::WebContents* contents,
                     const content::NavigationController::LoadURLParams& params,
                     ElectronNavigationReply reply);
}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NAVIGATION_H_
