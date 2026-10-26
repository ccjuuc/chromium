// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_CERTIFICATES_UI_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_CERTIFICATES_UI_CONTROLLER_H_

#include <memory>

#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"

namespace xenon {

class XenonCertificatesUIController : public content::WebUIController {
 public:
  explicit XenonCertificatesUIController(content::WebUI* web_ui);
  ~XenonCertificatesUIController() override;

  XenonCertificatesUIController(const XenonCertificatesUIController&) = delete;
  XenonCertificatesUIController& operator=(
      const XenonCertificatesUIController&) = delete;
};

class XenonCertificatesUIConfig : public content::WebUIConfig {
 public:
  XenonCertificatesUIConfig();
  ~XenonCertificatesUIConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_CERTIFICATES_UI_CONTROLLER_H_
