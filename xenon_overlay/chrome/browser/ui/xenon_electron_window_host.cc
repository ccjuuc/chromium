// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <utility>
#include <vector>

#include "base/auto_reset.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "third_party/blink/public/common/user_agent/user_agent_metadata.h"
#include "third_party/blink/public/common/web_preferences/web_preferences.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/base/models/image_model.h"
#include "ui/base/mojom/menu_source_type.mojom.h"
#include "ui/base/page_transition_types.h"
#include "ui/display/screen.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/point.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/image/image_skia.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/menus/simple_menu_model.h"
#include "ui/views/controls/menu/menu_runner.h"
#include "ui/views/controls/webview/web_dialog_view.h"
#include "ui/views/widget/widget.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_guest.h"
#include "xenon_overlay/chrome/browser/ui/xenon_menu_runner.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"

#if BUILDFLAG(IS_WIN)
#include <shobjidl.h>
#include <windows.h>

#include <commctrl.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include "base/win/windows_version.h"
#include "ui/display/win/screen_win.h"
#include "ui/views/win/hwnd_util.h"
#endif

namespace xenon {
namespace {

std::optional<int> FindInteger(const base::DictValue& dict,
                               const std::string& key) {
  const base::Value* value = dict.Find(key);
  if (!value) {
    return std::nullopt;
  }
  if (value->is_int()) {
    return value->GetInt();
  }
  if (value->is_double() && std::isfinite(value->GetDouble())) {
    return static_cast<int>(std::lround(value->GetDouble()));
  }
  return std::nullopt;
}

base::Value BoundsToValue(const gfx::Rect& bounds) {
  base::DictValue value;
  value.Set("x", bounds.x());
  value.Set("y", bounds.y());
  value.Set("width", bounds.width());
  value.Set("height", bounds.height());
  return base::Value(std::move(value));
}

#if BUILDFLAG(IS_WIN)
constexpr UINT_PTR kElectronWindowSubclassId = 0x58454e4f;  // "XENO"

void ConfigureFramelessDwmWindow(HWND hwnd, bool round_corners) {
  if (!hwnd || base::win::GetVersion() < base::win::Version::WIN11) {
    return;
  }
  // A top-level transparent BrowserWindow is an unconstrained layered canvas;
  // applying the system rounded clip would incorrectly trim its contents.
  // Owned overlays may opt into the native parent's rounded outer clip.
  DWM_WINDOW_CORNER_PREFERENCE corner_preference =
      round_corners ? DWMWCP_ROUND : DWMWCP_DONOTROUND;
  ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner_preference, sizeof(corner_preference));

  // DWMWA_BORDER_COLOR is intentionally numeric so the code also builds with
  // Windows SDKs predating the named constant.
  constexpr DWORD kDwmwaBorderColor = 34;
  constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;
  COLORREF border_color = kDwmColorNone;
  ::DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border_color,
                          sizeof(border_color));
}

bool CanResetDwmAppearance(uint32_t message) {
  return message == WM_SHOWWINDOW || message == WM_NCACTIVATE ||
         message == WM_THEMECHANGED ||
         message == WM_DWMCOLORIZATIONCOLORCHANGED ||
         message == WM_SETTINGCHANGE || message == WM_STYLECHANGED;
}

gfx::Rect GetVisibleWindowBoundsInScreen(views::Widget* widget) {
  if (!widget) {
    return gfx::Rect();
  }
  const gfx::Rect window_bounds = widget->GetWindowBoundsInScreen();
  HWND hwnd = views::HWNDForWidget(widget);
  RECT visible_bounds = {};
  if (!hwnd || FAILED(::DwmGetWindowAttribute(
                   hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &visible_bounds,
                   sizeof(visible_bounds))) ||
      visible_bounds.right <= visible_bounds.left ||
      visible_bounds.bottom <= visible_bounds.top) {
    return window_bounds;
  }
  return display::win::GetScreenWin()->ScreenToDIPRect(
      hwnd, gfx::Rect(visible_bounds.left, visible_bounds.top,
                      visible_bounds.right - visible_bounds.left,
                      visible_bounds.bottom - visible_bounds.top));
}

void SetVisibleWindowBounds(views::Widget* widget,
                            const gfx::Rect& visible_bounds) {
  if (!widget || visible_bounds.IsEmpty()) {
    return;
  }
  gfx::Rect window_bounds = widget->GetWindowBoundsInScreen();
  const gfx::Rect current_visible_bounds =
      GetVisibleWindowBoundsInScreen(widget);
  if (current_visible_bounds == visible_bounds) {
    return;
  }

  // GetWindowRect includes the invisible resize frame while
  // DWMWA_EXTENDED_FRAME_BOUNDS does not. Preserve those per-window insets so
  // windows with different opacity/style still share one visible outer edge.
  const int left = current_visible_bounds.x() - window_bounds.x();
  const int top = current_visible_bounds.y() - window_bounds.y();
  const int right = window_bounds.right() - current_visible_bounds.right();
  const int bottom = window_bounds.bottom() - current_visible_bounds.bottom();
  window_bounds.SetRect(
      visible_bounds.x() - left, visible_bounds.y() - top,
      std::max(1, visible_bounds.width() + left + right),
      std::max(1, visible_bounds.height() + top + bottom));
  widget->SetBounds(window_bounds);

  // The target may have crossed to a monitor with another device scale
  // factor. Correct the residual after Windows has recomputed its frame.
  const gfx::Rect actual_visible_bounds =
      GetVisibleWindowBoundsInScreen(widget);
  if (actual_visible_bounds != visible_bounds) {
    window_bounds = widget->GetWindowBoundsInScreen();
    window_bounds.Offset(visible_bounds.x() - actual_visible_bounds.x(),
                         visible_bounds.y() - actual_visible_bounds.y());
    window_bounds.set_width(std::max(
        1, window_bounds.width() + visible_bounds.width() -
               actual_visible_bounds.width()));
    window_bounds.set_height(std::max(
        1, window_bounds.height() + visible_bounds.height() -
               actual_visible_bounds.height()));
    widget->SetBounds(window_bounds);
  }
}

LRESULT CALLBACK ElectronWindowSubclassProc(HWND hwnd,
                                             UINT message,
                                             WPARAM w_param,
                                             LPARAM l_param,
                                             UINT_PTR subclass_id,
                                             DWORD_PTR) {
  int64_t handled_result = 0;
  if (XenonElectronWindowHost::GetInstance()
          ->HandleNativeNonClientMessage(
              static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd)),
              message, static_cast<uint64_t>(w_param),
              static_cast<int64_t>(l_param), &handled_result)) {
    return static_cast<LRESULT>(handled_result);
  }
  if (message == WM_NCDESTROY) {
    ::RemoveWindowSubclass(hwnd, ElectronWindowSubclassProc, subclass_id);
  }
  const LRESULT result =
      ::DefSubclassProc(hwnd, message, w_param, l_param);
  XenonElectronWindowHost::GetInstance()->OnNativeWindowMessage(
      static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd)), message,
      static_cast<uint64_t>(w_param), static_cast<int64_t>(l_param));
  return result;
}
#else
gfx::Rect GetVisibleWindowBoundsInScreen(views::Widget* widget) {
  return widget ? widget->GetWindowBoundsInScreen() : gfx::Rect();
}

void SetVisibleWindowBounds(views::Widget* widget,
                            const gfx::Rect& visible_bounds) {
  if (widget && !visible_bounds.IsEmpty() &&
      widget->GetWindowBoundsInScreen() != visible_bounds) {
    widget->SetBounds(visible_bounds);
  }
}
#endif

#if BUILDFLAG(IS_WIN)
void ResizeNativeHostChildren(views::Widget* widget) {
  HWND host = widget ? views::HWNDForWidget(widget) : nullptr;
  RECT client = {};
  if (!host || !::GetClientRect(host, &client)) {
    return;
  }
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  for (HWND child = ::GetWindow(host, GW_CHILD); child;
       child = ::GetWindow(child, GW_HWNDNEXT)) {
    ::SetWindowPos(child, nullptr, 0, 0, width, height,
                   SWP_NOACTIVATE | SWP_NOZORDER);
  }
}

void AppendShellItemPath(IShellItem* item, std::vector<std::string>* out) {
  if (!item || !out) {
    return;
  }
  PWSTR path_w = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path_w)) || !path_w) {
    return;
  }
  out->push_back(base::WideToUTF8(path_w));
  ::CoTaskMemFree(path_w);
}
#endif

ui::ImageModel LoadMenuIcon(const base::DictValue& dict) {
  const std::string* path = dict.FindString("icon");
  if (!path || path->empty()) {
    return ui::ImageModel();
  }
  std::optional<std::vector<uint8_t>> bytes =
      base::ReadFileToBytes(base::FilePath::FromUTF8Unsafe(*path));
  if (!bytes || bytes->empty()) {
    return ui::ImageModel();
  }
  const SkBitmap bitmap = gfx::PNGCodec::Decode(*bytes);
  if (bitmap.isNull() || bitmap.empty()) {
    return ui::ImageModel();
  }
  return ui::ImageModel::FromImageSkia(
      gfx::ImageSkia::CreateFrom1xBitmap(bitmap));
}

}  // namespace

class XenonElectronWindowHost::HostedWebContentsObserver
    : public content::WebContentsObserver {
 public:
  HostedWebContentsObserver(XenonElectronWindowHost* owner,
                            int32_t window_id,
                            content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents),
        owner_(owner),
        window_id_(window_id) {}

  void ReadyToCommitNavigation(
      content::NavigationHandle* navigation_handle) override {
    if (web_contents()) {
      blink::web_pref::WebPreferences prefs =
          web_contents()->GetOrCreateWebPreferences();
      if (!prefs.allow_scripts_to_close_windows) {
        prefs.allow_scripts_to_close_windows = true;
        web_contents()->SetWebPreferences(prefs);
      }
    }
  }

  void DidFinishNavigation(
      content::NavigationHandle* navigation_handle) override {
    if (navigation_handle && navigation_handle->IsInPrimaryMainFrame() &&
        navigation_handle->HasCommitted() && web_contents()) {
      blink::web_pref::WebPreferences prefs =
          web_contents()->GetOrCreateWebPreferences();
      if (!prefs.allow_scripts_to_close_windows) {
        prefs.allow_scripts_to_close_windows = true;
        web_contents()->SetWebPreferences(prefs);
      }
    }
  }

  void DOMContentLoaded(content::RenderFrameHost* render_frame_host) override {
    if (!IsLoadedPrimaryMainFrame(render_frame_host)) {
      return;
    }
    owner_->NotifyEvent(window_id_, "web-contents-dom-ready", base::Value());
    auto entry = owner_->windows_.find(window_id_);
    if (entry != owner_->windows_.end() &&
        !entry->second.ready_to_show_emitted) {
      entry->second.ready_to_show_emitted = true;
      owner_->NotifyEvent(window_id_, "ready-to-show", base::Value());
    }
  }

  void DidFirstVisuallyNonEmptyPaint() override {
    auto entry = owner_->windows_.find(window_id_);
    if (entry != owner_->windows_.end() &&
        !entry->second.ready_to_show_emitted &&
        entry->second.has_loaded_url) {
      entry->second.ready_to_show_emitted = true;
      owner_->NotifyEvent(window_id_, "ready-to-show", base::Value());
    }
  }

  void DidFinishLoad(content::RenderFrameHost* render_frame_host,
                     const GURL&) override {
    if (!IsLoadedPrimaryMainFrame(render_frame_host)) {
      return;
    }

    owner_->NotifyEvent(window_id_, "web-contents-did-finish-load",
                        base::Value());
    auto entry = owner_->windows_.find(window_id_);
    if (entry != owner_->windows_.end() &&
        !entry->second.ready_to_show_emitted) {
      entry->second.ready_to_show_emitted = true;
      owner_->NotifyEvent(window_id_, "ready-to-show", base::Value());
    }
  }

 private:
  bool IsLoadedPrimaryMainFrame(
      content::RenderFrameHost* render_frame_host) const {
    if (!render_frame_host || !web_contents() ||
        web_contents()->GetPrimaryMainFrame() != render_frame_host) {
      return false;
    }
    auto entry = owner_->windows_.find(window_id_);
    if (entry == owner_->windows_.end() || !entry->second.has_loaded_url) {
      return false;
    }
    return web_contents()->GetLastCommittedURL().spec() != "about:blank";
  }

  raw_ptr<XenonElectronWindowHost> owner_;
  const int32_t window_id_;
};

class XenonElectronWindowHost::PopupMenuSession
    : public ui::SimpleMenuModel::Delegate {
 public:
  PopupMenuSession(XenonElectronWindowHost* host, int32_t window_id)
      : host_(host), window_id_(window_id) {}

  PopupMenuSession(const PopupMenuSession&) = delete;
  PopupMenuSession& operator=(const PopupMenuSession&) = delete;
  ~PopupMenuSession() override = default;

  int32_t window_id() const { return window_id_; }

  void BuildAndRun(views::Widget* widget,
                   const base::ListValue& items,
                   const gfx::Point& anchor) {
    model_ = std::make_unique<ui::SimpleMenuModel>(this);
    AppendItems(model_.get(), items);
    runner_ = std::make_unique<XenonMenuRunner>(
        model_.get(), views::MenuRunner::CONTEXT_MENU,
        base::BindRepeating(&PopupMenuSession::OnRunnerClosed,
                            base::Unretained(this)));
    runner_->RunMenuAt(widget, nullptr, gfx::Rect(anchor, gfx::Size()),
                       views::MenuAnchorPosition::kTopLeft,
                       ui::mojom::MenuSourceType::kMouse);
  }

  bool IsCommandIdChecked(int command_id) const override {
    auto it = checked_.find(command_id);
    return it != checked_.end() && it->second;
  }
  bool IsCommandIdEnabled(int command_id) const override {
    auto it = enabled_.find(command_id);
    return it == enabled_.end() || it->second;
  }
  bool IsCommandIdVisible(int command_id) const override {
    auto it = visible_.find(command_id);
    return it == visible_.end() || it->second;
  }
  void ExecuteCommand(int command_id, int event_flags) override {
    base::DictValue args;
    args.Set("commandId", command_id);
    host_->NotifyEvent(window_id_, "native-menu-command",
                       base::Value(std::move(args)));
  }

 private:
  void AppendItems(ui::SimpleMenuModel* model, const base::ListValue& items) {
    for (const base::Value& value : items) {
      if (!value.is_dict()) {
        continue;
      }
      const base::DictValue& dict = value.GetDict();
      const std::string* type_ptr = dict.FindString("type");
      const std::string type = type_ptr ? *type_ptr : "normal";
      if (type == "separator") {
        model->AddSeparator(ui::NORMAL_SEPARATOR);
        continue;
      }
      const int id = FindInteger(dict, "id").value_or(0);
      if (id <= 0) {
        continue;
      }
      const std::string* label_ptr = dict.FindString("label");
      const std::u16string label =
          base::UTF8ToUTF16(label_ptr ? *label_ptr : std::string());
      enabled_[id] = dict.FindBool("enabled").value_or(true);
      visible_[id] = dict.FindBool("visible").value_or(true);
      checked_[id] = dict.FindBool("checked").value_or(false);
      const ui::ImageModel icon = LoadMenuIcon(dict);
      if (type == "submenu") {
        auto sub = std::make_unique<ui::SimpleMenuModel>(this);
        if (const base::ListValue* submenu = dict.FindList("submenu")) {
          AppendItems(sub.get(), *submenu);
        }
        if (!icon.IsEmpty()) {
          model->AddSubMenuWithIcon(id, label, sub.get(), icon);
        } else {
          model->AddSubMenu(id, label, sub.get());
        }
        submenus_.push_back(std::move(sub));
        continue;
      }
      if (type == "checkbox") {
        model->AddCheckItem(id, label);
        continue;
      }
      if (type == "radio") {
        model->AddRadioItem(id, label,
                            FindInteger(dict, "groupId").value_or(0));
        continue;
      }
      if (!icon.IsEmpty()) {
        model->AddItemWithIcon(id, label, icon);
      } else {
        model->AddItem(id, label);
      }
    }
  }

  void OnRunnerClosed() { host_->OnPopupMenuClosed(this); }

  const raw_ptr<XenonElectronWindowHost> host_;
  const int32_t window_id_;
  std::unique_ptr<ui::SimpleMenuModel> model_;
  std::vector<std::unique_ptr<ui::SimpleMenuModel>> submenus_;
  std::unique_ptr<XenonMenuRunner> runner_;
  std::map<int, bool> checked_;
  std::map<int, bool> enabled_;
  std::map<int, bool> visible_;
};

XenonElectronWindowHost* XenonElectronWindowHost::GetInstance() {
  static base::NoDestructor<XenonElectronWindowHost> instance;
  return instance.get();
}

XenonElectronWindowHost::XenonElectronWindowHost() = default;

XenonElectronWindowHost::~XenonElectronWindowHost() {
  popup_menu_.reset();
  for (auto& [id, entry] : windows_) {
    if (entry.widget) {
      entry.widget->RemoveObserver(this);
    }
  }
}

views::Widget* XenonElectronWindowHost::FindWidget(int32_t window_id) const {
  auto it = windows_.find(window_id);
  return it == windows_.end() ? nullptr : it->second.widget.get();
}

int32_t XenonElectronWindowHost::FindEntryWindowForContainer(
    const std::string& container_id) const {
  for (const auto& [id, entry] : windows_) {
    if (entry.widget && entry.has_loaded_url &&
        entry.container_id == container_id) {
      return id;
    }
  }
  return 0;
}

bool XenonElectronWindowHost::ActivateEntryWindow(int32_t window_id) {
  std::vector<int32_t> window_chain;
  std::set<int32_t> visited;
  for (int32_t id = window_id; id > 0 && visited.insert(id).second;) {
    auto it = windows_.find(id);
    if (it == windows_.end() || !it->second.widget) {
      break;
    }
    window_chain.push_back(id);
    id = it->second.parent_id;
  }
  if (window_chain.empty()) {
    return false;
  }

  // Show owner before owned content. Only the selected entry window and its
  // owner chain are activated; unrelated show:false windows in the same
  // container retain normal Electron visibility semantics.
  for (auto it = window_chain.rbegin(); it != window_chain.rend(); ++it) {
    windows_.at(*it).widget->Show();
  }
  windows_.at(window_chain.back()).widget->Activate();
  return true;
}

std::map<int32_t, XenonElectronWindowHost::Entry>::iterator
XenonElectronWindowHost::FindEntry(views::Widget* widget) {
  return std::find_if(windows_.begin(), windows_.end(),
                      [widget](const auto& item) {
                        return item.second.widget == widget;
                      });
}

bool XenonElectronWindowHost::CreateHostedWindow(content::BrowserContext* context,
                                           int width,
                                           int height,
                                           bool show,
                                           bool frame,
                                           bool transparent,
                                           int32_t parent_id,
                                           const std::string& title,
                                           const std::string& container_id,
                                           int32_t* window_id,
                                           uint64_t* hwnd,
                                           std::string* error) {
  if (shutting_down_) {
    *error = "Electron window host is shutting down";
    return false;
  }
  if (!context) {
    *error = "No browser context for Electron BrowserWindow";
    return false;
  }

  gfx::NativeView parent_view = gfx::NativeView();
  if (parent_id > 0) {
    if (views::Widget* parent = FindWidget(parent_id)) {
      parent_view = parent->GetNativeView();
    }
  }

  const int32_t id = next_id_++;
  Entry& entry = windows_[id];
  entry.frameless = !frame;
  entry.transparent = transparent;
  entry.parent_id = parent_id;
  entry.container_id = container_id;
  entry.sync_bounds_with_parent = transparent && parent_id > 0;

  base::DictValue options;
  options.Set("title", title.empty() ? "Electron Window" : title);
  options.Set("width", width > 0 ? width : 800);
  options.Set("height", height > 0 ? height : 600);
  options.Set("modal", false);
  options.Set("frame", frame);
  options.Set("dwm", !transparent);
  // DWM rounded clipping is only meaningful for an owned overlay that follows
  // its native parent. Top-level transparent canvases stay unrounded.
  options.Set("systemRoundedCorners",
              transparent && !frame && parent_id > 0);
  options.Set("resizable", true);
  options.Set("minimizable", true);
  options.Set("maximizable", true);
  options.Set("showCloseButton", true);
  options.Set("shadow", false);
  // Construct the final Widget and WebContents now, but do not paint the
  // initial about:blank surface. This keeps the HWND stable for native addons
  // and avoids replacing a live Views client tree during loadURL/loadFile.
  options.Set("show", false);
  options.Set("skipTaskbar", transparent || parent_id > 0);

  XenonWebDialog::ShowWithOptions(context, GURL("about:blank"), options,
                                  &entry.widget, parent_view,
                                  base::OnceClosure());
  if (!entry.widget) {
    windows_.erase(id);
    *error = "Failed to create Electron BrowserWindow widget";
    return false;
  }
  auto* dialog_view =
      static_cast<views::WebDialogView*>(entry.widget->widget_delegate());
  if (!dialog_view || !dialog_view->web_contents()) {
    entry.widget->CloseNow();
    windows_.erase(id);
    *error = "Failed to create Electron BrowserWindow WebContents";
    return false;
  }
  entry.web_contents_observer = std::make_unique<HostedWebContentsObserver>(
      this, id, dialog_view->web_contents());
  blink::web_pref::WebPreferences prefs =
      dialog_view->web_contents()->GetOrCreateWebPreferences();
  prefs.allow_scripts_to_close_windows = true;
  dialog_view->web_contents()->SetWebPreferences(prefs);
  const std::string default_ua =
      xenon::XenonManager::GetInstance()->GetDefaultUserAgent(container_id);
  if (!default_ua.empty()) {
    entry.user_agent = default_ua;
    blink::UserAgentOverride override;
    override.ua_string_override = default_ua;
    override.ua_metadata_override = embedder_support::GetUserAgentMetadata();
    dialog_view->web_contents()->SetUserAgentOverride(override, false);
  }
  XenonWebDialog::SetHostedContentVisible(entry.widget, false);
  if (show) {
    entry.widget->Show();
  }
  entry.widget->AddObserver(this);

#if BUILDFLAG(IS_WIN)
  HWND native = views::HWNDForWidget(entry.widget);
  entry.hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(native));
  if (native &&
      !::SetWindowSubclass(native, ElectronWindowSubclassProc,
                           kElectronWindowSubclassId, 0)) {
    LOG(WARNING) << "Failed to subclass Electron BrowserWindow id=" << id;
  }
  if (parent_id > 0) {
    auto parent_it = windows_.find(parent_id);
    if (parent_it != windows_.end() && parent_it->second.hwnd) {
      HWND parent_hwnd = reinterpret_cast<HWND>(
          static_cast<uintptr_t>(parent_it->second.hwnd));
      if (native && parent_hwnd) {
        ::SetWindowLongPtr(native, GWLP_HWNDPARENT,
                           reinterpret_cast<LONG_PTR>(parent_hwnd));
      }
    }
  }
  if (entry.frameless) {
    ConfigureFramelessDwmWindow(native, entry.sync_bounds_with_parent);
  }
  if (native && entry.frameless && !entry.has_loaded_url) {
    // Keep WS_THICKFRAME for resize semantics and the DWM shadow, but force a
    // new WM_NCCALCSIZE through our subclass so a frameless native host has no
    // one-pixel non-client strip inside its visible DWM bounds.
    ::SetWindowPos(native, nullptr, 0, 0, 0, 0,
                   SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                       SWP_NOACTIVATE);
  }
#else
  entry.hwnd = 0;
#endif

  // NativeFrameView::GetWindowBoundsForClientBounds uses AdjustWindowRectEx
  // and can inflate a translucent HWND. Preserve the BrowserWindow dimensions
  // requested by the application. Owned overlays follow the parent instead.
  if (!entry.sync_bounds_with_parent) {
    gfx::Rect visible = GetVisibleWindowBoundsInScreen(entry.widget);
    if (width > 0) {
      visible.set_width(width);
    }
    if (height > 0) {
      visible.set_height(height);
    }
    SetVisibleWindowBounds(entry.widget, visible);
  }

  // An owned overlay starts on its parent's visible DWM edge. The two HWNDs
  // can have different invisible resize-frame insets even when their
  // GetWindowRect values are identical.
  if (entry.sync_bounds_with_parent) {
    if (views::Widget* parent = FindWidget(entry.parent_id)) {
      SetVisibleWindowBounds(entry.widget,
                             GetVisibleWindowBoundsInScreen(parent));
    }
  }
  entry.bounds = GetVisibleWindowBoundsInScreen(entry.widget);
  entry.normal_bounds = entry.bounds;

  *window_id = id;
  *hwnd = entry.hwnd;
  *error = std::string();
  LOG(INFO) << "Electron BrowserWindow id=" << id << " hwnd=" << entry.hwnd
            << " container=" << container_id << " webdialog-pending";
  return true;
}

void XenonElectronWindowHost::LoadURL(int32_t window_id,
                                      const std::string& url) {
  if (auto* guest = ipc::XenonElectronGuest::FromId(window_id)) {
    guest->LoadURL(url);
    return;
  }
  auto it = windows_.find(window_id);
  if (it == windows_.end() || !it->second.widget) {
    LOG(ERROR) << "LoadURL unknown Electron window id=" << window_id;
    return;
  }
  Entry& entry = it->second;
  entry.url = url;
  views::Widget* widget = entry.widget;
  auto* dialog_view =
      static_cast<views::WebDialogView*>(widget->widget_delegate());
  content::WebContents* web_contents =
      dialog_view ? dialog_view->web_contents() : nullptr;
  if (!web_contents) {
    LOG(ERROR) << "LoadURL Electron window has no WebContents id=" << window_id;
    return;
  }
  const bool is_initial_navigation = !entry.has_loaded_url;
  entry.has_loaded_url = true;
  XenonWebDialog::SetHostedContentVisible(widget, true);
  const GURL destination(url);
  XenonWebDialog::SetHostedContentURL(widget, destination);
  blink::web_pref::WebPreferences prefs =
      web_contents->GetOrCreateWebPreferences();
  if (!prefs.allow_scripts_to_close_windows) {
    prefs.allow_scripts_to_close_windows = true;
    web_contents->SetWebPreferences(prefs);
  }
  if (!entry.user_agent.empty()) {
    blink::UserAgentOverride override;
    override.ua_string_override = entry.user_agent;
    override.ua_metadata_override = embedder_support::GetUserAgentMetadata();
    web_contents->SetUserAgentOverride(override, false);
  }
  LOG(INFO) << "Electron BrowserWindow loadURL id=" << window_id
            << " url=" << url;
  content::NavigationController::LoadURLParams params(destination);
  params.transition_type = ui::PageTransitionFromInt(
      ui::PAGE_TRANSITION_TYPED | ui::PAGE_TRANSITION_FROM_ADDRESS_BAR);
  // Electron WebContents::LoadURL always opts the navigation into the
  // WebContents user-agent override. This also lets a setUserAgent() call made
  // while the navigation is in flight update/reload it consistently.
  params.override_user_agent =
      content::NavigationController::UA_OVERRIDE_TRUE;
  if (is_initial_navigation) {
    // Replace the initial about:blank navigation entry so that the window's
    // session history length remains 1. Under WHATWG HTML specification
    // (#script-closable), a top-level browsing context with history length == 1
    // is natively script-closable by window.close().
    params.should_replace_current_entry = true;
  }
  web_contents->GetController().LoadURLWithParams(params);

  if (pending_activate_ ||
      pending_activate_containers_.contains(entry.container_id)) {
    pending_activate_ = false;
    pending_activate_containers_.erase(entry.container_id);
    ActivateEntryWindow(window_id);
  }
}

void XenonElectronWindowHost::SetVisible(int32_t window_id, bool visible) {
  views::Widget* widget = FindWidget(window_id);
  LOG(INFO) << "Electron BrowserWindow SetVisible id=" << window_id
            << " visible=" << visible
            << (widget ? "" : " (unknown window)");
  if (!widget) {
    return;
  }
  if (visible) {
    widget->Show();
    widget->Activate();
#if BUILDFLAG(IS_WIN)
    // Match BrowserWindow.show(): make the native window foreground-visible.
    if (HWND hwnd = views::HWNDForWidget(widget)) {
      ::ShowWindow(hwnd, SW_SHOW);
      ::SetForegroundWindow(hwnd);
    }
#endif
  } else {
    widget->Hide();
#if BUILDFLAG(IS_WIN)
    // Frameless / DWM / owned HWNDs can remain painted after Widget::Hide().
    if (HWND hwnd = views::HWNDForWidget(widget)) {
      ::ShowWindow(hwnd, SW_HIDE);
    }
#endif
  }
}

bool XenonElectronWindowHost::Call(int32_t window_id,
                                   const std::string& command,
                                   const base::Value& arguments,
                                   base::Value* result,
                                   std::string* error) {
  if (auto* guest = ipc::XenonElectronGuest::FromId(window_id)) {
    return guest->Call(command, arguments, result, error);
  }
  auto it = windows_.find(window_id);
  if (it == windows_.end() || !it->second.widget) {
    *error = "Unknown Electron BrowserWindow id=" +
             base::NumberToString(window_id);
    return false;
  }
  if (command == "hide" || command == "show" || command == "close" ||
      command == "set-always-on-top" || command == "minimize" ||
      command == "focus" || command == "popup-menu") {
    VLOG(1) << "Electron BrowserWindow Call id=" << window_id
            << " command=" << command;
  }
  Entry& entry = it->second;
  views::Widget* widget = entry.widget;
  const base::DictValue* options =
      arguments.is_dict() ? &arguments.GetDict() : nullptr;
  *result = base::Value();
  error->clear();

  if (command == "set-user-agent" || command == "get-user-agent") {
    auto* dialog_view =
        static_cast<views::WebDialogView*>(widget->widget_delegate());
    content::WebContents* web_contents =
        dialog_view ? dialog_view->web_contents() : nullptr;
    if (!web_contents) {
      *error = "BrowserWindow WebContents is unavailable";
      return false;
    }
    if (command == "set-user-agent") {
      const std::string* value = options ? options->FindString("value") : nullptr;
      if (!value) {
        *error = "set-user-agent expects a string value";
        return false;
      }
      entry.user_agent = *value;
      blink::UserAgentOverride override;
      override.ua_string_override = *value;
      if (!value->empty()) {
        override.ua_metadata_override =
            embedder_support::GetUserAgentMetadata();
      }
      web_contents->SetUserAgentOverride(override, false);
      return true;
    }
    entry.user_agent =
        web_contents->GetUserAgentOverride().ua_string_override;
    *result = base::Value(entry.user_agent.empty()
                              ? embedder_support::GetUserAgent()
                              : entry.user_agent);
    return true;
  }

  if (command == "execute-javascript") {
    auto* dialog_view =
        static_cast<views::WebDialogView*>(widget->widget_delegate());
    content::WebContents* web_contents =
        dialog_view ? dialog_view->web_contents() : nullptr;
    if (web_contents && options) {
      const std::string* code = options->FindString("code");
      if (code && !code->empty()) {
        web_contents->GetPrimaryMainFrame()->ExecuteJavaScript(
            base::UTF8ToUTF16(*code), base::NullCallback());
      }
    }
    *result = base::Value();
    return true;
  }

  if (command == "close" || command == "destroy") {
    Close(window_id);
    return true;
  }
  if (command == "set-bounds") {
    if (!options) {
      *error = "set-bounds expects an object";
      return false;
    }
    gfx::Rect bounds = GetVisibleWindowBoundsInScreen(widget);
    if (std::optional<int> value = FindInteger(*options, "x")) {
      bounds.set_x(*value);
    }
    if (std::optional<int> value = FindInteger(*options, "y")) {
      bounds.set_y(*value);
    }
    if (std::optional<int> value = FindInteger(*options, "width")) {
      bounds.set_width(std::max(1, *value));
    }
    if (std::optional<int> value = FindInteger(*options, "height")) {
      bounds.set_height(std::max(1, *value));
    }
    SetVisibleWindowBounds(widget, bounds);
    *result = BoundsToValue(GetVisibleWindowBoundsInScreen(widget));
    return true;
  }
  if (command == "get-bounds") {
    *result = BoundsToValue(GetVisibleWindowBoundsInScreen(widget));
    return true;
  }
  if (command == "get-normal-bounds") {
    *result = BoundsToValue(entry.normal_bounds.IsEmpty()
                                ? GetVisibleWindowBoundsInScreen(widget)
                                : entry.normal_bounds);
    return true;
  }
  if (command == "show") {
    widget->Show();
#if BUILDFLAG(IS_WIN)
    if (HWND hwnd = views::HWNDForWidget(widget)) {
      ::ShowWindow(hwnd, SW_SHOW);
    }
#endif
    return true;
  }
  if (command == "show-inactive") {
    widget->ShowInactive();
#if BUILDFLAG(IS_WIN)
    if (HWND hwnd = views::HWNDForWidget(widget)) {
      ::ShowWindow(hwnd, SW_SHOWNA);
    }
#endif
    return true;
  }
  if (command == "hide") {
    widget->Hide();
#if BUILDFLAG(IS_WIN)
    if (HWND hwnd = views::HWNDForWidget(widget)) {
      ::ShowWindow(hwnd, SW_HIDE);
    }
#endif
    return true;
  }
  if (command == "is-visible") {
    *result = base::Value(widget->IsVisible());
    return true;
  }
  if (command == "focus") {
    widget->Activate();
#if BUILDFLAG(IS_WIN)
    if (HWND hwnd = views::HWNDForWidget(widget)) {
      ::SetForegroundWindow(hwnd);
    }
#endif
    return true;
  }
  if (command == "blur") {
    widget->Deactivate();
    return true;
  }
  if (command == "is-focused") {
    *result = base::Value(widget->IsActive());
    return true;
  }
  if (command == "minimize") {
    // An owned overlay is hidden automatically when its owner is minimized.
    // Minimizing both HWNDs independently can destroy or detach a layered
    // owned window on Windows; Electron treats the pair as one window here.
    if (entry.sync_bounds_with_parent) {
      if (views::Widget* parent = FindWidget(entry.parent_id)) {
        parent->Minimize();
        return true;
      }
    }
    widget->Minimize();
    return true;
  }
  if (command == "maximize") {
    widget->Maximize();
    return true;
  }
  if (command == "restore" || command == "unmaximize") {
    if (entry.sync_bounds_with_parent) {
      if (views::Widget* parent = FindWidget(entry.parent_id);
          parent && parent->IsMinimized()) {
        parent->Restore();
        return true;
      }
    }
    widget->Restore();
    return true;
  }
  if (command == "is-minimized") {
    if (entry.sync_bounds_with_parent) {
      if (views::Widget* parent = FindWidget(entry.parent_id)) {
        *result = base::Value(parent->IsMinimized());
        return true;
      }
    }
    *result = base::Value(widget->IsMinimized());
    return true;
  }
  if (command == "is-maximized") {
    *result = base::Value(widget->IsMaximized());
    return true;
  }
  if (command == "set-fullscreen") {
    const bool fullscreen =
        options && options->FindBool("value").value_or(false);
    widget->SetFullscreen(fullscreen);
    return true;
  }
  if (command == "is-fullscreen") {
    *result = base::Value(widget->IsFullscreen());
    return true;
  }
  if (command == "center") {
    widget->CenterWindow(widget->GetWindowBoundsInScreen().size());
    return true;
  }
  if (command == "set-title") {
    const std::string* title = options ? options->FindString("title") : nullptr;
    if (!title) {
      *error = "set-title expects title";
      return false;
    }
    widget->widget_delegate()->SetTitle(base::UTF8ToUTF16(*title));
    widget->UpdateWindowTitle();
    return true;
  }
  if (command == "set-opacity") {
    const base::Value* opacity = options ? options->Find("value") : nullptr;
    if (!opacity || (!opacity->is_double() && !opacity->is_int())) {
      *error = "set-opacity expects a number";
      return false;
    }
    const double value = opacity->is_double() ? opacity->GetDouble()
                                               : opacity->GetInt();
    widget->SetOpacity(static_cast<float>(std::clamp(value, 0.0, 1.0)));
    return true;
  }
#if BUILDFLAG(IS_WIN)
  HWND hwnd = views::HWNDForWidget(widget);
  if (command == "set-always-on-top") {
    const bool value = options && options->FindBool("value").value_or(false);
    if (hwnd) {
      ::SetWindowPos(hwnd, value ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return true;
  }
  if (command == "set-enabled") {
    const bool value = options && options->FindBool("value").value_or(false);
    if (hwnd) {
      ::EnableWindow(hwnd, value);
    }
    return true;
  }
  if (command == "is-enabled") {
    *result = base::Value(hwnd && ::IsWindowEnabled(hwnd));
    return true;
  }
  if (command == "set-shape") {
    entry.shape_rects.clear();
    const base::Value* rects_val = options ? options->Find("rects") : nullptr;
    if (!rects_val || !rects_val->is_list() || rects_val->GetList().empty()) {
      if (hwnd) {
        ::SetWindowRgn(hwnd, nullptr, TRUE);
      }
      return true;
    }
    const auto& rects = rects_val->GetList();
    HRGN total_rgn = nullptr;
    float scale = 1.0f;
    if (hwnd) {
      scale = display::win::GetScreenWin()->GetScaleFactorForHWND(hwnd);
      if (scale <= 0.0f) {
        scale = 1.0f;
      }
    }
    for (const auto& rect_val : rects) {
      if (!rect_val.is_dict()) {
        continue;
      }
      const auto& d = rect_val.GetDict();
      int dip_x = d.FindInt("x").value_or(0);
      int dip_y = d.FindInt("y").value_or(0);
      int dip_w = d.FindInt("width").value_or(0);
      int dip_h = d.FindInt("height").value_or(0);
      if (dip_w <= 0 || dip_h <= 0) {
        continue;
      }
      entry.shape_rects.emplace_back(dip_x, dip_y, dip_w, dip_h);

      int px_x = static_cast<int>(std::round(dip_x * scale));
      int px_y = static_cast<int>(std::round(dip_y * scale));
      int px_w = static_cast<int>(std::round(dip_w * scale));
      int px_h = static_cast<int>(std::round(dip_h * scale));

      HRGN rect_rgn =
          ::CreateRectRgn(px_x, px_y, px_x + px_w, px_y + px_h);
      if (!rect_rgn) {
        continue;
      }
      if (!total_rgn) {
        total_rgn = rect_rgn;
      } else {
        ::CombineRgn(total_rgn, total_rgn, rect_rgn, RGN_OR);
        ::DeleteObject(rect_rgn);
      }
    }
    if (!total_rgn) {
      if (hwnd) {
        ::SetWindowRgn(hwnd, nullptr, TRUE);
      }
      return true;
    }
    if (!hwnd || !::SetWindowRgn(hwnd, total_rgn, TRUE)) {
      ::DeleteObject(total_rgn);
    }
    return true;
  }
  if (command == "set-ignore-mouse-events") {
    const bool value = options && options->FindBool("value").value_or(false);
    entry.ignore_mouse_events = value;
    if (hwnd) {
      LONG_PTR style = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
      style = value ? style | WS_EX_TRANSPARENT : style & ~WS_EX_TRANSPARENT;
      ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, style);
    }
    return true;
  }
  if (command == "hook-window-message" ||
      command == "unhook-window-message") {
    const std::optional<int> message =
        options ? FindInteger(*options, "message") : std::nullopt;
    if (!message || *message < 0) {
      *error = command + " expects a message number";
      return false;
    }
    if (command == "hook-window-message") {
      entry.hooked_messages.insert(static_cast<uint32_t>(*message));
    } else {
      entry.hooked_messages.erase(static_cast<uint32_t>(*message));
    }
    return true;
  }
#endif
  if (command == "popup-menu") {
    if (!options) {
      *error = "popup-menu expects an object";
      return false;
    }
    const base::ListValue* items = options->FindList("items");
    if (!items) {
      *error = "popup-menu expects items";
      return false;
    }
    const std::optional<int> x = FindInteger(*options, "x");
    const std::optional<int> y = FindInteger(*options, "y");
    gfx::Point screen_anchor;
    if (x.has_value() && y.has_value()) {
      // Electron `Menu.popup({x, y})` is window-relative DIP.
      const gfx::Rect bounds = GetVisibleWindowBoundsInScreen(widget);
      screen_anchor = gfx::Point(bounds.x() + *x, bounds.y() + *y);
    } else if (display::Screen* screen = display::Screen::Get()) {
      // `views::MenuRunner` anchors in screen DIP, same as BrowserWidget.
      screen_anchor = screen->GetCursorScreenPoint();
    } else {
      screen_anchor = GetVisibleWindowBoundsInScreen(widget).origin();
    }
    // Do not run the menu inside this sync Mojo call. Utility is blocked on
    // the reply and cannot receive native-menu-command / native-menu-closed.
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE,
        base::BindOnce(&XenonElectronWindowHost::ShowPopupMenu,
                       base::Unretained(this), window_id, items->Clone(),
                       screen_anchor));
    return true;
  }
  *error = "Unsupported Electron BrowserWindow command: " + command;
  return false;
}

int32_t XenonElectronWindowHost::FindWindowIdForWebContents(
    content::WebContents* web_contents) const {
  if (!web_contents) {
    return 0;
  }
  // Inner contents must never inherit the owner's BrowserWindow identity
  // through the native-window fallback below.
  if (ipc::XenonElectronGuest::FromWebContents(web_contents)) {
    return 0;
  }
  const gfx::NativeWindow top_level = web_contents->GetTopLevelNativeWindow();
  for (const auto& [id, entry] : windows_) {
    if (!entry.widget) {
      continue;
    }
    auto* dialog_view =
        static_cast<views::WebDialogView*>(entry.widget->widget_delegate());
    if (dialog_view && dialog_view->web_contents() == web_contents) {
      return id;
    }
    // Fallback: WebDialogView::web_contents() can lag during first bind.
    if (top_level && entry.widget->GetNativeWindow() == top_level) {
      return id;
    }
  }
  return 0;
}

std::string XenonElectronWindowHost::GetContainerIdForWebContents(
    content::WebContents* web_contents) const {
  const int32_t window_id = FindWindowIdForWebContents(web_contents);
  const auto it = windows_.find(window_id);
  return it == windows_.end() ? std::string() : it->second.container_id;
}

void XenonElectronWindowHost::Close(int32_t window_id) {
  views::Widget* widget = FindWidget(window_id);
  LOG(INFO) << "Electron BrowserWindow Close id=" << window_id
            << (widget ? "" : " (unknown window)");
  if (!widget) {
    return;
  }
#if BUILDFLAG(IS_WIN)
  // Hide first so a slow Close() animation does not leave a black host up.
  if (HWND hwnd = views::HWNDForWidget(widget)) {
    ::ShowWindow(hwnd, SW_HIDE);
  }
#endif
  widget->Close();
}

void XenonElectronWindowHost::ShutdownForProcessExit() {
  if (shutting_down_) {
    return;
  }
  shutting_down_ = true;

  // Widget::Close() is asynchronous and can leave its WebContents alive until
  // after Profile teardown. CloseNow() synchronously invokes
  // OnWidgetDestroying(), which removes the corresponding map entry. Re-read
  // begin() after each close because destroying an owner can also destroy its
  // owned BrowserWindows.
  while (!windows_.empty()) {
    auto it = windows_.begin();
    views::Widget* widget = it->second.widget;
    if (!widget) {
      windows_.erase(it);
      continue;
    }
    widget->CloseNow();
  }
}

void XenonElectronWindowHost::NotifyEvent(int32_t window_id,
                                          const std::string& event_name,
                                          base::Value arguments) {
  if (shutting_down_) {
    return;
  }
  XenonManager::GetInstance()->DispatchElectronWindowEvent(
      window_id, event_name, std::move(arguments));
}

void XenonElectronWindowHost::SynchronizeOverlayBounds(
    int32_t source_id,
    const gfx::Rect& bounds) {
  if (synchronizing_overlay_bounds_) {
    return;
  }
  auto source = windows_.find(source_id);
  if (source == windows_.end() || !source->second.widget) {
    return;
  }
  views::Widget* source_widget = source->second.widget;
  if (source_widget->IsMinimized() || source_widget->IsMaximized() ||
      source_widget->IsFullscreen()) {
    // Show-state transitions publish intermediate/off-screen bounds (for
    // example Windows' -32000 minimized placement). Let each Widget preserve
    // its native normal placement; show-state synchronization below handles
    // maximize, restore, minimize, and fullscreen as discrete operations.
    return;
  }
  const gfx::Rect visible_bounds =
      GetVisibleWindowBoundsInScreen(source_widget);
  const gfx::Rect& synchronized_bounds =
      visible_bounds.IsEmpty() ? bounds : visible_bounds;

  base::AutoReset<bool> synchronizing(&synchronizing_overlay_bounds_, true);
  if (source->second.sync_bounds_with_parent) {
    views::Widget* parent = FindWidget(source->second.parent_id);
    if (parent && !parent->IsMinimized() && !parent->IsMaximized() &&
        !parent->IsFullscreen() &&
        GetVisibleWindowBoundsInScreen(parent) != synchronized_bounds) {
      SetVisibleWindowBounds(parent, synchronized_bounds);
    }
    return;
  }

  for (auto& [id, entry] : windows_) {
    if (!entry.sync_bounds_with_parent || entry.parent_id != source_id ||
        !entry.widget || entry.widget->IsMinimized() ||
        entry.widget->IsMaximized() || entry.widget->IsFullscreen() ||
        GetVisibleWindowBoundsInScreen(entry.widget) == synchronized_bounds) {
      continue;
    }
    SetVisibleWindowBounds(entry.widget, synchronized_bounds);
  }
}

void XenonElectronWindowHost::ShowPopupMenu(int32_t window_id,
                                            base::ListValue items,
                                            gfx::Point screen_anchor) {
  auto it = windows_.find(window_id);
  if (it == windows_.end() || !it->second.widget || items.empty()) {
    return;
  }
  popup_menu_.reset();
  popup_menu_ = std::make_unique<PopupMenuSession>(this, window_id);
  popup_menu_->BuildAndRun(it->second.widget, items, screen_anchor);
}

void XenonElectronWindowHost::OnPopupMenuClosed(PopupMenuSession* session) {
  if (!session) {
    return;
  }
  NotifyEvent(session->window_id(), "native-menu-closed", base::Value());
  if (popup_menu_.get() == session) {
    content::GetUIThreadTaskRunner({})->DeleteSoon(FROM_HERE,
                                                   popup_menu_.release());
  }
}

void XenonElectronWindowHost::SynchronizeOverlayShowState(int32_t source_id) {
  if (synchronizing_overlay_show_state_) {
    return;
  }
  auto source = windows_.find(source_id);
  if (source == windows_.end() || !source->second.widget) {
    return;
  }

  std::vector<views::Widget*> targets;
  if (source->second.sync_bounds_with_parent) {
    if (views::Widget* parent = FindWidget(source->second.parent_id)) {
      targets.push_back(parent);
    }
  } else {
    for (auto& [id, entry] : windows_) {
      if (entry.sync_bounds_with_parent && entry.parent_id == source_id &&
          entry.widget) {
        targets.push_back(entry.widget);
      }
    }
  }
  if (targets.empty()) {
    return;
  }

  views::Widget* source_widget = source->second.widget;
  // Windows automatically hides owned overlays with a minimized owner. Do
  // not independently minimize the layered child: it must retain its HWND
  // and become visible again when the owner restores.
  if (source_widget->IsMinimized() &&
      !source->second.sync_bounds_with_parent) {
    return;
  }
  base::AutoReset<bool> synchronizing(&synchronizing_overlay_show_state_,
                                      true);
  for (views::Widget* target : targets) {
    if (source_widget->IsFullscreen()) {
      if (!target->IsFullscreen()) {
        target->SetFullscreen(true);
      }
    } else if (target->IsFullscreen()) {
      target->SetFullscreen(false);
    }

    if (source_widget->IsMinimized()) {
      if (!target->IsMinimized()) {
        target->Minimize();
      }
    } else if (source_widget->IsMaximized()) {
      if (!target->IsMaximized()) {
        target->Maximize();
      }
    } else if (target->IsMinimized() || target->IsMaximized()) {
      target->Restore();
    }
  }
}

void XenonElectronWindowHost::OnWidgetActivationChanged(views::Widget* widget,
                                                         bool active) {
  auto it = FindEntry(widget);
  if (it != windows_.end()) {
    NotifyEvent(it->first, active ? "focus" : "blur", base::Value());
  }
}

void XenonElectronWindowHost::OnWidgetBoundsChanged(
    views::Widget* widget,
    const gfx::Rect&) {
  auto it = FindEntry(widget);
  if (it == windows_.end()) {
    return;
  }
#if BUILDFLAG(IS_WIN)
  if (!it->second.has_loaded_url) {
    ResizeNativeHostChildren(widget);
  }
#endif
  const gfx::Rect visible_bounds = GetVisibleWindowBoundsInScreen(widget);
  if (it->second.bounds == visible_bounds) {
    return;
  }
  it->second.bounds = visible_bounds;
  if (!widget->IsMinimized() && !widget->IsMaximized() &&
      !widget->IsFullscreen()) {
    it->second.normal_bounds = visible_bounds;
  }
  NotifyEvent(it->first, "bounds-changed", BoundsToValue(visible_bounds));
  SynchronizeOverlayBounds(it->first, visible_bounds);
}

void XenonElectronWindowHost::OnWidgetShowStateChanged(views::Widget* widget) {
  auto it = FindEntry(widget);
  if (it == windows_.end()) {
    return;
  }
  base::DictValue state;
  it->second.minimized = widget->IsMinimized();
  it->second.maximized = widget->IsMaximized();
  it->second.fullscreen = widget->IsFullscreen();
  state.Set("minimized", it->second.minimized);
  state.Set("maximized", it->second.maximized);
  state.Set("fullscreen", it->second.fullscreen);
  NotifyEvent(it->first, "state-changed", base::Value(std::move(state)));
  SynchronizeOverlayShowState(it->first);
}

void XenonElectronWindowHost::OnWidgetVisibilityChanged(views::Widget* widget,
                                                         bool visible) {
  auto it = FindEntry(widget);
  if (it != windows_.end()) {
    NotifyEvent(it->first, visible ? "show" : "hide", base::Value());
  }
}

void XenonElectronWindowHost::OnNativeWindowMessage(uint64_t hwnd,
                                                     uint32_t message,
                                                     uint64_t w_param,
                                                     int64_t l_param) {
  for (const auto& [id, entry] : windows_) {
    if (entry.hwnd != hwnd) {
      continue;
    }
    if (entry.frameless && CanResetDwmAppearance(message)) {
      ConfigureFramelessDwmWindow(
          reinterpret_cast<HWND>(static_cast<uintptr_t>(hwnd)),
          entry.sync_bounds_with_parent);
    }
    if (!entry.hooked_messages.contains(message)) {
      return;
    }
    base::DictValue args;
    args.Set("message", static_cast<int>(message));
    args.Set("wParam", base::NumberToString(w_param));
    args.Set("lParam", base::NumberToString(l_param));
    NotifyEvent(id, "window-message", base::Value(std::move(args)));
    return;
  }
}

bool XenonElectronWindowHost::HandleNativeNonClientMessage(
    uint64_t hwnd,
    uint32_t message,
    uint64_t,
    int64_t,
    int64_t* result) {
#if BUILDFLAG(IS_WIN)
  if (!result) {
    return false;
  }
  if (message == WM_NCHITTEST) {
    for (const auto& [id, entry] : windows_) {
      if (entry.hwnd == hwnd && entry.ignore_mouse_events) {
        *result = HTTRANSPARENT;
        return true;
      }
    }
  }
  if (message == WM_NCCALCSIZE) {
    for (const auto& [id, entry] : windows_) {
      if (entry.hwnd == hwnd && entry.frameless && !entry.has_loaded_url) {
        // Leave the proposed RECT unchanged: the complete HWND becomes client
        // area. DWM still clips the invisible resize frame and rounded corners.
        *result = 0;
        return true;
      }
    }
  }
#endif
  return false;
}

bool XenonElectronWindowHost::ActivateAll() {
  if (windows_.empty()) {
    pending_activate_ = true;
    LOG(WARNING) << "Electron BrowserWindows not created yet; will show when "
                    "main.js constructs them";
    return false;
  }
  pending_activate_ = false;
  bool activated_host = false;
  for (auto& [id, entry] : windows_) {
    if (entry.widget) {
      entry.widget->Show();
      // Only the first top-level host should take foreground activation.
      if (!activated_host && !entry.sync_bounds_with_parent) {
        entry.widget->Activate();
        activated_host = true;
      }
    }
  }
  return true;
}

bool XenonElectronWindowHost::HasWindowsForContainer(
    const std::string& container_id) const {
  for (const auto& [id, entry] : windows_) {
    if (entry.container_id == container_id && entry.widget) {
      return true;
    }
  }
  return false;
}

bool XenonElectronWindowHost::ActivateForContainer(
    const std::string& container_id) {
  if (container_id.empty()) {
    return ActivateAll();
  }
  const int32_t entry_window_id =
      FindEntryWindowForContainer(container_id);
  if (entry_window_id <= 0) {
    pending_activate_containers_.insert(container_id);
    LOG(WARNING) << "Electron entry window for container '" << container_id
                 << "' not loaded yet; will show on its first loadURL/loadFile";
    return false;
  }
  pending_activate_containers_.erase(container_id);
  return ActivateEntryWindow(entry_window_id);
}

std::vector<std::string> XenonElectronWindowHost::ShowOpenDialog(
    const std::string& title,
    bool directory,
    bool allow_multi,
    const std::vector<std::string>& extensions) {
  std::vector<std::string> results;
#if BUILDFLAG(IS_WIN)
  Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
  HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
  if (FAILED(hr) || !dialog) {
    LOG(ERROR) << "IFileOpenDialog CoCreate failed hr=" << hr;
    return results;
  }

  FILEOPENDIALOGOPTIONS opts = FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
  if (directory) {
    opts |= FOS_PICKFOLDERS;
  } else {
    opts |= FOS_FILEMUSTEXIST;
  }
  if (allow_multi && !directory) {
    opts |= FOS_ALLOWMULTISELECT;
  }
  dialog->SetOptions(opts);

  const std::wstring title_w = base::UTF8ToWide(title);
  if (!title_w.empty()) {
    dialog->SetTitle(title_w.c_str());
  }

  std::wstring filter_name = L"媒体文件";
  std::wstring filter_spec;
  std::vector<COMDLG_FILTERSPEC> specs;
  if (!directory && !extensions.empty()) {
    for (size_t i = 0; i < extensions.size(); ++i) {
      if (i) {
        filter_spec += L";";
      }
      filter_spec += L"*.";
      filter_spec += base::UTF8ToWide(extensions[i]);
    }
    specs.push_back({filter_name.c_str(), filter_spec.c_str()});
    specs.push_back({L"所有文件", L"*.*"});
    dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
  }

  HWND owner = nullptr;
  uint64_t overlay_hwnd = 0;
  uint64_t any_hwnd = 0;
  for (const auto& [id, entry] : windows_) {
    if (!entry.hwnd) {
      continue;
    }
    any_hwnd = entry.hwnd;
    if (entry.has_loaded_url) {
      overlay_hwnd = entry.hwnd;
    }
  }
  const uint64_t owner_id = overlay_hwnd ? overlay_hwnd : any_hwnd;
  if (owner_id) {
    owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(owner_id));
  }

  LOG(INFO) << "IFileOpenDialog Show owner=" << owner_id
            << " directory=" << directory;
  hr = dialog->Show(owner);
  if (FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_CANCELLED) && owner) {
    LOG(WARNING) << "IFileOpenDialog Show owner failed hr=" << hr
                 << "; retry without owner";
    hr = dialog->Show(nullptr);
  }
  if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    LOG(INFO) << "IFileOpenDialog canceled";
    return results;
  }
  if (FAILED(hr)) {
    LOG(ERROR) << "IFileOpenDialog Show failed hr=" << hr;
    return results;
  }

  Microsoft::WRL::ComPtr<IShellItemArray> items;
  if (SUCCEEDED(dialog->GetResults(&items)) && items) {
    DWORD count = 0;
    items->GetCount(&count);
    for (DWORD i = 0; i < count; ++i) {
      Microsoft::WRL::ComPtr<IShellItem> item;
      if (SUCCEEDED(items->GetItemAt(i, &item)) && item) {
        AppendShellItemPath(item.Get(), &results);
      }
    }
  } else {
    Microsoft::WRL::ComPtr<IShellItem> item;
    if (SUCCEEDED(dialog->GetResult(&item)) && item) {
      AppendShellItemPath(item.Get(), &results);
    }
  }
#endif
  return results;
}

void XenonElectronWindowHost::OnWidgetDestroying(views::Widget* widget) {
  for (auto it = windows_.begin(); it != windows_.end(); ++it) {
    if (it->second.widget == widget) {
#if BUILDFLAG(IS_WIN)
      if (HWND hwnd = views::HWNDForWidget(widget)) {
        ::RemoveWindowSubclass(hwnd, ElectronWindowSubclassProc,
                               kElectronWindowSubclassId);
      }
#endif
      if (popup_menu_ && popup_menu_->window_id() == it->first) {
        popup_menu_.reset();
        NotifyEvent(it->first, "native-menu-closed", base::Value());
      }
      NotifyEvent(it->first, "closed", base::Value());
      widget->RemoveObserver(this);
      windows_.erase(it);
      return;
    }
  }
}

}  // namespace xenon
