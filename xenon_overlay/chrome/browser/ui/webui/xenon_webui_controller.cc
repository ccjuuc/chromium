#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"

#include <mutex>

#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

namespace {

constexpr char kHost[] = "xenon-overlay";

// `WebUIBrowserInterfaceBrokerRegistry::ForWebUI` must run at most once per
// controller type. Lazily register when the first xenon-overlay page is
// created so chrome:// need not list Xenon in central binders.
void EnsureTrustedBrokerKnowsPageHandler() {
  static std::once_flag once;
  std::call_once(once, [] {
    content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
        .ForWebUI<XenonWebUIController>()
        .Add<mojom::PageHandler>();
  });
}

}  // namespace

XenonWebUIController::XenonWebUIController(content::WebUI* web_ui)
    : ui::MojoWebUIController(web_ui) {
  EnsureTrustedBrokerKnowsPageHandler();

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
