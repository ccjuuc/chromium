// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_ai/xenon_ai_side_panel_ui.h"

#include <mutex>
#include <string>

#include "chrome/browser/profiles/profile.h"
#include "chrome/common/webui_url_constants.h"
#include "content/public/browser/page.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service_factory.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

namespace {

void EnsureAiBrokerKnowsPageHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
        .ForWebUI<XenonAiSidePanelUI>()
        .Add<mojom::XenonAiPageHandler>();
  });
}

}  // namespace

XenonAiSidePanelUIConfig::XenonAiSidePanelUIConfig()
    : DefaultTopChromeWebUIConfig(content::kChromeUIScheme,
                                  chrome::kChromeUIXenonAISidePanelHost) {}

XenonAiSidePanelUIConfig::~XenonAiSidePanelUIConfig() = default;

bool XenonAiSidePanelUIConfig::IsPreloadable() {
  return false;
}

XenonAiSidePanelUI::XenonAiSidePanelUI(content::WebUI* web_ui)
    : TopChromeWebUIController(web_ui, /*enable_chrome_send=*/false) {
  EnsureAiBrokerKnowsPageHandler();
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      Profile::FromWebUI(web_ui), chrome::kChromeUIXenonAISidePanelHost);

  source->AddResourcePath("xenon_ai.mojom-webui.js",
                          IDR_XENON_AI_MOJOM_WEBUI_JS);
  source->AddResourcePath("xenon_ai_side_panel.js",
                          IDR_XENON_AI_SIDE_PANEL_JS);
  source->SetDefaultResource(IDR_XENON_AI_SIDE_PANEL_HTML);

  XenonAiService* service = XenonAiServiceFactory::GetForProfile(
      Profile::FromWebUI(web_ui));
  if (service) {
    service->AddObserver(this);
  }
}

XenonAiSidePanelUI::~XenonAiSidePanelUI() {
  if (Profile::FromWebUI(web_ui())) {
    XenonAiService* service = XenonAiServiceFactory::GetForProfile(
        Profile::FromWebUI(web_ui()));
    if (service) {
      service->RemoveObserver(this);
    }
  }
}

void XenonAiSidePanelUI::SetPage(mojo::PendingRemote<mojom::XenonAiPage> page) {
  page_.reset();
  page_.Bind(std::move(page));
}

void XenonAiSidePanelUI::GetInitialPrompt(GetInitialPromptCallback callback) {
  XenonAiService* service = XenonAiServiceFactory::GetForProfile(
      Profile::FromWebUI(web_ui()));
  std::move(callback).Run(service ? service->GetPendingPrompt() : "");
}

void XenonAiSidePanelUI::OnPromptReceived(const std::string& prompt) {
  if (page_.is_bound()) {
    page_->OnPromptReceived(prompt);
  }
}

void XenonAiSidePanelUI::BindInterface(
    mojo::PendingReceiver<mojom::XenonAiPageHandler> receiver) {
  receiver_.reset();
  receiver_.Bind(std::move(receiver));
}

void XenonAiSidePanelUI::WebUIPrimaryPageChanged(content::Page& /*page*/) {
  if (base::WeakPtr<TopChromeWebUIController::Embedder> embedder_host =
          embedder()) {
    embedder_host->ShowUI();
  }
}

WEB_UI_CONTROLLER_TYPE_IMPL(XenonAiSidePanelUI)

}  // namespace xenon
