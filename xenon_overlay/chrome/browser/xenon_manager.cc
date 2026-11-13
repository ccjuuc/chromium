#include "xenon_overlay/chrome/browser/xenon_manager.h"

#include <string>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/hash/sha1.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/bind_post_task.h"
#include "base/task/thread_pool.h"
#include "build/buildflag.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/simple_message_box.h"
#include "chrome/common/chrome_paths.h"
#include "components/crash/core/app/crashpad.h"
#include "components/embedder_support/user_agent_utils.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/public/common/url_constants.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/filename_util.h"
#include "services/device/public/mojom/wake_lock_provider.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "url/origin.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_command_line.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_launch_arguments.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_single_instance.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_config.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_storage.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_storage_profile.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_desktop_capturer.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_guest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_jump_list.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_login_items.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_net_factory.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_notification.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_broker.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_check_remote.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_power_save_blocker.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_protocol.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_safe_storage.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_sandbox_policy.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_system_languages.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_tracing.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_user_agent.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_web_request.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_utility_process_host.h"
#include "xenon_overlay/chrome/browser/napi/napi_switches.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_dialog.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_view_host.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/ipc/xenon_privileged_schemes.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

#if BUILDFLAG(IS_MAC)
#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_host.h"
#endif

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

std::vector<std::string> GetXenonServiceExtraSwitches(
    bool isolated_app = false) {
  std::vector<std::string> switches;
  if (isolated_app) {
    switches.push_back(ipc::switches::kIsolatedElectronService);
  }
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (command_line->HasSwitch(napi_switches::kAllowExternalNodeAddons)) {
    switches.push_back(napi_switches::kAllowExternalNodeAddons);
  }
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (command_line->HasSwitch(napi_switches::kDisableNodeStaticRegistration)) {
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
  for (const char* name :
       {ipc::switches::kMainScript, ipc::switches::kElectronApp}) {
    if (command_line->HasSwitch(name)) {
      switches.emplace_back(
          name, command_line->GetSwitchValuePath(name).AsUTF8Unsafe());
    }
  }
  // ServiceProcessHost may have its own command-line allowlist. Forward the
  // same frozen snapshot used by all other Chromium children explicitly.
  base::CommandLine snapshot(base::CommandLine::NO_PROGRAM);
  ipc::AppendStartupPrivilegedSchemes(&snapshot);
  for (const auto& [name, value] : snapshot.GetSwitches()) {
    switches.emplace_back(name, snapshot.GetSwitchValueASCII(name));
  }
  return switches;
}

ipc::mojom::IpcResultPtr MakeElectronIpcFailure(const std::string& error) {
  auto result = ipc::mojom::IpcResult::New();
  result->success = false;
  result->error = error;
  return result;
}

std::string SingleInstanceOwner(const std::string& container,
                                uint64_t generation) {
  return container + ":" + base::NumberToString(generation);
}

bool IsRendererNodeAddonInvokeChannel(const std::string& channel) {
  return channel == "__xenon:node-addon:invoke-export" ||
         channel == "__xenon:node-addon:construct-export" ||
         channel == "__xenon:node-addon:invoke-instance";
}

GURL FileRendererMappingTarget(
    const ipc::mojom::IpcRendererUrlMapping& mapping) {
  const base::FilePath source =
      base::FilePath::FromUTF8Unsafe(mapping.source_path_prefix);
  const GURL target(mapping.target_base_url);
  if (!source.IsAbsolute() || source.ReferencesParent() || !target.is_valid() ||
      !target.SchemeIs(content::kChromeUIScheme) || target.has_username() ||
      target.has_password() || target.has_query() || target.has_ref()) {
    return GURL();
  }
  return target;
}

}  // namespace

XenonManager* XenonManager::GetInstance() {
  return base::Singleton<XenonManager>::get();
}

XenonManager::XenonManager() {
  ipc::SetElectronThemeEventCallback(base::BindRepeating(
      [](XenonManager* manager, base::DictValue details) {
        manager->DispatchElectronWindowEvent(0, "__xenon-system-theme",
                                             base::Value(std::move(details)));
      },
      base::Unretained(this)));
  ipc::SetAutoUpdaterEventCallback(base::BindRepeating(
      [](XenonManager* manager, const std::string& event,
         base::ListValue arguments) {
        manager->DispatchElectronWindowEvent(
            0, "auto-updater",
            base::Value(base::DictValue()
                            .Set("event", event)
                            .Set("args", std::move(arguments))));
      },
      base::Unretained(this)));
}

XenonManager::ContainerServiceConnection::ContainerServiceConnection() =
    default;
XenonManager::ContainerServiceConnection::~ContainerServiceConnection() =
    default;

XenonManager::~XenonManager() {
  weak_factory_.InvalidateWeakPtrs();
  single_instance_broker_.Reset();
  ipc::SetAutoUpdaterEventCallback({});
  ipc::SetElectronThemeEventCallback({});
  for (const auto& [container_id, config] : last_ipc_configs_) {
    asar::RemoveArchivePublicKeys("electron:" + container_id);
  }
}

ipc::ElectronSafeStorage* XenonManager::GetElectronSafeStorage() {
  if (!electron_safe_storage_) {
    electron_safe_storage_ = std::make_unique<ipc::ElectronSafeStorage>(
        base::BindRepeating([](ipc::ElectronSafeStorage::EncryptorReady ready) {
          auto* os_crypt =
              g_browser_process ? g_browser_process->os_crypt_async() : nullptr;
          if (os_crypt) {
            os_crypt->GetInstance(std::move(ready));
          } else {
            std::move(ready).Run(nullptr);
          }
        }));
  }
  return electron_safe_storage_.get();
}

ipc::ElectronSandboxPolicy* XenonManager::GetElectronSandboxPolicy() {
  if (!electron_sandbox_policy_) {
    electron_sandbox_policy_ = std::make_unique<ipc::ElectronSandboxPolicy>();
  }
  return electron_sandbox_policy_.get();
}

base::expected<base::DictValue, std::string>
XenonManager::NormalizeElectronWebPreferences(
    const std::string& container_id,
    const base::DictValue& preferences,
    bool hosted) {
  const std::string normalized =
      container_id.empty() ? "default" : container_id;
  auto result = GetElectronSandboxPolicy()->NormalizePreferences(
      SingleInstanceOwner(normalized, service_generation(normalized)),
      preferences, hosted);
  if (result.has_value() && result->FindBool("sandbox").value_or(false)) {
    auto supported = ipc::ElectronSandboxPolicy::CheckHostSandboxCompatibility(
        *base::CommandLine::ForCurrentProcess());
    if (!supported.has_value()) {
      return base::unexpected(supported.error());
    }
  }
  return result;
}

void XenonManager::NoteElectronSandboxDocument(
    const std::string& container_id) {
  const std::string normalized =
      container_id.empty() ? "default" : container_id;
  GetElectronSandboxPolicy()->NoteDocument(
      SingleInstanceOwner(normalized, service_generation(normalized)));
}

bool XenonManager::CanBindLegacyElectronNodeInterface(
    content::WebContents* contents) {
  if (!contents) {
    return false;
  }
  const base::DictValue* preferences = nullptr;
  std::string container;
  if (auto* guest = ipc::XenonElectronGuest::FromWebContents(contents)) {
    container = guest->container_id();
    preferences = &guest->preferences();
  } else {
    auto* windows = XenonElectronWindowHost::GetInstance();
    container = windows->GetContainerIdForWebContents(contents);
    preferences = windows->GetWebPreferencesForWebContents(contents);
  }
  if (container.empty()) {
    container = GetElectronIpcContainerForOrigin(
        url::Origin::Create(contents->GetLastCommittedURL()).Serialize());
  }
  const base::DictValue empty;
  auto effective = NormalizeElectronWebPreferences(
      container, preferences ? *preferences : empty, preferences != nullptr);
  if (!effective.has_value() ||
      effective->FindBool("sandbox").value_or(false)) {
    return false;
  }
  // A pre-existing legacy native channel cannot be retrospectively revoked
  // by app.enableSandbox. Record it before exposing any privileged endpoint.
  NoteElectronSandboxDocument(container);
  return true;
}

void XenonManager::ReleaseElectronPowerAndStorageOwner(
    const std::string& container_id,
    uint64_t generation) {
  const std::string owner = SingleInstanceOwner(container_id, generation);
  if (electron_power_save_blocker_) {
    electron_power_save_blocker_->ReleaseOwner(owner);
  }
  if (electron_safe_storage_) {
    electron_safe_storage_->ReleaseOwner(owner);
  }
  if (electron_sandbox_policy_) {
    electron_sandbox_policy_->ReleaseOwner(owner);
  }
}

void XenonManager::InitializeRuntimeMetadata() {
  if (runtime_metadata_initialized_) {
    return;
  }
  runtime_metadata_initialized_ = true;
  base::FilePath working_directory;
  if (base::GetCurrentDirectory(&working_directory)) {
    startup_working_directory_ = working_directory.AsUTF8Unsafe();
  }
  // Leave failures unavailable. Retrying from a renderer request would block
  // the Browser UI thread and would no longer describe the startup directory.
}

void XenonManager::SetBrowserContext(content::BrowserContext* context) {
  if (!context || context == last_browser_context_) {
    return;
  }
  profile_observation_.Reset();
  last_browser_context_ = context;
  profile_observation_.Observe(Profile::FromBrowserContext(context));
}

void XenonManager::OnProfileWillBeDestroyed(Profile* profile) {
  DCHECK_EQ(last_browser_context_, profile);
  // The manager outlives ProfileManager during browser shutdown. Drop the
  // context before Profile destruction, including when an update quits Chrome.
  last_browser_context_ = nullptr;
  profile_observation_.Reset();
}

void XenonManager::EnsureServiceStarted(content::BrowserContext* context) {
  if (shutting_down_for_process_exit_) {
    return;
  }
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

XenonManager::ContainerServiceConnection* XenonManager::FindContainerService(
    const std::string& container_id) {
  const std::string normalized_id =
      container_id.empty() ? "default" : container_id;
  auto it = container_services_.find(normalized_id);
  return it == container_services_.end() ? nullptr : it->second.get();
}

void XenonManager::InitializeServiceConnection(
    ContainerServiceConnection* connection,
    const std::string& service_id) {
  if (!connection || !connection->remote.is_bound()) {
    return;
  }
  // Fetch the host key asynchronously while the app starts. Legacy synchronous
  // safeStorage calls never spin a nested loop waiting for keychain readiness.
  GetElectronSafeStorage()->Initialize();
  if (connection->browser_context) {
    mojo::PendingRemote<network::mojom::URLLoaderFactory> factory_remote;
    connection->browser_context->GetDefaultStoragePartition()
        ->GetURLLoaderFactoryForBrowserProcess()
        ->Clone(factory_remote.InitWithNewPipeAndPassReceiver());
    connection->remote->Initialize(std::move(factory_remote));
  }
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  connection->observer_receiver_id =
      SetupBrowserObserver(&connection->remote, service_id);
  if (connection->browser_context) {
    const uint64_t generation = connection->generation;
    ipc::BindElectronPermissionOwner(
        connection->browser_context.get(), service_id,
        SingleInstanceOwner(service_id, generation),
        base::BindRepeating(
            [](base::WeakPtr<XenonManager> manager, std::string container,
               uint64_t generation)
                -> mojo::PendingRemote<ipc::mojom::ElectronPermissionCheck> {
              if (!manager || manager->shutting_down_for_process_exit_) {
                return {};
              }
              auto* current = manager->FindContainerService(container);
              if (!current || current->generation != generation ||
                  !current->remote.is_bound()) {
                return {};
              }
              mojo::PendingRemote<ipc::mojom::ElectronPermissionCheck> remote;
              current->remote->BindElectronPermissionCheck(
                  container, remote.InitWithNewPipeAndPassReceiver());
              return remote;
            },
            weak_factory_.GetWeakPtr(), service_id, generation),
        base::BindRepeating([](content::WebContents* contents) {
          if (auto* guest =
                  ipc::XenonElectronGuest::FromWebContents(contents)) {
            return guest->id();
          }
          return XenonElectronWindowHost::GetInstance()
              ->FindWindowIdForWebContents(contents);
        }),
        base::BindRepeating(
            [](base::WeakPtr<XenonManager> manager, std::string container,
               uint64_t generation, base::Value event) {
              if (!manager || manager->shutting_down_for_process_exit_) {
                return;
              }
              auto* current = manager->FindContainerService(container);
              if (current && current->generation == generation &&
                  current->remote.is_bound()) {
                current->remote->DispatchElectronPermissionRequest(
                    container, std::move(event));
              }
            },
            weak_factory_.GetWeakPtr(), service_id, generation));
  }
#endif
}

XenonManager::ContainerServiceConnection*
XenonManager::EnsureContainerServiceStarted(const std::string& container_id,
                                            bool explicit_restart) {
  if (shutting_down_for_process_exit_) {
    return nullptr;
  }
  const std::string normalized_id =
      container_id.empty() ? "default" : container_id;
  if (ContainerServiceConnection* existing =
          FindContainerService(normalized_id)) {
    if (existing->remote.is_bound()) {
      return existing;
    }
    CloseContainerService(normalized_id);
  }

  // Old documents keep sending IPC after a Utility crash. Starting their saved
  // main module again would create another set of windows on every crash. Only
  // an explicit browser action may retry a previously launched container.
  if (!explicit_restart && service_generation(normalized_id) != 0) {
    return nullptr;
  }

  // Queue initialization and subsequent calls on one pipe while userData is
  // prepared off UI. Binding its receiver only afterwards preserves Mojo
  // ordering, including renderer registrations made during startup.
  mojo::Remote<mojom::XenonMainService> launched;
  auto service_receiver = launched.BindNewPipeAndPassReceiver();

  auto connection = std::make_unique<ContainerServiceConnection>();
  const uint64_t generation = ++container_service_generations_[normalized_id];
  connection->generation = generation;
  if (last_browser_context_) {
    connection->browser_context = last_browser_context_->GetWeakPtr();
    const auto config = last_ipc_configs_.find(normalized_id);
    const auto initial_path =
        config != last_ipc_configs_.end() && config->second
            ? base::FilePath::FromUTF8Unsafe(
                  config->second->app_user_data_directory)
            : last_browser_context_->GetPath();
    auto registered = ipc::RegisterElectronAppStorage(
        last_browser_context_, normalized_id,
        SingleInstanceOwner(normalized_id, generation), initial_path,
        base::BindRepeating(&ipc::CreateElectronAppStorageProfile),
        base::BindRepeating(&ipc::DestroyElectronAppStorageProfile));
    if (!registered.has_value()) {
      LOG(ERROR) << "Cannot initialize application storage: "
                 << registered.error();
      return nullptr;
    }
  }
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  mojo::PendingRemote<mojom::XenonMainService> pending = launched.Unbind();
  connection->remote.Bind(std::move(pending), GetUiTaskRunner());
  connection->remote.set_disconnect_handler(
      base::BindOnce(&XenonManager::OnContainerServiceDisconnected,
                     base::Unretained(this), normalized_id, generation),
      GetUiTaskRunner());
#else
  connection->remote = std::move(launched);
  connection->remote.set_disconnect_handler(
      base::BindOnce(&XenonManager::OnContainerServiceDisconnected,
                     base::Unretained(this), normalized_id, generation));
#endif

  ContainerServiceConnection* result = connection.get();
  container_services_.insert_or_assign(normalized_id, std::move(connection));
  InitializeServiceConnection(result, normalized_id);

  const auto config = last_ipc_configs_.find(normalized_id);
  if (config != last_ipc_configs_.end() && config->second) {
    result->remote->InitializeElectronIpc(
        config->second.Clone(),
        base::BindOnce(
            [](std::string id, bool success, const std::string& error) {
              if (!success) {
                LOG(ERROR) << "Failed to initialize Electron container '" << id
                           << "': " << error;
                return;
              }
              LOG(INFO) << "Electron container service initialized: " << id;
            },
            normalized_id));
  }
  auto launch_after_storage = base::BindOnce(
      [](base::WeakPtr<XenonManager> manager, std::string id,
         uint64_t generation,
         mojo::PendingReceiver<mojom::XenonMainService> receiver,
         std::string error) {
        if (!manager || manager->shutting_down_for_process_exit_ ||
            !manager->IsElectronIpcGenerationCurrent(id, generation)) {
          return;
        }
        if (!error.empty()) {
          LOG(ERROR) << "Cannot prepare Electron application userData: "
                     << error;
          manager->CloseContainerService(id);
          return;
        }
        content::ServiceProcessHost::Launch(
            std::move(receiver),
            content::ServiceProcessHost::Options()
                .WithDisplayName("Xenon Electron Container " + id)
                .WithExtraCommandLineSwitches(GetXenonServiceExtraSwitches(true))
                .WithExtraCommandLineSwitchKeyValues(
                    GetXenonServiceExtraSwitchValues())
                .Pass());
        LOG(INFO) << "Launched isolated Electron container service: " << id;
      },
      weak_factory_.GetWeakPtr(), normalized_id, generation,
      std::move(service_receiver));
  if (last_browser_context_) {
    ipc::PrepareElectronAppStorageUserData(
        SingleInstanceOwner(normalized_id, generation),
        std::move(launch_after_storage));
  } else {
    std::move(launch_after_storage).Run("");
  }
  return result;
}

uint64_t XenonManager::service_generation(
    const std::string& container_id) const {
  const std::string normalized_id =
      container_id.empty() ? "default" : container_id;
  const auto it = container_service_generations_.find(normalized_id);
  return it == container_service_generations_.end() ? 0 : it->second;
}

bool XenonManager::IsElectronIpcGenerationCurrent(
    const std::string& container_id,
    uint64_t generation) {
  auto* connection = FindContainerService(container_id);
  return generation != 0 && connection && connection->remote.is_bound() &&
         connection->generation == generation;
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
  base::FilePath host_user_data;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &host_user_data) ||
      !host_user_data.IsAbsolute()) {
    LOG(ERROR) << "Cannot register Electron app without host user data path";
    return false;
  }
  config->host_user_data_directory = host_user_data.AsUTF8Unsafe();
  const auto crash_dumps = crash_reporter::GetCrashpadDatabasePath();
  config->host_crash_dumps_directory =
      crash_dumps ? crash_dumps->AsUTF8Unsafe() : std::string();
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
  const std::string container_id =
      config->container_id.empty() ? "default" : config->container_id;
  auto user_data = ipc::ResolveElectronAppStorageUserDataPath(*config);
  if (!user_data.has_value()) {
    LOG(ERROR) << "Cannot register Electron application storage: "
               << user_data.error();
    return false;
  }
  config->app_user_data_directory = user_data->AsUTF8Unsafe();
  const auto* launch = base::CommandLine::ForCurrentProcess();
  if (container_id == "default" &&
      launch->HasSwitch(ipc::switches::kElectronApp)) {
    config->app_argv =
        ipc::BuildElectronAppLaunchArgv(*launch, executable_path);
    InitializeRuntimeMetadata();
    if (config->working_directory.empty()) {
      config->working_directory = startup_working_directory_;
    }
  } else if (config->app_argv.empty()) {
    config->app_argv.push_back(config->executable_path);
  }
  const auto previous = last_ipc_configs_.find(container_id);
  if (FindContainerService(container_id)) {
    if (previous != last_ipc_configs_.end()) {
      if (previous->second->app_user_data_directory !=
          config->app_user_data_directory) {
        LOG(ERROR) << "Cannot change the default userData policy or identity "
                      "while the container is running";
        return false;
      }
      if (previous->second->native_addon_execution !=
          config->native_addon_execution) {
        LOG(ERROR) << "Cannot change native addon execution while the "
                      "container is running";
        return false;
      }
      const auto& old_keys = previous->second->archive_public_keys;
      const auto& new_keys = config->archive_public_keys;
      bool equal = old_keys.size() == new_keys.size();
      for (size_t i = 0; equal && i < old_keys.size(); ++i) {
        equal = old_keys[i]->root_path == new_keys[i]->root_path &&
                old_keys[i]->public_key_pem == new_keys[i]->public_key_pem;
      }
      if (!equal) {
        LOG(ERROR) << "Cannot replace ASAR keys while the container is running";
        return false;
      }
    }
  }
  std::vector<asar::ArchiveKeyConfig> previous_archive_keys;
  if (previous != last_ipc_configs_.end()) {
    for (const auto& key : previous->second->archive_public_keys) {
      previous_archive_keys.push_back(
          {base::FilePath::FromUTF8Unsafe(key->root_path),
           key->public_key_pem});
    }
  }
  std::vector<asar::ArchiveKeyConfig> archive_keys;
  for (const auto& key : config->archive_public_keys) {
    archive_keys.push_back(
        {base::FilePath::FromUTF8Unsafe(key->root_path), key->public_key_pem});
  }
  // Key validation and replacement are atomic. A malformed replacement must
  // not revoke the last usable configuration or partially update the reader.
  const std::string archive_key_owner = "electron:" + container_id;
  if (!asar::SetArchivePublicKeys(archive_key_owner, archive_keys)) {
    LOG(ERROR) << "Invalid or conflicting ASAR public key configuration";
    return false;
  }
  const bool disk_application =
      config->embedded_main_source.empty() && !config->app_path.empty();
  if (disk_application) {
    ipc::ElectronApplicationInfo application;
    if (!ipc::ResolveElectronApplication(
            base::FilePath::FromUTF8Unsafe(config->app_path), &application,
            &error)) {
      // The replacement key may be valid but unable to decrypt this app. Keep
      // the previous configuration usable by its existing renderer windows.
      if (!asar::SetArchivePublicKeys(archive_key_owner,
                                      previous_archive_keys)) {
        LOG(ERROR) << "Failed to restore previous ASAR public keys";
      }
      LOG(ERROR) << "Cannot register Electron container: " << error;
      return false;
    }
    config->app_path = application.app_path.AsUTF8Unsafe();
    if (config->app_name.empty()) {
      config->app_name =
          application.name.empty() ? "Application" : application.name;
    }
    if (config->app_version.empty()) {
      config->app_version = application.version;
    }
    if (config->resources_directory.empty()) {
      config->resources_directory =
          application.resources_directory.AsUTF8Unsafe();
    }
    if (config->runtime_directory.empty()) {
      config->runtime_directory = application.runtime_directory.AsUTF8Unsafe();
    }
    if (config->working_directory.empty()) {
      config->working_directory = config->runtime_directory;
    }
    if (!config->is_packaged.has_value()) {
      config->is_packaged = application.is_packaged;
    }
  } else if (!config->embedded_main_source.empty() &&
             !config->is_packaged.has_value()) {
    config->is_packaged = !config->renderer_base_url.empty() ||
                          !config->renderer_url_mappings.empty();
  }
  if (config->app_version.empty()) {
    config->app_version = ipc::GetAppExecutableVersion(executable_path);
    if (disk_application && config->app_version.empty()) {
      config->app_version = "0.0.0";
    }
  }
  if (config->default_user_agent.empty()) {
    const auto custom_ua = embedder_support::GetUserAgentFromCommandLine();
    config->default_user_agent =
        custom_ua.has_value() ? *custom_ua
                              : ipc::BuildElectronUserAgent(
                                    embedder_support::GetUserAgent(),
                                    config->app_name, config->app_version);
  }
  last_ipc_configs_.insert_or_assign(container_id, std::move(config));
  LOG(INFO) << "Registered Electron container configuration: " << container_id;
  return true;
}

bool XenonManager::IsDeclaredFileRendererURL(const std::string& container_id,
                                             const GURL& document_url) const {
  if (!document_url.is_valid() ||
      !document_url.SchemeIs(content::kChromeUIScheme) ||
      document_url.has_username() || document_url.has_password()) {
    return false;
  }
  // Encoded separators cannot denote a component of a mapped local filename.
  // Do not grant permission to a URL that decodes outside its declared subtree.
  const std::string path = base::ToLowerASCII(document_url.path());
  if (path.find("%2f") != std::string::npos ||
      path.find("%5c") != std::string::npos ||
      path.find("%00") != std::string::npos) {
    return false;
  }
  const auto config =
      last_ipc_configs_.find(container_id.empty() ? "default" : container_id);
  if (config == last_ipc_configs_.end() || !config->second) {
    return false;
  }
  for (const auto& mapping : config->second->renderer_url_mappings) {
    const GURL target = FileRendererMappingTarget(*mapping);
    if (!target.is_valid() ||
        url::Origin::Create(document_url) != url::Origin::Create(target)) {
      continue;
    }
    std::string prefix(target.path());
    if (!prefix.ends_with('/')) {
      prefix += '/';
    }
    if (document_url.path() == target.path() ||
        document_url.path().starts_with(prefix)) {
      return true;
    }
  }
  return false;
}

bool XenonManager::IsDeclaredFileRendererOrigin(
    const std::string& container_id,
    const url::Origin& origin) const {
  if (origin.opaque() || origin.scheme() != content::kChromeUIScheme) {
    return false;
  }
  const auto config =
      last_ipc_configs_.find(container_id.empty() ? "default" : container_id);
  if (config == last_ipc_configs_.end() || !config->second) {
    return false;
  }
  for (const auto& mapping : config->second->renderer_url_mappings) {
    const GURL target = FileRendererMappingTarget(*mapping);
    if (target.is_valid() && origin == url::Origin::Create(target)) {
      return true;
    }
  }
  return false;
}

bool XenonManager::ShouldPairElectronWindowWithParent(
    const std::string& container_id,
    const GURL& document_url) const {
  if (document_url.is_empty() || !document_url.is_valid() ||
      !document_url.has_scheme()) {
    return false;
  }
  const auto config =
      last_ipc_configs_.find(container_id.empty() ? "default" : container_id);
  if (config == last_ipc_configs_.end() || !config->second) {
    return false;
  }

  GURL::Replacements replacements;
  replacements.ClearQuery();
  replacements.ClearRef();
  const GURL normalized_document = document_url.ReplaceComponents(replacements);
  for (const std::string& declared :
       config->second->parent_window_pairing_urls) {
    const GURL declared_url(declared);
    if (!declared_url.is_empty() && declared_url.is_valid() &&
        declared_url.has_scheme() &&
        declared_url.ReplaceComponents(replacements) == normalized_document) {
      return true;
    }
  }
  return false;
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
  return EnsureContainerServiceStarted(normalized_id, true) != nullptr;
}

void XenonManager::ActivateElectronIpc(const std::string& container_id,
                                       bool has_visible_windows) {
  ContainerServiceConnection* connection = FindContainerService(container_id);
  if (!connection || !connection->remote.is_bound()) {
    return;
  }
  base::ListValue arguments;
  arguments.Append(has_visible_windows);
  connection->remote->DispatchElectronAppEvent(
      container_id, "activate", base::Value(std::move(arguments)));
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
      EnsureContainerServiceStarted(container_id, true);
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
  return it == electron_ipc_origin_containers_.end() ? "default" : it->second;
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
  renderer_config->resources_directory = config_it->second->resources_directory;
  renderer_config->is_packaged = config_it->second->is_packaged.value_or(false);
  renderer_config->working_directory =
      config_it->second->working_directory.empty()
          ? startup_working_directory_
          : config_it->second->working_directory;
  for (const auto& mapping : config_it->second->renderer_url_mappings) {
    renderer_config->renderer_url_mappings.push_back(mapping.Clone());
  }
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
  renderer_config->user_data_path =
      ipc::GetElectronAppStorageUserDataPath(
          SingleInstanceOwner(container_id, service_generation(container_id)))
          .AsUTF8Unsafe();
  if (renderer_config->user_data_path.empty()) {
    renderer_config->user_data_path =
        config_it->second->app_user_data_directory;
  }
  auto set_path = [](int key, std::string* output) {
    base::FilePath path;
    if (base::PathService::Get(key, &path)) {
      *output = path.AsUTF8Unsafe();
    }
  };
#if BUILDFLAG(IS_WIN)
  set_path(base::DIR_ROAMING_APP_DATA, &renderer_config->app_data_path);
  set_path(base::DIR_LOCAL_APP_DATA, &renderer_config->local_app_data_path);
#endif
  set_path(base::DIR_HOME, &renderer_config->home_path);
  set_path(base::DIR_TEMP, &renderer_config->temp_path);
  return renderer_config;
}

bool XenonManager::RegisterElectronIpcRenderer(
    const std::string& container_id,
    const std::string& endpoint_id,
    mojo::PendingRemote<ipc::mojom::IpcRenderer> renderer,
    int32_t process_id,
    int32_t frame_id,
    int32_t window_id,
    const std::string& document_url,
    bool is_main_frame,
    base::OnceCallback<void(bool)> registered) {
  if (ContainerServiceConnection* connection =
          EnsureContainerServiceStarted(container_id)) {
    if (!endpoint_id.empty()) {
      const RendererAddonKey key{
          container_id.empty() ? "default" : container_id, endpoint_id};
      auto [entry, inserted] = renderer_addon_services_.try_emplace(key);
      if (inserted) {
        entry->second = std::make_unique<RendererAddonServiceConnection>();
        entry->second->container_generation = connection->generation;
      }
    }
    connection->remote->RegisterElectronIpcRenderer(
        container_id, endpoint_id, std::move(renderer), process_id, frame_id,
        window_id, document_url, is_main_frame,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(registered),
                                                    false));
    return true;
  }
  std::move(registered).Run(false);
  return false;
}

void XenonManager::UpdateElectronIpcRendererFrame(
    const std::string& container_id,
    const std::string& endpoint_id,
    const std::string& document_url) {
  if (auto* connection = FindContainerService(container_id)) {
    connection->remote->UpdateElectronIpcRendererFrame(
        container_id, endpoint_id, document_url);
  }
}

void XenonManager::RegisterElectronIpcServiceWorker(
    const std::string& container_id,
    const std::string& endpoint_id,
    mojo::PendingRemote<ipc::mojom::IpcRenderer> renderer,
    int32_t process_id,
    ipc::mojom::IpcServiceWorkerInfoPtr identity) {
  if (auto* connection = FindContainerService(container_id)) {
    connection->remote->RegisterElectronIpcServiceWorker(
        container_id, endpoint_id, std::move(renderer), process_id,
        std::move(identity));
  }
}

void XenonManager::RemoveElectronIpcRenderer(const std::string& container_id,
                                             const std::string& endpoint_id) {
  const RendererAddonKey key{container_id.empty() ? "default" : container_id,
                             endpoint_id};
  auto addon = renderer_addon_services_.find(key);
  if (addon != renderer_addon_services_.end()) {
    CloseRendererAddonService(addon->second.get());
    renderer_addon_services_.erase(addon);
  }
  if (ContainerServiceConnection* connection =
          FindContainerService(container_id)) {
    connection->remote->RemoveElectronIpcRenderer(container_id, endpoint_id);
  }
}

void XenonManager::BindNodeAddonHost(
    const std::string& container_id,
    const std::string& endpoint_id,
    mojo::PendingReceiver<ipc::mojom::NodeAddonHost> receiver,
    mojo::PendingRemote<ipc::mojom::IpcRenderer> callback_renderer) {
  if (!receiver.is_valid() || !callback_renderer.is_valid()) {
    return;
  }
  const RendererAddonKey key{container_id.empty() ? "default" : container_id,
                             endpoint_id};
  if (RendererAddonServiceConnection* connection =
          EnsureRendererAddonServiceStarted(key)) {
    connection->remote->BindNodeAddonHost(container_id, endpoint_id,
                                          std::move(receiver),
                                          std::move(callback_renderer));
  }
}

XenonManager::RendererAddonServiceConnection*
XenonManager::FindRendererAddonService(const RendererAddonKey& key) {
  ContainerServiceConnection* container = FindContainerService(key.first);
  auto addon = renderer_addon_services_.find(key);
  if (!container || !container->remote.is_bound() ||
      addon == renderer_addon_services_.end() ||
      addon->second->container_generation != container->generation) {
    return nullptr;
  }
  return addon->second.get();
}

XenonManager::RendererAddonServiceConnection*
XenonManager::EnsureRendererAddonServiceStarted(const RendererAddonKey& key) {
  RendererAddonServiceConnection* connection = FindRendererAddonService(key);
  if (!connection) {
    return nullptr;
  }
  if (connection->remote.is_bound()) {
    return connection;
  }
  if (connection->generation != 0) {
    return nullptr;
  }

  mojo::Remote<mojom::XenonMainService> launched =
      content::ServiceProcessHost::Launch<mojom::XenonMainService>(
          content::ServiceProcessHost::Options()
              .WithDisplayName("Xenon Renderer Addons " + key.first)
              .WithExtraCommandLineSwitches(GetXenonServiceExtraSwitches(true))
              .WithExtraCommandLineSwitchKeyValues(
                  GetXenonServiceExtraSwitchValues())
              .Pass());
  connection->generation = ++renderer_addon_service_generation_;
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  connection->remote.Bind(launched.Unbind(), GetUiTaskRunner());
  connection->remote.set_disconnect_handler(
      base::BindOnce(&XenonManager::OnRendererAddonServiceDisconnected,
                     base::Unretained(this), key, connection->generation),
      GetUiTaskRunner());
#else
  connection->remote = std::move(launched);
  connection->remote.set_disconnect_handler(
      base::BindOnce(&XenonManager::OnRendererAddonServiceDisconnected,
                     base::Unretained(this), key, connection->generation));
#endif

  base::FilePath runtime_directory;
  std::string app_path;
  std::vector<ipc::mojom::IpcArchivePublicKeyPtr> archive_public_keys;
  auto config = last_ipc_configs_.find(key.first);
  if (config != last_ipc_configs_.end() && config->second) {
    app_path = config->second->app_path;
    for (const auto& archive_key : config->second->archive_public_keys) {
      archive_public_keys.push_back(archive_key.Clone());
    }
    if (!config->second->runtime_directory.empty()) {
      runtime_directory =
          base::FilePath::FromUTF8Unsafe(config->second->runtime_directory);
    } else if (!config->second->executable_path.empty()) {
      runtime_directory =
          base::FilePath::FromUTF8Unsafe(config->second->executable_path)
              .DirName();
    } else {
      runtime_directory =
          base::FilePath::FromUTF8Unsafe(config->second->app_path);
    }
  }
  // Only the native runtime is initialized here. ipcMain and its Browser
  // observer remain in the application's original service process.
  connection->remote->InitializeNodeAddonRuntime(
      key.first, runtime_directory.AsUTF8Unsafe(), app_path,
      std::move(archive_public_keys));
  return connection;
}

void XenonManager::OnRendererAddonServiceDisconnected(
    const RendererAddonKey& key,
    uint64_t generation) {
  auto addon = renderer_addon_services_.find(key);
  if (addon == renderer_addon_services_.end() ||
      addon->second->generation != generation) {
    return;
  }
  LOG(ERROR) << "Renderer addon service disconnected / crashed: " << key.first;
  CloseRendererAddonService(addon->second.get());
}

void XenonManager::CloseRendererAddonService(
    RendererAddonServiceConnection* connection) {
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  if (connection->remote.is_bound()) {
    connection->remote.Disconnect();
  }
#endif
  connection->remote.reset();
}

void XenonManager::CloseRendererAddonServicesForContainer(
    const std::string& container_id) {
  for (auto addon = renderer_addon_services_.begin();
       addon != renderer_addon_services_.end();) {
    if (addon->first.first == container_id) {
      CloseRendererAddonService(addon->second.get());
      addon = renderer_addon_services_.erase(addon);
    } else {
      ++addon;
    }
  }
}

void XenonManager::ElectronIpcSend(const std::string& container_id,
                                   const std::string& endpoint_id,
                                   const std::string& channel,
                                   base::Value arguments,
                                   uint64_t expected_generation) {
  auto* connection = expected_generation
                         ? FindContainerService(container_id)
                         : EnsureContainerServiceStarted(container_id);
  if (connection && connection->remote.is_bound() &&
      (!expected_generation || connection->generation == expected_generation)) {
    connection->remote->ElectronIpcSend(container_id, endpoint_id, channel,
                                        std::move(arguments));
  }
}

void XenonManager::ElectronIpcPostMessage(const std::string& container_id,
                                          const std::string& endpoint_id,
                                          const std::string& channel,
                                          blink::TransferableMessage message,
                                          uint64_t expected_generation) {
  // A document-bound endpoint cannot restart or join a different service
  // generation. Dropping a stale message closes its transferred port handles.
  auto* connection = FindContainerService(container_id);
  if (expected_generation && connection && connection->remote.is_bound() &&
      connection->generation == expected_generation) {
    connection->remote->ElectronIpcPostMessage(container_id, endpoint_id,
                                               channel, std::move(message));
  }
}

void XenonManager::ElectronIpcInvoke(const std::string& container_id,
                                     const std::string& endpoint_id,
                                     const std::string& channel,
                                     base::Value arguments,
                                     ElectronIpcInvokeCallback callback,
                                     uint64_t expected_generation) {
  if (expected_generation &&
      !IsElectronIpcGenerationCurrent(container_id, expected_generation)) {
    std::move(callback).Run(
        MakeElectronIpcFailure("Utility ipcMain service is unavailable"));
    return;
  }
  if (IsRendererNodeAddonInvokeChannel(channel)) {
    const RendererAddonKey key{container_id.empty() ? "default" : container_id,
                               endpoint_id};
    RendererAddonServiceConnection* addon = FindRendererAddonService(key);
    // The renderer binds its direct host and callback receiver before issuing
    // an asynchronous native call. Never launch a process without that binding.
    if (!addon || !addon->remote.is_bound()) {
      std::move(callback).Run(
          MakeElectronIpcFailure("Renderer addon service is unavailable"));
      return;
    }
    addon->remote->ElectronIpcInvoke(container_id, endpoint_id, channel,
                                     std::move(arguments), std::move(callback));
    return;
  }
  auto* connection = expected_generation
                         ? FindContainerService(container_id)
                         : EnsureContainerServiceStarted(container_id);
  if (!connection || !connection->remote.is_bound() ||
      (expected_generation && connection->generation != expected_generation)) {
    std::move(callback).Run(
        MakeElectronIpcFailure("Utility ipcMain service is unavailable"));
    return;
  }
  connection->remote->ElectronIpcInvoke(container_id, endpoint_id, channel,
                                        std::move(arguments),
                                        std::move(callback));
}

void XenonManager::RequestElectronProtocol(const std::string& container_id,
                                           const std::string& partition,
                                           uint64_t generation,
                                           const std::string& request_id,
                                           base::Value request,
                                           ElectronIpcInvokeCallback callback) {
  auto* connection = FindContainerService(container_id);
  if (!connection || !connection->remote.is_bound() ||
      connection->generation != generation) {
    std::move(callback).Run(
        MakeElectronIpcFailure("Protocol service is unavailable"));
    return;
  }
  connection->remote->HandleElectronProtocol(container_id, partition,
                                             request_id, std::move(request),
                                             std::move(callback));
}

void XenonManager::CancelElectronProtocol(const std::string& container_id,
                                          uint64_t generation,
                                          const std::string& request_id) {
  auto* connection = FindContainerService(container_id);
  if (connection && connection->remote.is_bound() &&
      connection->generation == generation) {
    connection->remote->CancelElectronProtocol(container_id, request_id);
  }
}

void XenonManager::ElectronIpcSendSync(const std::string& container_id,
                                       const std::string& endpoint_id,
                                       const std::string& channel,
                                       base::Value arguments,
                                       ElectronIpcSendSyncCallback callback,
                                       uint64_t expected_generation) {
  auto* connection = expected_generation
                         ? FindContainerService(container_id)
                         : EnsureContainerServiceStarted(container_id);
  if (!connection || !connection->remote.is_bound() ||
      (expected_generation && connection->generation != expected_generation)) {
    std::move(callback).Run(
        MakeElectronIpcFailure("Utility ipcMain service is unavailable"));
    return;
  }
  connection->remote->ElectronIpcSendSync(container_id, endpoint_id, channel,
                                          std::move(arguments),
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
mojo::ReceiverId XenonManager::SetupBrowserObserver(
    ServiceRemote* remote,
    const std::string& service_id) {
  if (!remote || !remote->is_bound()) {
    return 0;
  }
  mojo::PendingRemote<mojom::XenonBrowserObserver> pending_remote;
  mojo::PendingReceiver<mojom::XenonBrowserObserver> pending_receiver =
      pending_remote.InitWithNewPipeAndPassReceiver();
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  const mojo::ReceiverId receiver_id = browser_observer_receivers_.Add(
      this, std::move(pending_receiver), service_id, GetUiTaskRunner());
#else
  const mojo::ReceiverId receiver_id = browser_observer_receivers_.Add(
      this, std::move(pending_receiver), service_id);
#endif
  (*remote)->SetBrowserObserver(std::move(pending_remote));
  return receiver_id;
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

void XenonManager::OnElectronAppExit(const std::string& container_id,
                                     int32_t exit_code) {
  const std::string normalized_id =
      container_id.empty() ? "default" : container_id;
  // The caller's pipe identifies its authority. A main module cannot stop a
  // different container, the shared core service, or a newer launch of itself.
  ContainerServiceConnection* connection = FindContainerService(normalized_id);
  if (!connection ||
      browser_observer_receivers_.current_context() != normalized_id ||
      browser_observer_receivers_.current_receiver() !=
          connection->observer_receiver_id ||
      connection->generation != service_generation(normalized_id)) {
    return;
  }
  LOG(INFO) << "Electron container exited: " << normalized_id
            << " code=" << exit_code;
  CloseContainerService(normalized_id);
}
#endif

void XenonManager::ResetServiceConnection() {
  for (auto& [key, connection] : renderer_addon_services_) {
    CloseRendererAddonService(connection.get());
  }
  renderer_addon_services_.clear();
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
    const std::string& container_id,
    uint64_t generation) {
  ContainerServiceConnection* connection = FindContainerService(container_id);
  if (!connection || connection->generation != generation) {
    return;
  }
  LOG(ERROR) << "Electron container service disconnected / crashed: "
             << container_id << "; waiting for an explicit restart";
  CloseContainerService(container_id);
}

void XenonManager::CloseContainerService(const std::string& container_id) {
  ipc::CloseElectronPermissionOwner(
      SingleInstanceOwner(container_id, service_generation(container_id)));
  if (single_instance_broker_) {
    single_instance_broker_
        .AsyncCall(&ipc::XenonAppSingleInstanceBroker::Release)
        .WithArgs(SingleInstanceOwner(container_id,
                                      service_generation(container_id)));
  }
#if BUILDFLAG(IS_MAC)
  XenonMacPlayerHost::GetInstance()->CloseContainer(container_id);
#endif
  ipc::RemoveElectronProtocolHandlers(container_id);
  ipc::RemoveElectronWebRequestHandlers(container_id);
  ipc::RemoveElectronSessionsForContainer(container_id);
  CloseRendererAddonServicesForContainer(container_id);
  auto it = container_services_.find(container_id);
  if (it == container_services_.end()) {
    return;
  }
  auto connection = std::move(it->second);
  container_services_.erase(it);
  CancelElectronDialogs(
      SingleInstanceOwner(container_id, connection->generation));
  ReleaseElectronPowerAndStorageOwner(container_id, connection->generation);
  ipc::CloseElectronTracing(
      SingleInstanceOwner(container_id, connection->generation));
  ipc::CloseElectronAppCommandLine(
      SingleInstanceOwner(container_id, connection->generation));
  ipc::CloseElectronDesktopSources(
      SingleInstanceOwner(container_id, connection->generation));
  XenonElectronViewHost::GetInstance()->ReleaseOwner(
      SingleInstanceOwner(container_id, connection->generation));
  if (connection->browser_context) {
    ipc::CloseElectronNotifications(
        Profile::FromBrowserContext(connection->browser_context.get()),
        SingleInstanceOwner(container_id, connection->generation));
  }
  // Keep configuration and generation: old documents cannot implicitly start
  // the app while their windows are being torn down. The next explicit open
  // creates a fresh service with the saved configuration.
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  browser_observer_receivers_.Remove(connection->observer_receiver_id);
#endif
  if (connection->remote.is_bound()) {
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
    // reset() only releases this reference; native controllers may retain
    // copies. Disconnect the underlying pipe so ServiceFactory tears down the
    // Utility and its native addons even while those copies remain alive.
    connection->remote.Disconnect();
#else
    connection->remote.reset();
#endif
  }
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  XenonElectronWindowHost::GetInstance()->CloseForContainer(container_id);
#endif
  connection->utilities.clear();
  ipc::RetireElectronAppStorage(
      SingleInstanceOwner(container_id, connection->generation));
}

void XenonManager::ShutdownForProcessExit() {
  if (shutting_down_for_process_exit_) {
    return;
  }
  shutting_down_for_process_exit_ = true;
  single_instance_broker_.Reset();
#if BUILDFLAG(IS_MAC)
  XenonMacPlayerHost::GetInstance()->Shutdown();
#endif
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  browser_observer_receivers_.Clear();
#endif
  // Remove routable identities before disconnecting. Retained SharedRemote
  // copies must lose the actual pipe, rather than just this manager's
  // reference.
  auto services = std::exchange(container_services_, {});
  for (const auto& [id, connection] : services) {
    CancelElectronDialogs(SingleInstanceOwner(id, connection->generation));
    ipc::CloseElectronPermissionOwner(
        SingleInstanceOwner(id, connection->generation));
    ReleaseElectronPowerAndStorageOwner(id, connection->generation);
    ipc::CloseElectronTracing(SingleInstanceOwner(id, connection->generation));
    ipc::CloseElectronAppCommandLine(
        SingleInstanceOwner(id, connection->generation));
    ipc::CloseElectronDesktopSources(
        SingleInstanceOwner(id, connection->generation));
    XenonElectronViewHost::GetInstance()->ReleaseOwner(
        SingleInstanceOwner(id, connection->generation));
    if (connection->browser_context) {
      ipc::CloseElectronNotifications(
          Profile::FromBrowserContext(connection->browser_context.get()),
          SingleInstanceOwner(id, connection->generation));
    }
    ipc::RemoveElectronProtocolHandlers(id);
    ipc::RemoveElectronWebRequestHandlers(id);
    ipc::RemoveElectronSessionsForContainer(id);
    if (connection->remote.is_bound()) {
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
      connection->remote.Disconnect();
#else
      connection->remote.reset();
#endif
    }
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
    XenonElectronWindowHost::GetInstance()->CloseForContainer(id);
#endif
    connection->utilities.clear();
    ipc::RetireElectronAppStorage(
        SingleInstanceOwner(id, connection->generation));
  }
#if BUILDFLAG(ENABLE_XENON_MANAGER_SHARED_REMOTE)
  if (service_remote_.is_bound()) {
    service_remote_.Disconnect();
  }
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
void XenonManager::EnsureContainerStorage(
    const std::string& container,
    uint64_t generation,
    base::OnceCallback<void(std::string)> callback) {
  auto* current = FindContainerService(container);
  if (!current || current->generation != generation ||
      !current->remote.is_bound() || shutting_down_for_process_exit_) {
    std::move(callback).Run("ERR_ABORTED: application owner stopped");
    return;
  }
  if (current->storage_ready && current->browser_context &&
      ipc::PeekElectronAppStorageContext(SingleInstanceOwner(
          container, generation)) == current->browser_context.get() &&
      !current->browser_context->ShutdownStarted()) {
    // A permission callback may synchronously query its existing Session.
    // Once storage is fixed, no UI task or Profile initialization is needed.
    std::move(callback).Run("");
    return;
  }
  if (ipc::IsElectronPermissionCheckInProgress()) {
    std::move(callback).Run(
        "ERR_INVALID_STATE: storage cannot be initialized inside a synchronous "
        "permission check");
    return;
  }
  ipc::EnsureElectronAppStorageContext(
      SingleInstanceOwner(container, generation),
      base::BindOnce(
          [](base::WeakPtr<XenonManager> manager, std::string container,
             uint64_t generation,
             base::OnceCallback<void(std::string)> callback,
             content::BrowserContext* context, std::string error) {
            auto* connection =
                manager ? manager->FindContainerService(container) : nullptr;
            if (!manager || manager->shutting_down_for_process_exit_ ||
                !connection || connection->generation != generation ||
                !connection->remote.is_bound()) {
              std::move(callback).Run("ERR_ABORTED: application owner stopped");
              return;
            }
            if (!context || !error.empty()) {
              std::move(callback).Run(
                  error.empty() ? "ERR_ABORTED: application storage unavailable"
                                : std::move(error));
              return;
            }
            if (connection->browser_context.get() != context) {
              auto rebound = ipc::RebindElectronPermissionOwnerContext(
                  SingleInstanceOwner(container, generation), context);
              if (!rebound.has_value()) {
                std::move(callback).Run(rebound.error());
                return;
              }
              connection->browser_context = context->GetWeakPtr();
            }
            connection->storage_ready = true;
            std::move(callback).Run("");
          },
          weak_factory_.GetWeakPtr(), container, generation,
          std::move(callback)));
}

void XenonManager::OnThreadCallback(const std::string& message) {
  if (node_observer_) {
    node_observer_->OnThreadCallback(message);
  }
}

bool XenonManager::AuthorizeElectronWindowOperation(int32_t window_id,
                                                    std::string* container,
                                                    std::string* error) {
  *container = browser_observer_receivers_.current_context();
  auto* connection = FindContainerService(*container);
  if (shutting_down_for_process_exit_ || !connection ||
      !connection->browser_context || !connection->remote.is_bound() ||
      connection->observer_receiver_id !=
          browser_observer_receivers_.current_receiver() ||
      connection->generation != service_generation(*container)) {
    *error = "ERR_ACCESS_DENIED: stale window owner";
    return false;
  }
  if (window_id == 0) {
    return true;
  }
  if (auto* guest = ipc::XenonElectronGuest::FromId(window_id)) {
    auto* contents = guest->web_contents();
    if (contents && guest->container_id() == *container &&
        contents->GetBrowserContext() == connection->browser_context.get()) {
      return true;
    }
    *error = "ERR_ACCESS_DENIED: WebContents belongs to another application";
    return false;
  }
  views::Widget* widget = nullptr;
  content::WebContents* contents = nullptr;
  if (!XenonElectronWindowHost::GetInstance()->GetOwnedWindowTarget(
          *container, window_id, &widget, &contents, error)) {
    *error = "ERR_ACCESS_DENIED: " + *error;
    return false;
  }
  if (contents->GetBrowserContext() != connection->browser_context.get()) {
    *error = "ERR_ACCESS_DENIED: window belongs to another browser context";
    return false;
  }
  return true;
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
  std::string error;
  std::string authenticated_container;
  if (!AuthorizeElectronWindowOperation(parent_id, &authenticated_container,
                                        &error) ||
      authenticated_container != container_id) {
    std::move(callback).Run(
        0, 0,
        error.empty() ? "ERR_ACCESS_DENIED: invalid window container" : error);
    return;
  }
  const auto generation =
      FindContainerService(authenticated_container)->generation;
  EnsureContainerStorage(
      authenticated_container, generation,
      base::BindOnce(
          [](base::WeakPtr<XenonManager> manager, std::string container,
             uint64_t generation, int width, int height, bool show, bool frame,
             bool transparent, int parent_id, std::string title,
             CreateElectronWindowCallback callback, std::string error) {
            auto* connection =
                manager ? manager->FindContainerService(container) : nullptr;
            if (!connection || connection->generation != generation ||
                !error.empty()) {
              std::move(callback).Run(
                  0, 0,
                  error.empty() ? "ERR_ABORTED: window owner stopped" : error);
              return;
            }
            auto* windows = XenonElectronWindowHost::GetInstance();
            if (parent_id) {
              views::Widget* parent = nullptr;
              content::WebContents* contents = nullptr;
              if (!windows->GetOwnedWindowTarget(container, parent_id, &parent,
                                                 &contents, &error) ||
                  contents->GetBrowserContext() !=
                      connection->browser_context.get()) {
                std::move(callback).Run(
                    0, 0, "ERR_ACCESS_DENIED: parent window is unavailable");
                return;
              }
            }
            int32_t id = 0;
            uint64_t handle = 0;
            if (!windows->CreateHostedWindow(connection->browser_context.get(),
                                             width, height, show, frame,
                                             transparent, parent_id, title,
                                             container, &id, &handle, &error) &&
                error.empty()) {
              error = "Failed to create Electron BrowserWindow";
            }
            std::move(callback).Run(id, handle, error);
          },
          weak_factory_.GetWeakPtr(), authenticated_container, generation,
          width, height, show, frame, transparent, parent_id, title,
          std::move(callback)));
}

void XenonManager::LoadElectronWindowURL(int32_t window_id,
                                         const std::string& url) {
  std::string container;
  std::string error;
  if (!AuthorizeElectronWindowOperation(window_id, &container, &error)) {
    LOG(WARNING) << error;
    return;
  }
  XenonElectronWindowHost::GetInstance()->LoadURL(window_id, url);
}

void XenonManager::CreateElectronNetworkFactory(
    const std::string& partition,
    mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver> headers,
    CreateElectronNetworkFactoryCallback callback) {
  const std::string container = browser_observer_receivers_.current_context();
  auto* connection = FindContainerService(container);
  if (!connection ||
      connection->observer_receiver_id !=
          browser_observer_receivers_.current_receiver() ||
      connection->generation != service_generation(container) ||
      !connection->browser_context) {
    std::move(callback).Run({}, "ERR_ACCESS_DENIED: stale network owner");
    return;
  }
  CreateContainerNetworkFactory(container, connection->generation, partition,
                                std::move(headers), std::move(callback));
}

void XenonManager::CreateContainerNetworkFactory(
    const std::string& container,
    uint64_t generation,
    const std::string& partition,
    mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver> headers,
    CreateElectronNetworkFactoryCallback callback) {
  EnsureContainerStorage(
      container, generation,
      base::BindOnce(
          [](base::WeakPtr<XenonManager> manager, std::string container,
             uint64_t generation, std::string partition,
             mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver>
                 headers,
             CreateElectronNetworkFactoryCallback callback, std::string error) {
            auto* current =
                manager ? manager->FindContainerService(container) : nullptr;
            if (!current || current->generation != generation ||
                !error.empty()) {
              std::move(callback).Run(
                  {},
                  error.empty() ? "ERR_ABORTED: network owner stopped" : error);
              return;
            }
            auto factory = ipc::CreateElectronNetFactory(
                current->browser_context.get(), container, partition, &error,
                std::move(headers));
            std::move(callback).Run(std::move(factory), error);
          },
          weak_factory_.GetWeakPtr(), container, generation, partition,
          std::move(headers), std::move(callback)));
}

void XenonManager::GetElectronNetworkOnline(
    GetElectronNetworkOnlineCallback callback) {
  const std::string container = browser_observer_receivers_.current_context();
  auto* connection = FindContainerService(container);
  if (!connection ||
      connection->observer_receiver_id !=
          browser_observer_receivers_.current_receiver() ||
      connection->generation != service_generation(container)) {
    std::move(callback).Run(false, "ERR_ACCESS_DENIED: stale network owner");
    return;
  }
  std::move(callback).Run(ipc::IsElectronNetworkOnline(), "");
}

void XenonManager::ForkUtilityProcess(
    ipc::mojom::UtilityProcessOptionsPtr options,
    blink::MessagePortDescriptor parent_port,
    mojo::PendingReceiver<ipc::mojom::UtilityProcessControl> control,
    mojo::PendingRemote<ipc::mojom::UtilityProcessClient> client,
    ForkUtilityProcessCallback callback) {
  std::string container;
  std::string error;
  if (!AuthorizeElectronWindowOperation(0, &container, &error)) {
    std::move(callback).Run(false, error);
    return;
  }
  auto* connection = FindContainerService(container);
  const auto generation = connection->generation;
  const auto receiver = connection->observer_receiver_id;
  EnsureContainerStorage(
      container, generation,
      base::BindOnce(
          [](base::WeakPtr<XenonManager> manager, std::string container,
             uint64_t generation, mojo::ReceiverId receiver,
             ipc::mojom::UtilityProcessOptionsPtr options,
             blink::MessagePortDescriptor parent_port,
             mojo::PendingReceiver<ipc::mojom::UtilityProcessControl> control,
             mojo::PendingRemote<ipc::mojom::UtilityProcessClient> client,
             ForkUtilityProcessCallback callback, std::string error) {
            if (!manager || !error.empty()) {
              std::move(callback).Run(
                  false,
                  error.empty() ? "ERR_ABORTED: Utility owner stopped" : error);
              return;
            }
            manager->ForkUtilityProcessForOwner(
                container, generation, receiver, std::move(options),
                std::move(parent_port), std::move(control), std::move(client),
                std::move(callback));
          },
          weak_factory_.GetWeakPtr(), container, generation, receiver,
          std::move(options), std::move(parent_port), std::move(control),
          std::move(client), std::move(callback)));
}

void XenonManager::ForkUtilityProcessForOwner(
    const std::string& container,
    uint64_t generation,
    mojo::ReceiverId receiver,
    ipc::mojom::UtilityProcessOptionsPtr options,
    blink::MessagePortDescriptor parent_port,
    mojo::PendingReceiver<ipc::mojom::UtilityProcessControl> control,
    mojo::PendingRemote<ipc::mojom::UtilityProcessClient> client,
    ForkUtilityProcessCallback callback) {
  auto* connection = FindContainerService(container);
  auto config = last_ipc_configs_.find(container);
  if (shutting_down_for_process_exit_ || !connection ||
      !connection->browser_context ||
      connection->observer_receiver_id != receiver ||
      connection->generation != generation ||
      generation != service_generation(container) ||
      config == last_ipc_configs_.end() || !config->second || !options ||
      !control.is_valid() || !client.is_valid()) {
    std::move(callback).Run(false,
                            "ERR_ACCESS_DENIED: stale utility process owner");
    return;
  }
  if (connection->utilities.size() >= 64) {
    std::move(callback).Run(false,
                            "ERR_RESOURCE_BUSY: Utility process limit reached");
    return;
  }
  const uint64_t id = ++connection->next_utility_id;
  auto child = std::make_unique<ipc::XenonUtilityProcessHost>(
      std::move(control), std::move(client),
      base::BindRepeating(
          [](base::WeakPtr<XenonManager> manager, const std::string& owner,
             uint64_t generation, const std::string& partition,
             mojo::PendingRemote<ipc::mojom::ElectronNetworkHeadersObserver>
                 headers,
             ipc::ElectronNetEngine::FactoryCallback done) {
            auto* current =
                manager ? manager->FindContainerService(owner) : nullptr;
            if (!current || current->generation != generation ||
                !current->browser_context) {
              std::move(done).Run(
                  {}, "ERR_ACCESS_DENIED: stale utility network owner");
              return;
            }
            manager->CreateContainerNetworkFactory(
                owner, generation, partition, std::move(headers),
                base::BindOnce(
                    [](ipc::ElectronNetEngine::FactoryCallback done,
                       mojo::PendingRemote<network::mojom::URLLoaderFactory>
                           factory,
                       const std::string& error) {
                      std::move(done).Run(std::move(factory), error);
                    },
                    std::move(done)));
          },
          weak_factory_.GetWeakPtr(), container, connection->generation),
      base::BindRepeating(
          [](base::WeakPtr<XenonManager> manager, const std::string& owner,
             uint64_t generation, bool* online, std::string* error) {
            auto* current =
                manager ? manager->FindContainerService(owner) : nullptr;
            if (!current || current->generation != generation ||
                !current->browser_context) {
              *error = "ERR_ACCESS_DENIED: stale utility network owner";
              return false;
            }
            *online = ipc::IsElectronNetworkOnline();
            return true;
          },
          weak_factory_.GetWeakPtr(), container, connection->generation),
      base::BindOnce(
          [](base::WeakPtr<XenonManager> manager, std::string owner,
             uint64_t generation, uint64_t child_id) {
            if (!manager) {
              return;
            }
            auto* current = manager->FindContainerService(owner);
            if (current && current->generation == generation) {
              current->utilities.erase(child_id);
            }
          },
          weak_factory_.GetWeakPtr(), container, connection->generation, id));
  auto* instance = child.get();
  connection->utilities.emplace(id, std::move(child));
  mojo::PendingRemote<network::mojom::URLLoaderFactory> network_factory;
  connection->browser_context->GetDefaultStoragePartition()
      ->GetURLLoaderFactoryForBrowserProcess()
      ->Clone(network_factory.InitWithNewPipeAndPassReceiver());
  auto child_config = config->second.Clone();
  child_config->app_user_data_directory =
      ipc::GetElectronAppStorageUserDataPath(
          SingleInstanceOwner(container, connection->generation))
          .AsUTF8Unsafe();
  instance->Start(std::move(child_config), std::move(options),
                  std::move(parent_port), std::move(network_factory),
                  std::move(callback));
}

void XenonManager::SetElectronWindowVisible(int32_t window_id, bool visible) {
  std::string container;
  std::string error;
  if (!AuthorizeElectronWindowOperation(window_id, &container, &error)) {
    LOG(WARNING) << error;
    return;
  }
  XenonElectronWindowHost::GetInstance()->SetVisible(window_id, visible);
}

void XenonManager::ElectronWindowCall(int32_t window_id,
                                      const std::string& command,
                                      base::Value arguments,
                                      ElectronWindowCallCallback callback) {
  std::string authenticated_container;
  std::string authorization_error;
  if (!AuthorizeElectronWindowOperation(window_id, &authenticated_container,
                                        &authorization_error)) {
    std::move(callback).Run({}, authorization_error);
    return;
  }
  auto* authenticated = FindContainerService(authenticated_container);
  const uint64_t authenticated_generation = authenticated->generation;
  const auto receiver = authenticated->observer_receiver_id;
  if (command == "electron-message-box" || command == "electron-save-dialog" ||
      command == "electron-open-dialog") {
    if (ipc::IsElectronPermissionCheckInProgress()) {
      std::move(callback).Run(
          {},
          "ERR_INVALID_STATE: cannot display a dialog during a "
          "synchronous permission check");
      return;
    }
    const auto* options = arguments.GetIfDict();
    if (!options) {
      std::move(callback).Run(
          {}, "ERR_INVALID_ARG_TYPE: dialog options must be an object");
      return;
    }
    views::Widget* owner = nullptr;
    content::WebContents* contents = nullptr;
    std::string error;
    if (window_id != 0 &&
        !XenonElectronWindowHost::GetInstance()->GetOwnedWindowTarget(
            authenticated_container, window_id, &owner, &contents, &error)) {
      std::move(callback).Run({}, "ERR_ACCESS_DENIED: " + error);
      return;
    }
    auto reply = base::BindOnce(
        [](base::WeakPtr<XenonManager> manager, std::string container,
           uint64_t generation, mojo::ReceiverId receiver,
           ElectronWindowCallCallback callback, base::Value value,
           std::string error) {
          auto* current =
              manager ? manager->FindContainerService(container) : nullptr;
          if (!current || manager->shutting_down_for_process_exit_ ||
              current->generation != generation ||
              current->observer_receiver_id != receiver ||
              !current->remote.is_bound()) {
            std::move(callback).Run({},
                                    "ERR_ABORTED: dialog application stopped");
            return;
          }
          std::move(callback).Run(std::move(value), error);
        },
        weak_factory_.GetWeakPtr(), authenticated_container,
        authenticated_generation, receiver, std::move(callback));
    const auto owner_token =
        SingleInstanceOwner(authenticated_container, authenticated_generation);
    // Both synchronous and Promise-based Utility calls retain the same reply.
    // The Browser UI thread stays free to dispatch native dialog input.
    if (command == "electron-message-box") {
      ShowElectronMessageBox(owner_token, owner, *options, std::move(reply));
    } else if (command == "electron-open-dialog") {
      ShowElectronOpenDialog(owner_token, owner, *options, std::move(reply));
    } else {
      ShowElectronSaveDialog(owner_token, owner, *options, std::move(reply));
    }
    return;
  }
  if (window_id == 0 && command == "electron-app-storage") {
    const auto* path_arguments = arguments.GetIfDict();
    const auto* method =
        path_arguments ? path_arguments->FindString("method") : nullptr;
    if (method && *method == "set" &&
        ipc::IsElectronPermissionCheckInProgress()) {
      std::move(callback).Run(
          {},
          "ERR_INVALID_STATE: storage paths cannot change inside a "
          "synchronous permission check");
      return;
    }
    ipc::CallElectronAppStoragePath(
        SingleInstanceOwner(authenticated_container, authenticated_generation),
        std::move(arguments),
        base::BindOnce(
            [](ElectronWindowCallCallback callback, base::Value value,
               std::string error) {
              std::move(callback).Run(std::move(value), error);
            },
            std::move(callback)));
    return;
  }
  if (window_id == 0 &&
      (command == "electron-session" || command == "web-request-handler" ||
       command == "protocol-handler" || command == "electron-view" ||
       command == "notification")) {
    // Plain View objects do not instantiate browser storage. Every operation
    // that creates WebContents or uses a Session shares the same freeze point.
    const auto* options = arguments.GetIfDict();
    const auto* kind = options ? options->FindString("kind") : nullptr;
    if (command == "electron-view" && (!kind || *kind != "web-contents")) {
      ElectronStorageWindowCall(authenticated_container,
                                authenticated_generation, receiver, command,
                                std::move(arguments), std::move(callback));
      return;
    }
    EnsureContainerStorage(
        authenticated_container, authenticated_generation,
        base::BindOnce(
            [](base::WeakPtr<XenonManager> manager, std::string container,
               uint64_t generation, mojo::ReceiverId receiver,
               std::string command, base::Value arguments,
               ElectronWindowCallCallback callback, std::string error) {
              if (!manager || !error.empty()) {
                std::move(callback).Run(
                    {},
                    error.empty() ? "ERR_ABORTED: application stopped" : error);
                return;
              }
              manager->ElectronStorageWindowCall(
                  container, generation, receiver, command,
                  std::move(arguments), std::move(callback));
            },
            weak_factory_.GetWeakPtr(), authenticated_container,
            authenticated_generation, receiver, command, std::move(arguments),
            std::move(callback)));
    return;
  }
  if (window_id == 0 && command == "electron-permissions") {
    const std::string container = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(container);
    if (shutting_down_for_process_exit_ || !connection ||
        !connection->remote.is_bound() ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(container)) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale permission owner");
      return;
    }
    auto result = ipc::CallElectronPermissions(
        container, SingleInstanceOwner(container, connection->generation),
        arguments);
    if (result.has_value()) {
      std::move(callback).Run(std::move(*result), "");
    } else {
      std::move(callback).Run(base::Value(), result.error());
    }
    return;
  }
  if (window_id == 0 && command == "mac-player") {
#if BUILDFLAG(IS_MAC)
    const std::string container = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(container);
    auto config = last_ipc_configs_.find(container);
    if (!connection ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(container) ||
        config == last_ipc_configs_.end() || !config->second) {
      std::move(callback).Run(
          MakeMacPlayerReply({}, "ERR_ACCESS_DENIED: stale player owner"), "");
      return;
    }
    XenonMacPlayerHost::GetInstance()->Call(
        container, connection->generation,
        base::FilePath::FromUTF8Unsafe(config->second->resources_directory),
        base::FilePath::FromUTF8Unsafe(config->second->runtime_directory),
        std::move(arguments),
        base::BindRepeating(
            [](XenonManager* manager, std::string container,
               uint64_t generation, base::Value event) {
              auto* connection = manager->FindContainerService(container);
              if (connection && connection->generation == generation &&
                  connection->remote.is_bound()) {
                connection->remote->DispatchElectronWindowEvent(
                    0, "__xenon-mac-player-event", std::move(event));
              }
            },
            base::Unretained(this), container, connection->generation),
        std::move(callback));
#else
    std::move(callback).Run(base::Value(),
                            "Mac player is unavailable on this platform");
#endif
    return;
  }
  if (window_id == 0 &&
      (command == "app-set-user-model-id" || command == "app-set-jump-list")) {
    const std::string owner = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(owner);
    if (!connection ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(owner)) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale app shell owner");
      return;
    }
    const auto* args = arguments.GetIfDict();
    if (!args) {
      std::move(callback).Run(
          base::Value(), "ERR_INVALID_ARG_TYPE: expected app shell arguments");
      return;
    }
    if (command == "app-set-jump-list" &&
        ipc::IsElectronPermissionCheckInProgress()) {
      std::move(callback).Run(
          base::Value(),
          "ERR_INVALID_STATE: cannot update the jump list during a synchronous "
          "permission check");
      return;
    }
    if (command == "app-set-user-model-id") {
      const auto* id = args->FindString("id");
      if (!id || id->empty() || base::UTF8ToUTF16(*id).size() > 128 ||
          id->find('\0') != std::string::npos) {
        std::move(callback).Run(
            base::Value(), "ERR_INVALID_ARG_VALUE: invalid AppUserModelID");
        return;
      }
      connection->app_user_model_id = *id;
    } else if (connection->app_user_model_id.empty()) {
      const auto config = last_ipc_configs_.find(owner);
      if (config == last_ipc_configs_.end() || !config->second ||
          config->second->app_path.empty()) {
        std::move(callback).Run(
            base::Value(), "ERR_NOT_READY: application identity unavailable");
        return;
      }
      connection->app_user_model_id =
          "Xenon.Electron." +
          base::HexEncode(base::SHA1HashString(config->second->app_path));
    }
    XenonElectronWindowHost::GetInstance()->SetAppUserModelIdForContainer(
        owner, connection->app_user_model_id);
    if (command == "app-set-user-model-id") {
      std::move(callback).Run(base::Value(), "");
    } else if (const auto* categories = args->Find("categories")) {
      ipc::SetElectronJumpList(connection->app_user_model_id,
                               categories->Clone(), std::move(callback));
    } else {
      std::move(callback).Run(base::Value(),
                              "ERR_INVALID_ARG_TYPE: missing categories");
    }
    return;
  }
  if (window_id == 0 && command == "app-login-item-settings") {
    const std::string container = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(container);
    const auto config = last_ipc_configs_.find(container);
    if (shutting_down_for_process_exit_ || !connection ||
        !connection->remote.is_bound() ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(container) ||
        config == last_ipc_configs_.end() || !config->second) {
      std::move(callback).Run({},
                              "ERR_ACCESS_DENIED: stale login-item owner");
      return;
    }
    if (ipc::IsElectronPermissionCheckInProgress()) {
      std::move(callback).Run(
          {},
          "ERR_INVALID_STATE: cannot access login items during a "
          "synchronous permission check");
      return;
    }
    const auto& app_config = *config->second;
    ipc::ElectronLoginItemContext context;
    context.application_path =
        base::FilePath::FromUTF8Unsafe(app_config.app_path);
    context.launch_path =
        base::FilePath::FromUTF8Unsafe(app_config.launch_path);
    context.user_data_directory =
        base::FilePath::FromUTF8Unsafe(app_config.host_user_data_directory);
    if (!base::PathService::Get(base::FILE_EXE, &context.executable_path)) {
      std::move(callback).Run({}, "ERR_FAILED: cannot resolve host executable");
      return;
    }
    const auto generation = connection->generation;
    ipc::CallElectronLoginItemSettings(
        std::move(context), std::move(arguments),
        base::BindOnce(
            [](base::WeakPtr<XenonManager> manager, std::string container,
               uint64_t generation, ElectronWindowCallCallback callback,
               base::Value value, std::string error) {
              if (!manager || !manager->IsElectronIpcGenerationCurrent(
                                  container, generation)) {
                std::move(callback).Run(
                    {}, "ERR_ABORTED: login-item owner stopped");
                return;
              }
              std::move(callback).Run(std::move(value), std::move(error));
            },
            weak_factory_.GetWeakPtr(), container, generation,
            std::move(callback)));
    return;
  }
  if (window_id == 0 && command == "app-get-preferred-system-languages") {
    const std::string owner = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(owner);
    if (!connection ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(owner)) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale language owner");
      return;
    }
    auto languages = ipc::GetPreferredSystemLanguages();
    if (!languages.has_value()) {
      std::move(callback).Run(base::Value(), languages.error());
      return;
    }
    base::ListValue result;
    for (auto& language : *languages) {
      result.Append(std::move(language));
    }
    std::move(callback).Run(base::Value(std::move(result)), "");
    return;
  }
  if (window_id == 0 && command == "app-get-locale") {
    const std::string owner = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(owner);
    if (!connection ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(owner)) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale locale owner");
    } else if (!g_browser_process ||
               g_browser_process->GetApplicationLocale().empty()) {
      std::move(callback).Run(
          base::Value(), "ERR_NOT_READY: application locale is unavailable");
    } else {
      std::move(callback).Run(
          base::Value(g_browser_process->GetApplicationLocale()), "");
    }
    return;
  }
  if (window_id == 0 && command == "electron-error-box") {
    const std::string owner = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(owner);
    const auto* args = arguments.GetIfDict();
    const auto* title = args ? args->FindString("title") : nullptr;
    const auto* message = args ? args->FindString("content") : nullptr;
    if (!connection ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(owner)) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale dialog owner");
      return;
    }
    if (!title || !message || title->size() > 64 * 1024 ||
        message->size() > 1024 * 1024 ||
        title->find('\0') != std::string::npos ||
        message->find('\0') != std::string::npos) {
      std::move(callback).Run(
          base::Value(), "ERR_INVALID_ARG_VALUE: invalid error dialog text");
      return;
    }
    if (ipc::IsElectronPermissionCheckInProgress()) {
      std::move(callback).Run(
          base::Value(),
          "ERR_INVALID_STATE: cannot display an error dialog during a "
          "synchronous permission check");
      return;
    }
    LOG(ERROR) << "Electron error dialog: " << *title << ": " << *message;
    content::GetUIThreadTaskRunner({})->PostTask(
        FROM_HERE, base::BindOnce(
                       [](std::u16string title, std::u16string message,
                          ElectronWindowCallCallback callback) {
                         chrome::ShowWarningMessageBoxAsync(
                             gfx::NativeWindow(), title, message,
                             base::BindOnce(
                                 [](ElectronWindowCallCallback callback,
                                    chrome::MessageBoxResult) {
                                   std::move(callback).Run(base::Value(), "");
                                 },
                                 std::move(callback)));
                       },
                       base::UTF8ToUTF16(*title), base::UTF8ToUTF16(*message),
                       std::move(callback)));
    return;
  }
  if (window_id == 0 && command == "app-command-line") {
    const std::string container = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(container);
    if (shutting_down_for_process_exit_ || !connection ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(container)) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale command line owner");
      return;
    }
    base::Value result;
    std::string error;
    ipc::CallElectronAppCommandLine(
        SingleInstanceOwner(container, connection->generation), arguments,
        &result, &error);
    std::move(callback).Run(std::move(result), error);
    return;
  }
  if (window_id == 0 && command == "app-single-instance") {
    using Broker = ipc::XenonAppSingleInstanceBroker;
    const std::string container = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(container);
    const auto config = last_ipc_configs_.find(container);
    if (shutting_down_for_process_exit_ || !connection ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(container) ||
        config == last_ipc_configs_.end() || !config->second) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale single-instance owner");
      return;
    }
    const auto* args = arguments.GetIfDict();
    const auto* method = args ? args->FindString("method") : nullptr;
    if (!method ||
        (*method != "request" && *method != "has" && *method != "release")) {
      std::move(callback).Run(base::Value(),
                              "ERR_INVALID_ARG_VALUE: invalid lock method");
      return;
    }
    if (ipc::IsElectronPermissionCheckInProgress()) {
      std::move(callback).Run(
          base::Value(),
          "ERR_INVALID_STATE: cannot access single-instance locks during a "
          "synchronous permission check");
      return;
    }
    if (!single_instance_broker_) {
      single_instance_broker_.emplace(
          base::ThreadPool::CreateSequencedTaskRunner(
              {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
               base::TaskShutdownBehavior::BLOCK_SHUTDOWN}),
          base::BindPostTaskToCurrentDefault(base::BindRepeating(
              [](base::WeakPtr<XenonManager> manager, const std::string& owner,
                 Broker::Notification notification) {
                if (!manager || manager->shutting_down_for_process_exit_) {
                  return;
                }
                for (const auto& [owner_container, active_connection] :
                     manager->container_services_) {
                  if (active_connection &&
                      active_connection->remote.is_bound() &&
                      SingleInstanceOwner(owner_container,
                                          active_connection->generation) ==
                          owner) {
                    active_connection->remote->DispatchElectronWindowEvent(
                        0, "__xenon-second-instance",
                        base::Value(
                            base::DictValue()
                                .Set("argv", std::move(notification.argv))
                                .Set("workingDirectory",
                                     notification.working_directory
                                         .AsUTF8Unsafe())
                                .Set("additionalData",
                                     std::move(notification.additional_data))));
                    return;
                  }
                }
              },
              weak_factory_.GetWeakPtr())));
    }
    const auto generation = connection->generation;
    const std::string owner = SingleInstanceOwner(container, generation);
    if (*method == "release") {
      single_instance_broker_.AsyncCall(&Broker::Release)
          .WithArgs(owner)
          .Then(base::BindOnce(
              [](ElectronWindowCallCallback callback) {
                std::move(callback).Run(base::Value(), "");
              },
              std::move(callback)));
      return;
    }
    if (*method == "has") {
      single_instance_broker_.AsyncCall(&Broker::Has)
          .WithArgs(owner)
          .Then(base::BindOnce(
              [](ElectronWindowCallCallback callback, bool held) {
                std::move(callback).Run(base::Value(held), "");
              },
              std::move(callback)));
      return;
    }
    // argv remains the Browser's launch snapshot. The authenticated main
    // transport supplies its real current directory after native chdir.
    InitializeRuntimeMetadata();
    auto notification = Broker::NotificationFromNativeRequest(
        config->second->app_argv,
        base::FilePath::FromUTF8Unsafe(config->second->executable_path), *args);
    if (!notification.has_value()) {
      std::move(callback).Run(base::Value(), notification.error().code + ": " +
                                                 notification.error().message);
      return;
    }
    single_instance_broker_.AsyncCall(&Broker::Request)
        .WithArgs(owner,
                  base::FilePath::FromUTF8Unsafe(config->second->app_path),
                  ipc::GetElectronAppStorageUserDataPath(owner),
                  std::move(*notification))
        .Then(base::BindOnce(
            [](base::WeakPtr<XenonManager> manager, std::string container,
               uint64_t generation, ElectronWindowCallCallback callback,
               base::expected<bool, Broker::Error> result) {
              if (!manager || !manager->IsElectronIpcGenerationCurrent(
                                  container, generation)) {
                std::move(callback).Run(
                    base::Value(),
                    "ERR_ABORTED: single-instance owner stopped");
              } else if (!result.has_value()) {
                std::move(callback).Run(
                    base::Value(),
                    result.error().code + ": " + result.error().message);
              } else {
                std::move(callback).Run(base::Value(*result), "");
              }
            },
            weak_factory_.GetWeakPtr(), container, generation,
            std::move(callback)));
    return;
  }
  if (window_id == 0 &&
      (command == "content-tracing" || command == "desktop-capturer")) {
    const std::string container = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(container);
    if (shutting_down_for_process_exit_ || !connection ||
        !connection->remote.is_bound() ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(container)) {
      std::move(callback).Run({},
                              "ERR_ACCESS_DENIED: stale Electron API owner");
      return;
    }
    auto reply = base::BindOnce(
        [](ElectronWindowCallCallback callback, base::Value result,
           std::string error) {
          std::move(callback).Run(std::move(result), error);
        },
        std::move(callback));
    const auto owner = SingleInstanceOwner(container, connection->generation);
    if (command == "content-tracing") {
      ipc::CallElectronTracing(owner, arguments, std::move(reply));
    } else {
      ipc::GetElectronDesktopSources(owner, arguments, std::move(reply));
    }
    return;
  }
  if (window_id == 0 && command == "electron-sandbox") {
    const std::string container = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(container);
    if (shutting_down_for_process_exit_ || !connection ||
        !connection->remote.is_bound() ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(container)) {
      std::move(callback).Run(
          base::Value(), "ERR_ACCESS_DENIED: stale Electron sandbox owner");
      return;
    }
    const auto* options = arguments.GetIfDict();
    const auto* action = options ? options->FindString("action") : nullptr;
    if (action && *action == "enable") {
      const auto owner = SingleInstanceOwner(container, connection->generation);
      auto enabled = GetElectronSandboxPolicy()->Enable(
          owner, *base::CommandLine::ForCurrentProcess());
      if (!enabled.has_value()) {
        std::move(callback).Run(base::Value(), enabled.error());
        return;
      }
      ipc::EnableSandboxInElectronAppCommandLine(owner);
      std::move(callback).Run(base::Value(true), "");
      return;
    }
    std::move(callback).Run(
        base::Value(),
        "ERR_INVALID_ARG_TYPE: invalid Electron sandbox request");
    return;
  }
  if (window_id == 0 &&
      (command == "powerSaveBlocker" || command == "safeStorage")) {
    const std::string container = browser_observer_receivers_.current_context();
    auto* connection = FindContainerService(container);
    if (shutting_down_for_process_exit_ || !connection ||
        !connection->remote.is_bound() ||
        connection->observer_receiver_id !=
            browser_observer_receivers_.current_receiver() ||
        connection->generation != service_generation(container)) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale Electron API owner");
      return;
    }
    const std::string owner =
        SingleInstanceOwner(container, connection->generation);
    auto reply = base::BindOnce(
        [](ElectronWindowCallCallback callback,
           base::expected<base::Value, std::string> result) {
          if (result.has_value()) {
            std::move(callback).Run(std::move(*result), "");
          } else {
            std::move(callback).Run(base::Value(), result.error());
          }
        },
        std::move(callback));
    if (command == "powerSaveBlocker") {
      if (!electron_power_save_blocker_) {
        electron_power_save_blocker_ =
            std::make_unique<ipc::ElectronPowerSaveBlocker>(base::BindRepeating(
                [](device::mojom::WakeLockType type,
                   mojo::PendingReceiver<device::mojom::WakeLock> receiver) {
                  mojo::Remote<device::mojom::WakeLockProvider> provider;
                  content::GetDeviceService().BindWakeLockProvider(
                      provider.BindNewPipeAndPassReceiver());
                  provider->GetWakeLockWithoutContext(
                      type, device::mojom::WakeLockReason::kOther,
                      "Electron application", std::move(receiver));
                }));
      }
      std::move(reply).Run(
          electron_power_save_blocker_->Call(owner, arguments));
      return;
    }
    const auto* options = arguments.GetIfDict();
    const auto* method = options ? options->FindString("method") : nullptr;
    if (method &&
        (*method == "isAsyncEncryptionAvailable" ||
         *method == "encryptStringAsync" || *method == "decryptStringAsync")) {
      GetElectronSafeStorage()->CallAsync(owner, std::move(arguments),
                                          std::move(reply));
    } else {
      std::move(reply).Run(GetElectronSafeStorage()->Call(owner, arguments));
    }
    return;
  }
  if (window_id == 0 && command == "electron-api") {
    ipc::CallElectronApi(std::move(arguments),
                         base::BindOnce(
                             [](ElectronWindowCallCallback callback,
                                ipc::mojom::IpcResultPtr reply) {
                               std::move(callback).Run(std::move(reply->value),
                                                       reply->error);
                             },
                             std::move(callback)));
    return;
  }
  base::Value result;
  std::string error;
  std::string container;
  if (!AuthorizeElectronWindowOperation(window_id, &container, &error)) {
    std::move(callback).Run(base::Value(), error);
    return;
  }
  if (window_id != 0 && command == "load-url") {
    const auto* args = arguments.GetIfDict();
    const auto* url = args ? args->FindString("url") : nullptr;
    if (!url) {
      std::move(callback).Run(base::Value(),
                              "ERR_INVALID_ARG_TYPE: URL must be a string");
      return;
    }
    XenonElectronWindowHost::GetInstance()->LoadURLAsync(
        window_id, *url,
        base::BindOnce(
            [](ElectronWindowCallCallback callback, base::Value result,
               std::string error) {
              std::move(callback).Run(std::move(result), error);
            },
            std::move(callback)));
    return;
  }
  if (window_id != 0 && command == "wait-for-main-frame-runtime") {
    XenonElectronWindowHost::GetInstance()->WaitForMainFrameRuntime(
        window_id, base::BindOnce(
                       [](ElectronWindowCallCallback callback,
                          base::Value value, std::string error) {
                         std::move(callback).Run(std::move(value), error);
                       },
                       std::move(callback)));
    return;
  }
  XenonElectronWindowHost::GetInstance()->Call(window_id, command, arguments,
                                               &result, &error);
  std::move(callback).Run(std::move(result), error);
}

void XenonManager::ElectronStorageWindowCall(
    const std::string& authenticated_container,
    uint64_t expected_generation,
    mojo::ReceiverId authenticated_receiver,
    const std::string& command,
    base::Value arguments,
    ElectronWindowCallCallback callback) {
  auto* current = FindContainerService(authenticated_container);
  if (shutting_down_for_process_exit_ || !current ||
      current->generation != expected_generation ||
      !current->remote.is_bound() || !current->browser_context ||
      current->observer_receiver_id != authenticated_receiver) {
    std::move(callback).Run({},
                            "ERR_ABORTED: application storage owner stopped");
    return;
  }
  if (command == "electron-session") {
    const std::string container_id = authenticated_container;
    auto* connection = FindContainerService(container_id);
    if (!connection ||
        connection->observer_receiver_id != authenticated_receiver ||
        connection->generation != service_generation(container_id)) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale session owner");
      return;
    }
    if (arguments.is_dict() && arguments.GetDict().FindString("method") &&
        *arguments.GetDict().FindString("method") == "create-window") {
      const auto& args = arguments.GetDict();
      const auto* options = args.FindDict("options");
      const auto* partition = args.FindString("partition");
      if (!options || !partition) {
        std::move(callback).Run(base::Value(),
                                "Invalid session window options");
        return;
      }
      int32_t id = 0;
      uint64_t hwnd = 0;
      std::string error;
      const std::string* title = options->FindString("title");
      const std::string* title_bar_style = options->FindString("titleBarStyle");
      const auto parent_option = options->FindInt("parentId");
      const auto initial_paint = options->FindBool("paintWhenInitiallyHidden");
      if (options->contains("paintWhenInitiallyHidden") && !initial_paint) {
        std::move(callback).Run(
            {},
            "ERR_INVALID_ARG_TYPE: paintWhenInitiallyHidden must be a boolean");
        return;
      }
      if (options->contains("parentId") && !parent_option) {
        std::move(callback).Run(
            {}, "ERR_INVALID_ARG_TYPE: parentId must be an integer");
        return;
      }
      const int parent_id = parent_option.value_or(0);
      if (parent_id) {
        views::Widget* parent = nullptr;
        content::WebContents* contents = nullptr;
        if (!XenonElectronWindowHost::GetInstance()->GetOwnedWindowTarget(
                container_id, parent_id, &parent, &contents, &error) ||
            !contents ||
            contents->GetBrowserContext() !=
                connection->browser_context.get()) {
          std::move(callback).Run(
              {}, "ERR_ACCESS_DENIED: parent window is unavailable");
          return;
        }
      }
      XenonElectronWindowHost::GetInstance()->CreateHostedWindow(
          connection->browser_context.get(),
          options->FindInt("width").value_or(800),
          options->FindInt("height").value_or(600),
          options->FindBool("show").value_or(true),
          options->FindBool("frame").value_or(true),
          options->FindBool("transparent").value_or(false), parent_id,
          title ? *title : "Electron", container_id, &id, &hwnd, &error,
          *partition, options->FindBool("hasShadow").value_or(true),
          title_bar_style ? *title_bar_style : "default",
          initial_paint.value_or(true));
      std::move(callback).Run(base::Value(base::DictValue().Set("id", id).Set(
                                  "hwnd", base::NumberToString(hwnd))),
                              error);
      return;
    }
    ipc::CallElectronSession(
        connection->browser_context.get(), container_id, std::move(arguments),
        std::move(callback),
        base::BindRepeating(
            [](const std::string& owner, base::Value event) {
              XenonManager::GetInstance()->DispatchElectronSessionEvent(
                  owner, std::move(event));
            },
            container_id));
    return;
  }
  if (command == "web-request-handler") {
    const std::string container_id = authenticated_container;
    auto* connection = FindContainerService(container_id);
    std::string error;
    if (!connection ||
        connection->observer_receiver_id != authenticated_receiver ||
        connection->generation != service_generation(container_id)) {
      error = "ERR_ACCESS_DENIED: stale webRequest owner";
    } else {
      ipc::SetElectronWebRequestHandler(
          connection->browser_context.get(), container_id,
          connection->generation, arguments,
          base::BindRepeating(
              [](XenonManager* manager, const std::string& container,
                 uint64_t generation, base::Value request,
                 ipc::ElectronWebRequestReply reply) {
                auto* current = manager->FindContainerService(container);
                if (!current || !current->remote.is_bound() ||
                    current->generation != generation) {
                  std::move(reply).Run(base::Value(),
                                       "ERR_ABORTED: webRequest owner stopped");
                  return;
                }
                current->remote->HandleElectronWebRequest(
                    container, std::move(request),
                    base::BindOnce(
                        [](ipc::ElectronWebRequestReply reply,
                           ipc::mojom::IpcResultPtr result) {
                          if (!result || !result->success) {
                            std::move(reply).Run(
                                base::Value(), result
                                                   ? result->error
                                                   : "ERR_ABORTED: webRequest "
                                                     "owner disconnected");
                            return;
                          }
                          std::move(reply).Run(std::move(result->value), "");
                        },
                        std::move(reply)));
              },
              base::Unretained(this), container_id),
          base::BindRepeating(
              [](XenonManager* manager, const std::string& container,
                 uint64_t generation, const std::string& request_id) {
                auto* current = manager->FindContainerService(container);
                if (current && current->remote.is_bound() &&
                    current->generation == generation) {
                  current->remote->CancelElectronWebRequest(container,
                                                            request_id);
                }
              },
              base::Unretained(this), container_id),
          base::BindRepeating(
              [](XenonManager* manager, const std::string& container,
                 uint64_t generation, const std::string& registration_id) {
                auto* current = manager->FindContainerService(container);
                if (current && current->remote.is_bound() &&
                    current->generation == generation) {
                  current->remote->ReleaseElectronWebRequestRegistration(
                      container, registration_id);
                }
              },
              base::Unretained(this), container_id),
          &error);
    }
    std::move(callback).Run(base::Value(), error);
    return;
  }
  if (command == "protocol-handler") {
    const std::string container_id = authenticated_container;
    auto* connection = FindContainerService(container_id);
    std::string error;
    if (!connection ||
        connection->observer_receiver_id != authenticated_receiver ||
        connection->generation != service_generation(container_id)) {
      error = "ERR_ACCESS_DENIED: stale protocol registration owner";
    } else {
      ipc::SetElectronProtocolHandler(connection->browser_context.get(),
                                      container_id, connection->generation,
                                      arguments, &error);
    }
    std::move(callback).Run(base::Value(), error);
    return;
  }
  if (command == "electron-view") {
    const std::string container = authenticated_container;
    auto* connection = FindContainerService(container);
    if (shutting_down_for_process_exit_ || !connection ||
        !connection->browser_context || !connection->remote.is_bound() ||
        connection->observer_receiver_id != authenticated_receiver ||
        connection->generation != service_generation(container)) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale Electron View owner");
      return;
    }
    const auto generation = connection->generation;
    auto result = XenonElectronViewHost::GetInstance()->Call(
        connection->browser_context.get(), container,
        SingleInstanceOwner(container, generation), arguments,
        base::BindRepeating(
            [](base::WeakPtr<XenonManager> manager, std::string container,
               uint64_t generation, base::Value event) {
              if (!manager || manager->shutting_down_for_process_exit_) {
                return;
              }
              auto* current = manager->FindContainerService(container);
              if (current && current->generation == generation &&
                  current->remote.is_bound()) {
                current->remote->DispatchElectronWindowEvent(0, "electron-view",
                                                             std::move(event));
              }
            },
            weak_factory_.GetWeakPtr(), container, generation));
    if (result.has_value()) {
      std::move(callback).Run(std::move(*result), "");
    } else {
      std::move(callback).Run(base::Value(), result.error());
    }
    return;
  }
  if (command == "notification") {
    const std::string container = authenticated_container;
    auto* connection = FindContainerService(container);
    const auto config = last_ipc_configs_.find(container);
    if (shutting_down_for_process_exit_ || !connection ||
        !connection->browser_context ||
        connection->observer_receiver_id != authenticated_receiver ||
        connection->generation != service_generation(container) ||
        config == last_ipc_configs_.end() || !config->second) {
      std::move(callback).Run(base::Value(),
                              "ERR_ACCESS_DENIED: stale notification owner");
      return;
    }
    const auto generation = connection->generation;
    auto result = ipc::CallElectronNotification(
        Profile::FromBrowserContext(connection->browser_context.get()),
        SingleInstanceOwner(container, generation), config->second->app_name,
        arguments,
        base::BindRepeating(
            [](base::WeakPtr<XenonManager> manager, std::string container,
               uint64_t generation, base::Value event) {
              if (!manager || manager->shutting_down_for_process_exit_) {
                return;
              }
              auto* current = manager->FindContainerService(container);
              if (current && current->generation == generation &&
                  current->remote.is_bound()) {
                current->remote->DispatchElectronWindowEvent(0, "notification",
                                                             std::move(event));
              }
            },
            weak_factory_.GetWeakPtr(), container, generation));
    if (result.has_value()) {
      std::move(callback).Run(std::move(*result), "");
    } else {
      std::move(callback).Run(base::Value(), result.error());
    }
    return;
  }
  std::move(callback).Run({}, "ERR_INVALID_ARG_VALUE: unknown storage command");
}

void XenonManager::CloseElectronWindow(int32_t window_id) {
  std::string container;
  std::string error;
  if (!AuthorizeElectronWindowOperation(window_id, &container, &error)) {
    LOG(WARNING) << error;
    return;
  }
  XenonElectronWindowHost::GetInstance()->Close(window_id);
}

#endif

void XenonManager::DispatchElectronSessionEvent(const std::string& container_id,
                                                base::Value arguments) {
  auto* connection = FindContainerService(container_id);
  if (connection && connection->remote.is_bound()) {
    connection->remote->DispatchElectronWindowEvent(0, "__xenon-session-event",
                                                    std::move(arguments));
  }
}

void XenonManager::FilterElectronKeyboardEvent(
    const std::string& container_id,
    uint64_t generation,
    int32_t window_id,
    base::Value input,
    base::OnceCallback<void(bool)> callback) {
  auto* connection = FindContainerService(container_id);
  if (!connection || !connection->remote.is_bound() ||
      connection->generation != generation) {
    std::move(callback).Run(true);
    return;
  }
  connection->remote->FilterElectronKeyboardEvent(
      container_id, window_id, std::move(input),
      base::BindOnce(
          [](base::WeakPtr<XenonManager> manager, std::string container,
             uint64_t expected_generation, base::OnceCallback<void(bool)> done,
             bool prevented) {
            std::move(done).Run(prevented || !manager ||
                                !manager->IsElectronIpcGenerationCurrent(
                                    container, expected_generation));
          },
          weak_factory_.GetWeakPtr(), container_id, generation,
          std::move(callback)));
}

void XenonManager::DispatchElectronWindowEvent(int32_t window_id,
                                               const std::string& event_name,
                                               base::Value arguments) {
  for (auto& [container_id, connection] : container_services_) {
    if (connection && connection->remote.is_bound()) {
      connection->remote->DispatchElectronWindowEvent(window_id, event_name,
                                                      arguments.Clone());
    }
  }
}

}  // namespace xenon
