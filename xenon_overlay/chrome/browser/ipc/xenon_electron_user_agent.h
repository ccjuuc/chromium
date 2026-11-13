// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_USER_AGENT_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_USER_AGENT_H_

#include <string>
#include <string_view>

namespace xenon::ipc {

// Decorates the host's default Chromium user agent with the application's
// product and the compatibility runtime's identity. Callers must preserve
// explicitly configured user agents instead of passing them to this function.
std::string BuildElectronUserAgent(std::string_view chromium_user_agent,
                                   std::string_view app_name,
                                   std::string_view app_version);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_USER_AGENT_H_
