// Copyright 2026 The Xenon Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"

#include <mutex>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "xenon_overlay/chrome/browser/xenon_login_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_dev_proxy.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

namespace {

constexpr char kLoginMessageDone[] = "xenonLoginDone";
constexpr char kLoginMessageLogoutTest[] = "xenonLoginLogoutTest";
constexpr char kLoginMessageClose[] = "xenonLoginClose";

void EnsureTrustedBrokerKnowsPageHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
        .ForWebUI<XenonWebUIController>()
        .Add<mojom::PageHandler>();
  });
}

class XenonLoginWebUIMessageHandler : public content::WebUIMessageHandler {
 public:
  XenonLoginWebUIMessageHandler() = default;
  XenonLoginWebUIMessageHandler(const XenonLoginWebUIMessageHandler&) = delete;
  XenonLoginWebUIMessageHandler& operator=(const XenonLoginWebUIMessageHandler&) =
      delete;
  ~XenonLoginWebUIMessageHandler() override = default;

 private:
  void RegisterMessages() override {
    web_ui()->RegisterMessageCallback(
        kLoginMessageDone,
        base::BindRepeating(&XenonLoginWebUIMessageHandler::HandleLoginDone,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        kLoginMessageLogoutTest,
        base::BindRepeating(&XenonLoginWebUIMessageHandler::HandleLogoutTest,
                            base::Unretained(this)));
    web_ui()->RegisterMessageCallback(
        kLoginMessageClose,
        base::BindRepeating(&XenonLoginWebUIMessageHandler::HandleClose,
                            base::Unretained(this)));
  }

  void HandleLoginDone(const base::ListValue& args) {
    Profile* profile = Profile::FromWebUI(web_ui());
    if (profile) {
      XenonLoginController::GetInstance()->SetAppSessionLoggedIn(profile, true);
    }
  }

  void HandleLogoutTest(const base::ListValue& args) {
    Profile* profile = Profile::FromWebUI(web_ui());
    if (profile) {
      XenonLoginController::GetInstance()->SetAppSessionLoggedIn(profile, false);
    }
  }

  void HandleClose(const base::ListValue& args) {
    content::WebContents* contents = web_ui()->GetWebContents();
    if (!contents) {
      return;
    }
    if (content::WebContentsDelegate* delegate = contents->GetDelegate()) {
      delegate->CloseContents(contents);
    }
  }
};

}  // namespace

XenonWebUIController::XenonWebUIController(content::WebUI* web_ui,
                                             std::string webui_host)
    : ui::MojoWebUIController(web_ui, webui_host == kXenonLoginWebUIHost),
      webui_host_(std::move(webui_host)) {
  EnsureTrustedBrokerKnowsPageHandler();

  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), webui_host_);

  if (webui_host_ == kXenonLoginWebUIHost) {
    web_ui->AddMessageHandler(std::make_unique<XenonLoginWebUIMessageHandler>());
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
