#ifndef XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_
#define XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/memory/weak_ptr.h"
#include "base/values.h"
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

class XenonNodeExecutor;

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
  void SetNodeAddonObserver(
      int32_t client_id,
      mojo::PendingRemote<mojom::NodeAddonObserver> observer) override;

  // N-API / Node Addon testing methods
  void LoadAddon(const std::string& path, LoadAddonCallback callback) override;
  void InspectExport(const std::string& module_path,
                     const std::string& export_path,
                     InspectExportCallback callback) override;
  void ConstructExport(int32_t client_id,
                       const std::string& module_path,
                       const std::string& export_path,
                       std::vector<mojom::NodeInvokeArgPtr> args,
                       ConstructExportCallback callback) override;
  void InvokeInstance(int32_t client_id,
                      const std::string& module_path,
                      int32_t instance_id,
                      const std::string& method_name,
                      std::vector<mojom::NodeInvokeArgPtr> args,
                      InvokeInstanceCallback callback) override;
  void GetInstanceProperty(const std::string& module_path,
                           int32_t instance_id,
                           const std::string& property_name,
                           GetInstancePropertyCallback callback) override;
  void SetInstanceProperty(const std::string& module_path,
                           int32_t instance_id,
                           const std::string& property_name,
                           base::Value value,
                           SetInstancePropertyCallback callback) override;
  void ReleaseInstance(const std::string& module_path,
                       int32_t instance_id) override;
  void InvokeFunction(int32_t client_id,
                      const std::string& module_path,
                      const std::string& function_name,
                      std::vector<mojom::NodeInvokeArgPtr> args,
                      InvokeFunctionCallback callback) override;
  void GetExportProperty(const std::string& module_path,
                         const std::string& object_path,
                         const std::string& property_name,
                         GetExportPropertyCallback callback) override;
  void SetExportProperty(const std::string& module_path,
                         const std::string& object_path,
                         const std::string& property_name,
                         base::Value value,
                         SetExportPropertyCallback callback) override;
  void InvokeMany(const std::string& module_path,
                  std::vector<mojom::NodeInvokeCallPtr> calls,
                  InvokeManyCallback callback) override;

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  void PingAssociated(PingAssociatedCallback callback) override;
#endif

 private:
  XenonNodeExecutor* EnsureNodeExecutor();
  void OnNodeCallback(int32_t client_id,
                      int32_t callback_id,
                      std::vector<base::Value> args);
  void OnNodeCallbackReleased(int32_t client_id, int32_t callback_id);
  void OnNodeAddonObserverDisconnected(int32_t client_id);

  mojo::Receiver<mojom::XenonMainService> receiver_;
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  mojo::AssociatedReceiver<mojom::XenonAssociatedSide> associated_receiver_{this};
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  mojo::Remote<mojom::XenonBrowserObserver> browser_observer_;
#endif
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  // Dedicated executor for running addon code in utility process
  std::unique_ptr<XenonNodeExecutor> node_executor_;
  std::map<int32_t, mojo::Remote<mojom::NodeAddonObserver>>
      node_addon_observers_;

  base::WeakPtrFactory<XenonServiceImpl> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_
