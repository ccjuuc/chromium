#include "xenon_overlay/chrome/browser/xenon_manager.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"

namespace xenon {

// static
XenonManager* XenonManager::GetInstance() {
  return base::Singleton<XenonManager>::get();
}

XenonManager::XenonManager() = default;

XenonManager::~XenonManager() = default;

void XenonManager::EnsureServiceStarted(content::BrowserContext* context) {
  if (service_remote_.is_bound()) {
    return;
  }

  // Launch the service in the Utility process.
  // content::ServiceProcessHost::Launch ensures the service is started and connected.
  // The service name "XenonMainService" will appear in the Task Manager.
  service_remote_ = content::ServiceProcessHost::Launch<mojom::XenonMainService>(
      content::ServiceProcessHost::Options()
          .WithDisplayName("Xenon Overlay Service")
          .Pass());

  service_remote_.set_disconnect_handler(
      base::BindOnce(&XenonManager::OnDisconnected, base::Unretained(this)));

  // Perform initialization (pass URLLoaderFactory).
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
}

void XenonManager::OnDisconnected() {
  LOG(WARNING) << "Xenon Service disconnected / crashed. Resetting state.";
  service_remote_.reset();
}

void XenonManager::Ping(PingCallback callback) {
  if (!service_remote_.is_bound()) {
    std::move(callback).Run("Error: Service not running.");
    return;
  }
  service_remote_->Ping(std::move(callback));
}

}  // namespace xenon
