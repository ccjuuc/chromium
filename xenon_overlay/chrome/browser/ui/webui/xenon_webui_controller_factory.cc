#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller_factory.h"

#include <string>

#include "content/public/browser/web_ui.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"

namespace xenon {

namespace {

constexpr char kOverlayHost[] = "xenon-overlay";
constexpr char kLoginHost[] = "xenon-login";

}  // namespace
// static
XenonWebUIControllerFactory* XenonWebUIControllerFactory::GetInstance() {
  static base::NoDestructor<XenonWebUIControllerFactory> instance;
  return instance.get();
}

XenonWebUIControllerFactory::XenonWebUIControllerFactory() = default;
XenonWebUIControllerFactory::~XenonWebUIControllerFactory() = default;

std::unique_ptr<content::WebUIController>
XenonWebUIControllerFactory::CreateWebUIControllerForURL(content::WebUI* web_ui,
                                                         const GURL& url) {
  if (url.host() == kOverlayHost) {
    return std::make_unique<XenonWebUIController>(web_ui, std::string(kOverlayHost));
  }
  if (url.host() == kLoginHost) {
    return std::make_unique<XenonWebUIController>(web_ui, std::string(kLoginHost));
  }
  return nullptr;
}

content::WebUI::TypeID XenonWebUIControllerFactory::GetWebUIType(
    content::BrowserContext* browser_context,
    const GURL& url) {
  if (url.host() == kOverlayHost || url.host() == kLoginHost) {
    return reinterpret_cast<content::WebUI::TypeID>(0xBAD1);  // Unique ID
  }
  return content::WebUI::kNoWebUI;
}

bool XenonWebUIControllerFactory::UseWebUIForURL(
    content::BrowserContext* browser_context,
    const GURL& url) {
  return GetWebUIType(browser_context, url) != content::WebUI::kNoWebUI;
}

}  // namespace xenon
