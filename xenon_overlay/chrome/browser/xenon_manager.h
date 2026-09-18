#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_MANAGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_MANAGER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "base/functional/callback.h"
#include "base/gtest_prod_util.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/singleton.h"
#include "base/values.h"
#include "build/buildflag.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"

class GURL;

namespace url {
class Origin;
}

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
#include "mojo/public/cpp/bindings/associated_remote.h"
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
#include "mojo/public/cpp/bindings/receiver_set.h"
#endif

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
#include "mojo/public/cpp/bindings/shared_remote.h"
#else
#include "mojo/public/cpp/bindings/remote.h"
#endif

namespace content {
class BrowserContext;
}  // namespace content

namespace xenon {

FORWARD_DECLARE_TEST(XenonManagerTest, Ping_WhenDisconnected_ReturnsNotRunning);
FORWARD_DECLARE_TEST(XenonManagerTest,
                     DisconnectedContainerRejectsRendererTraffic);
FORWARD_DECLARE_TEST(XenonManagerTest,
                     StaleDisconnectPreservesRestartedContainer);
FORWARD_DECLARE_TEST(XenonManagerTest,
                     RendererAddonDisconnectDoesNotRestartOrCloseApp);
FORWARD_DECLARE_TEST(XenonManagerTest,
                     NativeInvokesUseDocumentServiceWhileAppIpcStaysInMain);
FORWARD_DECLARE_TEST(XenonManagerTest,
                     FileRendererPermissionRequiresDeclaredMapping);
FORWARD_DECLARE_TEST(XenonManagerTest,
                     ParentWindowPairingRequiresExactDeclaredURL);
FORWARD_DECLARE_TEST(XenonManagerTest,
                     RendererAddonRemovalAndAppExitCloseOnlyOwnedServices);
FORWARD_DECLARE_TEST(XenonManagerTest,
                     ResetClosesRendererAddonServicesWithoutRevivingDocuments);
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
FORWARD_DECLARE_TEST(XenonManagerTest,
                     AppExitDisconnectsAllCopiesAndPreservesConfiguration);
FORWARD_DECLARE_TEST(XenonManagerTest, AppExitRejectsOtherAndStaleObservers);
#endif
FORWARD_DECLARE_TEST(XenonManagerTest,
                     RuntimeMetadataIsCapturedOnceBeforeRendererRequests);
FORWARD_DECLARE_TEST(
    XenonManagerTest,
    RendererMetadataDoesNotReadWorkingDirectoryBeforeInitialization);
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
FORWARD_DECLARE_TEST(XenonManagerTest,
                     PingAssociated_WhenDisconnected_ReturnsError);
#endif

class XenonNodeObserver {
 public:
  virtual void OnThreadCallback(const std::string& message) = 0;
};

// Singleton manager for the Xenon Service in the Browser process.
//
// Optional pieces (GN in xenon_overlay/buildflags/features.gni):
// - enable_xenon_manager_shared_remote — SharedRemote vs Remote for
// XenonMainService.
// - enable_xenon_associated_side — AssociatedRemote for XenonAssociatedSide +
// PingAssociated.
// - enable_xenon_browser_observer — XenonBrowserObserver receiver +
// SetBrowserObserver.
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
class XenonManager : public mojom::XenonBrowserObserver {
#else
class XenonManager {
#endif
 public:
  static XenonManager* GetInstance();

  // Called during early browser startup while blocking is still allowed.
  // Renderer configuration requests only copy the captured host metadata.
  void InitializeRuntimeMetadata();

  // Remembers the BrowserContext used by lazily launched services without
  // starting a Utility process.
  void SetBrowserContext(content::BrowserContext* context);
  void EnsureServiceStarted(content::BrowserContext* context);
  uint64_t service_generation() const { return service_generation_; }
  uint64_t service_generation(const std::string& container_id) const;

  void RegisterNodeObserver(XenonNodeObserver* observer);
  void UnregisterNodeObserver(XenonNodeObserver* observer);

  using PingCallback = base::OnceCallback<void(const std::string&)>;
  void Ping(PingCallback callback);

  using ElectronIpcInvokeCallback =
      base::OnceCallback<void(ipc::mojom::IpcResultPtr)>;
  using ElectronIpcSendSyncCallback =
      base::OnceCallback<void(ipc::mojom::IpcResultPtr)>;
  // Validates and stores a container configuration without launching its
  // Utility process or executing the Electron main module.
  bool RegisterElectronIpc(ipc::mojom::IpcMainConfigPtr config);
  // Starts a previously registered container. Repeated calls are idempotent.
  bool EnsureElectronIpcStarted(const std::string& container_id);
  // Delivers an explicit host activation to the already started application.
  // A hidden or absent BrowserWindow does not imply the main module must rerun.
  void ActivateElectronIpc(const std::string& container_id,
                           bool has_visible_windows);
  // Registers and immediately starts (or reinitializes) a container.
  void InitializeElectronIpc(ipc::mojom::IpcMainConfigPtr config);
  std::string GetDefaultUserAgent(const std::string& container_id) const;
  // These predicates describe an application's explicitly declared file to
  // WebUI mappings. Callers must separately verify the actual window owner.
  bool IsDeclaredFileRendererURL(const std::string& container_id,
                                 const GURL& document_url) const;
  bool IsDeclaredFileRendererOrigin(const std::string& container_id,
                                    const url::Origin& origin) const;
  // Matches only an explicitly configured final document URL, ignoring query
  // and fragment. The caller must verify a parent in the same container exists.
  bool ShouldPairElectronWindowWithParent(const std::string& container_id,
                                          const GURL& document_url) const;
  void SetElectronIpcContainerForOrigin(const std::string& origin,
                                        const std::string& container_id);
  std::string GetElectronIpcContainerForOrigin(const std::string& origin) const;
  ipc::mojom::IpcRendererConfigPtr GetElectronIpcRendererConfigForOrigin(
      const std::string& origin) const;
  ipc::mojom::IpcRendererConfigPtr GetElectronIpcRendererConfigForContainer(
      const std::string& container_id,
      const std::string& document_url = std::string()) const;
  void RegisterElectronIpcRenderer(
      const std::string& container_id,
      const std::string& endpoint_id,
      mojo::PendingRemote<ipc::mojom::IpcRenderer> renderer,
      int32_t process_id,
      int32_t frame_id,
      int32_t window_id);
  void RemoveElectronIpcRenderer(const std::string& container_id,
                                 const std::string& endpoint_id);
  void BindNodeAddonHost(
      const std::string& container_id,
      const std::string& endpoint_id,
      mojo::PendingReceiver<ipc::mojom::NodeAddonHost> receiver,
      mojo::PendingRemote<ipc::mojom::IpcRenderer> callback_renderer);
  void ElectronIpcSend(const std::string& container_id,
                       const std::string& endpoint_id,
                       const std::string& channel,
                       base::Value arguments);
  void ElectronIpcInvoke(const std::string& container_id,
                         const std::string& endpoint_id,
                         const std::string& channel,
                         base::Value arguments,
                         ElectronIpcInvokeCallback callback);
  void ElectronIpcSendSync(const std::string& container_id,
                           const std::string& endpoint_id,
                           const std::string& channel,
                           base::Value arguments,
                           ElectronIpcSendSyncCallback callback);
  void DispatchElectronWindowEvent(int32_t window_id,
                                   const std::string& event_name,
                                   base::Value arguments);

  // Drops the Utility process connection during browser shutdown (login gate
  // dismiss before any Browser window exists).
  void ShutdownForProcessExit();

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  void PingAssociated(PingCallback callback);
#endif

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  mojo::SharedRemote<mojom::XenonMainService> DuplicateServiceRemote() const {
    return service_remote_;
  }
  mojo::SharedRemote<mojom::XenonMainService> DuplicateServiceRemote(
      const std::string& container_id);
#endif

#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  using ObserverEventTestCallback =
      base::OnceCallback<void(const std::string& message)>;

  void OnServiceEvent(const std::string& message) override;
  void OnElectronAppExit(const std::string& container_id,
                         int32_t exit_code) override;
  void OnThreadCallback(const std::string& message) override;
  void CreateElectronWindow(int32_t width,
                            int32_t height,
                            bool show,
                            bool frame,
                            bool transparent,
                            int32_t parent_id,
                            const std::string& title,
                            const std::string& container_id,
                            CreateElectronWindowCallback callback) override;
  void LoadElectronWindowURL(int32_t window_id,
                             const std::string& url) override;
  void SetElectronWindowVisible(int32_t window_id, bool visible) override;
  void ElectronWindowCall(int32_t window_id,
                          const std::string& command,
                          base::Value arguments,
                          ElectronWindowCallCallback callback) override;
  void CloseElectronWindow(int32_t window_id) override;
  void ShowElectronOpenDialog(const std::string& title,
                              bool directory,
                              bool allow_multi,
                              const std::vector<std::string>& extensions,
                              ShowElectronOpenDialogCallback callback) override;

  // WebUI: wait for the next Utility→Browser `OnServiceEvent` payload (tests
  // XenonBrowserObserver / SetBrowserObserver). Empty `message` means timeout,
  // disconnect, or no observer build.
  void CaptureNextObserverEventForTest(ObserverEventTestCallback callback);
#endif

 private:
  friend struct base::DefaultSingletonTraits<XenonManager>;
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest,
                           Ping_WhenDisconnected_ReturnsNotRunning);
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest,
                           DisconnectedContainerRejectsRendererTraffic);
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest,
                           StaleDisconnectPreservesRestartedContainer);
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest,
                           RendererAddonDisconnectDoesNotRestartOrCloseApp);
  FRIEND_TEST_ALL_PREFIXES(
      XenonManagerTest,
      NativeInvokesUseDocumentServiceWhileAppIpcStaysInMain);
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest,
                           FileRendererPermissionRequiresDeclaredMapping);
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest,
                           ParentWindowPairingRequiresExactDeclaredURL);
  FRIEND_TEST_ALL_PREFIXES(
      XenonManagerTest,
      RendererAddonRemovalAndAppExitCloseOnlyOwnedServices);
  FRIEND_TEST_ALL_PREFIXES(
      XenonManagerTest,
      ResetClosesRendererAddonServicesWithoutRevivingDocuments);
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  FRIEND_TEST_ALL_PREFIXES(
      XenonManagerTest,
      AppExitDisconnectsAllCopiesAndPreservesConfiguration);
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest,
                           AppExitRejectsOtherAndStaleObservers);
#endif
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest,
                           RuntimeMetadataIsCapturedOnceBeforeRendererRequests);
  FRIEND_TEST_ALL_PREFIXES(
      XenonManagerTest,
      RendererMetadataDoesNotReadWorkingDirectoryBeforeInitialization);
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  FRIEND_TEST_ALL_PREFIXES(XenonManagerTest,
                           PingAssociated_WhenDisconnected_ReturnsError);
#endif

  XenonManager();
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  ~XenonManager() override;
#else
  ~XenonManager();
#endif

  void OnDisconnected();
  void OnContainerServiceDisconnected(const std::string& container_id,
                                      uint64_t generation);
  void CloseContainerService(const std::string& container_id);
  void ResetServiceConnection();

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  using ServiceRemote = mojo::SharedRemote<mojom::XenonMainService>;
#else
  using ServiceRemote = mojo::Remote<mojom::XenonMainService>;
#endif
  struct ContainerServiceConnection {
    ServiceRemote remote;
    uint64_t generation = 0;
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
    mojo::ReceiverId observer_receiver_id = 0;
#endif
  };
  using RendererAddonKey = std::pair<std::string, std::string>;
  struct RendererAddonServiceConnection {
    ServiceRemote remote;
    uint64_t container_generation = 0;
    // A nonzero generation survives a crash until the Document is removed.
    // Its old native proxies must never reconnect to a new addon process.
    uint64_t generation = 0;
  };
  RendererAddonServiceConnection* FindRendererAddonService(
      const RendererAddonKey& key);
  RendererAddonServiceConnection* EnsureRendererAddonServiceStarted(
      const RendererAddonKey& key);
  void OnRendererAddonServiceDisconnected(const RendererAddonKey& key,
                                          uint64_t generation);
  void CloseRendererAddonService(RendererAddonServiceConnection* connection);
  void CloseRendererAddonServicesForContainer(const std::string& container_id);
  ContainerServiceConnection* EnsureContainerServiceStarted(
      const std::string& container_id,
      bool explicit_restart = false);
  ContainerServiceConnection* FindContainerService(
      const std::string& container_id);
  void InitializeServiceConnection(ContainerServiceConnection* connection,
                                   const std::string& service_id);
#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  void SetupAssociatedSide();
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  mojo::ReceiverId SetupBrowserObserver(ServiceRemote* remote,
                                        const std::string& service_id);
#endif

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  mojo::SharedRemote<mojom::XenonMainService> service_remote_;
#else
  mojo::Remote<mojom::XenonMainService> service_remote_;
#endif

#if BUILDFLAG(ENABLE_XENON_ASSOCIATED_SIDE)
  mojo::AssociatedRemote<mojom::XenonAssociatedSide> associated_side_remote_;
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  mojo::ReceiverSet<mojom::XenonBrowserObserver, std::string>
      browser_observer_receivers_;
  ObserverEventTestCallback observer_event_test_callback_;
#endif
  raw_ptr<XenonNodeObserver> node_observer_ = nullptr;
  raw_ptr<content::BrowserContext> last_browser_context_ = nullptr;
  bool runtime_metadata_initialized_ = false;
  std::string startup_working_directory_;
  std::map<std::string, ipc::mojom::IpcMainConfigPtr> last_ipc_configs_;
  std::map<std::string, std::string> electron_ipc_origin_containers_;
  std::map<std::string, std::unique_ptr<ContainerServiceConnection>>
      container_services_;
  std::map<std::string, uint64_t> container_service_generations_;
  std::map<RendererAddonKey, std::unique_ptr<RendererAddonServiceConnection>>
      renderer_addon_services_;
  uint64_t renderer_addon_service_generation_ = 0;
  uint64_t service_generation_ = 0;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_MANAGER_H_
