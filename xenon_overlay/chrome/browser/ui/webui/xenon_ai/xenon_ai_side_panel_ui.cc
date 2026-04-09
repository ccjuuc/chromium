// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_ai/xenon_ai_side_panel_ui.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/page.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

XenonAiSidePanelUIConfig::XenonAiSidePanelUIConfig()
    : DefaultTopChromeWebUIConfig(content::kChromeUIScheme,
                                  chrome::kChromeUIXenonAISidePanelHost) {}

XenonAiSidePanelUIConfig::~XenonAiSidePanelUIConfig() = default;

bool XenonAiSidePanelUIConfig::IsPreloadable() {
  return false;
}

XenonAiSidePanelUI::XenonAiSidePanelUI(content::WebUI* web_ui)
    : TopChromeWebUIController(web_ui, /*enable_chrome_send=*/false) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      Profile::FromWebUI(web_ui), chrome::kChromeUIXenonAISidePanelHost);
  source->SetDefaultResource(IDR_XENON_AI_SIDE_PANEL_HTML);
}

XenonAiSidePanelUI::~XenonAiSidePanelUI() = default;

void XenonAiSidePanelUI::WebUIPrimaryPageChanged(content::Page& /*page*/) {
  if (base::WeakPtr<TopChromeWebUIController::Embedder> embedder_host =
          embedder()) {
    embedder_host->ShowUI();
  }
}

WEB_UI_CONTROLLER_TYPE_IMPL(XenonAiSidePanelUI)

}  // namespace xenon
