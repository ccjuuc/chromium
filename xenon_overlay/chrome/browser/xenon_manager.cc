#include "xenon_overlay/chrome/browser/xenon_manager.h"

#include <string>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/run_loop.h"
#include "build/buildflag.h"
#include "components/embedder_support/user_agent_utils.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "net/base/filename_util.h"
#include "net/http/http_util.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"
#include "xenon_overlay/chrome/browser/napi/napi_switches.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

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

std::vector<std::string> GetXenonServiceExtraSwitches() {
  std::vector<std::string> switches;
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(napi_switches::kAllowExternalNodeAddons)) {
    switches.push_back(napi_switches::kAllowExternalNodeAddons);
  }
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (command_line->HasSwitch(
          napi_switches::kDisableNodeStaticRegistration)) {
    switches.push_back(napi_switches::kDisableNodeStaticRegistration);
  }
#endif
  return switches;
}

std::vector<std::pair<std::string, std::string>>
GetXenonServiceExtraSwitchValues() {
  std::vector<std::pair<std::string, std::string>> switches;
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  for (const char* name : {ipc::switches::kMainScript,
                           ipc::switches::kElectronApp}) {
    if (command_line->HasSwitch(name)) {
      switches.emplace_back(name,
                            command_line->GetSwitchValuePath(name)
                                .AsUTF8Unsafe());
    }
  }
  return switches;
}

ipc::mojom::IpcResultPtr MakeElectronIpcFailure(const std::string& error) {
  auto result = ipc::mojom::IpcResult::New();
  result->success = false;
  result->error = error;
  return result;
}

}  // namespace

XenonManager* XenonManager::GetInstance() {
  return base::Singleton<XenonManager>::get();
}

XenonManager::XenonManager() = default;

XenonManager::~XenonManager() = default;

void XenonManager::SetBrowserContext(content::BrowserContext* context) {
  if (context) {
    last_browser_context_ = context;
  }
}

void XenonManager::EnsureServiceStarted(content::BrowserContext* context) {
  SetBrowserContext(context);
  if (service_remote_.is_bound()) {
    return;
  }

  mojo::Remote<mojom::XenonMainService> launched =
      content::ServiceProcessHost::Launch<mojom::XenonMainService>(
          content::ServiceProcessHost::Options()
              .WithDisplayName("Xenon Overlay Service")
              .WithExtraCommandLineSwitches(GetXenonServiceExtraSwitches())
              .WithExtraCommandLineSwitchKeyValues(
                  GetXenonServiceExtraSwitchValues())
              .Pass());
  ++service_generation_;

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
  SetupBrowserObserver(&service_remote_, "core");
#endif
}

XenonManager::ContainerServiceConnection*
XenonManager::FindContainerService(const std::string& container_id) {
  const std::string normalized_id =
      container_id.empty() ? "default" : container_id;
  auto it = container_services_.find(normalized_id);
  return it == container_services_.end() ? nullptr : it->second.get();
}

void XenonManager::InitializeServiceConnection(
    ServiceRemote* remote,
    const std::string& service_id) {
  if (!remote || !remote->is_bound()) {
    return;
  }
  if (last_browser_context_) {
    mojo::PendingRemote<network::mojom::URLLoaderFactory> factory_remote;
    last_browser_context_->GetDefaultStoragePartition()
        ->GetURLLoaderFactoryForBrowserProcess()
        ->Clone(factory_remote.InitWithNewPipeAndPassReceiver());
    (*remote)->Initialize(std::move(factory_remote));
  }
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  SetupBrowserObserver(remote, service_id);
#endif
}

XenonManager::ContainerServiceConnection*
XenonManager::EnsureContainerServiceStarted(
    const std::string& container_id) {
  const std::string normalized_id =
      container_id.empty() ? "default" : container_id;
  if (ContainerServiceConnection* existing =
          FindContainerService(normalized_id)) {
    if (existing->remote.is_bound()) {
      return existing;
    }
    container_services_.erase(normalized_id);
  }

  mojo::Remote<mojom::XenonMainService> launched =
      content::ServiceProcessHost::Launch<mojom::XenonMainService>(
          content::ServiceProcessHost::Options()
              .WithDisplayName("Xenon Electron Container " + normalized_id)
              .WithExtraCommandLineSwitches(GetXenonServiceExtraSwitches())
              .WithExtraCommandLineSwitchKeyValues(
                  GetXenonServiceExtraSwitchValues())
              .Pass());

  auto connection = std::make_unique<ContainerServiceConnection>();
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  mojo::PendingRemote<mojom::XenonMainService> pending = launched.Unbind();
  connection->remote.Bind(std::move(pending), GetUiTaskRunner());
  connection->remote.set_disconnect_handler(
      base::BindOnce(&XenonManager::OnContainerServiceDisconnected,
                     base::Unretained(this), normalized_id),
      GetUiTaskRunner());
#else
  connection->remote = std::move(launched);
  connection->remote.set_disconnect_handler(base::BindOnce(
      &XenonManager::OnContainerServiceDisconnected, base::Unretained(this),
      normalized_id));
#endif

  ContainerServiceConnection* result = connection.get();
  container_services_.insert_or_assign(normalized_id, std::move(connection));
  ++container_service_generations_[normalized_id];
  InitializeServiceConnection(&result->remote, normalized_id);

  const auto config = last_ipc_configs_.find(normalized_id);
  if (config != last_ipc_configs_.end() && config->second) {
    result->remote->InitializeElectronIpc(
        config->second.Clone(),
        base::BindOnce(
            [](std::string id, bool success, const std::string& error) {
              if (!success) {
                LOG(ERROR) << "Failed to initialize Electron container '"
                           << id << "': " << error;
                return;
              }
              LOG(INFO) << "Electron container service initialized: " << id;
            },
            normalized_id));
  }
  LOG(INFO) << "Launched isolated Electron container service: "
            << normalized_id;
  return result;
}

uint64_t XenonManager::service_generation(
    const std::string& container_id) const {
  const std::string normalized_id =
      container_id.empty() ? "default" : container_id;
  const auto it = container_service_generations_.find(normalized_id);
  return it == container_service_generations_.end() ? 0 : it->second;
}

#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
mojo::SharedRemote<mojom::XenonMainService>
XenonManager::DuplicateServiceRemote(const std::string& container_id) {
  ContainerServiceConnection* connection =
      EnsureContainerServiceStarted(container_id);
  return connection ? connection->remote
                    : mojo::SharedRemote<mojom::XenonMainService>();
}
#endif

bool XenonManager::RegisterElectronIpc(ipc::mojom::IpcMainConfigPtr config) {
  if (!config) {
    LOG(ERROR) << "Cannot register an empty Electron IPC config";
    return false;
  }
  if (!config->runtime_directory.empty() &&
      !base::FilePath::FromUTF8Unsafe(config->runtime_directory).IsAbsolute()) {
    LOG(ERROR) << "Electron runtime directory must be absolute";
    return false;
  }
  base::FilePath executable_path;
  std::string error;
  if (!ipc::ResolveAppExecutable(
          base::FilePath::FromUTF8Unsafe(config->executable_path),
          &executable_path, &error)) {
    LOG(ERROR) << "Cannot register Electron container: " << error;
    return false;
  }
  config->executable_path = executable_path.AsUTF8Unsafe();
  if (config->app_version.empty()) {
    config->app_version = ipc::GetAppExecutableVersion(executable_path);
  }
  if (config->default_user_agent.empty()) {
    std::string ua = embedder_support::GetUserAgent();
    if (net::HttpUtil::IsToken(config->app_name) &&
        net::HttpUtil::IsToken(config->app_version)) {
      ua = config->app_name + "/" + config->app_version + " " + ua;
    }
    config->default_user_agent = ua;
  }
  const std::string container_id =
      config->container_id.empty() ? "default" : config->container_id;
  last_ipc_configs_.insert_or_assign(container_id, std::move(config));
  LOG(INFO) << "Registered Electron container configuration: " << container_id;
  return true;
}

bool XenonManager::EnsureElectronIpcStarted(const std::string& container_id) {
  const std::string normalized_id =
      container_id.empty() ? "default" : container_id;
  const auto config = last_ipc_configs_.find(normalized_id);
  if (config == last_ipc_configs_.end() || !config->second) {
    LOG(ERROR) << "Cannot start unregistered Electron container: "
               << normalized_id;
    return false;
  }
  return EnsureContainerServiceStarted(normalized_id) != nullptr;
}

void XenonManager::InitializeElectronIpc(ipc::mojom::IpcMainConfigPtr config) {
  if (!config) {
    LOG(ERROR) << "Cannot initialize an empty Electron IPC config";
    return;
  }
  const std::string container_id =
      config->container_id.empty() ? "default" : config->container_id;
  const bool service_already_existed =
      FindContainerService(container_id) != nullptr;
  if (!RegisterElectronIpc(std::move(config))) {
    return;
  }
  ContainerServiceConnection* connection =
      EnsureContainerServiceStarted(container_id);
  // A newly launched connection initializes its saved config as part of the
  // launch. Reinitialize only when the service already existed.
  if (connection && service_already_existed) {
    connection->remote->InitializeElectronIpc(
        last_ipc_configs_.at(container_id).Clone(),
        base::BindOnce(
            [](std::string id, bool success, const std::string& error) {
              if (!success) {
                LOG(ERROR) << "Failed to reinitialize Electron container '"
                           << id << "': " << error;
              }
            },
            container_id));
  }
}

std::string XenonManager::GetDefaultUserAgent(
    const std::string& container_id) const {
  const std::string id = container_id.empty() ? "default" : container_id;
  const auto it = last_ipc_configs_.find(id);
  if (it != last_ipc_configs_.end() && it->second &&
      !it->second->default_user_agent.empty()) {
    return it->second->default_user_agent;
  }
  return embedder_support::GetUserAgent();
}

void XenonManager::SetElectronIpcContainerForOrigin(
    const std::string& origin,
    const std::string& container_id) {
  if (!origin.empty() && !container_id.empty()) {
    electron_ipc_origin_containers_[origin] = container_id;
  }
}

std::string XenonManager::GetElectronIpcContainerForOrigin(
    const std::string& origin) const {
  const auto it = electron_ipc_origin_containers_.find(origin);
  return it == electron_ipc_origin_containers_.end() ? "default"
                                                      : it->second;
}

ipc::mojom::IpcRendererConfigPtr
XenonManager::GetElectronIpcRendererConfigForOrigin(
    const std::string& origin) const {
  return GetElectronIpcRendererConfigForContainer(
      GetElectronIpcContainerForOrigin(origin));
}

ipc::mojom::IpcRendererConfigPtr
XenonManager::GetElectronIpcRendererConfigForContainer(
    const std::string& container_id,
    const std::string& document_url) const {
  const auto config_it = last_ipc_configs_.find(container_id);
  if (config_it == last_ipc_configs_.end() || !config_it->second) {
    return nullptr;
  }

  auto renderer_config = ipc::mojom::IpcRendererConfig::New();
  renderer_config->app_name = config_it->second->app_name;
  renderer_config->app_version = config_it->second->app_version;
  renderer_config->app_path = config_it->second->app_path;
  renderer_config->executable_path = config_it->second->executable_path;
  // file: documents keep their real (possibly ASAR) path. For mapped WebUI
  // documents, reverse the app's declared URL mapping to retain relative
  // CommonJS/preload resolution instead of substituting app.getAppPath().
  const GURL url(document_url);
  base::FilePath document_path;
  if (!net::FileURLToFilePath(url, &document_path)) {
    for (const auto& mapping : config_it->second->renderer_url_mappings) {
      const GURL target(mapping->target_base_url);
      if (!url.is_valid() ||
          url.DeprecatedGetOriginAsURL() != target.DeprecatedGetOriginAsURL() ||
          !url.path().starts_with(target.path())) {
        continue;
      }
      const GURL source(net::FilePathToFileURL(base::FilePath::FromUTF8Unsafe(
                                                   mapping->source_path_prefix))
                            .spec() +
                        "/");
      net::FileURLToFilePath(
          source.Resolve(url.path().substr(target.path().size())),
          &document_path);
      break;
    }
  }
  renderer_config->document_path = document_path.AsUTF8Unsafe();
  if (last_browser_context_) {
    renderer_config->user_data_path =
        last_browser_context_->GetPath().AsUTF8Unsafe();
  }
  auto set_path = [](int key, std::string* output) {
    base::FilePath path;
    if (base::PathService::Get(key, &path)) {
      *output = path.AsUTF8Unsafe();
    }
  };
#if BUILDFLAG(IS_WIN)
  set_path(base::DIR_ROAMING_APP_DATA, &renderer_config->app_data_path);
  set_path(base::DIR_LOCAL_APP_DATA,
           &renderer_config->local_app_data_path);
#endif
  set_path(base::DIR_HOME, &renderer_config->home_path);
  set_path(base::DIR_TEMP, &renderer_config->temp_path);
  return renderer_config;
}

void XenonManager::RegisterElectronIpcRenderer(
    const std::string& container_id,
    const std::string& endpoint_id,
    mojo::PendingRemote<ipc::mojom::IpcRenderer> renderer,
    int32_t process_id,
    int32_t frame_id,
    int32_t window_id) {
  if (ContainerServiceConnection* connection =
          EnsureContainerServiceStarted(container_id)) {
    connection->remote->RegisterElectronIpcRenderer(
        container_id, endpoint_id, std::move(renderer), process_id, frame_id,
        window_id);
  }
}

void XenonManager::RemoveElectronIpcRenderer(
    const std::string& container_id,
    const std::string& endpoint_id) {
  if (ContainerServiceConnection* connection =
          FindContainerService(container_id)) {
    connection->remote->RemoveElectronIpcRenderer(container_id, endpoint_id);
  }
}

void XenonManager::BindNodeAddonHost(
    const std::string& container_id,
    mojo::PendingReceiver<ipc::mojom::NodeAddonHost> receiver) {
  if (ContainerServiceConnection* connection =
          EnsureContainerServiceStarted(container_id)) {
    connection->remote->BindNodeAddonHost(container_id, std::move(receiver));
  }
}

void XenonManager::ElectronIpcSend(const std::string& container_id,
                                   const std::string& endpoint_id,
                                   const std::string& channel,
                                   base::Value arguments) {
  if (ContainerServiceConnection* connection =
          EnsureContainerServiceStarted(container_id)) {
    connection->remote->ElectronIpcSend(container_id, endpoint_id, channel,
                                        std::move(arguments));
  }
}

void XenonManager::ElectronIpcInvoke(
    const std::string& container_id,
    const std::string& endpoint_id,
    const std::string& channel,
    base::Value arguments,
    ElectronIpcInvokeCallback callback) {
  ContainerServiceConnection* connection =
      EnsureContainerServiceStarted(container_id);
  if (!connection) {
    std::move(callback).Run(
        MakeElectronIpcFailure("Utility ipcMain service is unavailable"));
    return;
  }
  connection->remote->ElectronIpcInvoke(container_id, endpoint_id, channel,
                                        std::move(arguments),
                                        std::move(callback));
}

void XenonManager::ElectronIpcSendSync(
    const std::string& container_id,
    const std::string& endpoint_id,
    const std::string& channel,
    base::Value arguments,
    ElectronIpcSendSyncCallback callback) {
  ContainerServiceConnection* connection =
      EnsureContainerServiceStarted(container_id);
  if (!connection) {
    std::move(callback).Run(
        MakeElectronIpcFailure("Utility ipcMain service is unavailable"));
    return;
  }
  connection->remote->ElectronIpcSendSync(
      container_id, endpoint_id, channel, std::move(arguments),
      std::move(callback));
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
void XenonManager::SetupBrowserObserver(ServiceRemote* remote,
                                        const std::string& service_id) {
  if (!remote || !remote->is_bound()) {
    return;
  }
  mojo::PendingRemote<mojom::XenonBrowserObserver> pending_remote;
  mojo::PendingReceiver<mojom::XenonBrowserObserver> pending_receiver =
      pending_remote.InitWithNewPipeAndPassReceiver();
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  browser_observer_receivers_.Add(this, std::move(pending_receiver),
                                  service_id, GetUiTaskRunner());
#else
  browser_observer_receivers_.Add(this, std::move(pending_receiver),
                                  service_id);
#endif
  (*remote)->SetBrowserObserver(std::move(pending_remote));
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

void XenonManager::OnContainerServiceDisconnected(
    const std::string& container_id) {
  LOG(ERROR) << "Electron container service disconnected / crashed: "
             << container_id;
  container_services_.erase(container_id);
}

void XenonManager::ShutdownForProcessExit() {
  container_services_.clear();
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  browser_observer_receivers_.Clear();
#endif
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

void XenonManager::RegisterNodeObserver(XenonNodeObserver* observer) {
  node_observer_ = observer;
}

void XenonManager::UnregisterNodeObserver(XenonNodeObserver* observer) {
  if (node_observer_ == observer) {
    node_observer_ = nullptr;
  }
}

#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
void XenonManager::OnThreadCallback(const std::string& message) {
  if (node_observer_) {
    node_observer_->OnThreadCallback(message);
  }
}

void XenonManager::CreateElectronWindow(int32_t width,
                                        int32_t height,
                                        bool show,
                                        bool frame,
                                        bool transparent,
                                        int32_t parent_id,
                                        const std::string& title,
                                        const std::string& container_id,
                                        CreateElectronWindowCallback callback) {
  int32_t window_id = 0;
  uint64_t hwnd = 0;
  std::string error;
  if (!XenonElectronWindowHost::GetInstance()->CreateHostedWindow(
          last_browser_context_, width, height, show, frame, transparent,
          parent_id, title, container_id, &window_id, &hwnd, &error) &&
      error.empty()) {
    error = "Failed to create Electron BrowserWindow";
  }
  std::move(callback).Run(window_id, hwnd, error);
}

void XenonManager::LoadElectronWindowURL(int32_t window_id,
                                         const std::string& url) {
  XenonElectronWindowHost::GetInstance()->LoadURL(window_id, url);
}

void XenonManager::SetElectronWindowVisible(int32_t window_id, bool visible) {
  XenonElectronWindowHost::GetInstance()->SetVisible(window_id, visible);
}

void XenonManager::ElectronWindowCall(int32_t window_id,
                                      const std::string& command,
                                      base::Value arguments,
                                      ElectronWindowCallCallback callback) {
  base::Value result;
  std::string error;
  XenonElectronWindowHost::GetInstance()->Call(
      window_id, command, arguments, &result, &error);
  std::move(callback).Run(std::move(result), error);
}

void XenonManager::CloseElectronWindow(int32_t window_id) {
  XenonElectronWindowHost::GetInstance()->Close(window_id);
}

void XenonManager::ShowElectronOpenDialog(
    const std::string& title,
    bool directory,
    bool allow_multi,
    const std::vector<std::string>& extensions,
    ShowElectronOpenDialogCallback callback) {
  // This handler runs inside a sync Utility→Browser Mojo call. Showing a
  // modal picker on that stack never paints (no nested UI pump). Post the
  // dialog onto a nestable UI task, then spin until it finishes.
  LOG(INFO) << "Electron showOpenDialog begin title=" << title
            << " directory=" << directory;
  std::vector<std::string> paths;
  base::RunLoop loop(base::RunLoop::Type::kNestableTasksAllowed);
  content::GetUIThreadTaskRunner({})->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](std::string title, bool directory, bool allow_multi,
             std::vector<std::string> extensions, std::vector<std::string>* out,
             base::OnceClosure quit) {
            *out = XenonElectronWindowHost::GetInstance()->ShowOpenDialog(
                title, directory, allow_multi, extensions);
            std::move(quit).Run();
          },
          title, directory, allow_multi, extensions, &paths,
          loop.QuitClosure()));
  loop.Run();
  LOG(INFO) << "Electron showOpenDialog done selected=" << paths.size();
  std::move(callback).Run(std::move(paths));
}
#endif

void XenonManager::DispatchElectronWindowEvent(int32_t window_id,
                                               const std::string& event_name,
                                               base::Value arguments) {
  for (auto& [container_id, connection] : container_services_) {
    if (connection && connection->remote.is_bound()) {
      connection->remote->DispatchElectronWindowEvent(
          window_id, event_name, arguments.Clone());
    }
  }
}

}  // namespace xenon
