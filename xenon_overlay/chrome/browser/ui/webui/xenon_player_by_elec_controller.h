// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_PLAYER_BY_ELEC_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_PLAYER_BY_ELEC_CONTROLLER_H_

#include <memory>

#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"

namespace xenon {

// Standalone WebUI test surface for the generic Electron IPC container.
class XenonPlayerByElecController : public content::WebUIController {
 public:
  explicit XenonPlayerByElecController(content::WebUI* web_ui);
  ~XenonPlayerByElecController() override;

  XenonPlayerByElecController(const XenonPlayerByElecController&) = delete;
  XenonPlayerByElecController& operator=(const XenonPlayerByElecController&) =
      delete;
};

class XenonPlayerByElecConfig : public content::WebUIConfig {
 public:
  XenonPlayerByElecConfig();
  ~XenonPlayerByElecConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_PLAYER_BY_ELEC_CONTROLLER_H_
