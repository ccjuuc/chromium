// Copyright 2026 The Xenon Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"

#include <mutex>
#include <utility>

#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/web_ui_data_source.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_account_webui_handler.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_dev_proxy.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

namespace {

void EnsureTrustedBrokerKnowsPageHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
        .ForWebUI<XenonWebUIController>()
        .Add<mojom::PageHandler>();
  });
}

}  // namespace

XenonWebUIController::XenonWebUIController(content::WebUI* web_ui,
                                             std::string webui_host)
    : ui::MojoWebUIController(web_ui, webui_host == kXenonLoginWebUIHost),
      webui_host_(std::move(webui_host)) {
  EnsureTrustedBrokerKnowsPageHandler();

  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), webui_host_);

  if (webui_host_ == kXenonLoginWebUIHost) {
    web_ui->AddMessageHandler(std::make_unique<XenonAccountWebUIHandler>(true));
    source->AddResourcePath("login.css", IDR_XENON_LOGIN_CSS);
    source->AddResourcePath("login.js", IDR_XENON_LOGIN_JS);
    source->SetDefaultResource(IDR_XENON_LOGIN_HTML);
  } else {
    source->AddResourcePath("xenon.mojom-webui.js",
                            IDR_XENON_WEBUI_XENON_MOJOM_WEBUI_JS);
    source->AddResourcePath("index.css", IDR_XENON_WEBUI_INDEX_CSS);
    source->AddResourcePath("index.js", IDR_XENON_WEBUI_INDEX_JS);
    source->SetDefaultResource(IDR_XENON_WEBUI_INDEX_HTML);
  }

  XENON_WEBUI_DEV_PROXY_HOOK(web_ui, source, webui_host_)
}

XenonWebUIController::~XenonWebUIController() = default;

void XenonWebUIController::BindInterface(
    mojo::PendingReceiver<mojom::PageHandler> receiver) {
  page_handler_ =
      std::make_unique<XenonPageHandler>(std::move(receiver), web_ui());
}

WEB_UI_CONTROLLER_TYPE_IMPL(XenonWebUIController)

XenonWebUIConfig::XenonWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kXenonOverlayWebUIHost) {}

XenonWebUIConfig::~XenonWebUIConfig() = default;

std::unique_ptr<content::WebUIController> XenonWebUIConfig::CreateWebUIController(
    content::WebUI* web_ui,
    const GURL& url) {
  return std::make_unique<XenonWebUIController>(
      web_ui, std::string(kXenonOverlayWebUIHost));
}

XenonLoginWebUIConfig::XenonLoginWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kXenonLoginWebUIHost) {}

XenonLoginWebUIConfig::~XenonLoginWebUIConfig() = default;

std::unique_ptr<content::WebUIController>
XenonLoginWebUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                             const GURL& url) {
  return std::make_unique<XenonWebUIController>(
      web_ui, std::string(kXenonLoginWebUIHost));
}

}  // namespace xenon
