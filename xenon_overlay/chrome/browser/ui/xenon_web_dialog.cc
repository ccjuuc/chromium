#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"

#include "chrome/browser/ui/webui/chrome_web_contents_handler.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/page/draggable_region.mojom.h"
#include "third_party/skia/include/core/SkRegion.h"
#include "ui/base/hit_test.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/views/controls/webview/web_dialog_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/frame_view.h"

#include "ui/base/models/image_model.h"
#include "xenon_overlay/chrome/browser/ui/svg_image_source.h"

namespace xenon {

// A custom WebDialogView that supports CSS drag regions (-webkit-app-region: drag).
class XenonWebDialogView : public views::WebDialogView {
 public:
// ... constructors ...
  XenonWebDialogView(content::BrowserContext* context,
                     ui::WebDialogDelegate* delegate,
                     std::unique_ptr<WebContentsHandler> handler)
      : views::WebDialogView(context, delegate, std::move(handler)) {}
  ~XenonWebDialogView() override = default;

  // ... (DraggableRegionsChanged, NonClientHitTest, AddedToWidget, CreateClientView, CreateFrameView methods remain same)

  // Override GetWindowIcon to test SvgImageSource
  ui::ImageModel GetWindowIcon() override {
    constexpr char kSvgIcon[] =
        "<svg width=\"256\" height=\"256\" viewBox=\"0 0 24 24\" fill=\"none\" "
        "xmlns=\"http://www.w3.org/2000/svg\">"
        "<path d=\"M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 "
        "12 2zm-1 17.93c-3.95-.49-7-3.85-7-7.93 0-.62.08-1.21.21-1.79L9 "
        "15v1c0 1.1.9 2 2 2v1.93zm6.9-2.54c-.26-.81-1-1.39-1.9-1.39h-1v-3c0-.55-.45-1-1-1H8v-2h2c.55 0 1-.45 1-1V7h2c1.1 0 2-.9 2-2v-.41c2.93 1.19 5 4.06 5 7.41 0 2.08-.8 3.97-2.1 5.39z\" fill=\"#00FF00\"/>"
        "</svg>";
    
    gfx::ImageSkia image_skia(std::make_unique<SvgImageSource>(kSvgIcon, 256, std::nullopt),
                              gfx::Size(256, 256));
    return ui::ImageModel::FromImageSkia(image_skia);
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
  auto* delegate = new XenonWebDialog(url, width, height, title);

  views::Widget* widget = new views::Widget;
  views::Widget::InitParams params(
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);

  params.delegate = new XenonWebDialogView(
      context, delegate, std::make_unique<ChromeWebContentsHandler>());
  params.remove_standard_frame = true;
  params.type = views::Widget::InitParams::TYPE_WINDOW;

  widget->Init(std::move(params));
  widget->Show();
}

XenonWebDialog::XenonWebDialog(const GURL& url,
                               int width,
                               int height,
                               const std::u16string& title)
    : url_(url), width_(width), height_(height), title_(title) {}

XenonWebDialog::~XenonWebDialog() = default;

ui::mojom::ModalType XenonWebDialog::GetDialogModalType() const {
  return ui::mojom::ModalType::kNone;
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
  return false;
}

ui::WebDialogDelegate::FrameKind XenonWebDialog::GetWebDialogFrameKind() const {
  return ui::WebDialogDelegate::FrameKind::kNonClient;
}

}  // namespace xenon
