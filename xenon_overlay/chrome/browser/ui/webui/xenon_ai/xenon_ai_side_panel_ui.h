// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_AI_XENON_AI_SIDE_PANEL_UI_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_AI_XENON_AI_SIDE_PANEL_UI_H_

#include <string_view>

#include "chrome/browser/ui/webui/top_chrome/top_chrome_web_ui_controller.h"
#include "chrome/browser/ui/webui/top_chrome/top_chrome_webui_config.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_ai.mojom.h"
#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service.h"

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

class XenonAiSidePanelUI : public TopChromeWebUIController,
                           public mojom::XenonAiPageHandler,
                           public XenonAiService::Observer {
 public:
  explicit XenonAiSidePanelUI(content::WebUI* web_ui);
  XenonAiSidePanelUI(const XenonAiSidePanelUI&) = delete;
  XenonAiSidePanelUI& operator=(const XenonAiSidePanelUI&) = delete;
  ~XenonAiSidePanelUI() override;

  // mojom::XenonAiPageHandler:
  void SetPage(mojo::PendingRemote<mojom::XenonAiPage> page) override;
  void GetInitialPrompt(GetInitialPromptCallback callback) override;

  // XenonAiService::Observer:
  void OnPromptReceived(const std::string& prompt) override;

  void BindInterface(mojo::PendingReceiver<mojom::XenonAiPageHandler> receiver);

  // Side-panel WebContents wait for `embedder()->ShowUI()` before showing
  // (see ReadingListPageHandler::ShowUI()). Call when the primary page exists.
  void WebUIPrimaryPageChanged(content::Page& page) override;

  static constexpr std::string_view GetWebUIName() {
    return "XenonAiSidePanel";
  }

  WEB_UI_CONTROLLER_TYPE_DECL();

 private:
  mojo::Receiver<mojom::XenonAiPageHandler> receiver_{this};
  mojo::Remote<mojom::XenonAiPage> page_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_AI_XENON_AI_SIDE_PANEL_UI_H_
