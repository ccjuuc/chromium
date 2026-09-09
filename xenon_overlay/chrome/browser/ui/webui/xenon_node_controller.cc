// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_node_controller.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

#include "base/atomic_sequence_num.h"
#include "base/auto_reset.h"
#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/memory/ref_counted_memory.h"
#include "base/path_service.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_browser_interface_broker_registry.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "content/public/common/referrer.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/url_util.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/base/ui_base_types.h"
#include "ui/views/background.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"
#include "xenon_overlay/resources/webui/xenon_node/grit/xenon_node_webui_resources.h"
#include "xenon_overlay/resources/webui/xenon_node/grit/xenon_node_webui_resources_map.h"
#include "xenon_overlay/resources/webui/xenon_player/grit/xenon_player_webui_resources.h"
#include "xenon_overlay/resources/webui/xenon_player/grit/xenon_player_webui_resources_map.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>  // Must be in front of other Windows header files.

#include <commdlg.h>

#include "ui/display/win/screen_win.h"
#include "ui/views/win/hwnd_util.h"
#endif

namespace xenon {

namespace {

constexpr char kHost[] = "xenon-node";
constexpr char kPlayerHost[] = "xenon-player";
constexpr char kPlayerAppPrefix[] = "app/";
constexpr char kPlayerStaticPrefix[] = "static/";
constexpr char kPlayerFrontendDirSwitch[] = "xenon-player-frontend-dir";
constexpr char kServiceRestartingError[] =
    "Utility service restarted; retry the operation";

base::FilePath ResolvePlayerFrontendDir() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(kPlayerFrontendDirSwitch)) {
    base::FilePath path =
        command_line->GetSwitchValuePath(kPlayerFrontendDirSwitch);
    if (base::DirectoryExists(path)) {
      return path;
    }
    return {};
  }

  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return {};
  }
  base::FilePath path =
      exe_dir.AppendASCII("xenon_player").AppendASCII("frontend");
  return base::DirectoryExists(path) ? path : base::FilePath();
}

bool ShouldHandlePlayerAppRequest(const std::string& path) {
  return base::StartsWith(path, kPlayerAppPrefix) ||
         base::StartsWith(path, kPlayerStaticPrefix);
}

scoped_refptr<base::RefCountedMemory> ReadPlayerFrontendAsset(
    const base::FilePath& frontend_dir,
    const std::string& request_path) {
  if (frontend_dir.empty()) {
    return nullptr;
  }
  std::string relative_path = base::StartsWith(request_path, kPlayerAppPrefix)
                                  ? request_path.substr(
                                        sizeof(kPlayerAppPrefix) - 1)
                                  : request_path;
  if (relative_path.empty() || relative_path == "index.html") {
    relative_path = "index.html";
  }
  relative_path = base::UnescapeURLComponent(relative_path,
                                               base::UnescapeRule::NORMAL);

  const base::FilePath relative =
      base::FilePath::FromUTF8Unsafe(relative_path);
  if (relative.empty() || relative.IsAbsolute() || relative.ReferencesParent()) {
    return nullptr;
  }

  const base::FilePath file_path = frontend_dir.Append(relative);
  std::string contents;
  if (!base::ReadFileToString(file_path, &contents)) {
    return nullptr;
  }
  return base::MakeRefCounted<base::RefCountedString>(std::move(contents));
}

void HandlePlayerAppRequest(
    const base::FilePath& frontend_dir,
    const std::string& path,
    content::WebUIDataSource::GotDataCallback callback) {
  if (frontend_dir.empty()) {
    std::move(callback).Run(nullptr);
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&ReadPlayerFrontendAsset, frontend_dir, path),
      std::move(callback));
}

base::AtomicSequenceNumber& NodeClientIdSequence() {
  static base::AtomicSequenceNumber sequence;
  return sequence;
}

xenon_node::mojom::NodeExportInfoPtr ToPageExportInfo(
    const mojom::NodeExportInfoPtr& export_info) {
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

void EnsureTrustedBrokerKnowsXenonNode() {
  static std::once_flag once;
  std::call_once(once, [] {
    content::WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry()
        .ForWebUI<XenonNodeController>()
        .Add<xenon_node::mojom::PageHandlerFactory>();
  });
}

}  // namespace

XenonNodeController::XenonNodeController(content::WebUI* web_ui,
                                         XenonNodeHostKind host_kind)
    : ui::MojoWebUIController(web_ui, /*enable_chrome_send=*/false),
      client_id_(NodeClientIdSequence().GetNext() + 1),
      node_context_id_(host_kind == XenonNodeHostKind::kPlayer
                           ? "xenon-node-player"
                           : "xenon-node-test"),
      host_kind_(host_kind) {
  EnsureTrustedBrokerKnowsXenonNode();

  const char* host = (host_kind_ == XenonNodeHostKind::kPlayer) ? kPlayerHost : kHost;
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), host);

  if (host_kind_ == XenonNodeHostKind::kPlayer) {
    for (const auto& resource : kXenonPlayerWebuiResources) {
      source->AddResourcePath(resource.path, resource.id);
    }
    source->SetDefaultResource(IDR_XENON_PLAYER_WEBUI_XENON_PLAYER_HTML);
    source->DisableTrustedTypesCSP();
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::ScriptSrc,
        "script-src 'self' chrome://resources 'unsafe-eval';");
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::StyleSrc,
        "style-src 'self' chrome://resources 'unsafe-inline';");
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::ConnectSrc,
        "connect-src 'self';");
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::ImgSrc,
        "img-src 'self' https: http: data: blob:;");
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::MediaSrc,
        "media-src 'self' https: http: data: blob:;");
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::FontSrc,
        "font-src 'self' data:;");

    player_frontend_dir_ = ResolvePlayerFrontendDir();
    base::FilePath executable_path;
    base::PathService::Get(base::FILE_EXE, &executable_path);
    source->AddString("execPath", executable_path.AsUTF8Unsafe());
    source->AddString("frontendDir", player_frontend_dir_.AsUTF8Unsafe());
    source->AddBoolean("hasFrontend", !player_frontend_dir_.empty());
    source->UseStringsJs();
    source->SetRequestFilter(
        base::BindRepeating(&ShouldHandlePlayerAppRequest),
        base::BindRepeating(&HandlePlayerAppRequest, player_frontend_dir_));

    std::string open_dialog;
    content::WebContents* contents = web_ui->GetWebContents();
    if (net::GetValueForKeyInQuery(contents->GetVisibleURL(), "dialog",
                                   &open_dialog) &&
        open_dialog == "1") {
      base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(
              [](base::WeakPtr<content::WebContents> launcher) {
                if (!launcher) {
                  return;
                }
                XenonWebDialog::ShowXenonPlayer(Profile::FromBrowserContext(
                    launcher->GetBrowserContext()));
                launcher->GetController().LoadURL(
                    GURL("chrome://newtab/"), content::Referrer(),
                    ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
              },
              contents->GetWeakPtr()));
    }
  } else {
    for (const auto& resource : kXenonNodeWebuiResources) {
      source->AddResourcePath(resource.path, resource.id);
    }
    source->SetDefaultResource(IDR_XENON_NODE_WEBUI_XENON_NODE_HTML);
  }
}

XenonNodeController::~XenonNodeController() {
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

WEB_UI_CONTROLLER_TYPE_IMPL(XenonNodeController)

void XenonNodeController::PreparePlayerHost(
    PreparePlayerHostCallback callback) {
#if BUILDFLAG(IS_WIN)
  HWND web_ui_window = views::HWNDForNativeWindow(
      web_ui()->GetWebContents()->GetTopLevelNativeWindow());
  if (!web_ui_window) {
    std::move(callback).Run("", "", "Player WebUI has no native window");
    return;
  }

  views::Widget* widget = views::Widget::GetWidgetForNativeWindow(
      web_ui()->GetWebContents()->GetTopLevelNativeWindow());
  if (!widget) {
    std::move(callback).Run("", "", "Player WebUI has no native widget");
    return;
  }
  if (player_widget_ != widget) {
    if (player_widget_) {
      player_widget_->RemoveObserver(this);
    }
    player_widget_ = widget;
    player_widget_->AddObserver(this);
  }

  if (!player_host_widget_) {
    base::DictValue options;
    options.Set("title", "Xenon Player");
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
    options.Set("show", true);
    options.Set("shadow", false);

    XenonWebDialog::ShowWithOptions(
        web_ui()->GetWebContents()->GetBrowserContext(),
        GURL("data:text/html,<body style='margin:0;background:%23000'></body>"),
        options, &player_host_widget_, gfx::NativeView(),
        base::BindOnce(&XenonNodeController::OnPlayerHostClosed,
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

  // Match Electron's `parent: playerParentWnd` relationship. An owned control
  // window stays directly above its video host across activation changes.
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

  SyncPlayerHostWindow();
  std::move(callback).Run(
      base::NumberToString(reinterpret_cast<uintptr_t>(web_ui_window)),
      base::NumberToString(reinterpret_cast<uintptr_t>(parent_window)), "");
#else
  std::move(callback).Run("", "",
                          "Native player hosting is only supported on Windows");
#endif
}

void XenonNodeController::BindPlayerVideoWindow(
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

void XenonNodeController::ShowPlayerVideoHost(bool show) {
  player_window_requested_visible_ = show;
  UpdatePlayerWindow();
}

void XenonNodeController::ControlPlayerWindow(
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
  } else if (action == "toggle-maximize") {
    if (widget->IsMaximized()) {
      widget->Restore();
    } else {
      widget->Maximize();
    }
    state = widget->IsMaximized();
  } else if (action == "close") {
    should_update_player_window = false;
    state = true;
    widget->Close();
  } else if (action == "hide") {
    widget->Hide();
  } else if (action == "show") {
    widget->Show();
    widget->Activate();
    state = true;
  } else if (action == "focus") {
    widget->Activate();
    state = true;
  } else if (action == "fullscreen") {
    widget->SetFullscreen(flag);
    state = widget->IsFullscreen();
  } else if (action == "pin") {
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

  if (should_update_player_window && widget == player_widget_) {
    UpdatePlayerWindow();
  }
  std::move(callback).Run(state, "");
}

void XenonNodeController::OpenNativeFileDialog(
    const std::string& title,
    const std::vector<std::string>& filter_extensions,
    bool allow_multi,
    OpenNativeFileDialogCallback callback) {
#if BUILDFLAG(IS_WIN)
  HWND owner = nullptr;
  if (web_ui()->GetWebContents()->GetTopLevelNativeWindow()) {
    owner = views::HWNDForNativeWindow(
        web_ui()->GetWebContents()->GetTopLevelNativeWindow());
  }

  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
      base::BindOnce(
          [](HWND owner, std::wstring dialog_title,
             std::vector<std::string> extensions,
             bool allow_multi) -> std::vector<std::string> {
            std::wstring patterns;
            for (const std::string& extension : extensions) {
              std::wstring item = base::UTF8ToWide(extension);
              item.erase(std::remove(item.begin(), item.end(), L'.'),
                         item.end());
              if (item.empty() || item.find_first_of(L"\\/:*?\"<>|") !=
                                      std::wstring::npos) {
                continue;
              }
              if (!patterns.empty()) {
                patterns.append(L";");
              }
              patterns.append(L"*.").append(item);
            }
            if (patterns.empty()) {
              patterns = L"*.*";
            }

            std::wstring filter = L"Media files (" + patterns + L")";
            filter.push_back(L'\0');
            filter.append(patterns);
            filter.push_back(L'\0');
            filter.append(L"All files (*.*)");
            filter.push_back(L'\0');
            filter.append(L"*.*");
            filter.push_back(L'\0');

            std::vector<wchar_t> buffer(32768, L'\0');
            OPENFILENAMEW open_file = {};
            open_file.lStructSize = sizeof(open_file);
            open_file.hwndOwner = owner;
            open_file.lpstrFile = buffer.data();
            open_file.nMaxFile = static_cast<DWORD>(buffer.size());
            open_file.lpstrTitle = dialog_title.empty()
                                       ? L"Select media files"
                                       : dialog_title.c_str();
            open_file.lpstrFilter = filter.c_str();
            open_file.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST |
                              OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST;
            if (allow_multi) {
              open_file.Flags |= OFN_ALLOWMULTISELECT;
            }
            if (!::GetOpenFileNameW(&open_file)) {
              return {};
            }

            std::vector<std::string> paths;
            UNSAFE_BUFFERS({
              const wchar_t* item = buffer.data();
              const base::FilePath first(item);
              item += first.value().size() + 1;
              if (*item == L'\0') {
                paths.push_back(first.AsUTF8Unsafe());
              } else {
                while (*item != L'\0') {
                  const base::FilePath path = first.Append(item);
                  paths.push_back(path.AsUTF8Unsafe());
                  item += std::wcslen(item) + 1;
                }
              }
            });
            return paths;
          },
          owner, base::UTF8ToWide(title), filter_extensions, allow_multi),
      std::move(callback));
#else
  std::move(callback).Run({});
#endif
}

void XenonNodeController::ScanDirectoryVideos(
    const std::string& dir_path,
    ScanDirectoryVideosCallback callback) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](std::string path) {
            base::FilePath directory = base::FilePath::FromUTF8Unsafe(path);
            if (base::PathExists(directory) &&
                !base::DirectoryExists(directory)) {
              directory = directory.DirName();
            }

            std::vector<base::FilePath> files;
            if (!base::DirectoryExists(directory)) {
              return std::vector<std::string>();
            }
            base::FileEnumerator enumerator(
                directory, false, base::FileEnumerator::FILES);
            for (base::FilePath file = enumerator.Next(); !file.empty();
                 file = enumerator.Next()) {
              static constexpr const base::FilePath::CharType* kExtensions[] = {
                  FILE_PATH_LITERAL(".3gp"),  FILE_PATH_LITERAL(".asf"),
                  FILE_PATH_LITERAL(".avi"),  FILE_PATH_LITERAL(".divx"),
                  FILE_PATH_LITERAL(".f4v"),  FILE_PATH_LITERAL(".flv"),
                  FILE_PATH_LITERAL(".iso"),  FILE_PATH_LITERAL(".m2ts"),
                  FILE_PATH_LITERAL(".m4v"),  FILE_PATH_LITERAL(".mkv"),
                  FILE_PATH_LITERAL(".mov"),  FILE_PATH_LITERAL(".mp4"),
                  FILE_PATH_LITERAL(".mpeg"), FILE_PATH_LITERAL(".mpg"),
                  FILE_PATH_LITERAL(".mts"),  FILE_PATH_LITERAL(".rmvb"),
                  FILE_PATH_LITERAL(".ts"),   FILE_PATH_LITERAL(".vob"),
                  FILE_PATH_LITERAL(".webm"), FILE_PATH_LITERAL(".wmv"),
              };
              const base::FilePath::StringType extension =
                  file.FinalExtension();
              for (const auto* candidate : kExtensions) {
                if (base::FilePath::CompareEqualIgnoreCase(extension,
                                                           candidate)) {
                  files.push_back(file);
                  break;
                }
              }
            }
            std::sort(files.begin(), files.end());
            std::vector<std::string> paths;
            paths.reserve(files.size());
            for (const base::FilePath& file : files) {
              paths.push_back(file.AsUTF8Unsafe());
            }
            return paths;
          },
          dir_path),
      std::move(callback));
}

void XenonNodeController::OnPlayerHostClosed() {
  player_host_widget_ = nullptr;
  player_window_ = 0;
  player_window_requested_visible_ = false;
  if (player_widget_) {
    player_widget_->Close();
  }
}

void XenonNodeController::DetachPlayerControlWindow() {
#if BUILDFLAG(IS_WIN)
  if (!player_widget_ || !player_host_widget_) {
    return;
  }

  HWND control_hwnd = views::HWNDForWidget(player_widget_);
  HWND host_hwnd = views::HWNDForWidget(player_host_widget_);
  if (control_hwnd && host_hwnd &&
      ::GetWindow(control_hwnd, GW_OWNER) == host_hwnd) {
    ::SetWindowLongPtr(control_hwnd, GWLP_HWNDPARENT, 0);
  }
#endif
}

void XenonNodeController::SyncPlayerHostWindow() {
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
  if (!player_widget_->IsVisible() || player_widget_->IsMinimized()) {
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

void XenonNodeController::UpdatePlayerWindow() {
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
                   SWP_HIDEWINDOW | SWP_NOACTIVATE | SWP_NOMOVE |
                       SWP_NOSIZE);
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

void XenonNodeController::OnWidgetActivationChanged(views::Widget* widget,
                                                     bool active) {
  if (widget == player_widget_ && active) {
    SyncPlayerHostWindow();
  }
}

void XenonNodeController::OnWidgetBoundsChanged(
    views::Widget* widget,
    const gfx::Rect&) {
  if (widget == player_widget_) {
    UpdatePlayerWindow();
  }
}

void XenonNodeController::OnWidgetDestroying(views::Widget* widget) {
  if (widget != player_widget_) {
    return;
  }
  player_widget_->RemoveObserver(this);
  player_window_ = 0;
  player_window_requested_visible_ = false;
  if (player_host_widget_) {
    DetachPlayerControlWindow();
    views::Widget* host = player_host_widget_;
    player_host_widget_ = nullptr;
    host->CloseNow();
  }
  player_widget_ = nullptr;
}

void XenonNodeController::OnWidgetShowStateChanged(views::Widget* widget) {
  if (widget == player_widget_) {
    UpdatePlayerWindow();
  }
}

void XenonNodeController::OnWidgetVisibilityChanged(views::Widget* widget,
                                                     bool) {
  if (widget == player_widget_) {
    UpdatePlayerWindow();
  }
}

mojo::SharedRemote<mojom::XenonMainService>
XenonNodeController::GetBoundServiceRemote() {
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
        base::BindOnce(&XenonNodeController::OnNodeAddonObserverDisconnected,
                       weak_ptr_factory_.GetWeakPtr()));
  }
  if (remote.is_bound() && service_changed) {
    ReplayLoadedModules(remote);
  }
  return remote;
}

void XenonNodeController::ReplayLoadedModules(
    const mojo::SharedRemote<mojom::XenonMainService>& remote) {
  pending_module_reloads_ = 0;
  reload_in_progress_ = false;
  if (loaded_module_paths_.empty()) {
    RunDeferredServiceOperations();
    return;
  }
  reload_in_progress_ = true;
  pending_module_reloads_ = loaded_module_paths_.size();
  const uint64_t replay_generation = service_generation_;
  for (const std::string& path : loaded_module_paths_) {
    remote->LoadAddon(
        node_context_id_, path,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                  base::BindOnce(&XenonNodeController::OnNodeModuleReloaded,
                                 weak_ptr_factory_.GetWeakPtr(),
                                 replay_generation, path),
                  false, "Utility service disconnected during addon reload",
                  std::vector<mojom::NodeExportInfoPtr>()));
  }
}

void XenonNodeController::OnNodeModuleReloaded(
    uint64_t service_generation,
    const std::string& path,
    bool success,
    const std::string& error_msg,
    std::vector<mojom::NodeExportInfoPtr> exports) {
  if (service_generation != service_generation_) {
    return;
  }
  OnNodeModuleLoaded(path, success, error_msg, std::move(exports));
  CHECK_GT(pending_module_reloads_, 0u);
  --pending_module_reloads_;
  if (pending_module_reloads_ == 0) {
    reload_in_progress_ = false;
    RunDeferredServiceOperations();
  }
}

bool XenonNodeController::DeferUntilModulesReloaded(
    base::OnceClosure operation) {
  if (!reload_in_progress_) {
    return false;
  }
  deferred_service_operations_.push_back(std::move(operation));
  return true;
}

void XenonNodeController::RunDeferredServiceOperations() {
  std::vector<base::OnceClosure> operations;
  operations.swap(deferred_service_operations_);
  for (auto& operation : operations) {
    std::move(operation).Run();
  }
}

void XenonNodeController::OnNodeAddonObserverDisconnected() {
  node_addon_observer_receiver_.reset();
}

void XenonNodeController::BindInterface(
    mojo::PendingReceiver<xenon_node::mojom::PageHandlerFactory> receiver) {
  page_factory_receiver_.reset();
  page_factory_receiver_.Bind(std::move(receiver));
}

void XenonNodeController::CreatePageHandler(
    mojo::PendingRemote<xenon_node::mojom::Page> page,
    mojo::PendingReceiver<xenon_node::mojom::PageHandler> receiver) {
  page_.reset();
  page_.Bind(std::move(page));

  page_handler_receiver_.reset();
  page_handler_receiver_.Bind(std::move(receiver));

  node_addon_observer_receiver_.reset();
  GetBoundServiceRemote();
}

void XenonNodeController::RequireNodeModule(const std::string& path) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    DeferUntilModulesReloaded(
        base::BindOnce(&XenonNodeController::RequireNodeModule,
                       weak_ptr_factory_.GetWeakPtr(), path));
    return;
  }
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      page_->NodeModuleLoaded(path, false, "Utility service is not running",
                              {});
    }
    return;
  }

  remote->LoadAddon(node_context_id_, path,
                    mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                        base::BindOnce(&XenonNodeController::OnNodeModuleLoaded,
                                       weak_ptr_factory_.GetWeakPtr(), path),
                        false, "Utility service disconnected during addon load",
                        std::vector<mojom::NodeExportInfoPtr>()));
}

void XenonNodeController::OnNodeModuleLoaded(
    const std::string& path,
    bool success,
    const std::string& error_msg,
    std::vector<mojom::NodeExportInfoPtr> exports) {
  if (!page_.is_bound()) {
    return;
  }
  if (success) {
    loaded_module_paths_.insert(path);
  } else {
    loaded_module_paths_.erase(path);
  }
  page_->NodeModuleLoaded(path, success, error_msg,
                          ToPageExportInfos(exports));
}

void XenonNodeController::InvokeNodeExport(
    int32_t request_id,
    const std::string& module_path,
    const std::string& function_name,
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      page_->NodeInvokeResult(request_id, false, base::Value(), {},
                              kServiceRestartingError);
    }
    return;
  }
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
          base::BindOnce(&XenonNodeController::OnNodeExportInvoked,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(), std::vector<mojom::NodeCallbackResultPtr>(),
          "Utility service disconnected during native invocation"));
}

void XenonNodeController::OnNodeExportInvoked(
    int32_t request_id,
    bool success,
    base::Value result,
    std::vector<mojom::NodeCallbackResultPtr> callback_results,
    const std::string& error_msg) {
  if (!page_.is_bound()) {
    return;
  }

  std::vector<xenon_node::mojom::NodeCallbackResultPtr> page_results;
  page_results.reserve(callback_results.size());
  for (const auto& callback_result : callback_results) {
    auto page_result = xenon_node::mojom::NodeCallbackResult::New();
    page_result->callback_id = callback_result->callback_id;
    page_result->value = callback_result->value.Clone();
    page_results.push_back(std::move(page_result));
  }

  page_->NodeInvokeResult(request_id, success, std::move(result),
                          std::move(page_results), error_msg);
}

void XenonNodeController::InspectNodeExport(int32_t request_id,
                                            const std::string& module_path,
                                            const std::string& export_path) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      page_->NodeInspectResult(request_id, false, kServiceRestartingError,
                               nullptr);
    }
    return;
  }
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
          base::BindOnce(&XenonNodeController::OnNodeExportInspected,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, "Utility service disconnected during export inspection",
          mojom::NodeExportInfoPtr()));
}

void XenonNodeController::OnNodeExportInspected(
    int32_t request_id,
    bool success,
    const std::string& error_msg,
    mojom::NodeExportInfoPtr info) {
  if (!page_.is_bound()) {
    return;
  }
  xenon_node::mojom::NodeExportInfoPtr page_info;
  if (info) {
    page_info = ToPageExportInfo(info);
  }
  page_->NodeInspectResult(request_id, success, error_msg, std::move(page_info));
}

void XenonNodeController::ConstructNodeExport(
    int32_t request_id,
    const std::string& module_path,
    const std::string& export_path,
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      page_->NodeConstructResult(request_id, false, 0, kServiceRestartingError);
    }
    return;
  }
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
          base::BindOnce(&XenonNodeController::OnNodeExportConstructed,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, 0, "Utility service disconnected during native construction"));
}

void XenonNodeController::OnNodeExportConstructed(
    int32_t request_id,
    bool success,
    int32_t instance_id,
    const std::string& error_msg) {
  if (!page_.is_bound()) {
    return;
  }
  page_->NodeConstructResult(request_id, success, instance_id, error_msg);
}

void XenonNodeController::InvokeNodeInstance(
    int32_t request_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& method_name,
    std::vector<xenon_node::mojom::NodeInvokeArgPtr> args) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      page_->NodeInvokeResult(request_id, false, base::Value(), {},
                              kServiceRestartingError);
    }
    return;
  }
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
          base::BindOnce(&XenonNodeController::OnNodeExportInvoked,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(), std::vector<mojom::NodeCallbackResultPtr>(),
          "Utility service disconnected during native instance invocation"));
}

void XenonNodeController::GetNodeInstanceProperty(
    int32_t request_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       kServiceRestartingError);
    return;
  }
  if (!remote.is_bound()) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       "Utility service is not running");
    return;
  }
  remote->GetInstanceProperty(
      node_context_id_, module_path, instance_id, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodePropertyRead,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(),
          "Utility service disconnected during property read"));
}

void XenonNodeController::SetNodeInstanceProperty(
    int32_t request_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name,
    base::Value value) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    OnNodePropertyWritten(request_id, false, kServiceRestartingError);
    return;
  }
  if (!remote.is_bound()) {
    OnNodePropertyWritten(request_id, false, "Utility service is not running");
    return;
  }
  remote->SetInstanceProperty(
      node_context_id_, module_path, instance_id, property_name,
      std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodePropertyWritten,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, "Utility service disconnected during property write"));
}

void XenonNodeController::ReleaseNodeInstance(const std::string& module_path,
                                              int32_t instance_id) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    return;
  }
  if (remote.is_bound()) {
    remote->ReleaseInstance(node_context_id_, module_path, instance_id);
  }
}

void XenonNodeController::GetNodeExportProperty(
    int32_t request_id,
    const std::string& module_path,
    const std::string& object_path,
    const std::string& property_name) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       kServiceRestartingError);
    return;
  }
  if (!remote.is_bound()) {
    OnNodePropertyRead(request_id, false, base::Value(),
                       "Utility service is not running");
    return;
  }
  remote->GetExportProperty(
      node_context_id_, module_path, object_path, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodePropertyRead,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, base::Value(),
          "Utility service disconnected during export property read"));
}

void XenonNodeController::SetNodeExportProperty(
    int32_t request_id,
    const std::string& module_path,
    const std::string& object_path,
    const std::string& property_name,
    base::Value value) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    OnNodePropertyWritten(request_id, false, kServiceRestartingError);
    return;
  }
  if (!remote.is_bound()) {
    OnNodePropertyWritten(request_id, false, "Utility service is not running");
    return;
  }
  remote->SetExportProperty(
      node_context_id_, module_path, object_path, property_name,
      std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          base::BindOnce(&XenonNodeController::OnNodePropertyWritten,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          false, "Utility service disconnected during export property write"));
}

void XenonNodeController::OnCallback(int32_t callback_id,
                                     std::vector<base::Value> args) {
  if (page_.is_bound()) {
    page_->NodeCallbackInvoked(callback_id, std::move(args));
  }
}

void XenonNodeController::OnCallbackReleased(int32_t callback_id) {
  if (page_.is_bound()) {
    page_->NodeCallbackReleased(callback_id);
  }
}

void XenonNodeController::OnNodePropertyRead(int32_t request_id,
                                             bool success,
                                             base::Value result,
                                             const std::string& error_msg) {
  if (page_.is_bound()) {
    page_->NodePropertyResult(request_id, success, std::move(result),
                              error_msg);
  }
}

void XenonNodeController::OnNodePropertyWritten(int32_t request_id,
                                                bool success,
                                                const std::string& error_msg) {
  if (page_.is_bound()) {
    page_->NodeSetPropertyResult(request_id, success, error_msg);
  }
}

void XenonNodeController::InvokeNodeExports(
    int32_t request_id,
    const std::string& module_path,
    std::vector<xenon_node::mojom::NodeInvokeCallPtr> calls) {
  auto remote = GetBoundServiceRemote();
  if (reload_in_progress_) {
    if (page_.is_bound()) {
      OnNodeExportsInvoked(
          request_id,
          MakeServiceInvokeFailures(calls.size(), kServiceRestartingError));
    }
    return;
  }
  if (!remote.is_bound()) {
    if (page_.is_bound()) {
      OnNodeExportsInvoked(request_id,
                           MakeServiceInvokeFailures(
                               calls.size(), "Utility service is not running"));
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
          base::BindOnce(&XenonNodeController::OnNodeExportsInvoked,
                         weak_ptr_factory_.GetWeakPtr(), request_id),
          MakeServiceInvokeFailures(call_count,
                                    "Utility service disconnected during "
                                    "batched native invocation")));
}

void XenonNodeController::OnNodeExportsInvoked(
    int32_t request_id,
    std::vector<mojom::NodeInvokeCallResultPtr> results) {
  if (!page_.is_bound()) {
    return;
  }

  std::vector<xenon_node::mojom::NodeInvokeCallResultPtr> page_results;
  page_results.reserve(results.size());
  for (auto& result : results) {
    auto page_result = xenon_node::mojom::NodeInvokeCallResult::New();
    page_result->success = result->success;
    page_result->result = std::move(result->result);
    page_result->error_msg = result->error_msg;
    page_result->callback_results.reserve(result->callback_results.size());
    for (auto& callback_result : result->callback_results) {
      auto page_cb = xenon_node::mojom::NodeCallbackResult::New();
      page_cb->callback_id = callback_result->callback_id;
      page_cb->value = std::move(callback_result->value);
      page_result->callback_results.push_back(std::move(page_cb));
    }
    page_results.push_back(std::move(page_result));
  }

  page_->NodeInvokeManyResult(request_id, std::move(page_results));
}

XenonNodeConfig::XenonNodeConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kHost) {}

XenonNodeConfig::~XenonNodeConfig() = default;

std::unique_ptr<content::WebUIController> XenonNodeConfig::CreateWebUIController(
    content::WebUI* web_ui,
    const GURL& url) {
  return std::make_unique<XenonNodeController>(web_ui,
                                               XenonNodeHostKind::kNodeTest);
}

XenonPlayerConfig::XenonPlayerConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kPlayerHost) {}

XenonPlayerConfig::~XenonPlayerConfig() = default;

std::unique_ptr<content::WebUIController>
XenonPlayerConfig::CreateWebUIController(content::WebUI* web_ui,
                                         const GURL& url) {
  return std::make_unique<XenonNodeController>(web_ui,
                                               XenonNodeHostKind::kPlayer);
}

}  // namespace xenon
