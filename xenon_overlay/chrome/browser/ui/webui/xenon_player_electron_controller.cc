// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_player_electron_controller.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include "base/atomic_sequence_num.h"
#include "base/auto_reset.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/ref_counted_memory.h"
#include "base/path_service.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/cors_origin_pattern_setter.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/url_util.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "services/network/public/mojom/cors_origin_pattern.mojom.h"
#include "ui/views/background.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/win/hwnd_util.h"
#include "url/gurl.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/resources/webui/xenon_node/grit/xenon_node_webui_resources.h"
#include "xenon_overlay/resources/webui/xenon_node/grit/xenon_node_webui_resources_map.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include "ui/display/win/screen_win.h"
#endif

namespace xenon {

namespace {

constexpr char kPlayerElectronHost[] = "xenon-player-electron";
constexpr char kPlayerFrontendDirSwitch[] = "xenon-player-frontend-dir";

bool HostedFrontendDirectoryExists(const base::FilePath& path) {
  if (base::DirectoryExists(path)) {
    return true;
  }

  base::FilePath archive_path;
  base::FilePath archive_relative_path;
  if (!asar::GetAsarArchivePath(path, &archive_path, &archive_relative_path,
                                true)) {
    return false;
  }
  const auto archive = asar::GetOrCreateAsarArchive(archive_path);
  asar::Archive::FileInfo info;
  bool is_directory = false;
  return archive &&
         archive->StatPath(archive_relative_path, &info, &is_directory) &&
         is_directory;
}

base::AtomicSequenceNumber& PlayerElectronClientIdSequence() {
  static base::AtomicSequenceNumber seq;
  return seq;
}

base::FilePath ResolveHostedFrontendDir(std::string_view directory_switch,
                                        std::string_view directory_name) {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(directory_switch)) {
    base::FilePath path =
        command_line->GetSwitchValuePath(directory_switch);
    if (HostedFrontendDirectoryExists(path)) {
      return path;
    }
    return {};
  }

  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return {};
  }
  base::FilePath path = exe_dir.AppendASCII(directory_name);
  return HostedFrontendDirectoryExists(path) ? path : base::FilePath();
}

std::string HostedElectronAssetPath(const std::string& path) {
  const size_t end = path.find_first_of("?#");
  return end == std::string::npos ? path : path.substr(0, end);
}

bool ShouldHandleHostedElectronRequest(const std::string& path) {
  const std::string file_path = HostedElectronAssetPath(path);
  if (base::StartsWith(file_path, "chrome://") ||
      base::StartsWith(file_path, "resources/") ||
      file_path == "xenon_node.mojom-webui.js") {
    return false;
  }
  // Packed WebUI modules (require.js, boot script, mojom) must not be stolen
  // by the on-disk frontend filter.
  for (const auto& resource : kXenonNodeWebuiResources) {
    if (file_path == resource.path) {
      return false;
    }
  }
  return true;
}

scoped_refptr<base::RefCountedMemory> ReadHostedFrontendAsset(
    const base::FilePath& frontend_dir,
    const std::string& webui_host,
    const std::string& request_path) {
  if (frontend_dir.empty()) {
    return nullptr;
  }
  std::string relative_path = HostedElectronAssetPath(request_path);
  if (relative_path.empty() || relative_path == "index.html") {
    relative_path = "index.html";
  }
  relative_path = base::UnescapeURLComponent(relative_path,
                                               base::UnescapeRule::NORMAL);

  const base::FilePath relative =
      base::FilePath::FromUTF8Unsafe(relative_path);
  if (relative.empty() || relative.IsAbsolute() || relative.ReferencesParent()) {
    LOG(ERROR) << "Hosted Electron " << webui_host << " rejected path=["
               << request_path << "]";
    return nullptr;
  }

  const base::FilePath file_path = frontend_dir.Append(relative);
  std::string contents;
  auto read_asset = [&contents](const base::FilePath& candidate) {
    base::FilePath archive_path;
    base::FilePath archive_relative_path;
    if (asar::GetAsarArchivePath(candidate, &archive_path,
                                 &archive_relative_path)) {
      const auto archive = asar::GetOrCreateAsarArchive(archive_path);
      return archive && archive->ReadFile(archive_relative_path, &contents);
    }
    return base::ReadFileToString(candidate, &contents);
  };

  bool read = read_asset(file_path);
  if (!read) {
    // A renderer archive is only one part of an Electron app tree. Plugins
    // and other hosted assets remain beside that archive (for example,
    // app/plugins/*.asar), so retry relative to the archive's parent instead
    // of incorrectly looking for them under renderer.asar/main-renderer.
    base::FilePath frontend_archive;
    base::FilePath frontend_archive_relative;
    if (asar::GetAsarArchivePath(frontend_dir, &frontend_archive,
                                 &frontend_archive_relative, true)) {
      read = read_asset(frontend_archive.DirName().Append(relative));
    }
  }
  if (!read) {
    LOG(ERROR) << "Hosted Electron " << webui_host << " failed to read "
               << file_path;
    return nullptr;
  }
  return base::MakeRefCounted<base::RefCountedString>(std::move(contents));
}

void HandleHostedFrontendRequest(
    const base::FilePath& frontend_dir,
    const std::string& webui_host,
    const std::string& path,
    content::WebUIDataSource::GotDataCallback callback) {
  if (frontend_dir.empty()) {
    LOG(ERROR) << "Hosted Electron " << webui_host
               << " frontend dir empty for path=[" << path << "]";
    std::move(callback).Run(nullptr);
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadHostedFrontendAsset, frontend_dir, webui_host, path),
      std::move(callback));
}

xenon_node::mojom::NodeExportInfoPtr ToPageExportInfo(
    const mojom::NodeExportInfoPtr& export_info) {
  if (!export_info) return nullptr;
  auto page_info = xenon_node::mojom::NodeExportInfo::New();
  page_info->name = export_info->name;
  page_info->kind = export_info->kind;
  page_info->enumerable = export_info->enumerable;
  page_info->writable = export_info->writable;
  page_info->has_value = export_info->has_value;
  page_info->value = export_info->value.Clone();
  page_info->children.reserve(export_info->children.size());
  for (const auto& child : export_info->children) {
    page_info->children.push_back(ToPageExportInfo(child));
  }
  page_info->prototype.reserve(export_info->prototype.size());
  for (const auto& member : export_info->prototype) {
    page_info->prototype.push_back(ToPageExportInfo(member));
  }
  return page_info;
}

std::vector<xenon_node::mojom::NodeExportInfoPtr> ToPageExportInfos(
    const std::vector<mojom::NodeExportInfoPtr>& exports) {
  std::vector<xenon_node::mojom::NodeExportInfoPtr> page_exports;
  page_exports.reserve(exports.size());
  for (const auto& export_info : exports) {
    page_exports.push_back(ToPageExportInfo(export_info));
  }
  return page_exports;
}

std::vector<mojom::NodeInvokeArgPtr> ToServiceInvokeArgs(
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  std::vector<mojom::NodeInvokeArgPtr> service_args;
  service_args.reserve(args.size());
  for (const auto& arg : args) {
    auto service_arg = mojom::NodeInvokeArg::New();
    service_arg->is_callback = arg->is_callback;
    service_arg->callback_id = arg->callback_id;
    service_arg->value = arg->value.Clone();
    service_args.push_back(std::move(service_arg));
  }
  return service_args;
}

std::vector<mojom::NodeInvokeCallResultPtr> MakeServiceInvokeFailures(
    size_t count,
    const std::string& error_msg) {
  std::vector<mojom::NodeInvokeCallResultPtr> results;
  results.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    auto result = mojom::NodeInvokeCallResult::New();
    result->success = false;
    result->error_msg = error_msg;
    results.push_back(std::move(result));
  }
  return results;
}

void EnsureTrustedBrokerKnowsXenonPlayerElectron() {
  static std::once_flag once;
  std::call_once(once, [] {
    content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
        .ForWebUI<XenonPlayerElectronController>()
        .Add<xenon_node::mojom::PageHandlerFactory>();
  });
}

void AllowHostedCrossOriginAccess(
    content::BrowserContext* browser_context,
    std::string_view webui_host,
    const std::vector<std::string>& allowed_domains) {
  if (!browser_context) {
    return;
  }
  const GURL player_url(base::StringPrintf("%s://%s", content::kChromeUIScheme,
                                           webui_host.data()));
  const url::Origin source_origin = url::Origin::Create(player_url);

  auto make_pattern = [](const char* protocol, const char* domain) {
    return network::mojom::CorsOriginPattern::New(
        protocol, domain, /*port=*/0,
        network::mojom::CorsDomainMatchMode::kAllowSubdomains,
        network::mojom::CorsPortMatchMode::kAllowAnyPort,
        network::mojom::CorsOriginAccessMatchPriority::kDefaultPriority);
  };

  std::vector<network::mojom::CorsOriginPatternPtr> allow_patterns;
  for (const std::string& domain : allowed_domains) {
    allow_patterns.push_back(make_pattern("https", domain.c_str()));
    allow_patterns.push_back(make_pattern("http", domain.c_str()));
  }

  content::CorsOriginPatternSetter::Set(
      browser_context, source_origin, std::move(allow_patterns),
      /*block_patterns=*/{}, base::DoNothing());
}

}  // namespace

XenonPlayerElectronController::XenonPlayerElectronController(
    content::WebUI* web_ui)
    : XenonPlayerElectronController(web_ui,
                                    kPlayerElectronHost,
                                    kPlayerFrontendDirSwitch,
                                    "xenon_player/frontend",
                                    {"xunlei.com"}) {}

XenonPlayerElectronController::XenonPlayerElectronController(
    content::WebUI* web_ui,
    std::string_view webui_host,
    std::string_view frontend_dir_switch,
    std::string_view frontend_dir_name,
    std::vector<std::string> cors_domains)
    : ui::MojoWebUIController(web_ui, /*enable_chrome_send=*/false),
      client_id_(PlayerElectronClientIdSequence().GetNext() + 1),
      node_context_id_(
          XenonManager::GetInstance()->GetElectronIpcContainerForOrigin(
              "chrome://" + std::string(webui_host))) {
  EnsureTrustedBrokerKnowsXenonPlayerElectron();

  content::BrowserContext* const browser_context =
      web_ui->GetWebContents()->GetBrowserContext();
  AllowHostedCrossOriginAccess(browser_context, webui_host, cors_domains);

  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      browser_context, std::string(webui_host));

  for (const auto& resource : kXenonNodeWebuiResources) {
    source->AddResourcePath(resource.path, resource.id);
  }

  source->DisableTrustedTypesCSP();
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ScriptSrc,
      "script-src 'self' chrome://resources 'unsafe-eval' 'unsafe-inline';");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::StyleSrc,
      "style-src 'self' chrome://resources 'unsafe-inline';");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ConnectSrc,
      "connect-src 'self' https: http: data: blob:;");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ImgSrc,
      "img-src 'self' https: http: data: blob:;");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::MediaSrc,
      "media-src 'self' https: http: data: blob: file:;");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::FontSrc,
      "font-src 'self' data:;");

  player_frontend_dir_ =
      ResolveHostedFrontendDir(frontend_dir_switch, frontend_dir_name);
  LOG(INFO) << "Hosted Electron " << webui_host << " frontend_dir="
            << player_frontend_dir_.AsUTF8Unsafe();
  if (player_frontend_dir_.empty()) {
    LOG(ERROR) << "Hosted Electron frontend is missing next to the executable: "
               << frontend_dir_name;
  }
  if (!player_frontend_dir_.empty()) {
    source->SetRequestFilter(
        base::BindRepeating(&ShouldHandleHostedElectronRequest),
        base::BindRepeating(&HandleHostedFrontendRequest, player_frontend_dir_,
                            std::string(webui_host)));
  }
}

XenonPlayerElectronController::~XenonPlayerElectronController() {
  weak_ptr_factory_.InvalidateWeakPtrs();
  if (player_widget_) {
    player_widget_->RemoveObserver(this);
  }
  if (player_host_widget_) {
    DetachPlayerControlWindow();
    views::Widget* host = player_host_widget_;
    player_host_widget_ = nullptr;
    host->CloseNow();
  }
}

WEB_UI_CONTROLLER_TYPE_IMPL(XenonPlayerElectronController)

void XenonPlayerElectronController::BindInterface(
    mojo::PendingReceiver<xenon_node::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void XenonPlayerElectronController::CreatePageHandler(
    mojo::PendingRemote<xenon_node::mojom::Page> page,
    mojo::PendingReceiver<xenon_node::mojom::PageHandler> receiver) {
  LOG(INFO) << "XenonPlayerElectron CreatePageHandler client_id=" << client_id_;
  page_.reset();
  page_.Bind(std::move(page));
  page_handler_receiver_.reset();
  page_handler_receiver_.Bind(std::move(receiver));
}

void XenonPlayerElectronController::PreparePlayerHost(
    PreparePlayerHostCallback callback) {
  // Creating the host Widget/WebContents nested inside this Mojo reply
  // deadlocks the player renderer (it is waiting for this callback). Run the
  // window work on a fresh UI task so DevTools and boot can keep progressing.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&XenonPlayerElectronController::DoPreparePlayerHost,
                     weak_ptr_factory_.GetWeakPtr(), std::move(callback)));
}

void XenonPlayerElectronController::DoPreparePlayerHost(
    PreparePlayerHostCallback callback) {
#if BUILDFLAG(IS_WIN)
  LOG(INFO) << "DoPreparePlayerHost: start";
  content::WebContents* web_contents = web_ui()->GetWebContents();
  if (!web_contents) {
    std::move(callback).Run("", "", "Player WebUI has no WebContents");
    return;
  }

  if (BrowserWindowInterface* browser_window =
          GlobalBrowserCollection::GetInstance()->FindBrowserWithTab(
              web_contents)) {
    LOG(WARNING) << "DoPreparePlayerHost: opened as a tab; showing dialog";
    Browser* browser = browser_window->GetBrowserForMigrationOnly();
    if (browser) {
      XenonWebDialog::ShowXenonPlayerElectron(browser->GetProfile());
    }
    std::move(callback).Run(
        "", "", "Open the player from the sidebar, not as a browser tab");
    return;
  }

  HWND web_ui_window = views::HWNDForNativeWindow(
      web_contents->GetTopLevelNativeWindow());
  if (!web_ui_window) {
    std::move(callback).Run("", "", "Player WebUI has no native window");
    return;
  }

  views::Widget* top_widget =
      views::Widget::GetWidgetForNativeWindow(
          web_contents->GetTopLevelNativeWindow());
  if (!top_widget) {
    std::move(callback).Run("", "", "Failed to resolve player WebUI widget");
    return;
  }

  if (player_widget_ != top_widget) {
    if (player_widget_) {
      player_widget_->RemoveObserver(this);
    }
    player_widget_ = top_widget;
    player_widget_->AddObserver(this);
  }

  HWND parent_hwnd = views::HWNDForWidget(player_widget_);
  if (!parent_hwnd) {
    std::move(callback).Run("", "", "Failed to resolve top-level native window");
    return;
  }

  // Make the control window visible before it becomes owned by the host.
  // Windows will not Show() an owned HWND whose owner is hidden.
  if (!player_widget_->IsVisible()) {
    player_widget_->Show();
  }

  if (!player_host_widget_) {
    base::DictValue options;
    options.Set("title", "Xenon Player Host");
    options.Set("width", 1280);
    options.Set("height", 800);
    options.Set("modal", false);
    options.Set("frame", false);
    options.Set("dwm", true);
    options.Set("resizable", false);
    options.Set("minimizable", false);
    options.Set("maximizable", false);
    options.Set("showCloseButton", false);
    options.Set("skipTaskbar", true);
    // Host must be visible before it owns the control WebDialog. Hiding an
    // owner HWND also hides every owned window, which made the player vanish.
    options.Set("show", true);
    options.Set("shadow", false);

    XenonWebDialog::ShowWithOptions(
        web_contents->GetBrowserContext(),
        GURL("data:text/html,<body style='margin:0;background:%23000'></body>"),
        options, &player_host_widget_, gfx::NativeView(),
        base::BindOnce(&XenonPlayerElectronController::OnPlayerHostClosed,
                       weak_ptr_factory_.GetWeakPtr()));
  }
  if (!player_host_widget_ || !player_host_widget_->GetNativeWindow()) {
    std::move(callback).Run("", "", "Failed to create player host window");
    return;
  }

  HWND parent_window = views::HWNDForWidget(player_host_widget_);
  if (!parent_window) {
    std::move(callback).Run("", "", "Player host has no native window");
    return;
  }

  // Match Electron's parent: video host owns the control WebDialog.
  if (::GetWindow(web_ui_window, GW_OWNER) != parent_window) {
    ::SetLastError(ERROR_SUCCESS);
    const LONG_PTR previous_owner = ::SetWindowLongPtr(
        web_ui_window, GWLP_HWNDPARENT,
        reinterpret_cast<LONG_PTR>(parent_window));
    if (!previous_owner && ::GetLastError() != ERROR_SUCCESS) {
      std::move(callback).Run("", "",
                              "Failed to attach player control window");
      return;
    }
  }

  if (views::View* host_view = player_host_widget_->GetContentsView()) {
    host_view->SetBackground(views::CreateSolidBackground(SK_ColorBLACK));
  }

  if (!player_widget_->IsVisible()) {
    player_widget_->Show();
  }
  SyncPlayerHostWindow();
  LOG(INFO) << "DoPreparePlayerHost: ready ph="
            << reinterpret_cast<uintptr_t>(web_ui_window)
            << " ch=" << reinterpret_cast<uintptr_t>(parent_window);
  std::move(callback).Run(
      base::NumberToString(reinterpret_cast<uintptr_t>(web_ui_window)),
      base::NumberToString(reinterpret_cast<uintptr_t>(parent_window)), "");
#else
  std::move(callback).Run("", "",
                          "Native player hosting is only supported on Windows");
#endif
}

void XenonPlayerElectronController::BindPlayerVideoWindow(
    const std::string& player_window,
    BindPlayerVideoWindowCallback callback) {
#if BUILDFLAG(IS_WIN)
  uint64_t player_window_value = 0;
  if (!base::StringToUint64(player_window, &player_window_value) ||
      player_window_value == 0 ||
      player_window_value > std::numeric_limits<uintptr_t>::max()) {
    std::move(callback).Run("Invalid native player window handle");
    return;
  }

  HWND player_hwnd = reinterpret_cast<HWND>(
      static_cast<uintptr_t>(player_window_value));
  HWND host_hwnd =
      player_host_widget_ ? views::HWNDForWidget(player_host_widget_) : nullptr;
  if (!host_hwnd || !::IsWindow(player_hwnd)) {
    std::move(callback).Run("Invalid native player window or host window");
    return;
  }

  if (::GetParent(player_hwnd) != host_hwnd) {
    ::SetParent(player_hwnd, host_hwnd);
    LONG_PTR style = ::GetWindowLongPtr(player_hwnd, GWL_STYLE);
    ::SetWindowLongPtr(player_hwnd, GWL_STYLE, (style | WS_CHILD) & ~WS_POPUP);
  }

  player_window_ = reinterpret_cast<uintptr_t>(player_hwnd);
  UpdatePlayerWindow();
  std::move(callback).Run("");
#else
  std::move(callback).Run(
      "Native player hosting is only supported on Windows");
#endif
}

void XenonPlayerElectronController::ShowPlayerVideoHost(bool show) {
  player_window_requested_visible_ = show;
  UpdatePlayerWindow();
}

void XenonPlayerElectronController::ControlPlayerWindow(
    const std::string& action,
    bool flag,
    ControlPlayerWindowCallback callback) {
  views::Widget* widget = player_widget_;
  if (!widget) {
    widget = views::Widget::GetWidgetForNativeWindow(
        web_ui()->GetWebContents()->GetTopLevelNativeWindow());
  }
  if (!widget) {
    std::move(callback).Run(false, "Player WebUI has no native widget");
    return;
  }

  bool state = false;
  bool should_update_player_window = true;
  if (action == "minimize") {
    widget->Minimize();
    state = true;
  } else if (action == "maximize" || action == "toggle-maximize") {
    if (widget->IsMaximized()) {
      widget->Restore();
    } else {
      widget->Maximize();
    }
    state = widget->IsMaximized();
  } else if (action == "fullscreen") {
    widget->SetFullscreen(flag);
    state = widget->IsFullscreen();
  } else if (action == "close") {
    should_update_player_window = false;
    widget->Close();
    state = true;
  } else if (action == "hide") {
    widget->Hide();
  } else if (action == "show") {
    widget->Show();
    widget->Activate();
    state = true;
  } else if (action == "focus") {
    widget->Activate();
    state = true;
  } else if (action == "isMaximized") {
    state = widget->IsMaximized();
    should_update_player_window = false;
  } else if (action == "isFullscreen") {
    state = widget->IsFullscreen();
    should_update_player_window = false;
  } else if (action == "alwaysOnTop" || action == "pin") {
    const ui::ZOrderLevel level = flag ? ui::ZOrderLevel::kFloatingWindow
                                       : ui::ZOrderLevel::kNormal;
    widget->SetZOrderLevel(level);
    if (player_host_widget_) {
      player_host_widget_->SetZOrderLevel(level);
    }
    state = flag;
  } else {
    std::move(callback).Run(false,
                            "Unknown player window action: " + action);
    return;
  }

  if (should_update_player_window) {
    UpdatePlayerWindow();
  }
  std::move(callback).Run(state, "");
}

void XenonPlayerElectronController::OpenNativeFileDialog(
    const std::string& title,
    const std::vector<std::string>& filter_extensions,
    bool allow_multi,
    OpenNativeFileDialogCallback callback) {
  // Packaged player "打开文件" used to hop through Utility ipcMain
  // (`openElectronSelectFileDialog` → `dialog.showOpenDialogSync`). After a
  // Utility crash that pipe is dead, so the picker never appears. This Page
  // Handler runs on the Browser UI thread; IFileOpenDialog belongs here, not
  // on a worker (GetOpenFileNameW off-thread is why the dialog was flaky).
  bool directory = false;
  std::vector<std::string> extensions;
  extensions.reserve(filter_extensions.size());
  for (const auto& ext : filter_extensions) {
    if (ext == "__pick_directory__") {
      directory = true;
      continue;
    }
    extensions.push_back(ext);
  }
  LOG(INFO) << "PlayerElectron OpenNativeFileDialog title=" << title
            << " directory=" << directory << " multi=" << allow_multi;
  std::move(callback).Run(
      XenonElectronWindowHost::GetInstance()->ShowOpenDialog(
          title, directory, allow_multi, extensions));
}

void XenonPlayerElectronController::ScanDirectoryVideos(
    const std::string& dir_path,
    ScanDirectoryVideosCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](std::string dir_str) -> std::vector<std::string> {
            std::vector<std::string> results;
            base::FilePath dir = base::FilePath::FromUTF8Unsafe(dir_str);
            if (!base::DirectoryExists(dir)) return results;

            static const wchar_t* const kVideoExts[] = {
                L".mp4", L".mkv", L".avi", L".rmvb", L".wmv", L".flv", L".mov", L".ts"};
            base::FileEnumerator enumerator(dir, false,
                                           base::FileEnumerator::FILES);
            for (base::FilePath file = enumerator.Next(); !file.empty();
                 file = enumerator.Next()) {
              std::wstring ext = base::ToLowerASCII(file.Extension());
              for (const wchar_t* valid_ext : kVideoExts) {
                if (ext == valid_ext) {
                  results.push_back(file.AsUTF8Unsafe());
                  break;
                }
              }
            }
            return results;
          },
          dir_path),
      std::move(callback));
}

mojo::SharedRemote<mojom::XenonMainService>
XenonPlayerElectronController::GetBoundServiceRemote() {
  XenonManager* manager = XenonManager::GetInstance();
  manager->SetBrowserContext(web_ui()->GetWebContents()->GetBrowserContext());
  auto remote = manager->DuplicateServiceRemote(node_context_id_);
  const uint64_t previous_generation = service_generation_;
  const bool service_changed =
      service_generation_ != manager->service_generation(node_context_id_);
  if (service_changed) {
    service_generation_ = manager->service_generation(node_context_id_);
    node_addon_observer_receiver_.reset();
    if (previous_generation != 0 && page_.is_bound()) {
      page_->NodeServiceReset();
    }
  }
  if (remote.is_bound() && page_.is_bound() &&
      !node_addon_observer_receiver_.is_bound()) {
    remote->SetNodeAddonObserver(
        node_context_id_, client_id_,
        node_addon_observer_receiver_.BindNewPipeAndPassRemote());
    node_addon_observer_receiver_.set_disconnect_handler(
        base::BindOnce(
            &XenonPlayerElectronController::OnNodeAddonObserverDisconnected,
            weak_ptr_factory_.GetWeakPtr()));
  }
  if (remote.is_bound() && service_changed) {
    ReplayLoadedModules(remote);
  }
  return remote;
}

void XenonPlayerElectronController::ReplayLoadedModules(
    const mojo::SharedRemote<mojom::XenonMainService>& remote) {
  for (const std::string& path : loaded_module_paths_) {
    remote->LoadAddon(
        node_context_id_, path,
        base::BindOnce([](bool success, const std::string& error,
                                std::vector<mojom::NodeExportInfoPtr>) {
          if (!success) {
            LOG(WARNING) << "Failed to reload native addon after Utility "
                            "relaunch: "
                         << error;
          }
        }));
  }
}

void XenonPlayerElectronController::OnNodeAddonObserverDisconnected() {
  node_addon_observer_receiver_.reset();
}

void XenonPlayerElectronController::RequireNodeModule(
    const std::string& path) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeModuleLoaded(path, false, "Utility service is not running", {});
    }
    return;
  }
  remote->LoadAddon(
      node_context_id_, path,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonPlayerElectronController::OnNodeModuleLoaded,
                         weak_ptr_factory_.GetWeakPtr(), path),
          false, "Utility service disconnected during addon load",
          std::vector<mojom::NodeExportInfoPtr>()));
}

void XenonPlayerElectronController::OnNodeModuleLoaded(
    const std::string& path,
    bool success,
    const std::string& error_msg,
    std::vector<mojom::NodeExportInfoPtr> exports) {
  if (!page_.is_bound()) return;
  if (success) {
    loaded_module_paths_.insert(path);
  } else {
    loaded_module_paths_.erase(path);
  }
  page_->NodeModuleLoaded(path, success, error_msg,
                          ToPageExportInfos(exports));
}

void XenonPlayerElectronController::InvokeNodeExport(
    int32_t request_id,
    const std::string& module_path,
    const std::string& function_name,
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeInvokeResult(request_id, false, base::Value(), {},
                              "Utility service is not running");
    }
    return;
  }
  remote->InvokeFunction(
      node_context_id_, client_id_, module_path, function_name,
      ToServiceInvokeArgs(std::move(args)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonPlayerElectronController::OnNodeExportInvoked,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(), std::vector<mojom::NodeCallbackResultPtr>(),
          "Utility service disconnected during native invocation"));
}

void XenonPlayerElectronController::OnNodeExportInvoked(
    int32_t request_id,
    bool success,
    base::Value result,
    std::vector<mojom::NodeCallbackResultPtr> callback_results,
    const std::string& error_msg) {
  if (!page_.is_bound()) return;

  std::vector<xenon_node::mojom::NodeCallbackResultPtr> page_results;
  page_results.reserve(callback_results.size());
  for (const auto& cb : callback_results) {
    auto page_cb = xenon_node::mojom::NodeCallbackResult::New();
    page_cb->callback_id = cb->callback_id;
    page_cb->value = cb->value.Clone();
    page_results.push_back(std::move(page_cb));
  }
  page_->NodeInvokeResult(request_id, success, std::move(result),
                          std::move(page_results), error_msg);
}

void XenonPlayerElectronController::InspectNodeExport(
    int32_t request_id,
    const std::string& module_path,
    const std::string& export_path) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeInspectResult(request_id, false,
                               "Utility service is not running", nullptr);
    }
    return;
  }
  remote->InspectExport(
      node_context_id_, module_path, export_path,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonPlayerElectronController::OnNodeExportInspected,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, "Utility service disconnected during export inspection",
          mojom::NodeExportInfoPtr()));
}

void XenonPlayerElectronController::OnNodeExportInspected(
    int32_t request_id,
    bool success,
    const std::string& error_msg,
    mojom::NodeExportInfoPtr info) {
  if (!page_.is_bound()) return;
  xenon_node::mojom::NodeExportInfoPtr page_info;
  if (info) {
    page_info = ToPageExportInfo(info);
  }
  page_->NodeInspectResult(request_id, success, error_msg, std::move(page_info));
}

void XenonPlayerElectronController::ConstructNodeExport(
    int32_t request_id,
    const std::string& module_path,
    const std::string& export_path,
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeConstructResult(request_id, false, 0,
                                 "Utility service is not running");
    }
    return;
  }
  remote->ConstructExport(
      node_context_id_, client_id_, module_path, export_path,
      ToServiceInvokeArgs(std::move(args)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(
              &XenonPlayerElectronController::OnNodeExportConstructed,
              weak_ptr_factory_.GetWeakPtr(), request_id),
          false, 0, "Utility service disconnected during native construction"));
}

void XenonPlayerElectronController::OnNodeExportConstructed(
    int32_t request_id,
    bool success,
    int32_t instance_id,
    const std::string& error_msg) {
  if (!page_.is_bound()) return;
  page_->NodeConstructResult(request_id, success, instance_id, error_msg);
}

void XenonPlayerElectronController::InvokeNodeInstance(
    int32_t request_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& method_name,
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeInvokeResult(request_id, false, base::Value(), {},
                              "Utility service is not running");
    }
    return;
  }
  remote->InvokeInstance(
      node_context_id_, client_id_, module_path, instance_id, method_name,
      ToServiceInvokeArgs(std::move(args)),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonPlayerElectronController::OnNodeExportInvoked,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(), std::vector<mojom::NodeCallbackResultPtr>(),
          "Utility service disconnected during native instance invocation"));
}

void XenonPlayerElectronController::GetNodeInstanceProperty(
    int32_t request_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       "Utility service is not running");
    return;
  }
  remote->GetInstanceProperty(
      node_context_id_, module_path, instance_id, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonPlayerElectronController::OnNodePropertyRead,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(),
          "Utility service disconnected during property read"));
}

void XenonPlayerElectronController::SetNodeInstanceProperty(
    int32_t request_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name,
    base::Value value) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    OnNodePropertyWritten(request_id, false, "Utility service is not running");
    return;
  }
  remote->SetInstanceProperty(
      node_context_id_, module_path, instance_id, property_name,
      std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonPlayerElectronController::OnNodePropertyWritten,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, "Utility service disconnected during property write"));
}

void XenonPlayerElectronController::ReleaseNodeInstance(
    const std::string& module_path,
    int32_t instance_id) {
  auto remote = GetBoundServiceRemote();
  if (remote.is_bound()) {
    remote->ReleaseInstance(node_context_id_, module_path, instance_id);
  }
}

void XenonPlayerElectronController::GetNodeExportProperty(
    int32_t request_id,
    const std::string& module_path,
    const std::string& object_path,
    const std::string& property_name) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       "Utility service is not running");
    return;
  }
  remote->GetExportProperty(
      node_context_id_, module_path, object_path, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonPlayerElectronController::OnNodePropertyRead,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(),
          "Utility service disconnected during export property read"));
}

void XenonPlayerElectronController::SetNodeExportProperty(
    int32_t request_id,
    const std::string& module_path,
    const std::string& object_path,
    const std::string& property_name,
    base::Value value) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    OnNodePropertyWritten(request_id, false, "Utility service is not running");
    return;
  }
  remote->SetExportProperty(
      node_context_id_, module_path, object_path, property_name,
      std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonPlayerElectronController::OnNodePropertyWritten,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, "Utility service disconnected during export property write"));
}

void XenonPlayerElectronController::InvokeNodeExports(
    int32_t request_id,
    const std::string& module_path,
    std::vector<xenon_node::mojom::NodeInvokeCallPtr> calls) {
  auto remote = GetBoundServiceRemote();
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      OnNodeExportsInvoked(
          request_id,
          MakeServiceInvokeFailures(calls.size(), "Utility service is not running"));
    }
    return;
  }
  std::vector<mojom::NodeInvokeCallPtr> service_calls;
  service_calls.reserve(calls.size());
  for (auto& call : calls) {
    auto service_call = mojom::NodeInvokeCall::New();
    service_call->function_name = call->function_name;
    service_call->args = ToServiceInvokeArgs(std::move(call->args));
    service_calls.push_back(std::move(service_call));
  }
  const size_t call_count = service_calls.size();
  remote->InvokeMany(
      node_context_id_, module_path, std::move(service_calls),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonPlayerElectronController::OnNodeExportsInvoked,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          MakeServiceInvokeFailures(call_count,
                                    "Utility service disconnected during batched native invocation")));
}

void XenonPlayerElectronController::OnNodeExportsInvoked(
    int32_t request_id,
    std::vector<mojom::NodeInvokeCallResultPtr> results) {
  if (page_.is_bound()) {
    std::vector<xenon_node::mojom::NodeInvokeCallResultPtr> page_results;
    page_results.reserve(results.size());
    for (auto& result : results) {
      auto page_res = xenon_node::mojom::NodeInvokeCallResult::New();
      page_res->success = result->success;
      page_res->result = std::move(result->result);
      page_res->error_msg = result->error_msg;
      page_res->callback_results.reserve(result->callback_results.size());
      for (auto& callback_result : result->callback_results) {
        auto page_cb = xenon_node::mojom::NodeCallbackResult::New();
        page_cb->callback_id = callback_result->callback_id;
        page_cb->value = std::move(callback_result->value);
        page_res->callback_results.push_back(std::move(page_cb));
      }
      page_results.push_back(std::move(page_res));
    }
    page_->NodeInvokeManyResult(request_id, std::move(page_results));
  }
}

void XenonPlayerElectronController::OnNodePropertyRead(
    int32_t request_id,
    bool success,
    base::Value result,
    const std::string& error_msg) {
  if (page_.is_bound()) {
    page_->NodePropertyResult(request_id, success, std::move(result),
                              error_msg);
  }
}

void XenonPlayerElectronController::OnNodePropertyWritten(
    int32_t request_id,
    bool success,
    const std::string& error_msg) {
  if (page_.is_bound()) {
    page_->NodeSetPropertyResult(request_id, success, error_msg);
  }
}

void XenonPlayerElectronController::OnCallback(
    int32_t callback_id,
    std::vector<base::Value> args) {
  if (page_.is_bound()) {
    page_->NodeCallbackInvoked(callback_id, std::move(args));
  }
}

void XenonPlayerElectronController::OnCallbackReleased(int32_t callback_id) {
  if (page_.is_bound()) {
    page_->NodeCallbackReleased(callback_id);
  }
}

void XenonPlayerElectronController::OnWidgetActivationChanged(
    views::Widget* widget,
    bool active) {
  SyncPlayerHostWindow();
}

void XenonPlayerElectronController::OnWidgetBoundsChanged(
    views::Widget* widget,
    const gfx::Rect& new_bounds) {
  SyncPlayerHostWindow();
}

void XenonPlayerElectronController::OnWidgetDestroying(views::Widget* widget) {
  if (widget != player_widget_) {
    return;
  }
  DetachPlayerControlWindow();
  player_widget_->RemoveObserver(this);
  player_widget_ = nullptr;
  player_window_ = 0;
  player_window_requested_visible_ = false;
  if (player_host_widget_) {
    views::Widget* host = player_host_widget_;
    player_host_widget_ = nullptr;
    host->CloseNow();
  }
}

void XenonPlayerElectronController::OnWidgetShowStateChanged(
    views::Widget* widget) {
  SyncPlayerHostWindow();
}

void XenonPlayerElectronController::OnWidgetVisibilityChanged(
    views::Widget* widget,
    bool visible) {
  SyncPlayerHostWindow();
}

void XenonPlayerElectronController::OnPlayerHostClosed() {
  player_host_widget_ = nullptr;
  player_window_ = 0;
  player_window_requested_visible_ = false;
  if (player_widget_) {
    player_widget_->Close();
  }
}

void XenonPlayerElectronController::DetachPlayerControlWindow() {
#if BUILDFLAG(IS_WIN)
  if (player_window_ && ::IsWindow(reinterpret_cast<HWND>(player_window_))) {
    HWND player_hwnd = reinterpret_cast<HWND>(player_window_);
    HWND host_hwnd =
        player_host_widget_ ? views::HWNDForWidget(player_host_widget_) : nullptr;
    if (host_hwnd && ::GetParent(player_hwnd) == host_hwnd) {
      ::ShowWindow(player_hwnd, SW_HIDE);
      ::SetParent(player_hwnd, nullptr);
    }
  }
  if (player_widget_ && player_host_widget_) {
    HWND control_hwnd = views::HWNDForWidget(player_widget_);
    HWND host_hwnd = views::HWNDForWidget(player_host_widget_);
    if (control_hwnd && host_hwnd &&
        ::GetWindow(control_hwnd, GW_OWNER) == host_hwnd) {
      ::SetWindowLongPtr(control_hwnd, GWLP_HWNDPARENT, 0);
    }
  }
#endif
  player_window_ = 0;
}

void XenonPlayerElectronController::SyncPlayerHostWindow() {
#if BUILDFLAG(IS_WIN)
  if (syncing_player_windows_ || !player_widget_ || !player_host_widget_) {
    return;
  }
  base::AutoReset<bool> syncing(&syncing_player_windows_, true);

  HWND control_hwnd = views::HWNDForWidget(player_widget_);
  HWND host_hwnd = views::HWNDForWidget(player_host_widget_);
  if (!control_hwnd || !host_hwnd) {
    return;
  }

  content::WebContents* control_contents = web_ui()->GetWebContents();
  if (!control_contents) {
    return;
  }
  const gfx::Rect content_bounds =
      display::win::GetScreenWin()->DIPToScreenRect(
          control_hwnd, control_contents->GetContainerBounds());
  if (player_host_widget_->GetWindowBoundsInScreen() != content_bounds) {
    player_host_widget_->SetBounds(content_bounds);
  }
  // Never Hide() the host just because the control is not visible yet.
  // The host owns the control HWND (GWLP_HWNDPARENT); hiding the owner hides
  // the dialog during first-show, which looks like the popup never appeared.
  if (player_widget_->IsMinimized()) {
    if (player_host_widget_->IsVisible()) {
      player_host_widget_->Hide();
    }
    return;
  }

  if (!player_host_widget_->IsVisible()) {
    player_host_widget_->ShowInactive();
  }
  ::SetWindowPos(host_hwnd, control_hwnd, 0, 0, 0, 0,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
#endif
}

void XenonPlayerElectronController::UpdatePlayerWindow() {
#if BUILDFLAG(IS_WIN)
  SyncPlayerHostWindow();
  if (!player_window_ || !player_widget_ || !player_host_widget_) {
    return;
  }

  HWND player_hwnd =
      reinterpret_cast<HWND>(static_cast<uintptr_t>(player_window_));
  HWND host_hwnd = views::HWNDForWidget(player_host_widget_);
  if (!::IsWindow(player_hwnd) || !host_hwnd ||
      ::GetParent(player_hwnd) != host_hwnd) {
    player_window_ = 0;
    return;
  }

  const bool should_show = player_window_requested_visible_ &&
                           player_widget_->IsVisible() &&
                           !player_widget_->IsMinimized();
  if (!should_show) {
    ::SetWindowPos(player_hwnd, HWND_TOP, 0, 0, 0, 0,
                   SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE);
    return;
  }

  RECT host_client_bounds = {};
  if (!::GetClientRect(host_hwnd, &host_client_bounds)) {
    return;
  }
  ::SetWindowPos(player_hwnd, HWND_TOP, 0, 0,
                 host_client_bounds.right - host_client_bounds.left,
                 host_client_bounds.bottom - host_client_bounds.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
#endif
}

XenonPlayerElectronConfig::XenonPlayerElectronConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kPlayerElectronHost) {}

XenonPlayerElectronConfig::~XenonPlayerElectronConfig() = default;

std::unique_ptr<content::WebUIController>
XenonPlayerElectronConfig::CreateWebUIController(content::WebUI* web_ui,
                                                const GURL& url) {
  return std::make_unique<XenonPlayerElectronController>(web_ui);
}

}  // namespace xenon
