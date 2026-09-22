// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_UPDATE_UI_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_UPDATE_UI_CONTROLLER_H_

#include <memory>

#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"

namespace xenon {

// WebUI controller for chrome://xenon-update/
class XenonUpdateUIController : public content::WebUIController {
 public:
  explicit XenonUpdateUIController(content::WebUI* web_ui);
  ~XenonUpdateUIController() override;

  XenonUpdateUIController(const XenonUpdateUIController&) = delete;
  XenonUpdateUIController& operator=(const XenonUpdateUIController&) = delete;
};

class XenonUpdateConfig : public content::WebUIConfig {
 public:
  XenonUpdateConfig();
  ~XenonUpdateConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_UPDATE_UI_CONTROLLER_H_
