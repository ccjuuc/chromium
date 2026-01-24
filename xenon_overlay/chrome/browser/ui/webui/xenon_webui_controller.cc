#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"

#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/browser_context.h"


#include "content/public/common/url_constants.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

namespace {
constexpr char kHost[] = "xenon-overlay";
}  // namespace

XenonWebUIController::XenonWebUIController(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kHost);

  source->AddResourcePath("index.css", IDR_XENON_WEBUI_INDEX_CSS);
  source->AddResourcePath("index.js", IDR_XENON_WEBUI_INDEX_JS);
  source->AddResourcePath("xenon.mojom-webui.js",
                          IDR_XENON_WEBUI_XENON_MOJOM_WEBUI_JS);
  source->SetDefaultResource(IDR_XENON_WEBUI_INDEX_HTML);
}

XenonWebUIController::~XenonWebUIController() = default;

void XenonWebUIController::BindInterface(
    mojo::PendingReceiver<mojom::PageHandler> receiver) {
  page_handler_ =
      std::make_unique<XenonPageHandler>(std::move(receiver), web_ui());
}

WEB_UI_CONTROLLER_TYPE_IMPL(XenonWebUIController)

XenonWebUIConfig::XenonWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kHost) {}

XenonWebUIConfig::~XenonWebUIConfig() = default;

std::unique_ptr<content::WebUIController> XenonWebUIConfig::CreateWebUIController(
    content::WebUI* web_ui,
    const GURL& url) {
  return std::make_unique<XenonWebUIController>(web_ui);
}





}  // namespace xenon
