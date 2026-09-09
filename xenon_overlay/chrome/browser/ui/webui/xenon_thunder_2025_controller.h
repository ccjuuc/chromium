// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_THUNDER_2025_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_THUNDER_2025_CONTROLLER_H_

#include "content/public/browser/webui_config.h"

namespace content {
class WebUI;
}  // namespace content

namespace xenon {

class XenonThunder2025Config : public content::WebUIConfig {
 public:
  XenonThunder2025Config();
  ~XenonThunder2025Config() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_THUNDER_2025_CONTROLLER_H_
