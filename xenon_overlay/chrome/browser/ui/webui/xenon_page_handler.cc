#include "xenon_overlay/chrome/browser/ui/webui/xenon_page_handler.h"

#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"

namespace xenon {

XenonPageHandler::XenonPageHandler(
    mojo::PendingReceiver<mojom::PageHandler> receiver,
    content::WebUI* web_ui)
    : receiver_(this, std::move(receiver)), web_ui_(web_ui) {}

XenonPageHandler::~XenonPageHandler() = default;

void XenonPageHandler::Close() {
  LOG(INFO) << "XenonPageHandler: Close requested";
  if (web_ui_ && web_ui_->GetWebContents()) {
    web_ui_->GetWebContents()->ClosePage();
  }
}

void XenonPageHandler::ConnectToService(ConnectToServiceCallback callback) {
  LOG(INFO) << "XenonPageHandler: ConnectToService requested";
  
  if (!web_ui_ || !web_ui_->GetWebContents()) {
    std::move(callback).Run(false, "WebUI context lost");
    return;
  }

  // Ensure service is started via manager
  XenonManager* manager = XenonManager::GetInstance();
  if (!manager) {
     std::move(callback).Run(false, "XenonManager not available");
     return;
  }
  
  // Ensure the service is up and running
  manager->EnsureServiceStarted(web_ui_->GetWebContents()->GetBrowserContext());

  // Ping the service to verify connection
  manager->Ping(base::BindOnce(
      [](ConnectToServiceCallback callback, const std::string& response) {
        if (response.empty() || response.rfind("Error:", 0) == 0) {
            std::move(callback).Run(false, response);
        } else {
            // Success: response contains pong/status
            std::move(callback).Run(true, "Connected: " + response);
        }
      },
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback), false, "Service connection failed (callback dropped)")));
}

}  // namespace xenon
