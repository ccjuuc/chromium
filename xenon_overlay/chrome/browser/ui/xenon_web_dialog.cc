#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"

#include <algorithm>
#include <memory>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/chrome_web_contents_handler.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/page/draggable_region.mojom.h"
#include "third_party/skia/include/core/SkRegion.h"
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
#include "ui/views/widget/widget.h"
#include "ui/views/window/frame_view.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <dwmapi.h>

#include "base/win/windows_version.h"
#include "ui/display/win/screen_win.h"
#include "ui/views/win/hwnd_util.h"
#endif

namespace xenon {
namespace {

constexpr int kDialogCornerRadius = 16;
constexpr int kFramelessCompositorShadowElevation = 8;

#if BUILDFLAG(IS_WIN)
bool IsWin11OrLater() {
  return base::win::GetVersion() >= base::win::Version::WIN11;
}

bool UseFramelessCompositorShadow() {
  return !IsWin11OrLater();
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
bool IsWin11OrLater() {
  return false;
}

bool UseFramelessCompositorShadow() {
  return false;
}

int FramelessCompositorShadowMargin() {
  return 0;
}
#endif

void EnlargeForFramelessCompositorShadow(gfx::Size* size) {
  if (UseFramelessCompositorShadow()) {
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

bool IsInResizeBorder(const gfx::Point& point, const gfx::Size& size) {
  const int border = ResizeBorderThickness();
  return point.x() < border || point.y() < border ||
         point.x() >= size.width() - border ||
         point.y() >= size.height() - border;
}

// Frameless WebDialogView: -webkit-app-region drag + edge resize.
class XenonWebDialogView : public views::WebDialogView {
 public:
  XenonWebDialogView(content::BrowserContext* context,
                     ui::WebDialogDelegate* delegate,
                     std::unique_ptr<WebContentsHandler> handler)
      : views::WebDialogView(context, delegate, std::move(handler)) {}
  ~XenonWebDialogView() override = default;

  void DraggableRegionsChanged(
      const std::vector<blink::mojom::DraggableRegionPtr>& regions,
      content::WebContents* contents) override {
    draggable_region_ = std::make_unique<SkRegion>();
    for (const auto& region : regions) {
      draggable_region_->op(
          gfx::RectToSkIRect(region->bounds),
          region->draggable ? SkRegion::kUnion_Op : SkRegion::kDifference_Op);
    }
  }

  void AddedToWidget() override {
    views::WebDialogView::AddedToWidget();
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&XenonWebDialogView::FinishAddedToWidget,
                                  weak_ptr_factory_.GetWeakPtr()));
  }

  void OnThemeChanged() override {
    views::WebDialogView::OnThemeChanged();
    SetBackground(nullptr);
  }

  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    views::WebDialogView::OnBoundsChanged(previous_bounds);
    UpdateFramelessCompositorShadowBounds();
  }

  int NonClientHitTest(const gfx::Point& point) override {
    if (draggable_region_ &&
        draggable_region_->contains(point.x(), point.y())) {
      return HTCAPTION;
    }
    return views::WebDialogView::NonClientHitTest(point);
  }

  views::ClientView* CreateClientView(views::Widget* widget) override {
    return this;
  }

  std::unique_ptr<views::FrameView> CreateFrameView(
      views::Widget* widget) override {
    return std::make_unique<FrameView>();
  }

  bool ShouldDescendIntoChildForEventHandling(
      gfx::NativeView child,
      const gfx::Point& location) override {
    if (CanResizeFrame(GetWidget()) && IsInResizeBorder(location, size())) {
      return false;
    }
    if (draggable_region_ &&
        draggable_region_->contains(location.x(), location.y())) {
      return false;
    }
    return views::WebDialogView::ShouldDescendIntoChildForEventHandling(
        child, location);
  }

 private:
  void FinishAddedToWidget() {
    if (!GetWidget()) {
      return;
    }

    SetBackground(nullptr);
    SetPaintToLayer();
    layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(kDialogCornerRadius));
    layer()->SetFillsBoundsOpaquely(false);
#if BUILDFLAG(IS_WIN)
    if (UseFramelessCompositorShadow()) {
      SetWebViewCornersRadii(gfx::RoundedCornersF(kDialogCornerRadius));
      SetupFramelessCompositorShadow();
    } else {
      RemoveDwmBorder();
    }
#endif

    if (web_contents()) {
      web_contents()->SetSupportsDraggableRegions(true);
    }

    ui::Layer* widget_layer = GetWidget()->GetLayer();
    if (!widget_layer) {
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

    gfx::Rect GetWindowBoundsForClientBounds(
        const gfx::Rect& client_bounds) const override {
      return client_bounds;
    }

    int NonClientHitTest(const gfx::Point& point) override {
      views::Widget* widget = GetWidget();
      if (!widget || !bounds().Contains(point)) {
        return HTNOWHERE;
      }
      if (CanResizeFrame(widget)) {
        const int border = ResizeBorderThickness();
        const int corner = std::max(0, 16 - border);
        const int hit = GetHTComponentForFrame(point, gfx::Insets(border),
                                               corner, corner, true);
        if (hit != HTNOWHERE) {
          return hit;
        }
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

  std::unique_ptr<SkRegion> draggable_region_;
  std::unique_ptr<ui::Shadow> compositor_shadow_;
  base::WeakPtrFactory<XenonWebDialogView> weak_ptr_factory_{this};
};

}  // namespace

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
  auto* delegate =
      new XenonWebDialog(url, width, height, title, modal_type,
                         std::move(on_dialog_closed), show_close_button);

  views::Widget* widget = new views::Widget;
  views::Widget::InitParams params(
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
  params.delegate = new XenonWebDialogView(
      context, delegate, std::make_unique<ChromeWebContentsHandler>());
  params.remove_standard_frame = true;
  params.type = views::Widget::InitParams::TYPE_WINDOW;
  params.parent = parent;
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
#if BUILDFLAG(IS_WIN)
  if (IsWin11OrLater()) {
    params.rounded_corners = gfx::RoundedCornersF(kDialogCornerRadius);
  }
#else
  params.rounded_corners = gfx::RoundedCornersF(kDialogCornerRadius);
#endif

  widget->Init(std::move(params));
  // TYPE_WINDOW + CanResize() keeps WS_CAPTION/WS_THICKFRAME on Windows via
  // HWNDMessageHandler::SizeConstraintsChanged(); no manual WS_CAPTION needed.
  widget->Show();
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

void XenonWebDialog::ShowXenonOverlay(Profile* profile) {
  Show(profile, GetXenonOverlayWebUIUrl(), 800, 600, u"Xenon Overlay");
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
                               bool show_close_button)
    : url_(url),
      width_(width),
      height_(height),
      title_(title),
      modal_type_(modal_type),
      on_dialog_closed_(std::move(on_dialog_closed)),
      show_close_button_(show_close_button) {
  set_can_resize(true);
}

XenonWebDialog::~XenonWebDialog() = default;

ui::mojom::ModalType XenonWebDialog::GetDialogModalType() const {
  return modal_type_;
}

std::u16string XenonWebDialog::GetDialogTitle() const {
  return std::u16string();
}

GURL XenonWebDialog::GetDialogContentURL() const {
  return url_;
}

void XenonWebDialog::GetWebUIMessageHandlers(
    std::vector<content::WebUIMessageHandler*>* handlers) {}

void XenonWebDialog::GetDialogSize(gfx::Size* size) const {
  size->SetSize(width_, height_);
  EnlargeForFramelessCompositorShadow(size);
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

void XenonWebDialog::OnCloseContents(content::WebContents* source,
                                     bool* out_close_dialog) {
  if (out_close_dialog) {
    *out_close_dialog = true;
  }
}

bool XenonWebDialog::ShouldShowDialogTitle() const {
  return false;
}

bool XenonWebDialog::ShouldShowCloseButton() const {
  return show_close_button_;
}

ui::WebDialogDelegate::FrameKind XenonWebDialog::GetWebDialogFrameKind() const {
  return ui::WebDialogDelegate::FrameKind::kNonClient;
}

}  // namespace xenon
