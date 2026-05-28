#include "xenon_overlay/chrome/browser/xenon_manager.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "build/buildflag.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
#include "content/public/browser/browser_thread.h"
#endif

namespace xenon {

namespace {

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
scoped_refptr<base::SequencedTaskRunner> GetUiTaskRunner() {
  return content::GetUIThreadTaskRunner({});
}
#endif

}  // namespace

XenonManager* XenonManager::GetInstance() {
  return base::Singleton<XenonManager>::get();
}

XenonManager::XenonManager() = default;

XenonManager::~XenonManager() = default;

void XenonManager::EnsureServiceStarted(content::BrowserContext* context) {
  if (service_remote_.is_bound()) {
    return;
  }

  mojo::Remote<mojom::XenonMainService> launched =
      content::ServiceProcessHost::Launch<mojom::XenonMainService>(
          content::ServiceProcessHost::Options()
              .WithDisplayName("Xenon Overlay Service")
              .Pass());

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  mojo::PendingRemote<mojom::XenonMainService> pending = launched.Unbind();
  service_remote_.Bind(std::move(pending), GetUiTaskRunner());
  service_remote_.set_disconnect_handler(
      base::BindOnce(&XenonManager::OnDisconnected, base::Unretained(this)),
      GetUiTaskRunner());
#else
  service_remote_ = std::move(launched);
  service_remote_.set_disconnect_handler(
      base::BindOnce(&XenonManager::OnDisconnected, base::Unretained(this)));
#endif

  if (context) {
    mojo::PendingRemote<network::mojom::URLLoaderFactory> factory_remote;
    context->GetDefaultStoragePartition()
        ->GetURLLoaderFactoryForBrowserProcess()
        ->Clone(factory_remote.InitWithNewPipeAndPassReceiver());

    service_remote_->Initialize(std::move(factory_remote));
    LOG(INFO) << "Xenon Service launched and initialized with network access.";
  } else {
    LOG(WARNING) << "Xenon Service launched but NOT initialized (no context).";
  }

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  SetupAssociatedSide();
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  SetupBrowserObserver();
#endif
}

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
void XenonManager::SetupAssociatedSide() {
  if (!service_remote_.is_bound() || associated_side_remote_.is_bound()) {
    return;
  }
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  scoped_refptr<base::SequencedTaskRunner> runner = GetUiTaskRunner();
#else
  scoped_refptr<base::SequencedTaskRunner> runner = nullptr;
#endif
  service_remote_->BindAssociatedSide(
      associated_side_remote_.BindNewEndpointAndPassReceiver(runner));
}
#endif

#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
void XenonManager::SetupBrowserObserver() {
  if (!service_remote_.is_bound() || browser_observer_receiver_.is_bound()) {
    return;
  }
  mojo::PendingRemote<mojom::XenonBrowserObserver> pending_remote;
  mojo::PendingReceiver<mojom::XenonBrowserObserver> pending_receiver =
      pending_remote.InitWithNewPipeAndPassReceiver();
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  browser_observer_receiver_.Bind(std::move(pending_receiver),
                                  GetUiTaskRunner());
#else
  browser_observer_receiver_.Bind(std::move(pending_receiver));
#endif
  service_remote_->SetBrowserObserver(std::move(pending_remote));
}

void XenonManager::CaptureNextObserverEventForTest(
    ObserverEventTestCallback callback) {
  observer_event_test_callback_ = std::move(callback);
}

void XenonManager::OnServiceEvent(const std::string& message) {
  LOG(INFO) << "XenonManager::OnServiceEvent (Utility→Browser): " << message;
  if (observer_event_test_callback_) {
    std::move(observer_event_test_callback_).Run(message);
  }
}
#endif

void XenonManager::ResetServiceConnection() {
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  associated_side_remote_.reset();
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  browser_observer_receiver_.reset();
#endif
  service_remote_.reset();
}

void XenonManager::OnDisconnected() {
  LOG(WARNING) << "Xenon Service disconnected / crashed. Resetting state.";
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  if (observer_event_test_callback_) {
    std::move(observer_event_test_callback_).Run("");
  }
#endif
  ResetServiceConnection();
}

void XenonManager::ShutdownForProcessExit() {
  if (!service_remote_.is_bound()) {
    return;
  }
  ResetServiceConnection();
}

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
void XenonManager::PingAssociated(PingCallback callback) {
  if (!associated_side_remote_.is_bound()) {
    std::move(callback).Run("Error: Xenon associated side not bound.");
    return;
  }
  associated_side_remote_->PingAssociated(std::move(callback));
}
#endif

void XenonManager::Ping(PingCallback callback) {
  if (!service_remote_.is_bound()) {
    std::move(callback).Run("Error: Service not running.");
    return;
  }
  service_remote_->Ping(std::move(callback));
}

}  // namespace xenon
