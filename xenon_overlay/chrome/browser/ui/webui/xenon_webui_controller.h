#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_WEBUI_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_WEBUI_CONTROLLER_H_

#include <string>

#include "content/public/browser/webui_config.h"
#include "content/public/common/url_constants.h"
#include "ui/webui/mojo_web_ui_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon.mojom.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_page_handler.h"


namespace xenon {

// chrome://xenon-overlay/ — main overlay WebUI.
inline constexpr char kXenonOverlayWebUIHost[] = "xenon-overlay";
// chrome://xenon-login/ — 独立 login/* 资源，chrome.send（无页面侧 Mojo）。
inline constexpr char kXenonLoginWebUIHost[] = "xenon-login";

class XenonWebUIController : public ui::MojoWebUIController {
 public:
  XenonWebUIController(content::WebUI* web_ui, std::string webui_host);
  ~XenonWebUIController() override;

  XenonWebUIController(const XenonWebUIController&) = delete;
  XenonWebUIController& operator=(const XenonWebUIController&) = delete;

  // Instantiates the implementor of the mojom::PageHandler mojo interface
  // passing the pending receiver that will be internally bound.
  void BindInterface(mojo::PendingReceiver<mojom::PageHandler> receiver);

 private:
  const std::string webui_host_;
  std::unique_ptr<XenonPageHandler> page_handler_;

  WEB_UI_CONTROLLER_TYPE_DECL();
};

class XenonWebUIConfig : public content::WebUIConfig {
 public:
  XenonWebUIConfig();
  ~XenonWebUIConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

class XenonLoginWebUIConfig : public content::WebUIConfig {
 public:
  XenonLoginWebUIConfig();
  ~XenonLoginWebUIConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_WEBUI_CONTROLLER_H_
