#ifndef XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_
#define XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "build/buildflag.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
#include "mojo/public/cpp/bindings/associated_receiver.h"
#endif

namespace xenon {

class XenonNodeExecutor;
class XenonNetPipeBridge;
namespace ipc {
class XenonIpcMainContainer;
}

class XenonServiceImpl : public mojom::XenonMainService,
                         public ipc::mojom::NodeAddonHost
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
      const std::string& context_id,
      int32_t client_id,
      mojo::PendingRemote<mojom::NodeAddonObserver> observer) override;
  void BindNodeAddonHost(
      const std::string& context_id,
      mojo::PendingReceiver<ipc::mojom::NodeAddonHost> receiver) override;
  void DispatchElectronWindowEvent(int32_t window_id,
                                   const std::string& event_name,
                                   base::Value arguments) override;
  void InitializeElectronIpc(
      ipc::mojom::IpcMainConfigPtr config,
      InitializeElectronIpcCallback callback) override;
  void RegisterElectronIpcRenderer(
      const std::string& container_id,
      const std::string& endpoint_id,
      mojo::PendingRemote<ipc::mojom::IpcRenderer> renderer,
      int32_t process_id,
      int32_t frame_id,
      int32_t window_id) override;
  void RemoveElectronIpcRenderer(const std::string& container_id,
                                 const std::string& endpoint_id) override;
  void ElectronIpcSend(const std::string& container_id,
                       const std::string& endpoint_id,
                       const std::string& channel,
                       base::Value arguments) override;
  void ElectronIpcInvoke(const std::string& container_id,
                         const std::string& endpoint_id,
                         const std::string& channel,
                         base::Value arguments,
                         ElectronIpcInvokeCallback callback) override;
  void ElectronIpcSendSync(const std::string& container_id,
                           const std::string& endpoint_id,
                           const std::string& channel,
                           base::Value arguments,
                           ElectronIpcSendSyncCallback callback) override;

  // N-API / Node Addon testing methods
  void LoadAddon(const std::string& context_id,
                 const std::string& path,
                 LoadAddonCallback callback) override;
  void InspectExport(const std::string& context_id,
                     const std::string& module_path,
                     const std::string& export_path,
                     InspectExportCallback callback) override;
  void ConstructExport(const std::string& context_id,
                       int32_t client_id,
                       const std::string& module_path,
                       const std::string& export_path,
                       std::vector<mojom::NodeInvokeArgPtr> args,
                       ConstructExportCallback callback) override;
  void InvokeInstance(const std::string& context_id,
                      int32_t client_id,
                      const std::string& module_path,
                      int32_t instance_id,
                      const std::string& method_name,
                      std::vector<mojom::NodeInvokeArgPtr> args,
                      InvokeInstanceCallback callback) override;
  void GetInstanceProperty(const std::string& context_id,
                           const std::string& module_path,
                           int32_t instance_id,
                           const std::string& property_name,
                           GetInstancePropertyCallback callback) override;
  void SetInstanceProperty(const std::string& context_id,
                           const std::string& module_path,
                           int32_t instance_id,
                           const std::string& property_name,
                           base::Value value,
                           SetInstancePropertyCallback callback) override;
  void ReleaseInstance(const std::string& context_id,
                       const std::string& module_path,
                       int32_t instance_id) override;
  void InvokeFunction(const std::string& context_id,
                      int32_t client_id,
                      const std::string& module_path,
                      const std::string& function_name,
                      std::vector<mojom::NodeInvokeArgPtr> args,
                      InvokeFunctionCallback callback) override;
  void GetExportProperty(const std::string& context_id,
                         const std::string& module_path,
                         const std::string& object_path,
                         const std::string& property_name,
                         GetExportPropertyCallback callback) override;
  void SetExportProperty(const std::string& context_id,
                         const std::string& module_path,
                         const std::string& object_path,
                         const std::string& property_name,
                         base::Value value,
                         SetExportPropertyCallback callback) override;
  void InvokeMany(const std::string& context_id,
                  const std::string& module_path,
                  std::vector<mojom::NodeInvokeCallPtr> calls,
                  InvokeManyCallback callback) override;

  // ipc::mojom::NodeAddonHost. These receivers connect renderers directly to
  // this Utility sequence, keeping Browser UI out of synchronous N-API calls.
  void RequireNodeModuleSync(
      const std::string& module_path,
      RequireNodeModuleSyncCallback callback) override;
  void InvokeNodeExportSync(
      const std::string& module_path,
      const std::string& function_name,
      base::Value arguments,
      InvokeNodeExportSyncCallback callback) override;
  void ConstructNodeExportSync(
      const std::string& module_path,
      const std::string& export_path,
      base::Value arguments,
      ConstructNodeExportSyncCallback callback) override;
  void InvokeNodeInstanceSync(
      const std::string& module_path,
      int32_t instance_id,
      const std::string& method_name,
      base::Value arguments,
      InvokeNodeInstanceSyncCallback callback) override;
  void InspectNodeInstanceMemberSync(
      const std::string& module_path,
      int32_t instance_id,
      const std::string& property_name,
      InspectNodeInstanceMemberSyncCallback callback) override;

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  void PingAssociated(PingAssociatedCallback callback) override;
#endif

 private:
  struct PendingIpcSend {
    std::string endpoint_id;
    std::string channel;
    base::Value arguments;
  };
  struct PendingIpcInvoke {
    std::string endpoint_id;
    std::string channel;
    base::Value arguments;
    ElectronIpcInvokeCallback callback;
  };
  using PendingIpcCall = std::variant<PendingIpcSend, PendingIpcInvoke>;

  void MarkElectronIpcReady(const std::string& container_id);
  void FlushPendingElectronIpc(const std::string& container_id);
  bool IsElectronIpcReady(const std::string& container_id) const;
  using NodeClientKey = std::pair<std::string, int32_t>;
  using RendererEndpointKey = std::pair<std::string, std::string>;

  XenonNodeExecutor* EnsureNodeExecutor(const std::string& context_id);
  XenonNodeExecutor* GetNodeExecutor(const std::string& context_id);
  void EnsureAddonLoaded(
      const std::string& context_id,
      const std::string& path,
      base::OnceCallback<void(bool success, const std::string& error)> done);
  void OnNodeCallback(const std::string& context_id,
                      int32_t client_id,
                      int32_t callback_id,
                      std::vector<base::Value> args);
  void OnNodeCallbackReleased(const std::string& context_id,
                              int32_t client_id,
                              int32_t callback_id);
  void OnNodeAddonObserverDisconnected(const std::string& context_id,
                                       int32_t client_id);
  void FlushPendingNodeCallbacks(const std::string& context_id,
                                 int32_t client_id);
  void HandleRendererNodeAddonInvoke(
      const std::string& context_id,
      const std::string& endpoint_id,
      const std::string& channel,
      base::Value arguments,
      ElectronIpcInvokeCallback callback);
  int32_t GetOrCreateRendererNodeClient(const std::string& context_id,
                                        const std::string& endpoint_id);
  void RemoveRendererNodeClient(const std::string& context_id,
                                const std::string& endpoint_id);
  XenonNetPipeBridge* EnsureNetPipeBridge();
  bool HandleNetPipeMessage(const std::string& container_id,
                            const std::string& endpoint_id,
                            const std::string& channel,
                            const base::Value& arguments);
  void DispatchNetPipeEvent(const std::string& container_id,
                            const std::string& endpoint_id,
                            const std::string& channel,
                            base::Value payload);

  mojo::Receiver<mojom::XenonMainService> receiver_;
  mojo::ReceiverSet<ipc::mojom::NodeAddonHost, std::string>
      node_addon_host_receivers_;
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  mojo::AssociatedReceiver<mojom::XenonAssociatedSide> associated_receiver_{this};
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  mojo::Remote<mojom::XenonBrowserObserver> browser_observer_;
#endif
  scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory_;

  // Each Electron container owns an independent N-API isolate and module /
  // instance cache. Declared before ipcMain containers so the executors are
  // destroyed last (containers may still hold Unlocker callbacks during
  // teardown).
  std::map<std::string, std::unique_ptr<XenonNodeExecutor>> node_executors_;
  std::unique_ptr<XenonNetPipeBridge> net_pipe_bridge_;
  std::map<std::string, std::unique_ptr<ipc::XenonIpcMainContainer>>
      ipc_main_containers_;
  std::set<std::string> ipc_main_ready_containers_;
  std::map<std::string, std::deque<PendingIpcCall>> pending_ipc_calls_;
  std::map<NodeClientKey, mojo::Remote<mojom::NodeAddonObserver>>
      node_addon_observers_;
  std::map<NodeClientKey,
           std::deque<std::pair<int32_t, std::vector<base::Value>>>>
      pending_node_callbacks_;
  // Renderer-native callback scopes use negative ids so they cannot collide
  // with the positive ids allocated by WebUI Node controllers.
  std::map<RendererEndpointKey, int32_t> renderer_node_clients_;
  std::map<NodeClientKey, std::string> renderer_node_endpoints_;
  int32_t next_renderer_node_client_id_ = -1;

  base::WeakPtrFactory<XenonServiceImpl> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_SERVICES_XENON_SERVICE_IMPL_H_
