#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/chrome_web_contents_handler.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/context_menu_params.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/page/draggable_region.mojom.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/hit_test.h"
#include "ui/compositor/layer.h"
#include "ui/compositor_extra/shadow.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/gfx/shadow_value.h"
#include "ui/views/animation/animation_builder.h"
#include "ui/views/border.h"
#include "ui/views/controls/webview/web_dialog_view.h"
#include "ui/views/view_targeter.h"
#include "ui/views/view_targeter_delegate.h"
#include "ui/views/view_utils.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/frame_view.h"
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"
#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "components/constrained_window/constrained_window_views.h"
#include "chrome/common/chrome_render_frame.mojom.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "third_party/blink/public/common/associated_interfaces/associated_interface_provider.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <dwmapi.h>

#include "base/win/windows_version.h"
#include "ui/display/win/screen_win.h"
#include "ui/views/widget/widget_hwnd_utils.h"
#include "ui/views/win/hwnd_util.h"
#include "ui/aura/window.h"
#include "ui/events/event.h"
#include "ui/events/event_handler.h"
#endif

namespace xenon {
namespace {

constexpr int kDialogCornerRadius = 16;
#if BUILDFLAG(IS_WIN)
// On Win11 this only enables DWMWCP_ROUND; the OS chooses the actual radius.
constexpr int kDwmRoundedCornerHintRadius = 1;
#endif
constexpr int kFramelessCompositorShadowElevation = 8;

#if BUILDFLAG(IS_WIN)
class ModalEventBlocker : public ui::EventHandler {
 public:
  ModalEventBlocker(aura::Window* parent_window, views::Widget* dialog_widget)
      : parent_window_(parent_window), dialog_widget_(dialog_widget) {
    if (parent_window_) {
      parent_window_->AddPreTargetHandler(this);
    }
  }

  ~ModalEventBlocker() override {
    if (parent_window_) {
      parent_window_->RemovePreTargetHandler(this);
    }
  }

  // Prevent copying
  ModalEventBlocker(const ModalEventBlocker&) = delete;
  ModalEventBlocker& operator=(const ModalEventBlocker&) = delete;

  // ui::EventHandler:
  void OnMouseEvent(ui::MouseEvent* event) override {
    if (IsEventForDialog(event)) {
      return;
    }
    if (IsClientAreaEvent(event)) {
      event->SetHandled();
      if (event->type() == ui::EventType::kMousePressed && dialog_widget_) {
        dialog_widget_->Activate();
      }
    }
  }

  void OnKeyEvent(ui::KeyEvent* event) override {
    if (dialog_widget_ && dialog_widget_->IsActive()) {
      return;
    }
    event->SetHandled();
    if (dialog_widget_) {
      dialog_widget_->Activate();
    }
  }

  void OnTouchEvent(ui::TouchEvent* event) override {
    if (IsEventForDialog(event)) {
      return;
    }
    if (IsClientAreaEvent(event)) {
      event->SetHandled();
      if (event->type() == ui::EventType::kTouchPressed && dialog_widget_) {
        dialog_widget_->Activate();
      }
    }
  }

 private:
  bool IsEventForDialog(ui::LocatedEvent* event) {
    if (!dialog_widget_ || !dialog_widget_->GetNativeWindow()) {
      return false;
    }
    aura::Window* target = static_cast<aura::Window*>(event->target());
    aura::Window* dialog_window = dialog_widget_->GetNativeWindow();
    if (!target || !dialog_window) {
      return false;
    }
    gfx::Point point = event->location();
    aura::Window::ConvertPointToTarget(target, dialog_window, &point);
    return gfx::Rect(dialog_window->bounds().size()).Contains(point);
  }

  bool IsClientAreaEvent(ui::LocatedEvent* event) {
    if (!parent_window_) {
      return false;
    }
    views::Widget* parent_widget = views::Widget::GetWidgetForNativeView(parent_window_);
    if (parent_widget && parent_widget->non_client_view()) {
      gfx::Point point = event->location();
      aura::Window* target = static_cast<aura::Window*>(event->target());
      if (target && target != parent_window_) {
        aura::Window::ConvertPointToTarget(target, parent_window_, &point);
      }
      int hit_test = parent_widget->non_client_view()->NonClientHitTest(point);
      if (hit_test != HTCLIENT) {
        return false;
      }
    }
    return true;
  }

  raw_ptr<aura::Window> parent_window_;
  raw_ptr<views::Widget> dialog_widget_;
};

bool IsWin11OrLater() {
  return base::win::GetVersion() >= base::win::Version::WIN11;
}

bool UseDwmRoundedCorners(bool use_dwm) {
  return use_dwm && IsWin11OrLater();
}

bool UseFramelessCompositorShadow(bool use_dwm, bool show_shadow) {
  return show_shadow && !UseDwmRoundedCorners(use_dwm);
}

int FramelessCompositorShadowMargin() {
  static const int margin = [] {
    const gfx::ShadowValues values = gfx::ShadowValue::MakeMdShadowValues(
        kFramelessCompositorShadowElevation);
    const gfx::Insets insets = gfx::ShadowValue::GetMargin(values);
    return std::max(
        {-insets.left(), -insets.top(), -insets.right(), -insets.bottom()});
  }();
  return margin;
}
#else
bool UseDwmRoundedCorners(bool) {
  return false;
}

bool UseFramelessCompositorShadow(bool, bool show_shadow) {
  return show_shadow;
}

int FramelessCompositorShadowMargin() {
  return 0;
}
#endif

void EnlargeForFramelessCompositorShadow(gfx::Size* size,
                                          bool use_native_frame,
                                          bool use_dwm,
                                          bool show_shadow) {
  if (!use_native_frame &&
      UseFramelessCompositorShadow(use_dwm, show_shadow)) {
    const int margin = FramelessCompositorShadowMargin();
    size->Enlarge(2 * margin, 2 * margin);
  }
}

int ResizeBorderThickness() {
#if BUILDFLAG(IS_WIN)
  return display::win::GetScreenWin()->GetSystemMetricsInDIP(SM_CXSIZEFRAME);
#else
  return 8;
#endif
}

bool CanResizeFrame(const views::Widget* widget) {
  return widget && widget->widget_delegate() &&
         widget->widget_delegate()->CanResize() && !widget->IsMaximized() &&
         !widget->IsFullscreen();
}

gfx::Rect GetResizeHitTestBounds(const gfx::Size& size,
                                 bool use_native_frame,
                                 bool use_dwm,
                                 bool show_shadow) {
  gfx::Rect bounds(size);
  if (!use_native_frame &&
      UseFramelessCompositorShadow(use_dwm, show_shadow)) {
    const int margin = FramelessCompositorShadowMargin();
    bounds.Inset(gfx::Insets(margin));
  }
  return bounds;
}

int GetResizeHitTest(const gfx::Point& point, const gfx::Rect& bounds) {
  const int border = ResizeBorderThickness();
  if (point.x() < bounds.x() - border ||
      point.x() >= bounds.right() + border ||
      point.y() < bounds.y() - border ||
      point.y() >= bounds.bottom() + border) {
    return HTNOWHERE;
  }

  // Check if point is outside the rounded corner active area
  constexpr int R = kDialogCornerRadius;
  const int max_dist = R + border + 2;
  const int max_dist_sq = max_dist * max_dist;
  // Top-Left corner
  if (point.x() < bounds.x() + R && point.y() < bounds.y() + R) {
    int dx = point.x() - (bounds.x() + R);
    int dy = point.y() - (bounds.y() + R);
    if (dx * dx + dy * dy > max_dist_sq) {
      return HTNOWHERE;
    }
  }
  // Top-Right corner
  else if (point.x() > bounds.right() - R && point.y() < bounds.y() + R) {
    int dx = point.x() - (bounds.right() - R);
    int dy = point.y() - (bounds.y() + R);
    if (dx * dx + dy * dy > max_dist_sq) {
      return HTNOWHERE;
    }
  }
  // Bottom-Left corner
  else if (point.x() < bounds.x() + R && point.y() > bounds.bottom() - R) {
    int dx = point.x() - (bounds.x() + R);
    int dy = point.y() - (bounds.bottom() - R);
    if (dx * dx + dy * dy > max_dist_sq) {
      return HTNOWHERE;
    }
  }
  // Bottom-Right corner
  else if (point.x() > bounds.right() - R && point.y() > bounds.bottom() - R) {
    int dx = point.x() - (bounds.right() - R);
    int dy = point.y() - (bounds.bottom() - R);
    if (dx * dx + dy * dy > max_dist_sq) {
      return HTNOWHERE;
    }
  }

  const bool left = point.x() < bounds.x() + border;
  const bool right = point.x() >= bounds.right() - border;
  const bool top = point.y() < bounds.y() + border;
  const bool bottom = point.y() >= bounds.bottom() - border;

  if (top && left) {
    return HTTOPLEFT;
  }
  if (top && right) {
    return HTTOPRIGHT;
  }
  if (bottom && left) {
    return HTBOTTOMLEFT;
  }
  if (bottom && right) {
    return HTBOTTOMRIGHT;
  }
  if (left) {
    return HTLEFT;
  }
  if (right) {
    return HTRIGHT;
  }
  if (top) {
    return HTTOP;
  }
  if (bottom) {
    return HTBOTTOM;
  }
  return HTNOWHERE;
}

bool IsInResizeBorder(const gfx::Point& point, const gfx::Rect& bounds) {
  return GetResizeHitTest(point, bounds) != HTNOWHERE;
}

ui::mojom::CursorType CursorTypeForResizeHitTest(int hit_test) {
  switch (hit_test) {
    case HTLEFT:
    case HTRIGHT:
      return ui::mojom::CursorType::kEastWestResize;
    case HTTOP:
    case HTBOTTOM:
      return ui::mojom::CursorType::kNorthSouthResize;
    case HTTOPLEFT:
    case HTBOTTOMRIGHT:
      return ui::mojom::CursorType::kNorthWestSouthEastResize;
    case HTTOPRIGHT:
    case HTBOTTOMLEFT:
      return ui::mojom::CursorType::kNorthEastSouthWestResize;
    default:
      return ui::mojom::CursorType::kNull;
  }
}

bool ResizeHitTestAffectsLeft(int hit_test) {
  return hit_test == HTLEFT || hit_test == HTTOPLEFT ||
         hit_test == HTBOTTOMLEFT;
}

bool ResizeHitTestAffectsRight(int hit_test) {
  return hit_test == HTRIGHT || hit_test == HTTOPRIGHT ||
         hit_test == HTBOTTOMRIGHT;
}

bool ResizeHitTestAffectsTop(int hit_test) {
  return hit_test == HTTOP || hit_test == HTTOPLEFT ||
         hit_test == HTTOPRIGHT;
}

bool ResizeHitTestAffectsBottom(int hit_test) {
  return hit_test == HTBOTTOM || hit_test == HTBOTTOMLEFT ||
         hit_test == HTBOTTOMRIGHT;
}

void EnableDraggableRegionsForFrame(content::RenderFrameHost* rfh) {
  if (!rfh || !rfh->IsRenderFrameLive()) {
    return;
  }
  content::WebContents* web_contents = content::WebContents::FromRenderFrameHost(rfh);
  if (web_contents) {
    web_contents->SetSupportsDraggableRegions(true);
  }
}

class XenonDraggableRegionsEnabler : public content::WebContentsObserver {
 public:
  XenonDraggableRegionsEnabler(content::WebContents* web_contents,
                               bool use_transparent_background)
      : content::WebContentsObserver(web_contents),
        background_color_(use_transparent_background
                              ? std::make_optional(SK_ColorTRANSPARENT)
                              : std::nullopt) {
    ApplyBackgroundColor();
    if (web_contents && web_contents->GetPrimaryMainFrame()) {
      EnableDraggableRegionsForFrame(web_contents->GetPrimaryMainFrame());
    }
  }

  void SetBackgroundColor(SkColor color) {
    background_color_ = color;
    ApplyBackgroundColor();
  }

  void RenderFrameCreated(content::RenderFrameHost* render_frame_host) override {
    if (render_frame_host->IsInPrimaryMainFrame()) {
      if (background_color_ && render_frame_host->GetView()) {
        render_frame_host->GetView()->SetBackgroundColor(
            GetViewBackgroundColor());
      }
      EnableDraggableRegionsForFrame(render_frame_host);
    }
  }

  void DOMContentLoaded(content::RenderFrameHost* render_frame_host) override {
    if (render_frame_host->IsInPrimaryMainFrame()) {
      ApplyBackgroundColor();
      EnableDraggableRegionsForFrame(render_frame_host);
    }
  }

 private:
  SkColor GetViewBackgroundColor() const {
    // RenderWidgetHostView only accepts fully opaque or transparent colors.
    // The page base color retains intermediate alpha for renderer compositing.
    return SkColorGetA(*background_color_) == SK_AlphaOPAQUE
               ? *background_color_
               : SK_ColorTRANSPARENT;
  }

  void ApplyBackgroundColor() {
    if (!background_color_ || !web_contents()) {
      return;
    }
    web_contents()->SetPageBaseBackgroundColor(*background_color_);
    if (content::RenderWidgetHostView* view =
            web_contents()->GetRenderWidgetHostView()) {
      view->SetBackgroundColor(GetViewBackgroundColor());
    }
  }

  std::optional<SkColor> background_color_;
};

// Frameless WebDialogView: -webkit-app-region drag + edge resize.
class XenonWebDialogView : public views::WebDialogView,
                           public views::ViewTargeterDelegate {
 public:
  XenonWebDialogView(content::BrowserContext* context,
                     ui::WebDialogDelegate* delegate,
                     std::unique_ptr<WebContentsHandler> handler)
      : views::WebDialogView(context, delegate, std::move(handler)),
        xenon_delegate_(static_cast<XenonWebDialog*>(delegate)) {
    SetEventTargeter(std::make_unique<views::ViewTargeter>(this));
  }
  ~XenonWebDialogView() override = default;

  void SetContentURL(const GURL& url) {
    if (xenon_delegate_) {
      xenon_delegate_->SetContentURL(url);
    }
  }

  void SetCloseRequestHandler(base::RepeatingCallback<bool()> handler) {
    xenon_delegate_->SetCloseRequestHandler(std::move(handler));
  }

  void CloseContents(content::WebContents* source) override {
    // WebDialogView sets close_contents_called_ before consulting its delegate.
    // A veto at that point would make the next native close ignore the veto.
    // Ask the same delegate before entering the base close state machine.
    if (!xenon_delegate_->RequestClose()) {
      return;
    }
    views::WebDialogView::CloseContents(source);
  }

  bool SetHostedContentTitle(const std::u16string& title) {
    if (!xenon_delegate_ || !GetWidget()) {
      return false;
    }
    // WebDialogView::GetWindowTitle reads the WebDialogDelegate, rather than
    // WidgetDelegate::params_.title. Update that source before the HWND title.
    xenon_delegate_->SetContentTitle(title);
    GetWidget()->UpdateWindowTitle();
    return true;
  }

  void SetHostedContentVisible(bool visible) {
    for (views::View* child : children()) {
      if (auto* web_view = views::AsViewClass<views::WebView>(child)) {
        web_view->SetVisible(visible);
        return;
      }
    }
  }

  void SetHostedMinimumSize(const gfx::Size& minimum_size) {
    xenon_delegate_->set_minimum_dialog_size(minimum_size);
    GetWidget()->OnSizeConstraintsChanged();
  }

  bool SetHostedContentBackgroundColor(SkColor color) {
    if (!web_contents()) {
      return false;
    }
    xenon_delegate_->SetContentBackgroundColor(color);
    if (!draggable_regions_enabler_) {
      draggable_regions_enabler_ =
          std::make_unique<XenonDraggableRegionsEnabler>(
              web_contents(),
              xenon_delegate_->UseTransparentWebContentsBackground());
    }
    draggable_regions_enabler_->SetBackgroundColor(color);
    // An acrylic HWND can keep an opaque native window style while its web
    // surface has alpha. Do not turn it into a parent-sized overlay window.
    if (SkColorGetA(color) != SK_AlphaOPAQUE && GetWidget()) {
      if (ui::Layer* widget_layer = GetWidget()->GetLayer()) {
        widget_layer->SetFillsBoundsOpaquely(false);
      }
#if BUILDFLAG(IS_WIN)
      gfx::NativeWindow native_window = GetWidget()->GetNativeWindow();
      if (native_window && native_window->layer()) {
        native_window->layer()->SetFillsBoundsOpaquely(false);
      }
#endif
    }
    return true;
  }

  void ViewHierarchyChanged(
      const views::ViewHierarchyChangedDetails& details) override {
    views::WebDialogView::ViewHierarchyChanged(details);
    if (!details.is_add || !GetWidget()) {
      return;
    }
    content::WebContents* wc = web_contents();
    if (!wc || draggable_regions_enabler_) {
      return;
    }
    draggable_regions_enabler_ =
        std::make_unique<XenonDraggableRegionsEnabler>(
            wc, xenon_delegate_->UseTransparentWebContentsBackground());
  }

  void DraggableRegionsChanged(
      const std::vector<blink::mojom::DraggableRegionPtr>& regions,
      content::WebContents* contents) override {
    if (contents != web_contents()) {
      return;
    }
    draggable_region_ = std::make_unique<SkRegion>();
    // Pass 1: Union all draggable areas.
    for (const auto& region : regions) {
      if (region && region->draggable) {
        draggable_region_->op(gfx::RectToSkIRect(region->bounds),
                              SkRegion::kUnion_Op);
      }
    }
    // Pass 2: Subtract all non-draggable areas (e.g. close buttons, inputs, controls).
    // This ensures child/overlapping buttons with -webkit-app-region: no-drag
    // are never overwritten by a parent/sibling drag container or header title.
    for (const auto& region : regions) {
      if (region && !region->draggable) {
        draggable_region_->op(gfx::RectToSkIRect(region->bounds),
                              SkRegion::kDifference_Op);
      }
    }
  }

  bool HandleContextMenu(content::RenderFrameHost& render_frame_host,
                         const content::ContextMenuParams& params) override {
    // Electron BrowserWindow never shows Chromium's native context menu.
    // Page-authored menus (DOM `contextmenu`) already ran in the renderer.
    if (XenonElectronWindowHost::GetInstance()->FindWindowIdForWebContents(
            web_contents()) > 0) {
      return true;
    }
    return views::WebDialogView::HandleContextMenu(render_frame_host, params);
  }

  void AddedToWidget() override {
    views::WebDialogView::AddedToWidget();
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&XenonWebDialogView::FinishAddedToWidget,
                                  weak_ptr_factory_.GetWeakPtr()));
  }

  void OnThemeChanged() override {
    views::WebDialogView::OnThemeChanged();
    if (!xenon_delegate_->UseNativeFrame()) {
      SetBackground(nullptr);
    }
  }

  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    views::WebDialogView::OnBoundsChanged(previous_bounds);
    UpdateFramelessCompositorShadowBounds();
  }

  bool CanResize() const override { return xenon_delegate_->can_resize(); }

  int NonClientHitTest(const gfx::Point& point) override {
    if (!xenon_delegate_->UseNativeFrame() && CanResizeFrame(GetWidget())) {
      const int hit = GetCurrentResizeHitTest(point);
      if (hit != HTNOWHERE) {
        if (UseManualResize()) {
          return HTCLIENT;
        }
        return hit;
      }
    }
    gfx::Point web_point = point;
    web_point.Offset(-GetInsets().left(), -GetInsets().top());
    if (draggable_region_ &&
        draggable_region_->contains(web_point.x(), web_point.y())) {
      return HTCAPTION;
    }
    return views::WebDialogView::NonClientHitTest(point);
  }

  ui::Cursor GetCursor(const ui::MouseEvent& event) override {
    if (UseManualResize() && CanResizeFrame(GetWidget())) {
      const int hit = manual_resize_hit_test_ != HTNOWHERE
                          ? manual_resize_hit_test_
                          : GetCurrentResizeHitTest(event.location());
      const ui::mojom::CursorType cursor_type =
          CursorTypeForResizeHitTest(hit);
      if (cursor_type != ui::mojom::CursorType::kNull) {
        return ui::Cursor(cursor_type);
      }
    }
    return views::WebDialogView::GetCursor(event);
  }

  views::View* TargetForRect(views::View* root,
                             const gfx::Rect& rect) override {
    DCHECK_EQ(root, this);
    if (UseManualResize() && CanResizeFrame(GetWidget()) &&
        GetCurrentResizeHitTest(rect.CenterPoint()) != HTNOWHERE) {
      return this;
    }
    return views::ViewTargeterDelegate::TargetForRect(root, rect);
  }

  views::ClientView* CreateClientView(views::Widget* widget) override {
    return this;
  }

  std::unique_ptr<views::FrameView> CreateFrameView(
      views::Widget* widget) override {
    if (xenon_delegate_->UseNativeFrame()) {
      return views::WebDialogView::CreateFrameView(widget);
    }
    return std::make_unique<FrameView>();
  }

  bool ShouldDescendIntoChildForEventHandling(
      gfx::NativeView child,
      const gfx::Point& location) override {
    if (!xenon_delegate_->UseNativeFrame() && CanResizeFrame(GetWidget()) &&
        IsInResizeBorder(location, GetCurrentResizeHitTestBounds())) {
      return false;
    }
    gfx::Point web_location = location;
    web_location.Offset(-GetInsets().left(), -GetInsets().top());
    if (draggable_region_ &&
        draggable_region_->contains(web_location.x(), web_location.y())) {
      return false;
    }
    return views::WebDialogView::ShouldDescendIntoChildForEventHandling(
        child, location);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    if (UseManualResize() && CanResizeFrame(GetWidget()) &&
        event.IsOnlyLeftMouseButton()) {
      const int hit = GetCurrentResizeHitTest(event.location());
      if (hit != HTNOWHERE) {
        manual_resize_hit_test_ = hit;
        manual_resize_start_screen_location_ =
            views::View::ConvertPointToScreen(this, event.location());
        manual_resize_start_bounds_ = GetWidget()->GetWindowBoundsInScreen();
        GetWidget()->SetCapture(this);
        return true;
      }
    }
    return views::WebDialogView::OnMousePressed(event);
  }

  bool OnMouseDragged(const ui::MouseEvent& event) override {
    if (manual_resize_hit_test_ != HTNOWHERE) {
      UpdateManualResize(
          views::View::ConvertPointToScreen(this, event.location()));
      return true;
    }
    return views::WebDialogView::OnMouseDragged(event);
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    if (manual_resize_hit_test_ != HTNOWHERE) {
      EndManualResize();
      return;
    }
    views::WebDialogView::OnMouseReleased(event);
  }

  void OnMouseCaptureLost() override {
    EndManualResize();
    views::WebDialogView::OnMouseCaptureLost();
  }

 private:
  // Chromium removes WS_THICKFRAME from translucent windows, so native resize
  // cannot work there even if hit-test returns HT*. Handle resize in Views;
  // this is independent of whether the window paints a shadow.
  bool UseManualResize() const {
    return !xenon_delegate_->UseNativeFrame() &&
           !UseDwmRoundedCorners(xenon_delegate_->UseDwm());
  }

  gfx::Rect GetCurrentResizeHitTestBounds() const {
    return GetResizeHitTestBounds(size(), xenon_delegate_->UseNativeFrame(),
                                  xenon_delegate_->UseDwm(),
                                  xenon_delegate_->ShouldShowShadow());
  }

  int GetCurrentResizeHitTest(const gfx::Point& point) const {
    return GetResizeHitTest(point, GetCurrentResizeHitTestBounds());
  }

  gfx::Size GetManualResizeMinimumSize() const {
    gfx::Size minimum_size =
        GetWidget() ? GetWidget()->GetMinimumSize() : gfx::Size();
    // Widget constraints already include the dialog's shadow insets. Enlarge
    // only the fallback content size, not the declared window minimum again.
    gfx::Size fallback_minimum(64, 64);
    EnlargeForFramelessCompositorShadow(
        &fallback_minimum, xenon_delegate_->UseNativeFrame(),
        xenon_delegate_->UseDwm(), xenon_delegate_->ShouldShowShadow());
    minimum_size.SetToMax(fallback_minimum);
    return minimum_size;
  }

  void UpdateManualResize(const gfx::Point& screen_location) {
    views::Widget* widget = GetWidget();
    if (!widget || manual_resize_hit_test_ == HTNOWHERE) {
      return;
    }

    const gfx::Vector2d delta =
        screen_location - manual_resize_start_screen_location_;
    const int start_left = manual_resize_start_bounds_.x();
    const int start_top = manual_resize_start_bounds_.y();
    const int start_right = manual_resize_start_bounds_.right();
    const int start_bottom = manual_resize_start_bounds_.bottom();

    int left = start_left;
    int top = start_top;
    int right = start_right;
    int bottom = start_bottom;

    if (ResizeHitTestAffectsLeft(manual_resize_hit_test_)) {
      left = start_left + delta.x();
    } else if (ResizeHitTestAffectsRight(manual_resize_hit_test_)) {
      right = start_right + delta.x();
    }

    if (ResizeHitTestAffectsTop(manual_resize_hit_test_)) {
      top = start_top + delta.y();
    } else if (ResizeHitTestAffectsBottom(manual_resize_hit_test_)) {
      bottom = start_bottom + delta.y();
    }

    const gfx::Size minimum_size = GetManualResizeMinimumSize();
    if (right - left < minimum_size.width()) {
      if (ResizeHitTestAffectsLeft(manual_resize_hit_test_)) {
        left = right - minimum_size.width();
      } else {
        right = left + minimum_size.width();
      }
    }
    if (bottom - top < minimum_size.height()) {
      if (ResizeHitTestAffectsTop(manual_resize_hit_test_)) {
        top = bottom - minimum_size.height();
      } else {
        bottom = top + minimum_size.height();
      }
    }

    gfx::Rect bounds;
    bounds.SetByBounds(left, top, right, bottom);
    widget->SetBounds(bounds);
  }

  void EndManualResize() {
    manual_resize_hit_test_ = HTNOWHERE;
    if (views::Widget* widget = GetWidget(); widget && widget->HasCapture()) {
      widget->ReleaseCapture();
    }
  }

  void FinishAddedToWidget() {
    if (!GetWidget()) {
      return;
    }

    if (!xenon_delegate_->UseNativeFrame()) {
      SetBackground(nullptr);
      SetPaintToLayer();
      layer()->SetFillsBoundsOpaquely(false);
      // A transparent Electron canvas must not acquire the compositor's
      // default rounded card. Normal dialogs still use the standard radius
      // when DWM rounding is unavailable.
      const bool transparent_canvas =
          xenon_delegate_->UseTransparentWebContentsBackground();
      if (!transparent_canvas &&
          !UseDwmRoundedCorners(xenon_delegate_->UseDwm() ||
                                xenon_delegate_->UseSystemRoundedCorners())) {
        layer()->SetRoundedCornerRadius(
            gfx::RoundedCornersF(kDialogCornerRadius));
        SetWebViewCornersRadii(gfx::RoundedCornersF(kDialogCornerRadius));
      }
#if BUILDFLAG(IS_WIN)
      if (UseFramelessCompositorShadow(
              xenon_delegate_->UseDwm(),
              xenon_delegate_->ShouldShowShadow())) {
        SetupFramelessCompositorShadow();
      } else {
        RemoveDwmBorder();
      }
#endif
    }

    if (web_contents()) {
      if (!draggable_regions_enabler_) {
        draggable_regions_enabler_ =
            std::make_unique<XenonDraggableRegionsEnabler>(
                web_contents(),
                xenon_delegate_->UseTransparentWebContentsBackground());
      }
    }

    ui::Layer* widget_layer = GetWidget()->GetLayer();
    if (widget_layer &&
        xenon_delegate_->UseTransparentWebContentsBackground()) {
      widget_layer->SetFillsBoundsOpaquely(false);
    }
#if BUILDFLAG(IS_WIN)
    gfx::NativeWindow native_window = GetWidget()->GetNativeWindow();
    if (native_window && native_window->layer() &&
        xenon_delegate_->UseTransparentWebContentsBackground()) {
      native_window->layer()->SetFillsBoundsOpaquely(false);
    }
#endif
    if (!widget_layer) {
      return;
    }
    if (!xenon_delegate_->ShouldShowShadow() &&
        xenon_delegate_->UseTransparentWebContentsBackground()) {
      widget_layer->SetOpacity(1.0f);
      widget_layer->SetTransform(gfx::Transform());
      return;
    }
    widget_layer->SetOpacity(0.0f);
    gfx::Transform transform;
    transform.Translate(0, -10);
    widget_layer->SetTransform(transform);

    views::AnimationBuilder()
        .Once()
        .SetDuration(base::Milliseconds(150))
        .SetOpacity(widget_layer, 1.0f, gfx::Tween::EASE_OUT)
        .SetTransform(widget_layer, gfx::Transform(), gfx::Tween::EASE_OUT);
  }

  void RemoveDwmBorder() {
#if BUILDFLAG(IS_WIN)
    views::Widget* widget = GetWidget();
    if (!widget) {
      return;
    }
    HWND hwnd = views::HWNDForNativeWindow(widget->GetNativeWindow());
    if (!hwnd) {
      return;
    }
    constexpr DWORD kDwmwaBorderColor = 34;
    constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;
    COLORREF border_color = kDwmColorNone;
    ::DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border_color,
                            sizeof(border_color));
#endif
  }

  void SetupFramelessCompositorShadow() {
    const int margin = FramelessCompositorShadowMargin();
    SetBorder(views::CreateEmptyBorder(gfx::Insets(margin)));

    compositor_shadow_ = std::make_unique<ui::Shadow>();
    compositor_shadow_->Init(kFramelessCompositorShadowElevation);
    compositor_shadow_->SetRoundedCornerRadius(kDialogCornerRadius);
    AddLayerToRegion(compositor_shadow_->layer(), views::LayerRegion::kBelow);
    UpdateFramelessCompositorShadowBounds();
  }

  void UpdateFramelessCompositorShadowBounds() {
    if (!compositor_shadow_) {
      return;
    }
    const int margin = FramelessCompositorShadowMargin();
    compositor_shadow_->SetContentBounds(
        gfx::Rect(margin, margin, std::max(0, width() - 2 * margin),
                  std::max(0, height() - 2 * margin)));
  }

  class FrameView : public views::FrameView {
   public:
    gfx::Rect GetBoundsForClientView() const override { return bounds(); }

    gfx::Size GetMinimumSize() const override {
      // Widget and native edge resizing query the frame, not the dialog view.
      // Preserve the delegate's constraints just as NativeFrameView does.
      const views::Widget* widget = GetWidget();
      return widget && widget->client_view()
                 ? widget->client_view()->GetMinimumSize()
                 : gfx::Size();
    }

    gfx::Rect GetWindowBoundsForClientBounds(
        const gfx::Rect& client_bounds) const override {
      return client_bounds;
    }

    int NonClientHitTest(const gfx::Point& point) override {
      views::Widget* widget = GetWidget();
      if (!widget || !bounds().Contains(point)) {
        return HTNOWHERE;
      }
      if (views::ClientView* client = widget->client_view()) {
        gfx::Point p = point;
        const gfx::Rect client_bounds = GetBoundsForClientView();
        p.Offset(-client_bounds.x(), -client_bounds.y());
        return client->NonClientHitTest(p);
      }
      return HTNOWHERE;
    }
  };

  const raw_ptr<XenonWebDialog> xenon_delegate_;
  std::unique_ptr<SkRegion> draggable_region_;
  std::unique_ptr<XenonDraggableRegionsEnabler> draggable_regions_enabler_;
  std::unique_ptr<ui::Shadow> compositor_shadow_;
  int manual_resize_hit_test_ = HTNOWHERE;
  gfx::Point manual_resize_start_screen_location_;
  gfx::Rect manual_resize_start_bounds_;
  base::WeakPtrFactory<XenonWebDialogView> weak_ptr_factory_{this};
};

}  // namespace

void XenonWebDialog::SetHostedContentURL(views::Widget* widget,
                                         const GURL& url) {
  if (!widget) {
    return;
  }
  auto* view = static_cast<XenonWebDialogView*>(widget->widget_delegate());
  if (view) {
    view->SetContentURL(url);
  }
}

void XenonWebDialog::SetContentTitle(const std::u16string& title) {
  title_ = title;
  // Notify WebDialogView's accessibility callback as well as maintaining the
  // title returned by this delegate's GetDialogTitle override.
  set_dialog_title(title);
}

void XenonWebDialog::SetHostedCloseRequestHandler(
    views::Widget* widget,
    base::RepeatingCallback<bool()> handler) {
  if (!widget) {
    return;
  }
  auto* view = static_cast<XenonWebDialogView*>(widget->widget_delegate());
  if (view) {
    view->SetCloseRequestHandler(std::move(handler));
  }
}

void XenonWebDialog::SetCloseRequestHandler(
    base::RepeatingCallback<bool()> handler) {
  close_request_handler_ = std::move(handler);
}

bool XenonWebDialog::RequestClose() {
  return close_request_handler_.is_null() || close_request_handler_.Run();
}

bool XenonWebDialog::SetHostedContentTitle(views::Widget* widget,
                                           const std::u16string& title) {
  if (!widget) {
    return false;
  }
  auto* view = static_cast<XenonWebDialogView*>(widget->widget_delegate());
  return view && view->SetHostedContentTitle(title);
}

void XenonWebDialog::SetHostedContentVisible(views::Widget* widget,
                                             bool visible) {
  if (!widget) {
    return;
  }
  auto* view = static_cast<XenonWebDialogView*>(widget->widget_delegate());
  if (view) {
    view->SetHostedContentVisible(visible);
  }
}

bool XenonWebDialog::SetHostedMinimumSize(views::Widget* widget,
                                          const gfx::Size& minimum_size) {
  if (!widget || !widget->widget_delegate()) {
    return false;
  }
  auto* view = static_cast<XenonWebDialogView*>(widget->widget_delegate());
  view->SetHostedMinimumSize(minimum_size);
  return true;
}

bool XenonWebDialog::SetHostedContentBackgroundColor(views::Widget* widget,
                                                     SkColor color) {
  if (!widget) {
    return false;
  }
  auto* view = static_cast<XenonWebDialogView*>(widget->widget_delegate());
  return view && view->SetHostedContentBackgroundColor(color);
}

void XenonWebDialog::Show(content::BrowserContext* context,
                          const GURL& url,
                          int width,
                          int height,
                          const std::u16string& title) {
  ShowForLogin(context, url, width, height, title, /*out_widget=*/nullptr,
               gfx::NativeView(), ui::mojom::ModalType::kNone,
               base::OnceClosure(), /*show_close_button=*/false);
}

void XenonWebDialog::ShowForLogin(content::BrowserContext* context,
                                  const GURL& url,
                                  int width,
                                  int height,
                                  const std::u16string& title,
                                  raw_ptr<views::Widget>* out_widget,
                                  gfx::NativeView parent,
                                  ui::mojom::ModalType modal_type,
                                  base::OnceClosure on_dialog_closed,
                                  bool show_close_button) {
  ShowInternal(context, url, width, height, title, out_widget, parent,
               modal_type, std::move(on_dialog_closed), show_close_button,
               /*frame=*/false, /*dwm=*/XenonWebDialog::kDefaultUseDwm,
               /*system_rounded_corners=*/false,
               /*resizable=*/XenonWebDialog::kDefaultResizable,
                /*minimizable=*/true, /*maximizable=*/true,
                /*always_on_top=*/false, /*skip_taskbar=*/false,
                /*show=*/true, /*show_shadow=*/true);
}

void XenonWebDialog::ShowWithOptions(content::BrowserContext* context,
                                     const GURL& url,
                                     const base::DictValue& options,
                                     raw_ptr<views::Widget>* out_widget,
                                     gfx::NativeView parent,
                                     base::OnceClosure on_dialog_closed) {
  const std::string* title_str = options.FindString("title");
  const std::u16string title =
      title_str ? base::UTF8ToUTF16(*title_str) : u"Xenon Web Dialog";
  const int width = options.FindInt("width").value_or(800);
  const int height = options.FindInt("height").value_or(600);
  ui::mojom::ModalType modal_type = ui::mojom::ModalType::kNone;
  bool use_custom_modal = false;
  if (options.FindBool("modal").value_or(true)) {
    const std::string* modal_type_str = options.FindString("modalType");
    if (modal_type_str && (*modal_type_str == "child" || *modal_type_str == "tab")) {
      modal_type = ui::mojom::ModalType::kChild;
    } else if (modal_type_str && *modal_type_str == "window_custom") {
      modal_type = ui::mojom::ModalType::kWindow;
      use_custom_modal = true;
    } else {
      modal_type = ui::mojom::ModalType::kWindow;
    }
  }

  ShowInternal(
      context, url, width > 0 ? width : 800, height > 0 ? height : 600, title,
      out_widget, parent,
      modal_type,
      std::move(on_dialog_closed),
      options.FindBool("showCloseButton").value_or(true),
      options.FindBool("frame").value_or(false),
      options.FindBool("dwm").value_or(XenonWebDialog::kDefaultUseDwm),
      options.FindBool("systemRoundedCorners").value_or(false),
      options.FindBool("resizable").value_or(
          XenonWebDialog::kDefaultResizable),
      options.FindBool("minimizable").value_or(true),
      options.FindBool("maximizable").value_or(true),
      options.FindBool("alwaysOnTop").value_or(false),
      options.FindBool("skipTaskbar").value_or(false),
      options.FindBool("show").value_or(true),
      options.FindBool("shadow").value_or(true),
      use_custom_modal,
      options.FindBool("preserveNativeChildContent").value_or(false));
}

void XenonWebDialog::ShowInternal(content::BrowserContext* context,
                                  const GURL& url,
                                  int width,
                                  int height,
                                  const std::u16string& title,
                                  raw_ptr<views::Widget>* out_widget,
                                  gfx::NativeView parent,
                                  ui::mojom::ModalType modal_type,
                                  base::OnceClosure on_dialog_closed,
                                  bool show_close_button,
                                  bool frame,
                                  bool dwm,
                                  bool system_rounded_corners,
                                  bool resizable,
                                  bool minimizable,
                                  bool maximizable,
                                  bool always_on_top,
                                  bool skip_taskbar,
                                  bool show,
                                  bool show_shadow,
                                  bool use_custom_modal,
                                  bool preserve_native_child_content) {
  content::WebContents* web_contents = nullptr;
  if (modal_type == ui::mojom::ModalType::kChild && parent) {
    GlobalBrowserCollection* browsers = GlobalBrowserCollection::GetInstance();
    BrowserWindowInterface* browser_window = nullptr;
    if (views::Widget* parent_widget =
            views::Widget::GetWidgetForNativeView(parent)) {
      browser_window =
          browsers->FindBrowserWithWindow(parent_widget->GetNativeWindow());
      if (!browser_window) {
        if (views::Widget* top = parent_widget->GetTopLevelWidget()) {
          browser_window =
              browsers->FindBrowserWithWindow(top->GetNativeWindow());
        }
      }
    }
    Browser* browser = browser_window
                           ? browser_window->GetBrowserForMigrationOnly()
                           : nullptr;
    if (browser) {
      web_contents = browser->GetTabStripModel()->GetActiveWebContents();
    }
  }

  ui::mojom::ModalType delegate_modal_type = modal_type;
#if BUILDFLAG(IS_WIN)
  if (modal_type == ui::mojom::ModalType::kWindow && parent && use_custom_modal) {
    delegate_modal_type = ui::mojom::ModalType::kNone;
  }
#endif

  auto* delegate =
      new XenonWebDialog(url, width, height, title, delegate_modal_type,
                          std::move(on_dialog_closed), show_close_button, frame,
                          dwm, system_rounded_corners, show_shadow);
  delegate->set_can_resize(resizable);
  delegate->set_can_minimize(minimizable);
  delegate->set_can_maximize(maximizable);

  views::Widget* widget = nullptr;
  if (modal_type == ui::mojom::ModalType::kChild && web_contents) {
    widget = constrained_window::ShowWebModalDialogViews(
        new XenonWebDialogView(context, delegate,
                               std::make_unique<ChromeWebContentsHandler>()),
        web_contents);
  } else {
    widget = new views::Widget;
    views::Widget::InitParams params(
        views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
    params.delegate = new XenonWebDialogView(
        context, delegate, std::make_unique<ChromeWebContentsHandler>());
    params.remove_standard_frame = !frame;
#if BUILDFLAG(IS_WIN)
    params.dont_show_in_taskbar = skip_taskbar;
#if BUILDFLAG(ENABLE_XENON_SERVICE)
    params.init_properties_container.SetProperty(
        views::kRetainRedirectionBitmapKey, preserve_native_child_content);
#endif
#endif
    params.type = views::Widget::InitParams::TYPE_WINDOW;
    params.parent = parent;
    if (!show_shadow) {
      params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
    }
    if (!frame) {
#if BUILDFLAG(IS_WIN)
      const bool use_system_rounded_corners =
          UseDwmRoundedCorners(dwm || system_rounded_corners);
      if (use_system_rounded_corners) {
        params.rounded_corners =
            gfx::RoundedCornersF(kDwmRoundedCornerHintRadius);
      }
      if (!dwm) {
        // Transparent BrowserWindows remain layered even when DWM owns their
        // outer clip. Chromium removes WS_THICKFRAME in this mode, so the
        // WebDialogView manual-resize path remains active.
        params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
      }
#else
      params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
      params.rounded_corners = gfx::RoundedCornersF(kDialogCornerRadius);
#endif
    }

    widget->Init(std::move(params));
#if BUILDFLAG(IS_WIN)
    if (modal_type == ui::mojom::ModalType::kWindow && parent && use_custom_modal) {
      delegate->set_event_blocker(
          std::make_unique<ModalEventBlocker>(parent, widget));
    }
#endif
    // Match Electron BrowserWindow: no explicit origin → center on screen.
    // Owned/parented overlays keep the parent-relative placement from Init.
    if (!parent) {
      const gfx::Size size = widget->GetWindowBoundsInScreen().size();
      if (!size.IsEmpty()) {
        widget->CenterWindow(size);
      }
    }
    if (show) {
      widget->Show();
    }
  }
#if BUILDFLAG(IS_WIN)
  if (HWND hwnd = views::HWNDForNativeWindow(widget->GetNativeWindow())) {
    LONG_PTR ex_style = ::GetWindowLongPtr(hwnd, GWL_EXSTYLE);
    if (skip_taskbar) {
      ex_style |= WS_EX_TOOLWINDOW;
      ex_style &= ~WS_EX_APPWINDOW;
    } else {
      ex_style &= ~WS_EX_TOOLWINDOW;
      ex_style |= WS_EX_APPWINDOW;
    }
    ::SetWindowLongPtr(hwnd, GWL_EXSTYLE, ex_style);
  }
#endif
  if (always_on_top) {
    widget->SetZOrderLevel(ui::ZOrderLevel::kFloatingWindow);
  }
  if (show) {
    widget->Show();
  }
  if (out_widget) {
    *out_widget = widget;
  }
}

GURL XenonWebDialog::GetXenonOverlayWebUIUrl() {
  return GURL("chrome://xenon-overlay/");
}

GURL XenonWebDialog::GetXenonLoginWebUIUrl() {
  return GURL("chrome://xenon-login/");
}

GURL XenonWebDialog::GetXenonPlayerWebUIUrl() {
  return GURL("chrome://xenon-player/");
}

GURL XenonWebDialog::GetXenonPlayerByElecWebUIUrl() {
  return GURL("chrome://xenon-player-by-elec/");
}

GURL XenonWebDialog::GetXenonPlayerElectronWebUIUrl() {
  return GURL("chrome://xenon-player-electron/");
}

GURL XenonWebDialog::GetThunder2025WebUIUrl() {
  return GURL("chrome://thunder-2025/");
}

bool XenonWebDialog::UseTransparentWebContentsBackground() const {
  if (background_color_) {
    return SkColorGetA(*background_color_) != SK_AlphaOPAQUE;
  }
  // Electron playerControlWnd is created with transparent:true (dwm=false).
  // The first URL is about:blank; still keep the WebContents clear so the
  // native video HWND can show through CSS-transparent areas.
  if (!dwm_) {
    return true;
  }
  return url_.SchemeIs("chrome") && (url_.host() == "xenon-player" ||
                                     url_.host() == "xenon-player-by-elec" ||
                                     url_.host() == "xenon-player-electron");
}

namespace {

raw_ptr<views::Widget>& XenonPlayerDialogWidget() {
  static base::NoDestructor<raw_ptr<views::Widget>> widget;
  return *widget;
}

void ClearXenonPlayerDialogWidget() {
  XenonPlayerDialogWidget() = nullptr;
}

raw_ptr<views::Widget>& XenonPlayerByElecDialogWidget() {
  static base::NoDestructor<raw_ptr<views::Widget>> widget;
  return *widget;
}

void ClearXenonPlayerByElecDialogWidget() {
  XenonPlayerByElecDialogWidget() = nullptr;
}

}  // namespace

void XenonWebDialog::ShowXenonOverlay(Profile* profile) {
  Show(profile, GetXenonOverlayWebUIUrl(), 800, 600, u"Xenon Overlay");
}

void XenonWebDialog::ShowXenonPlayer(Profile* profile) {
  if (!profile) {
    LOG(WARNING) << "ShowXenonPlayer: no profile";
    return;
  }

  if (XenonPlayerDialogWidget()) {
    XenonPlayerDialogWidget()->Show();
    XenonPlayerDialogWidget()->Activate();
    return;
  }

  base::DictValue options;
  options.Set("title", "Xenon Player");
  options.Set("width", 1280);
  options.Set("height", 800);
  options.Set("modal", false);
  options.Set("frame", false);
  options.Set("resizable", true);
  options.Set("minimizable", true);
  options.Set("maximizable", true);
  options.Set("showCloseButton", true);
  options.Set("shadow", false);

  ShowWithOptions(profile, GetXenonPlayerWebUIUrl(), options,
                  &XenonPlayerDialogWidget(), gfx::NativeView(),
                  base::BindOnce(&ClearXenonPlayerDialogWidget));
}

void XenonWebDialog::ShowXenonPlayerByElec(Profile* profile) {
  if (!profile) {
    LOG(WARNING) << "ShowXenonPlayerByElec: no profile";
    return;
  }

  XenonManager* manager = XenonManager::GetInstance();
  manager->SetBrowserContext(profile);
  const std::string container_id = manager->GetElectronIpcContainerForOrigin(
      "chrome://xenon-player-by-elec");
  if (!manager->EnsureElectronIpcStarted(container_id)) {
    LOG(ERROR) << "ShowXenonPlayerByElec: failed to start container "
               << container_id;
    return;
  }

  if (XenonPlayerByElecDialogWidget()) {
    XenonPlayerByElecDialogWidget()->Show();
    XenonPlayerByElecDialogWidget()->Activate();
    return;
  }

  base::DictValue options;
  options.Set("title", "Electron Container Test");
  options.Set("width", 1280);
  options.Set("height", 800);
  options.Set("modal", false);
  options.Set("frame", false);
  options.Set("resizable", true);
  options.Set("minimizable", true);
  options.Set("maximizable", true);
  options.Set("showCloseButton", true);
  options.Set("shadow", false);

  ShowWithOptions(profile, GetXenonPlayerByElecWebUIUrl(), options,
                  &XenonPlayerByElecDialogWidget(), gfx::NativeView(),
                  base::BindOnce(&ClearXenonPlayerByElecDialogWidget));
}

void XenonWebDialog::ShowXenonPlayerElectron(Profile* profile) {
  if (!profile) {
    LOG(WARNING) << "ShowXenonPlayerElectron: no profile";
    return;
  }
  XenonManager* manager = XenonManager::GetInstance();
  manager->SetBrowserContext(profile);
  if (!XenonElectronWindowHost::GetInstance()->HasWindowsForContainer(
          "xenon-player-test")) {
    base::FilePath executable_dir;
    if (base::PathService::Get(base::DIR_EXE, &executable_dir)) {
      const base::FilePath player_dir =
          executable_dir.AppendASCII("xenon_player").AppendASCII("main");
      const base::FilePath player_main_path = player_dir.AppendASCII("main.js");
      std::string player_main_source;
      if (base::ReadFileToString(player_main_path, &player_main_source)) {
        auto player_config = xenon::ipc::mojom::IpcMainConfig::New();
        player_config->container_id = "xenon-player-test";
        player_config->embedded_main_source = std::move(player_main_source);
        player_config->virtual_main_path = player_main_path.AsUTF8Unsafe();
        player_config->app_path = player_dir.AsUTF8Unsafe();
        player_config->runtime_directory = player_dir.AsUTF8Unsafe();
        player_config->app_name = "xmp";
        const base::FilePath packaged_app =
            player_dir.AppendASCII("resources").AppendASCII("app");
        const base::FilePath archive_key_path =
            packaged_app.AppendASCII("asar-public-key.pem");
        if (base::PathExists(archive_key_path)) {
          std::string archive_key;
          if (!base::ReadFileToStringWithMaxSize(archive_key_path, &archive_key,
                                                 64 * 1024)) {
            LOG(ERROR) << "Unable to read packaged ASAR public key";
            return;
          }
          player_config->archive_public_keys.push_back(
              xenon::ipc::mojom::IpcArchivePublicKey::New(
                  packaged_app.AsUTF8Unsafe(), std::move(archive_key)));
        }
        // Preserve the packaged executable identity for process.execPath and
        // app.getPath("exe"). The container does not launch this executable.
        player_config->executable_path =
            player_dir.AppendASCII("xmp.exe").AsUTF8Unsafe();
        // The WebUI frontend is extracted from this packaged renderer. Keep
        // that source location so bundled CommonJS dependencies resolve their
        // own package and native addons under resources/app.
        auto renderer_mapping = xenon::ipc::mojom::IpcRendererUrlMapping::New();
        renderer_mapping->source_path_prefix =
            player_dir.AppendASCII("resources")
                .AppendASCII("app")
                .AppendASCII("out")
                .AppendASCII("main-renderer")
                .AsUTF8Unsafe();
        renderer_mapping->target_base_url = "chrome://xenon-player-electron/";
        // The embedded main entry is beside the runtime executable and builds
        // sibling renderer URLs from that location. Declare this source alias
        // explicitly, keeping the packaged source first for reverse CommonJS
        // path resolution in renderer documents.
        auto main_renderer_mapping = renderer_mapping.Clone();
        main_renderer_mapping->source_path_prefix =
            player_dir.AppendASCII("main-renderer").AsUTF8Unsafe();
        player_config->renderer_url_mappings.push_back(
            std::move(renderer_mapping));
        player_config->renderer_url_mappings.push_back(
            std::move(main_renderer_mapping));
        player_config->renderer_base_url = "chrome://xenon-player-electron/";
        // These tool pages are control surfaces for their native parent. Keep
        // the declaration in application configuration; the window host uses
        // the same pairing contract for any explicitly configured document.
        player_config->parent_window_pairing_urls = {
            "chrome://xenon-player-electron/clipper.html",
            "chrome://xenon-player-electron/gifClipper.html"};
        manager->SetElectronIpcContainerForOrigin(
            "chrome://xenon-player-electron", "xenon-player-test");
        manager->InitializeElectronIpc(std::move(player_config));
      }
    }
  }
  // A crashed Utility can leave its old browser window visible. An explicit
  // sidebar action must still be able to restart that container.
  if (!manager->EnsureElectronIpcStarted("xenon-player-test")) {
    LOG(ERROR) << "ShowXenonPlayerElectron: failed to start Electron container";
    return;
  }
  if (!XenonElectronWindowHost::GetInstance()->ActivateForContainer(
          "xenon-player-test")) {
    LOG(WARNING) << "ShowXenonPlayerElectron: waiting for ipcMain BrowserWindow";
  }
}

void XenonWebDialog::ShowThunder2025(Profile* profile) {
  if (!profile) {
    LOG(WARNING) << "ShowThunder2025: no profile";
    return;
  }
  XenonManager* manager = XenonManager::GetInstance();
  manager->SetBrowserContext(profile);
  if (!manager->EnsureElectronIpcStarted("thunder-2025")) {
    LOG(ERROR) << "ShowThunder2025: failed to start Electron container";
    return;
  }
  if (!XenonElectronWindowHost::GetInstance()->ActivateForContainer(
          "thunder-2025")) {
    LOG(WARNING) << "ShowThunder2025: waiting for ipcMain BrowserWindow";
  }
}

void XenonWebDialog::ShowDataMaskTest(Profile* profile) {
  LOG(INFO)
      << "DataMask test enabled from XenonWebDialog (Rule injected natively).";
}

void XenonWebDialog::OpenComponentExtensionWindow(Profile* profile) {
  if (!profile) {
    LOG(WARNING) << "OpenComponentExtensionWindow: no profile";
    return;
  }
  XenonExtensionManager* mgr = XenonExtensionManager::GetInstance();
  if (!mgr) {
    LOG(WARNING) << "OpenComponentExtensionWindow: XenonExtensionManager null";
    return;
  }
  if (!mgr->ShowExtension(profile)) {
    LOG(WARNING)
        << "OpenComponentExtensionWindow: ShowExtension failed (extension "
           "not loaded — avoid --show-xenon-extension on first run)";
  }
}

XenonWebDialog::XenonWebDialog(const GURL& url,
                               int width,
                               int height,
                               const std::u16string& title,
                               ui::mojom::ModalType modal_type,
                               base::OnceClosure on_dialog_closed,
                               bool show_close_button,
                               bool frame,
                               bool dwm,
                               bool system_rounded_corners,
                               bool show_shadow)
    : url_(url),
      width_(width),
      height_(height),
      title_(title),
      modal_type_(modal_type),
      on_dialog_closed_(std::move(on_dialog_closed)),
      show_close_button_(show_close_button),
      frame_(frame),
      dwm_(dwm),
      system_rounded_corners_(system_rounded_corners),
      show_shadow_(show_shadow) {}

XenonWebDialog::~XenonWebDialog() = default;

ui::mojom::ModalType XenonWebDialog::GetDialogModalType() const {
  return modal_type_;
}

std::u16string XenonWebDialog::GetDialogTitle() const {
  return title_;
}

GURL XenonWebDialog::GetDialogContentURL() const {
  return url_;
}

void XenonWebDialog::GetWebUIMessageHandlers(
    std::vector<content::WebUIMessageHandler*>* handlers) {}

void XenonWebDialog::GetDialogSize(gfx::Size* size) const {
  size->SetSize(width_, height_);
  EnlargeForFramelessCompositorShadow(size, frame_, dwm_, show_shadow_);
}

std::string XenonWebDialog::GetDialogArgs() const {
  return std::string();
}

void XenonWebDialog::OnDialogClosed(const std::string& json_retval) {
  if (on_dialog_closed_) {
    std::move(on_dialog_closed_).Run();
  }
  delete this;
}

bool XenonWebDialog::OnDialogCloseRequested() {
  return close_request_handler_.is_null()
             ? ui::WebDialogDelegate::OnDialogCloseRequested()
             : RequestClose();
}

void XenonWebDialog::OnCloseContents(content::WebContents* source,
                                     bool* out_close_dialog) {
  if (!RequestClose()) {
    if (out_close_dialog) {
      *out_close_dialog = false;
    }
    return;
  }
  if (out_close_dialog) {
    *out_close_dialog = true;
  }
  if (source) {
    if (views::Widget* widget =
            views::Widget::GetTopLevelWidgetForNativeView(source->GetNativeView())) {
      widget->Hide();
    }
  }
}

bool XenonWebDialog::ShouldCloseDialogOnEscape() const {
  // BrowserWindow leaves Escape to the application. WebDialogView's default
  // Escape handler would instead issue both DOM and native close requests.
  return close_request_handler_.is_null() &&
         ui::WebDialogDelegate::ShouldCloseDialogOnEscape();
}

bool XenonWebDialog::ShouldShowDialogTitle() const {
  return frame_ && !title_.empty();
}

bool XenonWebDialog::ShouldShowCloseButton() const {
  return show_close_button_;
}

ui::WebDialogDelegate::FrameKind XenonWebDialog::GetWebDialogFrameKind() const {
  return ui::WebDialogDelegate::FrameKind::kNonClient;
}

}  // namespace xenon
