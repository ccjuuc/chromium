#include "xenon_overlay/chrome/browser/xenon_browser_main_extra_parts.h"

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/webui_config_map.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/resource/resource_scale_factor.h"
#include "url/gurl.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_list.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_manager.h"
#include "xenon_overlay/chrome/browser/ui/xenon_reminder_browser_observer.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/ui/webui/simple_webui_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"
#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"

XenonBrowserMainExtraParts::XenonBrowserMainExtraParts() = default;

XenonBrowserMainExtraParts::~XenonBrowserMainExtraParts() {
  // 观察者已在 PostMainMessageLoopRun 中移除并 reset，此处仅防御性清空
  reminder_browser_observer_.reset();
}

void XenonBrowserMainExtraParts::PostMainMessageLoopRun() {
  // 在有序关闭阶段从 BrowserList 移除观察者并析构，避免进程退出时
  // 静态析构顺序导致 ObserverList 先于本对象析构而触发 observers_.empty() 的 CHECK
  if (reminder_browser_observer_) {
    BrowserList::GetInstance()->RemoveObserver(reminder_browser_observer_.get());
    reminder_browser_observer_.reset();
  }
}

void XenonBrowserMainExtraParts::PostProfileInit(Profile* profile,
                                                 bool is_initial_profile) {
  if (!is_initial_profile) {
    return;
  }

  LOG(INFO) << "XenonBrowserMainExtraParts: Initializing XenonManager for profile: " 
            << profile->GetDebugName();

  // Load Xenon Resources
  base::FilePath pak_path;
  if (base::PathService::Get(base::DIR_MODULE, &pak_path)) {
    pak_path = pak_path.AppendASCII("xenon_resources.pak");
    if (base::PathExists(pak_path)) {
      ui::ResourceBundle::GetSharedInstance().AddDataPackFromPath(
          pak_path, ui::kScaleFactorNone);
      LOG(INFO) << "XenonBrowserMainExtraParts: Loaded resource pak from " << pak_path;
    } else {
      LOG(WARNING) << "XenonBrowserMainExtraParts: Resource pak not found at " << pak_path;
    }
  } else {
    LOG(WARNING) << "XenonBrowserMainExtraParts: Failed to get module directory";
  }
            
  // Register the Xenon WebUI Config (with Mojo)
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonWebUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonWebUIConfig";

  // Register Simple WebUI Config (without Mojo)
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::SimpleWebUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered SimpleWebUIConfig";

  xenon::XenonManager::GetInstance()->EnsureServiceStarted(profile);

  // 初始化提醒 Manager 并注册 Browser 观察者（为每个窗口创建提醒 Widget + 注册 Ctrl+Shift+R 测试快捷键）
  xenon::XenonReminderNotificationManager::GetInstance()->Initialize();
  reminder_browser_observer_ = std::make_unique<xenon::XenonReminderBrowserObserver>();
  BrowserList::GetInstance()->AddObserver(reminder_browser_observer_.get());
  for (Browser* browser : *BrowserList::GetInstance()) {
    reminder_browser_observer_->OnBrowserAdded(browser);
  }

  // Optional: Trigger a ping to verify connectivity.
  xenon::XenonManager::GetInstance()->Ping(base::BindOnce(
      [](const std::string& response) {
        LOG(INFO) << "Xenon Startup Ping Response: " << response;
      }));
  

  //根据命令行参数 --show-xenon-extension 决定是否显示扩展界面
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch("show-xenon-extension")) {
      // Load and register the component extension
      xenon::XenonExtensionManager* extension_manager =
          xenon::XenonExtensionManager::GetInstance();
      extension_manager->LoadExtensionFromDefaultPath(
          profile,
          base::BindOnce(
              [](content::BrowserContext* context,
                 const extensions::ExtensionId& extension_id) {
                if (!extension_id.empty()) {
                  LOG(INFO) << "XenonBrowserMainExtraParts: Component extension "
                               "loaded with ID: "
                            << extension_id;

                  // Verify Extension UI by launching it on startup
                  xenon::XenonExtensionManager::GetInstance()->ShowExtension(
                      context);
                }
              },
              profile));
  } else {
      // Register Mojo interfaces for Xenon WebUI
      content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
      .ForWebUI<xenon::XenonWebUIController>()
      .Add<xenon::mojom::PageHandler>();


      GURL app_url("chrome://xenon-overlay/");

      xenon::XenonWebDialog::Show(profile, app_url, 800, 600, u"Xenon Overlay");
  }
}
