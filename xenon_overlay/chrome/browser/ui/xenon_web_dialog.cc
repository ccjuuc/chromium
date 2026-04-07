#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "ui/gfx/native_ui_types.h"
#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"
#include "chrome/browser/ui/webui/chrome_web_contents_handler.h"
#include "content/public/browser/browser_context.h"
#include "url/gurl.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/page/draggable_region.mojom.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/base/hit_test.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/views/controls/webview/web_dialog_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/frame_view.h"

#include "third_party/blink/public/mojom/frame/data_mask.mojom.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/navigation_handle.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "chrome/browser/ui/browser_list.h"
#include "chrome/browser/ui/browser_list_observer.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model_observer.h"

namespace xenon {


// A custom WebDialogView that supports CSS drag regions (-webkit-app-region: drag).
class XenonWebDialogView : public views::WebDialogView {
 public:
  XenonWebDialogView(content::BrowserContext* context,
                     ui::WebDialogDelegate* delegate,
                     std::unique_ptr<WebContentsHandler> handler)
      : views::WebDialogView(context, delegate, std::move(handler)) {}
  ~XenonWebDialogView() override = default;

  // content::WebContentsDelegate:
  void DraggableRegionsChanged(
      const std::vector<blink::mojom::DraggableRegionPtr>& regions,
      content::WebContents* contents) override {
    draggable_region_ = std::make_unique<SkRegion>();
    for (const auto& region : regions) {
      draggable_region_->op(gfx::RectToSkIRect(region->bounds),
                            region->draggable ? SkRegion::kUnion_Op
                                              : SkRegion::kDifference_Op);
    }
  }

  // views::ClientView:
  int NonClientHitTest(const gfx::Point& point) override {
    if (draggable_region_ &&
        draggable_region_->contains(point.x(), point.y())) {
      return HTCAPTION;
    }
    return views::WebDialogView::NonClientHitTest(point);
  }

  // views::View:
  void AddedToWidget() override {
    views::WebDialogView::AddedToWidget();
    if (web_contents()) {
      web_contents()->SetSupportsDraggableRegions(true);
    }
  }

  views::ClientView* CreateClientView(views::Widget* widget) override {
    return this;
  }

  std::unique_ptr<views::FrameView> CreateFrameView(
      views::Widget* widget) override {
    return std::make_unique<EmptyFrameView>();
  }

  bool ShouldDescendIntoChildForEventHandling(
      gfx::NativeView child,
      const gfx::Point& location) override {
    // Prevent WebView from consuming mouse events in draggable regions.
    if (draggable_region_ &&
        draggable_region_->contains(location.x(), location.y())) {
      return false;
    }
    return views::WebDialogView::ShouldDescendIntoChildForEventHandling(
        child, location);
  }

 private:
  std::unique_ptr<SkRegion> draggable_region_;

  // A minimal FrameView that delegates hit testing to the ClientView.
  class EmptyFrameView : public views::FrameView {
   public:
    EmptyFrameView() = default;
    ~EmptyFrameView() override = default;

    gfx::Rect GetBoundsForClientView() const override { return bounds(); }

    gfx::Rect GetWindowBoundsForClientBounds(
        const gfx::Rect& client_bounds) const override {
      return client_bounds;
    }

    int NonClientHitTest(const gfx::Point& point) override {
      views::Widget* widget = GetWidget();
      if (widget && widget->client_view()) {
        gfx::Point point_in_client = point;
        gfx::Rect client_bounds = GetBoundsForClientView();
        point_in_client.Offset(-client_bounds.x(), -client_bounds.y());
        return widget->client_view()->NonClientHitTest(point_in_client);
      }
      return HTNOWHERE;
    }

    void GetWindowMask(const gfx::Size& size, SkPath* window_mask) override {}
    void ResetWindowControls() override {}
    void UpdateWindowIcon() override {}
    void UpdateWindowTitle() override {}
    void SizeConstraintsChanged() override {}
  };
};

// static
void XenonWebDialog::Show(content::BrowserContext* context,
                          const GURL& url,
                          int width,
                          int height,
                          const std::u16string& title) {
  ShowForLogin(context, url, width, height, title, /*out_widget=*/nullptr,
               gfx::NativeView(), ui::mojom::ModalType::kNone,
               base::OnceClosure(), /*show_close_button=*/false);
}

// static
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

  widget->Init(std::move(params));
  widget->Show();
  if (out_widget) {
    *out_widget = widget;
  }
}

// static
GURL XenonWebDialog::GetXenonOverlayWebUIUrl() {
  return GURL("chrome://xenon-overlay/");
}

// static
GURL XenonWebDialog::GetXenonLoginWebUIUrl() {
  return GURL("chrome://xenon-login/");
}

// static
void XenonWebDialog::ShowXenonOverlay(Profile* profile) {
  Show(profile, GetXenonOverlayWebUIUrl(), 800, 600, u"Xenon Overlay");
}

// static
void XenonWebDialog::ShowDataMaskTest(Profile* profile) {
  // Global datamask policy is injected via xenon::RenderFrameHostDataMaskApplyPolicy.
  // No additional Activator needed.
  LOG(INFO) << "DataMask test enabled from XenonWebDialog (Rule injected natively).";
}

// static
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
      show_close_button_(show_close_button) {}

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
