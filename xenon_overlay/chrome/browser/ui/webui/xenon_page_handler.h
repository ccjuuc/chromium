#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_PAGE_HANDLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_PAGE_HANDLER_H_

#include "base/memory/raw_ptr.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon.mojom.h"

namespace content {
class WebUI;
}

namespace xenon {

class XenonPageHandler : public mojom::PageHandler {
 public:
  XenonPageHandler(mojo::PendingReceiver<mojom::PageHandler> receiver,
                   content::WebUI* web_ui);
  ~XenonPageHandler() override;

  XenonPageHandler(const XenonPageHandler&) = delete;
  XenonPageHandler& operator=(const XenonPageHandler&) = delete;

  // mojom::PageHandler:
  void Close() override;
  void ConnectToService(ConnectToServiceCallback callback) override;
  void PingMainService(PingMainServiceCallback callback) override;
  void TestSharedRemoteDuplicate(
      TestSharedRemoteDuplicateCallback callback) override;
  void PingAssociatedRemote(PingAssociatedRemoteCallback callback) override;
  void TestUtilityToBrowserObserver(
      TestUtilityToBrowserObserverCallback callback) override;
  void TestDataMask(TestDataMaskCallback callback) override;
  void OpenComponentExtensionDialog(
      OpenComponentExtensionDialogCallback callback) override;
  void SetAppSessionLoggedIn(bool logged_in) override;

 private:
  mojo::Receiver<mojom::PageHandler> receiver_;
  raw_ptr<content::WebUI> web_ui_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_XENON_PAGE_HANDLER_H_
