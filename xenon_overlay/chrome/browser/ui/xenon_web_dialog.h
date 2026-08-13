#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_WEB_DIALOG_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_WEB_DIALOG_H_

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/values.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/gfx/native_ui_types.h"
#include "ui/web_dialogs/web_dialog_delegate.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_WIN)
#include "ui/events/event_handler.h"
#endif

namespace views {
class Widget;
}  // namespace views

namespace content {
class BrowserContext;
}

class Profile;

namespace xenon {

// A generic WebDialog delegate for showing HTML content in a dialog.
// Supports CSS drag regions via -webkit-app-region property:
//   - Use -webkit-app-region: drag; to make an element draggable
//   - Use -webkit-app-region: no-drag; to exclude an element from dragging
// The dialog will automatically handle window dragging based on these CSS properties.
class XenonWebDialog : public ui::WebDialogDelegate {
 public:
  // Default used when create options omit the `dwm` parameter.
  static constexpr bool kDefaultUseDwm = false;
  // Default used when create options omit the `resizable` parameter.
  static constexpr bool kDefaultResizable = false;

  // Shows the dialog.
  // |context|: The browser profile.
  // |url|: The URL to load.
  // |width|, |height|: The size of the dialog.
  // |title|: The window title.
  static void Show(content::BrowserContext* context,
                   const GURL& url,
                   int width,
                   int height,
                   const std::u16string& title);

  // Login / modal: optional parent, modal type, widget output, close callback.
  static void ShowForLogin(content::BrowserContext* context,
                           const GURL& url,
                           int width,
                           int height,
                           const std::u16string& title,
                           raw_ptr<views::Widget>* out_widget,
                           gfx::NativeView parent,
                           ui::mojom::ModalType modal_type,
                           base::OnceClosure on_dialog_closed,
                           bool show_close_button = true);

  static void ShowWithOptions(content::BrowserContext* context,
                              const GURL& url,
                              const base::DictValue& options,
                              raw_ptr<views::Widget>* out_widget,
                              gfx::NativeView parent,
                              base::OnceClosure on_dialog_closed);

  // `--show-xenon-extension`: register Xenon WebUI Mojo and open chrome://xenon-overlay/.
  // Remote / observer checks run from the WebUI page (split Mojo tests).
  static void ShowXenonOverlay(Profile* profile);

  // Single source of truth for the WebUI URL shown by ShowXenonOverlay(). The WebUI
  // page exercises `window.xenon` (XenonPageHost) in resources/webui/xenon/index.ts.
  static GURL GetXenonOverlayWebUIUrl();

  // Login gate WebUI (`chrome://xenon-login/`)：独立 login HTML/CSS/JS，`chrome.send`。
  static GURL GetXenonLoginWebUIUrl();

  // test entry for Data Mask (网页打码).
  static void ShowDataMaskTest(Profile* profile);

  // Opens the Xenon component extension UI in a WebDialog (same path as
  // XenonExtensionManager::ShowExtension). Use when extension is already
  // loaded (normal startup loads it; `--show-xenon-extension` skips load).
  static void OpenComponentExtensionWindow(Profile* profile);

  bool UseNativeFrame() const { return frame_; }
  bool UseDwm() const { return dwm_; }

#if BUILDFLAG(IS_WIN)
  void set_event_blocker(std::unique_ptr<ui::EventHandler> blocker) {
    event_blocker_ = std::move(blocker);
  }
#endif

 private:
  XenonWebDialog(const GURL& url,
                 int width,
                 int height,
                 const std::u16string& title,
                 ui::mojom::ModalType modal_type,
                 base::OnceClosure on_dialog_closed,
                 bool show_close_button,
                 bool frame,
                 bool dwm);
  ~XenonWebDialog() override;

  static void ShowInternal(content::BrowserContext* context,
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
                           bool resizable,
                           bool minimizable,
                           bool maximizable,
                           bool always_on_top,
                           bool skip_taskbar,
                           bool show,
                           bool use_custom_modal = false);

  // ui::WebDialogDelegate:
  ui::mojom::ModalType GetDialogModalType() const override;
  std::u16string GetDialogTitle() const override;
  GURL GetDialogContentURL() const override;
  void GetWebUIMessageHandlers(
      std::vector<content::WebUIMessageHandler*>* handlers) override;
  void GetDialogSize(gfx::Size* size) const override;
  std::string GetDialogArgs() const override;
  void OnDialogClosed(const std::string& json_retval) override;
  void OnCloseContents(content::WebContents* source,
                       bool* out_close_dialog) override;
  bool ShouldShowDialogTitle() const override;
  bool ShouldShowCloseButton() const override;
  FrameKind GetWebDialogFrameKind() const override;

  GURL url_;
  int width_;
  int height_;
  std::u16string title_;
  ui::mojom::ModalType modal_type_ = ui::mojom::ModalType::kNone;
  base::OnceClosure on_dialog_closed_;
  bool show_close_button_ = false;
  bool frame_ = false;
  bool dwm_ = kDefaultUseDwm;
#if BUILDFLAG(IS_WIN)
  std::unique_ptr<ui::EventHandler> event_blocker_;
#endif
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_WEB_DIALOG_H_
