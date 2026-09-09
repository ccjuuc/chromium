#include "xenon_overlay/chrome/browser/xenon_browser_main_extra_parts.h"

#include <algorithm>
#include <utility>
#include <vector>

#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/xenon_login_startup_registration.h"

#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "chrome/browser/profiles/profile.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/browser/webui_config_map.h"
#include "net/base/filename_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_manager.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"
#include "xenon_overlay/chrome/browser/ui/xenon_reminder_browser_observer.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/ui/webui/dev_test_pages_webui.h"
#include "xenon_overlay/chrome/browser/ui/webui/simple_webui_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_player_by_elec_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_player_electron_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_thunder_2025_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_ui_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_node_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/video_sniffer_webui_controller.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.h"
#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"
#include "xenon_overlay/xenon/chrome/browser/fonts/xenon_font_loader.h"

#if BUILDFLAG(ENABLE_XENON_AI)
#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/toolbar/pinned_toolbar/pinned_toolbar_actions_model.h"
#include "components/prefs/pref_service.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_ai/xenon_ai_side_panel_ui.h"
#include "xenon_overlay/chrome/browser/xenon_prefs.h"
#endif

namespace {

constexpr char kIpcTestOrigin[] = "chrome://xenon-player-by-elec";
constexpr char kPlayerElectronOrigin[] = "chrome://xenon-player-electron";
constexpr char kThunder2025Origin[] = "chrome://thunder-2025";
constexpr char kIpcTestContainerId[] = "xenon-ipc-test";
constexpr char kThunder2025ContainerId[] = "thunder-2025";

void EnableBuiltInIpcOrigins(base::CommandLine* command_line) {
  command_line->AppendSwitch(xenon::ipc::switches::kEnable);
  std::vector<std::string> allowed_origins = base::SplitString(
      command_line->GetSwitchValueASCII(
          xenon::ipc::switches::kAllowedOrigins),
      ",",
      base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  for (const char* origin :
       {kIpcTestOrigin, kPlayerElectronOrigin, kThunder2025Origin}) {
    if (std::find(allowed_origins.begin(), allowed_origins.end(), origin) ==
        allowed_origins.end()) {
      allowed_origins.emplace_back(origin);
    }
  }
  command_line->AppendSwitchASCII(
      xenon::ipc::switches::kAllowedOrigins,
      base::JoinString(allowed_origins, ","));
}

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
  // Hosted BrowserWindows own WebContents associated with a Profile. They must
  // be destroyed before BrowserProcessImpl tears down ProfileManager.
  xenon::XenonElectronWindowHost::GetInstance()->ShutdownForProcessExit();

  // 在有序关闭阶段析构 BrowserCollection 观察者，避免进程退出时
  // 静态析构顺序导致 ObserverList 先于本对象析构而触发 observers_.empty() 的 CHECK
  if (reminder_browser_observer_) {
    reminder_browser_observer_.reset();
  }
  xenon_fonts::UnregisterXunleiFonts();
}

void XenonBrowserMainExtraParts::PreProfileInit() {
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  EnableBuiltInIpcOrigins(command_line);
  const bool has_configured_main =
      command_line->HasSwitch(xenon::ipc::switches::kMainScript) ||
      command_line->HasSwitch(xenon::ipc::switches::kElectronApp);
  if (!has_configured_main) {
    use_embedded_ipc_test_main_ = true;
  }
}

void XenonBrowserMainExtraParts::PostProfileInit(Profile* profile,
                                                 bool is_initial_profile) {
  if (profile->IsRegularProfile()) {
    xenon::XenonExtensionManager::GetInstance()->LoadAllExtensions(profile);
  }

#if BUILDFLAG(ENABLE_XENON_AI)
  MaybePinXenonAiToolbarAction(profile);
#endif

  if (!is_initial_profile) {
    return;
  }

  xenon::XenonManager* manager = xenon::XenonManager::GetInstance();
  manager->SetBrowserContext(profile);
  auto ipc_config = xenon::ipc::mojom::IpcMainConfig::New();
  if (use_embedded_ipc_test_main_) {
    ipc_config->container_id = kIpcTestContainerId;
    std::string source =
        ui::ResourceBundle::GetSharedInstance().LoadDataResourceString(
            IDR_XENON_IPC_TEST_MAIN_JS);
    base::FilePath executable_dir;
    base::PathService::Get(base::DIR_EXE, &executable_dir);
    base::FilePath app_path = executable_dir.AppendASCII("xenon-ipc-test");
    ipc_config->embedded_main_source = std::move(source);
    ipc_config->virtual_main_path =
        app_path.AppendASCII("main.js").AsUTF8Unsafe();
    ipc_config->app_path = app_path.AsUTF8Unsafe();
    ipc_config->app_name = "Xenon IPC Test";
    ipc_config->app_version = "1.0.0";
    if (manager->RegisterElectronIpc(std::move(ipc_config))) {
      manager->SetElectronIpcContainerForOrigin(kIpcTestOrigin,
                                                kIpcTestContainerId);
    }
  } else {
    // An explicit command-line Electron application is itself an activation
    // request, so preserve eager startup for that mode.
    manager->InitializeElectronIpc(std::move(ipc_config));
    manager->SetElectronIpcContainerForOrigin(kIpcTestOrigin, "default");
  }

  base::FilePath executable_dir;
  if (base::PathService::Get(base::DIR_EXE, &executable_dir)) {
    const base::FilePath thunder_runtime_dir =
        executable_dir.AppendASCII("thunder_2025");
    const base::FilePath thunder_app_dir =
        thunder_runtime_dir.AppendASCII("resources").AppendASCII("app");
    const base::FilePath thunder_main_dir =
        thunder_app_dir.AppendASCII("out");
    const base::FilePath thunder_main_path =
        thunder_main_dir.AppendASCII("main.js");
    std::string thunder_main_source;
    if (base::ReadFileToString(thunder_main_path, &thunder_main_source)) {
      auto thunder_config = xenon::ipc::mojom::IpcMainConfig::New();
      thunder_config->container_id = kThunder2025ContainerId;
      thunder_config->embedded_main_source = std::move(thunder_main_source);
      thunder_config->virtual_main_path = thunder_main_path.AsUTF8Unsafe();
      thunder_config->app_path = thunder_app_dir.AsUTF8Unsafe();
      thunder_config->app_name = "Thunder";
      // Preserve the hosted application's executable identity and version.
      // This file is not launched: Xenon still executes the main module.
      thunder_config->executable_path =
          thunder_runtime_dir.AppendASCII("Thunder.exe").AsUTF8Unsafe();
      const std::string thunder_app_version =
          xenon::ipc::GetAppExecutableVersion(base::FilePath::FromUTF8Unsafe(
              thunder_config->executable_path));
      if (!thunder_app_version.empty()) {
        thunder_config->app_version = thunder_app_version;
        thunder_config->default_user_agent = base::StringPrintf(
            "Thunder/%s XDASKernel/%s %s", thunder_app_version.c_str(),
            thunder_app_version.c_str(),
            embedder_support::GetUserAgent().c_str());
      }
      auto renderer_mapping = xenon::ipc::mojom::IpcRendererUrlMapping::New();
      renderer_mapping->source_path_prefix =
          thunder_main_dir.AppendASCII("main-renderer").AsUTF8Unsafe();
      renderer_mapping->target_base_url = "chrome://thunder-2025/";
      thunder_config->renderer_url_mappings.push_back(
          std::move(renderer_mapping));
      const base::FilePath thunder_renderer_archive =
          thunder_app_dir.AppendASCII("renderer.asar");
      if (base::PathExists(thunder_renderer_archive)) {
        auto archive_mapping =
            xenon::ipc::mojom::IpcRendererUrlMapping::New();
        archive_mapping->source_path_prefix = thunder_main_dir.AsUTF8Unsafe();
        archive_mapping->target_base_url =
            net::FilePathToFileURL(thunder_renderer_archive).spec() + "/";
        thunder_config->renderer_url_mappings.push_back(
            std::move(archive_mapping));
      }
      if (manager->RegisterElectronIpc(std::move(thunder_config))) {
        manager->SetElectronIpcContainerForOrigin(kThunder2025Origin,
                                                  kThunder2025ContainerId);
      }
    } else {
      LOG(WARNING) << "Thunder 2025 main script not found: "
                   << thunder_main_path;
    }
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

  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonPlayerConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonPlayerConfig";

  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonPlayerElectronConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered "
               "XenonPlayerElectronConfig";

  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonThunder2025Config>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonThunder2025Config";

  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonPlayerByElecConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonPlayerByElecConfig";

#if BUILDFLAG(ENABLE_XENON_AI)
  content::WebUIConfigMap::GetInstance().AddWebUIConfig(
      std::make_unique<xenon::XenonAiSidePanelUIConfig>());
  LOG(INFO) << "XenonBrowserMainExtraParts: Registered XenonAiSidePanelUIConfig";
#endif

  // 初始化提醒 Manager 并注册 Browser 观察者（为每个窗口创建提醒 Widget + 注册 Ctrl+Shift+R 测试快捷键）
  xenon::XenonReminderNotificationManager::GetInstance()->Initialize();
  reminder_browser_observer_ = std::make_unique<xenon::XenonReminderBrowserObserver>();
  GlobalBrowserCollection::GetInstance()->ForEach(
      [&](BrowserWindowInterface* browser_window) {
        reminder_browser_observer_->AttachToBrowser(
            browser_window->GetBrowserForMigrationOnly());
        return true;
      });

  // Only open the Xenon WebUI overlay when explicitly requested.
  if (base::CommandLine::ForCurrentProcess()->HasSwitch("show-xenon-extension")) {
    xenon::XenonWebDialog::ShowXenonOverlay(profile);
  }
}
