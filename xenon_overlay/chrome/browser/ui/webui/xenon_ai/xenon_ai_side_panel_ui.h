// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_AI_XENON_AI_SIDE_PANEL_UI_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_AI_XENON_AI_SIDE_PANEL_UI_H_

#include <string_view>

#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"

namespace content {
class Page;
class WebUI;
}  // namespace content

namespace xenon {

class XenonAiSidePanelUI;

class XenonAiSidePanelUIConfig
    : public DefaultTopChromeWebUIConfig<XenonAiSidePanelUI> {
 public:
  XenonAiSidePanelUIConfig();
  XenonAiSidePanelUIConfig(const XenonAiSidePanelUIConfig&) = delete;
  XenonAiSidePanelUIConfig& operator=(const XenonAiSidePanelUIConfig&) = delete;
  ~XenonAiSidePanelUIConfig() override;

  bool IsPreloadable() override;
};

class XenonAiSidePanelUI : public TopChromeWebUIController {
 public:
  explicit XenonAiSidePanelUI(content::WebUI* web_ui);
  XenonAiSidePanelUI(const XenonAiSidePanelUI&) = delete;
  XenonAiSidePanelUI& operator=(const XenonAiSidePanelUI&) = delete;
  ~XenonAiSidePanelUI() override;

  // Side-panel WebContents wait for `embedder()->ShowUI()` before showing
  // (see ReadingListPageHandler::ShowUI()). Call when the primary page exists.
  void WebUIPrimaryPageChanged(content::Page& page) override;

  static constexpr std::string_view GetWebUIName() {
    return "XenonAiSidePanel";
  }

  WEB_UI_CONTROLLER_TYPE_DECL();
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_AI_XENON_AI_SIDE_PANEL_UI_H_
