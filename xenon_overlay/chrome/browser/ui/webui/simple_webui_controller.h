// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_SIMPLE_WEBUI_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_SIMPLE_WEBUI_CONTROLLER_H_

#include <memory>
#include <string>

#include "base/values.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"

namespace xenon {

// A simple WebUI controller without Mojo bindings.
// This demonstrates traditional message-based communication using
// chrome.send() from JavaScript and RegisterMessageCallback() in C++.
class SimpleWebUIController : public content::WebUIController {
 public:
  explicit SimpleWebUIController(content::WebUI* web_ui);
  ~SimpleWebUIController() override;

  SimpleWebUIController(const SimpleWebUIController&) = delete;
  SimpleWebUIController& operator=(const SimpleWebUIController&) = delete;

 private:
  // Message handlers for JavaScript chrome.send() calls.

  // Handles "getSystemInfo" - returns mock system information.
  void HandleGetSystemInfo(const base::ListValue& args);

  // Handles "logMessage" - logs a message from JavaScript.
  void HandleLogMessage(const base::ListValue& args);

  // Handles "performAction" - performs an action and sends result back.
  void HandlePerformAction(const base::ListValue& args);
};

// WebUIConfig for SimpleWebUI - handles URL matching and controller creation.
class SimpleWebUIConfig : public content::WebUIConfig {
 public:
  SimpleWebUIConfig();
  ~SimpleWebUIConfig() override;

  // content::WebUIConfig:
  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_SIMPLE_WEBUI_CONTROLLER_H_
