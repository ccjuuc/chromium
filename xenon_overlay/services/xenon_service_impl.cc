#include "xenon_overlay/services/xenon_service_impl.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "build/buildflag.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"
#include "xenon_overlay/buildflags/buildflags.h"

namespace xenon {

XenonServiceImpl::XenonServiceImpl(
    mojo::PendingReceiver<mojom::XenonMainService> receiver)
    : receiver_(this, std::move(receiver)) {}

XenonServiceImpl::~XenonServiceImpl() = default;

void XenonServiceImpl::Initialize(
    mojo::PendingRemote<network::mojom::URLLoaderFactory> url_loader_factory) {
  url_loader_factory_ =
      base::MakeRefCounted<network::WrapperSharedURLLoaderFactory>(
          std::move(url_loader_factory));
}

void XenonServiceImpl::SetBrowserObserver(
    mojo::PendingRemote<mojom::XenonBrowserObserver> observer) {
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  browser_observer_.reset();
  if (observer) {
    browser_observer_.Bind(std::move(observer));
  }
#else
  (void)observer;
#endif
}

void XenonServiceImpl::BindAssociatedSide(
    mojo::PendingAssociatedReceiver<mojom::XenonAssociatedSide> receiver) {
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  if (associated_receiver_.is_bound()) {
    associated_receiver_.reset();
  }
  associated_receiver_.Bind(std::move(receiver));
#else
  (void)receiver;
#endif
}

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
void XenonServiceImpl::PingAssociated(PingAssociatedCallback callback) {
  LOG(INFO) << "XenonServiceImpl::PingAssociated received";
  std::move(callback).Run("XenonAssociatedSide ok");
}
#endif

void XenonServiceImpl::Ping(PingCallback callback) {
  LOG(INFO) << "XenonServiceImpl::Ping received";

  if (!url_loader_factory_) {
    std::move(callback).Run("Error: URLLoaderFactory not initialized.");
    return;
  }

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = GURL("https://www.baidu.com");
  resource_request->method = "GET";
  resource_request->credentials_mode = network::mojom::CredentialsMode::kOmit;

  std::unique_ptr<network::SimpleURLLoader> loader =
      network::SimpleURLLoader::Create(std::move(resource_request),
                                       MISSING_TRAFFIC_ANNOTATION);

  auto* loader_ptr = loader.get();
  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(
          [](base::WeakPtr<XenonServiceImpl> self,
             std::unique_ptr<network::SimpleURLLoader> keep_alive_loader,
             PingCallback user_callback,
             std::optional<std::string> response_body) {
            (void)keep_alive_loader;
            if (!self) {
              std::move(user_callback).Run("Error: XenonServiceImpl destroyed.");
              return;
            }
            if (response_body) {
              const std::string message = "Baidu page download success. Size: " +
                                          base::NumberToString(response_body->size());
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
              if (self->browser_observer_.is_connected()) {
                self->browser_observer_->OnServiceEvent(
                    "Utility→Browser: Ping completed, " + message);
              }
#endif
              std::move(user_callback).Run(message);
            } else {
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
              if (self->browser_observer_.is_connected()) {
                self->browser_observer_->OnServiceEvent(
                    "Utility→Browser: Ping failed (no response body)");
              }
#endif
              std::move(user_callback).Run("Baidu page download failed.");
            }
          },
          weak_factory_.GetWeakPtr(), std::move(loader), std::move(callback)),
      1024 * 1024 /* 1MB limit */);
}

}  // namespace xenon
