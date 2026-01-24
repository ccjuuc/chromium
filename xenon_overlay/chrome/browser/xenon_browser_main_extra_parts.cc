#include "xenon_overlay/chrome/browser/xenon_browser_main_extra_parts.h"

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_ui_controller_factory.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/resource/resource_scale_factor.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "url/gurl.h"
#include "content/public/browser/webui_config_map.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"

XenonBrowserMainExtraParts::XenonBrowserMainExtraParts() = default;

XenonBrowserMainExtraParts::~XenonBrowserMainExtraParts() = default;

void XenonBrowserMainExtraParts::PostProfileInit(Profile* profile,
                                                 bool is_initial_profile) {
  if (!is_initial_profile) {
    return;
  }

  LOG(INFO) << "XenonBrowserMainExtraParts: Initializing XenonManager for profile: " 
            << profile->GetDebugName();

  // Load Xenon Resources
  base::FilePath pak_path;
  base::PathService::Get(base::DIR_MODULE, &pak_path);
  pak_path = pak_path.AppendASCII("xenon_resources.pak");
  ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
      pak_path, ui::kScaleFactorNone);
  LOG(INFO) << "XenonBrowserMainExtraParts: Loaded resource pak from " << pak_path;
            
  // Register the Xenon WebUI Config
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonWebUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonWebUIConfig";

  xenon::XenonManager::GetInstance()->EnsureServiceStarted(profile);
  
  // Optional: Trigger a ping to verify connectivity.
  xenon::XenonManager::GetInstance()->Ping(base::BindOnce(
      [](const std::string& response) {
        LOG(INFO) << "Xenon Startup Ping Response: " << response;
      }));
  
  // Launch the Xenon WebUI
  GURL app_url("chrome://xenon-overlay/");
  
  // Register Mojo interfaces for Xenon WebUI
  content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
      .ForWebUI<xenon::XenonWebUIController>()
      .Add<xenon::mojom::PageHandler>();

  xenon::XenonWebDialog::Show(profile, app_url, 800, 600, u"Xenon Overlay");
}
