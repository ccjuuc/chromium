#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_WEB_DIALOG_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_WEB_DIALOG_H_

#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/web_dialogs/web_dialog_delegate.h"
#include "url/gurl.h"

namespace content {
class BrowserContext;
}

namespace xenon {

// A generic WebDialog delegate for showing HTML content in a dialog.
// Supports CSS drag regions via -webkit-app-region property:
//   - Use -webkit-app-region: drag; to make an element draggable
//   - Use -webkit-app-region: no-drag; to exclude an element from dragging
// The dialog will automatically handle window dragging based on these CSS properties.
class XenonWebDialog : public ui::WebDialogDelegate {
 public:
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

 private:
  XenonWebDialog(const GURL& url,
                 int width,
                 int height,
                 const std::u16string& title);
  ~XenonWebDialog() override;

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
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_WEB_DIALOG_H_
