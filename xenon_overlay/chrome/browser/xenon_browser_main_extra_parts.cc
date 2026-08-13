#include "xenon_overlay/chrome/browser/xenon_browser_main_extra_parts.h"

#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/xenon_login_startup_registration.h"

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/webui_config_map.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_manager.h"
#include "xenon_overlay/chrome/browser/ui/xenon_reminder_browser_observer.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/ui/webui/dev_test_pages_webui.h"
#include "xenon_overlay/chrome/browser/ui/webui/simple_webui_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_ui_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_node_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/video_sniffer_webui_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"
#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/xenon/chrome/browser/fonts/xenon_font_loader.h"

#if BUILDFLAG(ENABLE_XENON_AI)
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/toolbar/pinned_toolbar/pinned_toolbar_actions_model.h"
#include "components/prefs/pref_service.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_ai/xenon_ai_side_panel_ui.h"
#include "xenon_overlay/chrome/browser/xenon_prefs.h"
#endif

namespace {

#if BUILDFLAG(ENABLE_XENON_AI)
// Chromium shows side panels via pinned toolbar actions; with an empty pin list
// there is no visible launcher. Pin Xenon AI once per regular profile.
void MaybePinXenonAiToolbarAction(Profile* profile) {
  if (!profile || !profile->IsRegularProfile()) {
    return;
  }
  PrefService* prefs = profile->GetPrefs();
  if (prefs->GetBoolean(xenon::prefs::kAiSidePanelToolbarPinMigrated)) {
    return;
  }
  PinnedToolbarActionsModel::Get(profile)->UpdatePinnedState(
      kActionSidePanelShowXenonAI, true);
  prefs->SetBoolean(xenon::prefs::kAiSidePanelToolbarPinMigrated, true);
}
#endif

}  // namespace

XenonBrowserMainExtraParts::XenonBrowserMainExtraParts() = default;

XenonBrowserMainExtraParts::~XenonBrowserMainExtraParts() {
  // 观察者已在 PostMainMessageLoopRun 中移除并 reset，此处仅防御性清空
  reminder_browser_observer_.reset();
}

void XenonBrowserMainExtraParts::PostEarlyInitialization() {
  xenon_fonts::RegisterXunleiFonts();
}

void XenonBrowserMainExtraParts::PostMainMessageLoopRun() {
  // 在有序关闭阶段析构 BrowserCollection 观察者，避免进程退出时
  // 静态析构顺序导致 ObserverList 先于本对象析构而触发 observers_.empty() 的 CHECK
  if (reminder_browser_observer_) {
    reminder_browser_observer_.reset();
  }
  xenon_fonts::UnregisterXunleiFonts();
}

void XenonBrowserMainExtraParts::PostProfileInit(Profile* profile,
                                                 bool is_initial_profile) {
#if BUILDFLAG(ENABLE_XENON_AI)
  MaybePinXenonAiToolbarAction(profile);
#endif

  if (!is_initial_profile) {
    return;
  }

#if BUILDFLAG(ENABLE_XENON_SERVICE)
  xenon::RegisterXenonLoginStartupHooks();
#endif

  LOG(INFO) << "XenonBrowserMainExtraParts: Initializing XenonManager for profile: "
            << profile->GetDebugName();

  // Register the Xenon WebUI Config (with Mojo)
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonWebUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonWebUIConfig";
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonLoginWebUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonLoginWebUIConfig";

  // Register Simple WebUI Config (without Mojo)
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::SimpleWebUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered SimpleWebUIConfig";

  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::LocalVideoTestWebUIConfig>());
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::RenderDllTestWebUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered local-video-test / "
               "render-dll-test WebUI";

  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::VideoSnifferWebUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered VideoSnifferWebUIConfig";

  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonUIConfig";

  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonNodeConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonNodeConfig";

#if BUILDFLAG(ENABLE_XENON_AI)
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonAiSidePanelUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonAiSidePanelUIConfig";
#endif

  xenon::XenonManager::GetInstance()->EnsureServiceStarted(profile);

  // 初始化提醒 Manager 并注册 Browser 观察者（为每个窗口创建提醒 Widget + 注册 Ctrl+Shift+R 测试快捷键）
  xenon::XenonReminderNotificationManager::GetInstance()->Initialize();
  reminder_browser_observer_ = std::make_unique<xenon::XenonReminderBrowserObserver>();
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* browser_window) {
        reminder_browser_observer_->AttachToBrowser(
            browser_window->GetBrowserForMigrationOnly());
        return true;
      });

  // 默认仅加载组件扩展（不自动打开扩展弹窗）；--show-xenon-extension 则打开 Xenon WebUI 浮层
  if (!base::CommandLine::ForCurrentProcess()->HasSwitch("show-xenon-extension")) {
      // Load and register the component extension
      xenon::XenonExtensionManager* extension_manager =
          xenon::XenonExtensionManager::GetInstance();
      extension_manager->LoadExtensionFromDefaultPath(
          profile,
          base::BindOnce([](const extensions::ExtensionId& extension_id) {
            if (!extension_id.empty()) {
              LOG(INFO) << "XenonBrowserMainExtraParts: Component extension "
                           "loaded with ID: "
                        << extension_id;
            }
          }));
  } else {
    xenon::XenonWebDialog::ShowXenonOverlay(profile);
  }
}
