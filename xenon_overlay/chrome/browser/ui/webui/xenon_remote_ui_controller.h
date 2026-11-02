// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_REMOTE_UI_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_REMOTE_UI_CONTROLLER_H_

#include <memory>

#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"

namespace xenon {

// Hosts the local sharing controls and the separately usable receiving page.
class XenonRemoteUIController : public content::WebUIController {
 public:
  XenonRemoteUIController(content::WebUI* web_ui, const GURL& url);
  ~XenonRemoteUIController() override;

  XenonRemoteUIController(const XenonRemoteUIController&) = delete;
  XenonRemoteUIController& operator=(const XenonRemoteUIController&) = delete;
};

class XenonRemoteUIConfig : public content::WebUIConfig {
 public:
  XenonRemoteUIConfig();
  ~XenonRemoteUIConfig() override;

  bool IsWebUIEnabled(content::BrowserContext* browser_context) override;
  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_REMOTE_UI_CONTROLLER_H_
