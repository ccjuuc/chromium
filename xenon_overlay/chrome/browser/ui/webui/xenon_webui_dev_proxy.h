// Copyright 2026 The Xenon Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_WEBUI_DEV_PROXY_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_WEBUI_DEV_PROXY_H_

#include <string>

namespace content {
class WebUI;
class WebUIDataSource;
}

namespace xenon {

// Sets up a dev-server proxy overlay when --[host]-dev-url is present.
// Falls back to --xenon-dev-server-url for xenon-login (legacy).
// Returns true if proxying is enabled.
bool TrySetupXenonWebuiDevProxy(content::WebUI* web_ui,
                                content::WebUIDataSource* source,
                                const std::string& webui_host);

}  // namespace xenon

// Hook into WebUI controllers after grit resources are registered on `source`.
#define XENON_WEBUI_DEV_PROXY_HOOK(web_ui, source, webui_host) \
  if (xenon::TrySetupXenonWebuiDevProxy(web_ui, source, webui_host)) { \
    return; \
  }

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_WEBUI_DEV_PROXY_H_
