#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_WEBUI_CONTROLLER_FACTORY_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_WEBUI_CONTROLLER_FACTORY_H_

#include "base/no_destructor.h"
#include "content/public/browser/web_ui_controller_factory.h"

namespace xenon {

class XenonWebUIControllerFactory : public content::WebUIControllerFactory {
 public:
  static XenonWebUIControllerFactory* GetInstance();

  XenonWebUIControllerFactory(const XenonWebUIControllerFactory&) = delete;
  XenonWebUIControllerFactory& operator=(const XenonWebUIControllerFactory&) = delete;

  // content::WebUIControllerFactory:
  std::unique_ptr<content::WebUIController> CreateWebUIControllerForURL(
      content::WebUI* web_ui,
      const GURL& url) override;
  content::WebUI::TypeID GetWebUIType(content::BrowserContext* browser_context,
                                      const GURL& url) override;
  bool UseWebUIForURL(content::BrowserContext* browser_context,
                      const GURL& url) override;

 private:
  friend class base::NoDestructor<XenonWebUIControllerFactory>;

  XenonWebUIControllerFactory();
  ~XenonWebUIControllerFactory() override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_WEBUI_CONTROLLER_FACTORY_H_
