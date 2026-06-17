// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_UI_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_UI_CONTROLLER_H_

#include <memory>

#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"

namespace xenon {

// WebUI controller for chrome://xenon-ui/
class XenonUIController : public content::WebUIController {
 public:
  explicit XenonUIController(content::WebUI* web_ui);
  ~XenonUIController() override;

  XenonUIController(const XenonUIController&) = delete;
  XenonUIController& operator=(const XenonUIController&) = delete;
};

class XenonUIConfig : public content::WebUIConfig {
 public:
  XenonUIConfig();
  ~XenonUIConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_UI_CONTROLLER_H_
