// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_CROSS_TRANSFER_TEST_UI_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_CROSS_TRANSFER_TEST_UI_H_

#include <memory>

#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"

namespace xenon {

// chrome://cross-transfer-test/ — native TLS/HTTPS transfer diagnostics.
class CrossTransferTestUI : public content::WebUIController {
 public:
  explicit CrossTransferTestUI(content::WebUI* web_ui);
  ~CrossTransferTestUI() override;

  CrossTransferTestUI(const CrossTransferTestUI&) = delete;
  CrossTransferTestUI& operator=(const CrossTransferTestUI&) = delete;
};

class CrossTransferTestUIConfig : public content::WebUIConfig {
 public:
  CrossTransferTestUIConfig();
  ~CrossTransferTestUIConfig() override;
  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_CROSS_TRANSFER_TEST_UI_H_
