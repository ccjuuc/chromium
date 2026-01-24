#ifndef XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_
#define XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_

#include "mojo/public/cpp/bindings/receiver.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"

namespace xenon {

class XenonServiceImpl : public mojom::XenonMainService {
 public:
  explicit XenonServiceImpl(mojo::PendingReceiver<mojom::XenonMainService> receiver);
  ~XenonServiceImpl() override;

  // mojom::XenonMainService:
  void Initialize(
      mojo::PendingRemote<network::mojom::URLLoaderFactory> url_loader_factory) override;
  void Ping(PingCallback callback) override;

 private:
  mojo::Receiver<mojom::XenonMainService> receiver_;
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_
