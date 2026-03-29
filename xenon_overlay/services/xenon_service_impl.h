#ifndef XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_
#define XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_

#include "base/memory/weak_ptr.h"
#include "build/buildflag.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
#include "mojo/public/cpp/bindings/associated_receiver.h"
#endif

namespace xenon {

class XenonServiceImpl : public mojom::XenonMainService
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
    ,
                         public mojom::XenonAssociatedSide
#endif
{
 public:
  explicit XenonServiceImpl(mojo::PendingReceiver<mojom::XenonMainService> receiver);
  ~XenonServiceImpl() override;

  void Initialize(
      mojo::PendingRemote<network::mojom::URLLoaderFactory> url_loader_factory) override;
  void Ping(PingCallback callback) override;
  void BindAssociatedSide(
      mojo::PendingAssociatedReceiver<mojom::XenonAssociatedSide> receiver) override;
  void SetBrowserObserver(
      mojo::PendingRemote<mojom::XenonBrowserObserver> observer) override;

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  void PingAssociated(PingAssociatedCallback callback) override;
#endif

 private:
  mojo::Receiver<mojom::XenonMainService> receiver_;
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  mojo::AssociatedReceiver<mojom::XenonAssociatedSide> associated_receiver_{this};
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  mojo::Remote<mojom::XenonBrowserObserver> browser_observer_;
#endif
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  base::WeakPtrFactory<XenonServiceImpl> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_
