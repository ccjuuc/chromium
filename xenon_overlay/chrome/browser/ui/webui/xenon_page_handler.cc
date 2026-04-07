#include "xenon_overlay/chrome/browser/ui/webui/xenon_page_handler.h"

#include "build/buildflag.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"
#include "xenon_overlay/chrome/browser/xenon_extension_manager.h"
#include "xenon_overlay/chrome/browser/xenon_login_controller.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"

namespace xenon {

namespace {

bool IsOkPingReply(const std::string& response) {
  return !response.empty() && response.rfind("Error:", 0) != 0;
}

}  // namespace

XenonPageHandler::XenonPageHandler(
    mojo::PendingReceiver<mojom::PageHandler> receiver,
    content::WebUI* web_ui)
    : receiver_(this, std::move(receiver)), web_ui_(web_ui) {}

XenonPageHandler::~XenonPageHandler() = default;

void XenonPageHandler::Close() {
  LOG(INFO) << "XenonPageHandler: Close requested";
  if (web_ui_ && web_ui_->GetWebContents()) {
    web_ui_->GetWebContents()->ClosePage();
  }
}

void XenonPageHandler::ConnectToService(ConnectToServiceCallback callback) {
  // Same behavior as PingMainService with slightly different success wording.
  PingMainService(base::BindOnce(
      [](ConnectToServiceCallback cb, bool success, const std::string& message) {
        if (success) {
          std::move(cb).Run(true, "Connected: " + message);
        } else {
          std::move(cb).Run(false, message);
        }
      },
      std::move(callback)));
}

void XenonPageHandler::PingMainService(PingMainServiceCallback callback) {
  LOG(INFO) << "XenonPageHandler: PingMainService requested";

  if (!web_ui_ || !web_ui_->GetWebContents()) {
    std::move(callback).Run(false, "WebUI context lost");
    return;
  }

  XenonManager* manager = XenonManager::GetInstance();
  if (!manager) {
    std::move(callback).Run(false, "XenonManager not available");
    return;
  }

  manager->EnsureServiceStarted(web_ui_->GetWebContents()->GetBrowserContext());

  manager->Ping(base::BindOnce(
      [](PingMainServiceCallback callback, const std::string& response) {
        if (IsOkPingReply(response)) {
          std::move(callback).Run(
              true, "[Main Remote] Ping OK: " + response);
        } else {
          std::move(callback).Run(false, response);
        }
      },
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false,
          "Main Remote Ping failed (callback dropped)")));
}

void XenonPageHandler::TestSharedRemoteDuplicate(
    TestSharedRemoteDuplicateCallback callback) {
  LOG(INFO) << "XenonPageHandler: TestSharedRemoteDuplicate requested";

  if (!web_ui_ || !web_ui_->GetWebContents()) {
    std::move(callback).Run(false, "WebUI context lost");
    return;
  }

  XenonManager* manager = XenonManager::GetInstance();
  if (!manager) {
    std::move(callback).Run(false, "XenonManager not available");
    return;
  }

  manager->EnsureServiceStarted(web_ui_->GetWebContents()->GetBrowserContext());

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  auto dup = manager->DuplicateServiceRemote();
  const bool bound = dup.is_bound();
  std::move(callback).Run(
      bound, bound ? "[SharedRemote duplicate] is_bound=true (same pipe handle family)"
                   : "[SharedRemote duplicate] is_bound=false");
#else
  std::move(callback).Run(
      false, "[SharedRemote duplicate] disabled (enable_xenon_manager_shared_remote=false)");
#endif
}

void XenonPageHandler::PingAssociatedRemote(
    PingAssociatedRemoteCallback callback) {
  LOG(INFO) << "XenonPageHandler: PingAssociatedRemote requested";

  if (!web_ui_ || !web_ui_->GetWebContents()) {
    std::move(callback).Run(false, "WebUI context lost");
    return;
  }

  XenonManager* manager = XenonManager::GetInstance();
  if (!manager) {
    std::move(callback).Run(false, "XenonManager not available");
    return;
  }

  manager->EnsureServiceStarted(web_ui_->GetWebContents()->GetBrowserContext());

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  manager->PingAssociated(base::BindOnce(
      [](PingAssociatedRemoteCallback callback, const std::string& response) {
        if (IsOkPingReply(response)) {
          std::move(callback).Run(
              true, "[Associated Remote] PingAssociated OK: " + response);
        } else {
          std::move(callback).Run(false, response);
        }
      },
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false,
          "Associated Remote failed (callback dropped)")));
#else
  std::move(callback).Run(
      false,
      "[Associated Remote] disabled (enable_xenon_associated_side=false)");
#endif
}

void XenonPageHandler::TestUtilityToBrowserObserver(
    TestUtilityToBrowserObserverCallback callback) {
  LOG(INFO) << "XenonPageHandler: TestUtilityToBrowserObserver requested";

  if (!web_ui_ || !web_ui_->GetWebContents()) {
    std::move(callback).Run(false, "WebUI context lost");
    return;
  }

  XenonManager* manager = XenonManager::GetInstance();
  if (!manager) {
    std::move(callback).Run(false, "XenonManager not available");
    return;
  }

#if !BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  std::move(callback).Run(
      false,
      "[Observer] disabled (enable_xenon_browser_observer=false)");
  return;
#endif

  manager->EnsureServiceStarted(web_ui_->GetWebContents()->GetBrowserContext());

  manager->CaptureNextObserverEventForTest(base::BindOnce(
      [](TestUtilityToBrowserObserverCallback callback,
         const std::string& observer_message) {
        if (observer_message.empty()) {
          std::move(callback).Run(
              false,
              "[Observer] no OnServiceEvent (disconnect, blocked, or no Utility push)");
        } else {
          std::move(callback).Run(
              true, "[Observer] OnServiceEvent: " + observer_message);
        }
      },
      std::move(callback)));

  manager->Ping(base::BindOnce([](const std::string& ping_reply) {
    LOG(INFO) << "Observer test: main Ping finished: " << ping_reply;
  }));
}

void XenonPageHandler::TestDataMask(TestDataMaskCallback callback) {
  LOG(INFO) << "XenonPageHandler: TestDataMask requested";

  if (!web_ui_ || !web_ui_->GetWebContents()) {
    std::move(callback).Run(false, "WebUI context lost");
    return;
  }

  Profile* profile = Profile::FromBrowserContext(
      web_ui_->GetWebContents()->GetBrowserContext());
      
  if (profile) {
    XenonWebDialog::ShowDataMaskTest(profile);
    std::move(callback).Run(true, "Data Mask Test Dialog opened (Baidu)");
  } else {
    std::move(callback).Run(false, "No Profile found");
  }
}

void XenonPageHandler::OpenComponentExtensionDialog(
    OpenComponentExtensionDialogCallback callback) {
  LOG(INFO) << "XenonPageHandler: OpenComponentExtensionDialog requested";

  if (!web_ui_ || !web_ui_->GetWebContents()) {
    std::move(callback).Run(false, "WebUI context lost");
    return;
  }

  Profile* profile = Profile::FromBrowserContext(
      web_ui_->GetWebContents()->GetBrowserContext());
  if (!profile) {
    std::move(callback).Run(false, "No profile");
    return;
  }

  XenonExtensionManager* extension_manager = XenonExtensionManager::GetInstance();
  if (!extension_manager) {
    std::move(callback).Run(false, "XenonExtensionManager not available");
    return;
  }

  if (!extension_manager->ShowExtension(profile)) {
    std::move(callback).Run(
        false,
        "ShowExtension failed — component extension not in registry yet. "
        "Restart without --show-xenon-extension so PostProfileInit loads it, "
        "then open this WebUI again.");
    return;
  }

  std::move(callback).Run(
      true,
      "Opened extension WebDialog (chrome-extension://…/index.html). "
      "Use popup buttons or DevTools → Extensions → service worker for "
      "chrome.xenonPrivate.ping.");
}

void XenonPageHandler::SetAppSessionLoggedIn(bool logged_in) {
  if (!web_ui_) {
    return;
  }
  Profile* profile = Profile::FromWebUI(web_ui_);
  if (!profile) {
    return;
  }
  XenonLoginController::GetInstance()->SetAppSessionLoggedIn(profile,
                                                              logged_in);
}

}  // namespace xenon
