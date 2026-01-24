#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_MANAGER_H_

#include "base/memory/singleton.h"
#include "base/memory/weak_ptr.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"

namespace content {
class BrowserContext;
}

namespace xenon {

// Singleton manager for the Xenon Service in the Browser process.
// Handles service launch, initialization, and communication.
class XenonManager {
 public:
  static XenonManager* GetInstance();

  // Ensures the service is launched and initialized.
  // Requires a BrowserContext to obtain the URLLoaderFactory.
  void EnsureServiceStarted(content::BrowserContext* context);

  // Sends a Ping request to the service.
  // The callback receives the response string.
  using PingCallback = base::OnceCallback<void(const std::string&)>;
  void Ping(PingCallback callback);

 private:
  friend struct base::DefaultSingletonTraits<XenonManager>;
  XenonManager();
  ~XenonManager();

  void OnDisconnected();

  [[maybe_unused]] mojo::Remote<mojom::XenonMainService> service_remote_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_MANAGER_H_
