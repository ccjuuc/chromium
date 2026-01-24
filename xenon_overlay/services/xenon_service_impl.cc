#include "xenon_overlay/services/xenon_service_impl.h"

#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"

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
  
  // Use DownloadToString with a reasonable limit (e.g. 1MB)
  loader_ptr->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(
          [](std::unique_ptr<network::SimpleURLLoader> loader,
             PingCallback callback, std::optional<std::string> response_body) {
            if (response_body) {
              std::string message = "Baidu page download success. Size: " +
                                    base::NumberToString(response_body->size());
              std::move(callback).Run(message);
            } else {
              std::move(callback).Run("Baidu page download failed.");
            }
          },
          std::move(loader), std::move(callback)),
      1024 * 1024 /* 1MB limit */);
}

}  // namespace xenon
