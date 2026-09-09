// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_service_impl.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/task/sequenced_task_runner.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/cpp/wrapper_shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"
#include "xenon_overlay/services/xenon_net_pipe_bridge.h"
#include "xenon_overlay/services/xenon_node_executor.h"

namespace xenon {

namespace {

constexpr char kDefaultIpcContainerId[] = "default";
// Native child processes started by hosted applications inherit this value.
// It lets a native component that derives resources from its host executable
// resolve the hosted application's private resource root without duplicating
// that application's files beside xenon.exe.
constexpr char kHostedAppDirectoryEnvironmentVariable[] =
    "XENON_HOSTED_APP_DIR";
constexpr size_t kMaxPendingIpcCallsPerContainer = 1024;
constexpr char kNodeAddonInvokeExportChannel[] =
    "__xenon:node-addon:invoke-export";
constexpr char kNodeAddonConstructExportChannel[] =
    "__xenon:node-addon:construct-export";
constexpr char kNodeAddonInvokeInstanceChannel[] =
    "__xenon:node-addon:invoke-instance";
constexpr char kNodeAddonCallbackChannel[] =
    "__xenon:node-addon:callback";
constexpr char kNodeAddonCallbackReleasedChannel[] =
    "__xenon:node-addon:callback-released";

bool IsRendererNodeAddonInvokeChannel(const std::string& channel) {
  return channel == kNodeAddonInvokeExportChannel ||
         channel == kNodeAddonConstructExportChannel ||
         channel == kNodeAddonInvokeInstanceChannel;
}

std::string NormalizeIpcContainerId(const std::string& container_id) {
  return container_id.empty() ? kDefaultIpcContainerId : container_id;
}

std::vector<mojom::NodeInvokeCallResultPtr> MakeInvokeFailures(
    size_t count,
    const std::string& error_msg) {
  std::vector<mojom::NodeInvokeCallResultPtr> results;
  results.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    auto result = mojom::NodeInvokeCallResult::New();
    result->success = false;
    result->error_msg = error_msg;
    results.push_back(std::move(result));
  }
  return results;
}

ipc::mojom::IpcResultPtr MakeIpcFailure(const std::string& error) {
  auto result = ipc::mojom::IpcResult::New();
  result->success = false;
  result->error = error;
  return result;
}

base::Value NodeExportInfoToValue(const mojom::NodeExportInfoPtr& info) {
  if (!info) {
    return base::Value();
  }
  base::DictValue value;
  value.Set("name", info->name);
  value.Set("kind", info->kind);
  value.Set("enumerable", info->enumerable);
  value.Set("writable", info->writable);
  value.Set("hasValue", info->has_value);
  value.Set("value", info->value.Clone());
  base::ListValue children;
  for (const auto& child : info->children) {
    children.Append(NodeExportInfoToValue(child));
  }
  value.Set("children", std::move(children));
  base::ListValue prototype;
  for (const auto& member : info->prototype) {
    prototype.Append(NodeExportInfoToValue(member));
  }
  value.Set("prototype", std::move(prototype));
  return base::Value(std::move(value));
}

std::vector<mojom::NodeInvokeArgPtr> ListValueToInvokeArgs(
    const base::Value& arguments) {
  std::vector<mojom::NodeInvokeArgPtr> invoke_args;
  if (!arguments.is_list()) {
    return invoke_args;
  }
  invoke_args.reserve(arguments.GetList().size());
  for (const base::Value& argument : arguments.GetList()) {
    auto invoke_arg = mojom::NodeInvokeArg::New();
    invoke_arg->is_callback = false;
    invoke_arg->callback_id = 0;
    invoke_arg->value = argument.Clone();
    invoke_args.push_back(std::move(invoke_arg));
  }
  return invoke_args;
}

ipc::mojom::IpcResultPtr MakeNativeInvokeSuccess(base::Value value) {
  auto result = ipc::mojom::IpcResult::New();
  result->success = true;
  result->value = std::move(value);
  return result;
}

}  // namespace

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

void XenonServiceImpl::SetNodeAddonObserver(
    const std::string& context_id,
    int32_t client_id,
    mojo::PendingRemote<mojom::NodeAddonObserver> observer) {
  const NodeClientKey key =
      {NormalizeIpcContainerId(context_id), client_id};
  node_addon_observers_.erase(key);
  if (!observer) {
    return;
  }
  mojo::Remote<mojom::NodeAddonObserver>& remote =
      node_addon_observers_[key];
  remote.Bind(std::move(observer));
  remote.set_disconnect_handler(
      base::BindOnce(&XenonServiceImpl::OnNodeAddonObserverDisconnected,
                     weak_factory_.GetWeakPtr(), key.first, client_id));
  FlushPendingNodeCallbacks(key.first, client_id);
}

void XenonServiceImpl::BindNodeAddonHost(
    const std::string& context_id,
    mojo::PendingReceiver<ipc::mojom::NodeAddonHost> receiver) {
  node_addon_host_receivers_.Add(
      this, std::move(receiver), NormalizeIpcContainerId(context_id));
}

void XenonServiceImpl::DispatchElectronWindowEvent(
    int32_t window_id,
    const std::string& event_name,
    base::Value arguments) {
  for (auto& item : ipc_main_containers_) {
    item.second->DispatchWindowEvent(window_id, event_name,
                                     arguments.Clone());
  }
}

void XenonServiceImpl::InitializeElectronIpc(
    ipc::mojom::IpcMainConfigPtr config,
    InitializeElectronIpcCallback callback) {
  const std::string container_id =
      NormalizeIpcContainerId(config ? config->container_id : std::string());
  auto existing = ipc_main_containers_.find(container_id);
  if (existing != ipc_main_containers_.end() &&
      existing->second->is_initialized()) {
    std::move(callback).Run(true, std::string());
    return;
  }

  ipc_main_ready_containers_.erase(container_id);

  // The Xenon executable is the process image for the generic container, but
  // native Electron components may start child processes that locate their
  // resources relative to the hosted application's executable directory.
  // Propagate the hosted directory through the inherited environment. Each
  // Electron container has its own service process, so this remains scoped to
  // the application being initialized.
  base::FilePath hosted_directory;
  // Resource location is independent of the executable used as the process
  // image. Preserve executable-relative resolution for existing configs.
  if (config && !config->runtime_directory.empty()) {
    hosted_directory =
        base::FilePath::FromUTF8Unsafe(config->runtime_directory);
  } else if (config && !config->executable_path.empty()) {
    hosted_directory =
        base::FilePath::FromUTF8Unsafe(config->executable_path).DirName();
  } else if (config && !config->app_path.empty()) {
    hosted_directory = base::FilePath::FromUTF8Unsafe(config->app_path);
  }
  {
    std::unique_ptr<base::Environment> environment =
        base::Environment::Create();
    if (!hosted_directory.empty()) {
      if (!environment->SetVar(kHostedAppDirectoryEnvironmentVariable,
                               hosted_directory.AsUTF8Unsafe())) {
        LOG(WARNING) << "Failed to propagate hosted application directory: "
                     << hosted_directory.AsUTF8Unsafe();
      }
    } else {
      environment->UnSetVar(kHostedAppDirectoryEnvironmentVariable);
    }
  }

  auto container = std::make_unique<ipc::XenonIpcMainContainer>();
  XenonNodeExecutor* executor = EnsureNodeExecutor(container_id);
  executor->SetRuntimeDirectory(hosted_directory);
  ipc::XenonIpcMainContainer::NativeAddonHooks hooks;
  hooks.load =
      base::BindRepeating(&XenonNodeExecutor::LoadAddonFromCurrentThread,
                          base::Unretained(executor));
  hooks.invoke =
      base::BindRepeating(&XenonNodeExecutor::InvokeExportFromCurrentThread,
                          base::Unretained(executor));
  hooks.construct =
      base::BindRepeating(&XenonNodeExecutor::ConstructExportFromCurrentThread,
                          base::Unretained(executor));
  hooks.invoke_instance = base::BindRepeating(
      &XenonNodeExecutor::InvokeInstanceFromCurrentThread,
      base::Unretained(executor));
  container->SetNativeAddonHooks(std::move(hooks));
#if BUILDFLAG(ENABLE_XENON_BROWSER_OBSERVER)
  ipc::XenonIpcMainContainer::WindowHooks window_hooks;
  window_hooks.create = base::BindRepeating(
      [](XenonServiceImpl* self, std::string container_id, int width,
         int height, bool show, bool frame, bool transparent,
         int32_t parent_id, const std::string& title, int32_t* window_id,
         uint64_t* hwnd, std::string* error) {
        if (!self->browser_observer_.is_bound()) {
          *error = "Browser observer is not bound";
          return false;
        }
        return self->browser_observer_->CreateElectronWindow(
            width, height, show, frame, transparent, parent_id, title,
            container_id, window_id, hwnd, error);
      },
      base::Unretained(this), container_id);
  window_hooks.load_url = base::BindRepeating(
      [](XenonServiceImpl* self, int32_t window_id, const std::string& url) {
        if (self->browser_observer_.is_bound()) {
          self->browser_observer_->LoadElectronWindowURL(window_id, url);
        }
      },
      base::Unretained(this));
  window_hooks.set_visible = base::BindRepeating(
      [](XenonServiceImpl* self, int32_t window_id, bool visible) {
        if (self->browser_observer_.is_bound()) {
          self->browser_observer_->SetElectronWindowVisible(window_id, visible);
        }
      },
      base::Unretained(this));
  window_hooks.call = base::BindRepeating(
      [](XenonServiceImpl* self, int32_t window_id,
         const std::string& command, const base::Value& arguments,
         base::Value* result, std::string* error) {
        if (!self->browser_observer_.is_bound() || !result || !error) {
          if (error) {
            *error = "Browser observer is not bound";
          }
          return false;
        }
        return self->browser_observer_->ElectronWindowCall(
            window_id, command, arguments.Clone(), result, error);
      },
      base::Unretained(this));
  window_hooks.close = base::BindRepeating(
      [](XenonServiceImpl* self, int32_t window_id) {
        if (self->browser_observer_.is_bound()) {
          self->browser_observer_->CloseElectronWindow(window_id);
        }
      },
      base::Unretained(this));
  window_hooks.show_open_dialog = base::BindRepeating(
      [](XenonServiceImpl* self, const std::string& title, bool directory,
         bool allow_multi, const std::vector<std::string>& extensions,
         std::vector<std::string>* paths) {
        if (!self->browser_observer_.is_bound() || !paths) {
          return false;
        }
        return self->browser_observer_->ShowElectronOpenDialog(
            title, directory, allow_multi, extensions, paths);
      },
      base::Unretained(this));
  container->SetWindowHooks(std::move(window_hooks));
#endif
  bool initialized = false;
  if (config && !config->embedded_main_source.empty()) {
    std::vector<std::pair<std::string, std::string>> renderer_url_mappings;
    renderer_url_mappings.reserve(config->renderer_url_mappings.size());
    for (const auto& mapping : config->renderer_url_mappings) {
      if (mapping) {
        renderer_url_mappings.emplace_back(mapping->source_path_prefix,
                                           mapping->target_base_url);
      }
    }
    ipc::XenonIpcMainContainer::EmbeddedMainModule main_module{
        .source = std::move(config->embedded_main_source),
        .virtual_path =
            base::FilePath::FromUTF8Unsafe(config->virtual_main_path),
        .app_path = base::FilePath::FromUTF8Unsafe(config->app_path),
        .executable_path =
            base::FilePath::FromUTF8Unsafe(config->executable_path),
        .app_name = std::move(config->app_name),
        .app_version = std::move(config->app_version),
        .default_user_agent = std::move(config->default_user_agent),
        .renderer_url_mappings = std::move(renderer_url_mappings),
        .renderer_base_url = std::move(config->renderer_base_url),
    };
    initialized = container->Initialize(std::move(main_module));
  } else {
    initialized = container->Initialize();
  }
  if (!initialized) {
    std::string error = container->startup_error();
    std::move(callback).Run(false, std::move(error));
    return;
  }
  ipc_main_containers_[container_id] = std::move(container);
  std::move(callback).Run(true, std::string());
  // whenReady() must run after this Mojo reply, otherwise a sync
  // BrowserWindow constructor would deadlock the browser UI thread.
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&XenonServiceImpl::MarkElectronIpcReady,
                     weak_factory_.GetWeakPtr(), container_id));
}

bool XenonServiceImpl::IsElectronIpcReady(
    const std::string& container_id) const {
  return ipc_main_ready_containers_.contains(
      NormalizeIpcContainerId(container_id));
}

void XenonServiceImpl::MarkElectronIpcReady(
    const std::string& container_id) {
  const std::string normalized_id = NormalizeIpcContainerId(container_id);
  auto container = ipc_main_containers_.find(normalized_id);
  if (container == ipc_main_containers_.end()) {
    return;
  }

  // MarkAppReady runs app ready/whenReady listeners and drains their V8
  // microtasks. Only after that checkpoint can renderer IPC be dispatched
  // without racing listener registration in the main module.
  container->second->MarkAppReady();
  ipc_main_ready_containers_.insert(normalized_id);
  const auto pending = pending_ipc_calls_.find(normalized_id);
  const size_t pending_count =
      pending == pending_ipc_calls_.end() ? 0u : pending->second.size();
  LOG(INFO) << "Utility ipcMain container '" << normalized_id
            << "' ready; flushing " << pending_count << " queued IPC calls";
  FlushPendingElectronIpc(normalized_id);
}

void XenonServiceImpl::FlushPendingElectronIpc(
    const std::string& container_id) {
  auto container = ipc_main_containers_.find(container_id);
  auto pending = pending_ipc_calls_.find(container_id);
  if (container == ipc_main_containers_.end() ||
      pending == pending_ipc_calls_.end()) {
    return;
  }

  std::deque<PendingIpcCall> calls = std::move(pending->second);
  pending_ipc_calls_.erase(pending);
  for (PendingIpcCall& call : calls) {
    if (auto* send = std::get_if<PendingIpcSend>(&call)) {
      container->second->Send(send->endpoint_id, send->channel,
                              std::move(send->arguments));
      continue;
    }
    auto& invoke = std::get<PendingIpcInvoke>(call);
    container->second->Invoke(invoke.endpoint_id, invoke.channel,
                              std::move(invoke.arguments),
                              std::move(invoke.callback));
  }
}

void XenonServiceImpl::RegisterElectronIpcRenderer(
    const std::string& container_id,
    const std::string& endpoint_id,
    mojo::PendingRemote<ipc::mojom::IpcRenderer> renderer,
    int32_t process_id,
    int32_t frame_id,
    int32_t window_id) {
  auto it = ipc_main_containers_.find(NormalizeIpcContainerId(container_id));
  if (it != ipc_main_containers_.end()) {
    it->second->AddRenderer(endpoint_id, std::move(renderer), process_id,
                            frame_id, window_id);
  }
}

void XenonServiceImpl::RemoveElectronIpcRenderer(
    const std::string& container_id,
    const std::string& endpoint_id) {
  const std::string normalized_id = NormalizeIpcContainerId(container_id);
  RemoveRendererNodeClient(normalized_id, endpoint_id);
  if (net_pipe_bridge_) {
    net_pipe_bridge_->RemoveEndpoint(normalized_id, endpoint_id);
  }
  auto it = ipc_main_containers_.find(normalized_id);
  if (it != ipc_main_containers_.end()) {
    it->second->RemoveRenderer(endpoint_id);
  }

  auto pending = pending_ipc_calls_.find(normalized_id);
  if (pending == pending_ipc_calls_.end()) {
    return;
  }
  for (auto call = pending->second.begin(); call != pending->second.end();) {
    const std::string& pending_endpoint =
        std::holds_alternative<PendingIpcSend>(*call)
            ? std::get<PendingIpcSend>(*call).endpoint_id
            : std::get<PendingIpcInvoke>(*call).endpoint_id;
    if (pending_endpoint != endpoint_id) {
      ++call;
      continue;
    }
    if (auto* invoke = std::get_if<PendingIpcInvoke>(&*call)) {
      std::move(invoke->callback)
          .Run(MakeIpcFailure(
              "Renderer disconnected before ipcMain became ready"));
    }
    call = pending->second.erase(call);
  }
}

void XenonServiceImpl::ElectronIpcSend(const std::string& container_id,
                                       const std::string& endpoint_id,
                                       const std::string& channel,
                                       base::Value arguments) {
  const std::string normalized_id = NormalizeIpcContainerId(container_id);
  if (HandleNetPipeMessage(normalized_id, endpoint_id, channel, arguments)) {
    return;
  }
  if (channel == "ipc-renderer-ready" ||
      channel == "ipc-renderer-client-ready" ||
      channel == "ipc-renderer-server-ready" ||
      channel.starts_with("__xenon:net:")) {
    LOG(INFO) << "Renderer IPC send container='" << normalized_id
              << "' endpoint='" << endpoint_id << "' channel='" << channel
              << "' ready=" << IsElectronIpcReady(normalized_id);
  }
  auto it = ipc_main_containers_.find(normalized_id);
  if (it != ipc_main_containers_.end()) {
    if (!IsElectronIpcReady(normalized_id)) {
      auto& pending = pending_ipc_calls_[normalized_id];
      if (pending.size() >= kMaxPendingIpcCallsPerContainer) {
        LOG(ERROR) << "Dropping renderer IPC send while container '"
                   << normalized_id << "' starts: pending queue is full";
        return;
      }
      pending.emplace_back(PendingIpcSend{
          endpoint_id, channel, std::move(arguments)});
      LOG(INFO) << "Queued renderer IPC send '" << channel
                << "' until ipcMain container '" << normalized_id
                << "' becomes ready";
      return;
    }
    it->second->Send(endpoint_id, channel, std::move(arguments));
  }
}

XenonNetPipeBridge* XenonServiceImpl::EnsureNetPipeBridge() {
  if (!net_pipe_bridge_) {
    net_pipe_bridge_ = std::make_unique<XenonNetPipeBridge>(
        base::BindRepeating(&XenonServiceImpl::DispatchNetPipeEvent,
                            weak_factory_.GetWeakPtr()));
  }
  return net_pipe_bridge_.get();
}

bool XenonServiceImpl::HandleNetPipeMessage(
    const std::string& container_id,
    const std::string& endpoint_id,
    const std::string& channel,
    const base::Value& arguments) {
  if (!channel.starts_with("__xenon:net:")) {
    return false;
  }
  if (!arguments.is_list() || arguments.GetList().empty() ||
      !arguments.GetList().front().is_dict()) {
    return false;
  }
  const base::DictValue& message = arguments.GetList().front().GetDict();
  if (channel == "__xenon:net:listen") {
    const std::string* server_id = message.FindString("serverId");
    const std::string* path = message.FindString("path");
    if (!server_id || !path) {
      return true;
    }
    std::string error;
    if (!EnsureNetPipeBridge()->Listen(container_id, endpoint_id, *server_id,
                                       *path, &error)) {
      base::DictValue payload;
      payload.Set("serverId", *server_id);
      payload.Set("code", error.empty() ? "EADDRINUSE" : error);
      DispatchNetPipeEvent(container_id, endpoint_id, "__xenon:net:error",
                           base::Value(std::move(payload)));
      return true;
    }
    base::DictValue payload;
    payload.Set("serverId", *server_id);
    DispatchNetPipeEvent(container_id, endpoint_id,
                         "__xenon:net:listening",
                         base::Value(std::move(payload)));
    return true;
  }
  if (!net_pipe_bridge_) {
    return false;
  }
  if (channel == "__xenon:net:unlisten") {
    const std::string* server_id = message.FindString("serverId");
    return server_id && net_pipe_bridge_->CloseServer(*server_id);
  }
  if (channel == "__xenon:net:connect") {
    const std::string* from_id = message.FindString("fromId");
    const std::string* path = message.FindString("path");
    if (!from_id || !path) {
      return true;
    }
    if (!net_pipe_bridge_->HasListenerForPath(*path)) {
      return false;
    }
    LOG(INFO) << "Named-pipe connect path='" << *path << "' from='" << *from_id
              << "' endpoint='" << endpoint_id << "'";
    std::string error;
    if (!net_pipe_bridge_->Connect(container_id, endpoint_id, *from_id, *path,
                                   &error)) {
      base::DictValue payload;
      payload.Set("toId", *from_id);
      payload.Set("code", error.empty() ? "ECONNREFUSED" : error);
      payload.Set("path", *path);
      DispatchNetPipeEvent(container_id, endpoint_id, "__xenon:net:error",
                           base::Value(std::move(payload)));
    }
    return true;
  }
  const std::string* socket_id = message.FindString("toId");
  if (!socket_id || !net_pipe_bridge_->HasSocket(*socket_id)) {
    return false;
  }
  if (channel == "__xenon:net:data") {
    const base::DictValue* wire = message.FindDict("wire");
    std::string error;
    if (!wire || !net_pipe_bridge_->Write(*socket_id, *wire, &error)) {
      return false;
    }
    if (!error.empty()) {
      base::DictValue payload;
      payload.Set("toId", *socket_id);
      payload.Set("code", error);
      DispatchNetPipeEvent(container_id, endpoint_id, "__xenon:net:error",
                           base::Value(std::move(payload)));
    }
    return true;
  }
  if (channel == "__xenon:net:close") {
    return net_pipe_bridge_->CloseSocket(*socket_id);
  }
  return false;
}

void XenonServiceImpl::DispatchNetPipeEvent(
    const std::string& container_id,
    const std::string& endpoint_id,
    const std::string& channel,
    base::Value payload) {
  auto container = ipc_main_containers_.find(container_id);
  if (container == ipc_main_containers_.end()) {
    return;
  }
  base::ListValue arguments;
  arguments.Append(std::move(payload));
  container->second->DispatchToRenderer(endpoint_id, channel,
                                        base::Value(std::move(arguments)));
}

void XenonServiceImpl::ElectronIpcInvoke(
    const std::string& container_id,
    const std::string& endpoint_id,
    const std::string& channel,
    base::Value arguments,
    ElectronIpcInvokeCallback callback) {
  const std::string normalized_id = NormalizeIpcContainerId(container_id);
  if (IsRendererNodeAddonInvokeChannel(channel)) {
    HandleRendererNodeAddonInvoke(normalized_id, endpoint_id, channel,
                                  std::move(arguments), std::move(callback));
    return;
  }
  auto it = ipc_main_containers_.find(normalized_id);
  if (it == ipc_main_containers_.end()) {
    std::move(callback).Run(
        MakeIpcFailure("Utility ipcMain container is unavailable"));
    return;
  }
  if (!IsElectronIpcReady(normalized_id)) {
    auto& pending = pending_ipc_calls_[normalized_id];
    if (pending.size() >= kMaxPendingIpcCallsPerContainer) {
      std::move(callback).Run(
          MakeIpcFailure("ipcMain startup queue is full"));
      return;
    }
    pending.emplace_back(PendingIpcInvoke{
        endpoint_id, channel, std::move(arguments), std::move(callback)});
    LOG(INFO) << "Queued renderer IPC invoke '" << channel
              << "' until ipcMain container '" << normalized_id
              << "' becomes ready";
    return;
  }
  it->second->Invoke(endpoint_id, channel, std::move(arguments),
                     std::move(callback));
}

int32_t XenonServiceImpl::GetOrCreateRendererNodeClient(
    const std::string& context_id,
    const std::string& endpoint_id) {
  const RendererEndpointKey endpoint_key =
      {NormalizeIpcContainerId(context_id), endpoint_id};
  const auto existing = renderer_node_clients_.find(endpoint_key);
  if (existing != renderer_node_clients_.end()) {
    return existing->second;
  }
  const int32_t client_id = next_renderer_node_client_id_--;
  renderer_node_clients_.emplace(endpoint_key, client_id);
  renderer_node_endpoints_.emplace(
      NodeClientKey{endpoint_key.first, client_id}, endpoint_id);
  return client_id;
}

void XenonServiceImpl::RemoveRendererNodeClient(
    const std::string& context_id,
    const std::string& endpoint_id) {
  const RendererEndpointKey endpoint_key =
      {NormalizeIpcContainerId(context_id), endpoint_id};
  const auto client = renderer_node_clients_.find(endpoint_key);
  if (client == renderer_node_clients_.end()) {
    return;
  }
  const NodeClientKey client_key = {endpoint_key.first, client->second};
  renderer_node_endpoints_.erase(client_key);
  pending_node_callbacks_.erase(client_key);
  renderer_node_clients_.erase(client);
}

void XenonServiceImpl::HandleRendererNodeAddonInvoke(
    const std::string& context_id,
    const std::string& endpoint_id,
    const std::string& channel,
    base::Value arguments,
    ElectronIpcInvokeCallback callback) {
  const base::DictValue* request = nullptr;
  if (arguments.is_list() && !arguments.GetList().empty() &&
      arguments.GetList().front().is_dict()) {
    request = &arguments.GetList().front().GetDict();
  }
  const std::string* module_path =
      request ? request->FindString("modulePath") : nullptr;
  const base::Value* invoke_arguments =
      request ? request->Find("arguments") : nullptr;
  if (!request || !module_path || module_path->empty() ||
      !invoke_arguments || !invoke_arguments->is_list()) {
    std::move(callback).Run(
        MakeIpcFailure("Invalid renderer Node addon invocation"));
    return;
  }

  const int32_t client_id =
      GetOrCreateRendererNodeClient(context_id, endpoint_id);
  std::vector<mojom::NodeInvokeArgPtr> args =
      ListValueToInvokeArgs(*invoke_arguments);
  if (channel == kNodeAddonInvokeExportChannel) {
    const std::string* function_name = request->FindString("functionName");
    if (!function_name || function_name->empty()) {
      std::move(callback).Run(
          MakeIpcFailure("Native addon function name is required"));
      return;
    }
    InvokeFunction(
        context_id, client_id, *module_path, *function_name, std::move(args),
        base::BindOnce(
            [](ElectronIpcInvokeCallback callback, bool success,
               base::Value value,
               std::vector<mojom::NodeCallbackResultPtr> callback_results,
               const std::string& error) {
              std::move(callback).Run(
                  success ? MakeNativeInvokeSuccess(std::move(value))
                          : MakeIpcFailure(error.empty()
                                               ? "Native addon invocation failed"
                                               : error));
            },
            std::move(callback)));
    return;
  }

  if (channel == kNodeAddonConstructExportChannel) {
    const std::string* export_path = request->FindString("exportPath");
    if (!export_path || export_path->empty()) {
      std::move(callback).Run(
          MakeIpcFailure("Native addon constructor name is required"));
      return;
    }
    ConstructExport(
        context_id, client_id, *module_path, *export_path, std::move(args),
        base::BindOnce(
            [](ElectronIpcInvokeCallback callback, bool success,
               int32_t instance_id, const std::string& error) {
              std::move(callback).Run(
                  success
                      ? MakeNativeInvokeSuccess(base::Value(instance_id))
                      : MakeIpcFailure(error.empty()
                                           ? "Native addon construction failed"
                                           : error));
            },
            std::move(callback)));
    return;
  }

  const std::optional<int> instance_id = request->FindInt("instanceId");
  const std::string* method_name = request->FindString("methodName");
  if (!instance_id || !method_name || method_name->empty()) {
    std::move(callback).Run(
        MakeIpcFailure("Native addon instance and method are required"));
    return;
  }
  InvokeInstance(
      context_id, client_id, *module_path, *instance_id, *method_name,
      std::move(args),
      base::BindOnce(
          [](ElectronIpcInvokeCallback callback, bool success,
             base::Value value,
             std::vector<mojom::NodeCallbackResultPtr> callback_results,
             const std::string& error) {
            std::move(callback).Run(
                success ? MakeNativeInvokeSuccess(std::move(value))
                        : MakeIpcFailure(
                              error.empty()
                                  ? "Native addon instance invocation failed"
                                  : error));
          },
          std::move(callback)));
}

void XenonServiceImpl::ElectronIpcSendSync(
    const std::string& container_id,
    const std::string& endpoint_id,
    const std::string& channel,
    base::Value arguments,
    ElectronIpcSendSyncCallback callback) {
  const std::string normalized_id = NormalizeIpcContainerId(container_id);
  auto it = ipc_main_containers_.find(normalized_id);
  if (it == ipc_main_containers_.end()) {
    std::move(callback).Run(
        MakeIpcFailure("Utility ipcMain container is unavailable"));
    return;
  }
  if (!IsElectronIpcReady(normalized_id)) {
    std::move(callback).Run(
        MakeIpcFailure("Utility ipcMain container is still starting"));
    return;
  }
  std::move(callback).Run(it->second->SendSync(
      endpoint_id, channel, std::move(arguments)));
}

void XenonServiceImpl::RequireNodeModuleSync(
    const std::string& module_path,
    RequireNodeModuleSyncCallback callback) {
  EnsureNodeExecutor(node_addon_host_receivers_.current_context())->LoadAddon(
      module_path,
      base::BindOnce(
          [](RequireNodeModuleSyncCallback callback, bool success,
             const std::string& error,
             std::vector<mojom::NodeExportInfoPtr> exports) {
            if (!success) {
              std::move(callback).Run(MakeIpcFailure(
                  error.empty() ? "Native module load failed" : error));
              return;
            }
            base::ListValue values;
            for (const auto& info : exports) {
              values.Append(NodeExportInfoToValue(info));
            }
            std::move(callback).Run(
                MakeNativeInvokeSuccess(base::Value(std::move(values))));
          },
          std::move(callback)));
}

void XenonServiceImpl::InvokeNodeExportSync(
    const std::string& module_path,
    const std::string& function_name,
    base::Value arguments,
    InvokeNodeExportSyncCallback callback) {
  if (!arguments.is_list()) {
    std::move(callback).Run(
        MakeIpcFailure("Native addon arguments must be a list"));
    return;
  }
  EnsureNodeExecutor(node_addon_host_receivers_.current_context())
      ->InvokeFunction(
      module_path, function_name, /*client_id=*/0,
      ListValueToInvokeArgs(arguments),
      base::BindOnce(
          [](InvokeNodeExportSyncCallback callback, bool success,
             base::Value value,
             std::vector<mojom::NodeCallbackResultPtr> callback_results,
             const std::string& error) {
            if (!success) {
              std::move(callback).Run(MakeIpcFailure(
                  error.empty() ? "Native addon invocation failed" : error));
              return;
            }
            std::move(callback).Run(
                MakeNativeInvokeSuccess(std::move(value)));
          },
          std::move(callback)),
      /*allow_pending_promise=*/false);
}

void XenonServiceImpl::ConstructNodeExportSync(
    const std::string& module_path,
    const std::string& export_path,
    base::Value arguments,
    ConstructNodeExportSyncCallback callback) {
  if (!arguments.is_list()) {
    std::move(callback).Run(
        MakeIpcFailure("Native addon arguments must be a list"));
    return;
  }
  EnsureNodeExecutor(node_addon_host_receivers_.current_context())
      ->ConstructExport(
      module_path, export_path, /*client_id=*/0,
      ListValueToInvokeArgs(arguments),
      base::BindOnce(
          [](ConstructNodeExportSyncCallback callback, bool success,
             int32_t instance_id, const std::string& error) {
            if (!success) {
              std::move(callback).Run(MakeIpcFailure(
                  error.empty() ? "Native construct failed" : error));
              return;
            }
            std::move(callback).Run(
                MakeNativeInvokeSuccess(base::Value(instance_id)));
          },
          std::move(callback)));
}

void XenonServiceImpl::InvokeNodeInstanceSync(
    const std::string& module_path,
    int32_t instance_id,
    const std::string& method_name,
    base::Value arguments,
    InvokeNodeInstanceSyncCallback callback) {
  if (!arguments.is_list()) {
    std::move(callback).Run(
        MakeIpcFailure("Native addon arguments must be a list"));
    return;
  }
  EnsureNodeExecutor(node_addon_host_receivers_.current_context())
      ->InvokeInstance(
      module_path, instance_id, method_name, /*client_id=*/0,
      ListValueToInvokeArgs(arguments),
      base::BindOnce(
          [](InvokeNodeInstanceSyncCallback callback, bool success,
             base::Value value,
             std::vector<mojom::NodeCallbackResultPtr> callback_results,
             const std::string& error) {
            if (!success) {
              std::move(callback).Run(MakeIpcFailure(
                  error.empty() ? "Native instance invocation failed"
                                : error));
              return;
            }
            std::move(callback).Run(
                MakeNativeInvokeSuccess(std::move(value)));
          },
          std::move(callback)),
      /*allow_pending_promise=*/false);
}

void XenonServiceImpl::InspectNodeInstanceMemberSync(
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name,
    InspectNodeInstanceMemberSyncCallback callback) {
  EnsureNodeExecutor(node_addon_host_receivers_.current_context())
      ->InspectInstanceMember(
      module_path, instance_id, property_name,
      base::BindOnce(
          [](InspectNodeInstanceMemberSyncCallback callback, bool success,
             base::Value value, const std::string& error) {
            if (!success) {
              std::move(callback).Run(MakeIpcFailure(
                  error.empty() ? "Native instance inspection failed"
                                : error));
              return;
            }
            std::move(callback).Run(
                MakeNativeInvokeSuccess(std::move(value)));
          },
          std::move(callback)));
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
  // Generic HTTPS probe; uses the same proxy resolution as all browser traffic.
  resource_request->url = GURL("https://example.com/");
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
              const std::string message = "Probe download success. Size: " +
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
              std::move(user_callback).Run("Probe download failed.");
            }
          },
          weak_factory_.GetWeakPtr(), std::move(loader), std::move(callback)),
      1024 * 1024 /* 1MB limit */);
}

XenonNodeExecutor* XenonServiceImpl::GetNodeExecutor(
    const std::string& context_id) {
  const auto it =
      node_executors_.find(NormalizeIpcContainerId(context_id));
  return it == node_executors_.end() ? nullptr : it->second.get();
}

XenonNodeExecutor* XenonServiceImpl::EnsureNodeExecutor(
    const std::string& context_id) {
  const std::string normalized_id = NormalizeIpcContainerId(context_id);
  if (XenonNodeExecutor* executor = GetNodeExecutor(normalized_id)) {
    return executor;
  }

  auto executor = std::make_unique<XenonNodeExecutor>();
  executor->SetCallbackHandlers(
      base::BindRepeating(&XenonServiceImpl::OnNodeCallback,
                          weak_factory_.GetWeakPtr(), normalized_id),
      base::BindRepeating(&XenonServiceImpl::OnNodeCallbackReleased,
                          weak_factory_.GetWeakPtr(), normalized_id));
  XenonNodeExecutor* result = executor.get();
  node_executors_.emplace(normalized_id, std::move(executor));
  LOG(INFO) << "Created Node addon context: " << normalized_id;
  return result;
}

void XenonServiceImpl::EnsureAddonLoaded(
    const std::string& context_id,
    const std::string& path,
    base::OnceCallback<void(bool success, const std::string& error)> done) {
  XenonNodeExecutor* executor = EnsureNodeExecutor(context_id);
  if (executor->HasModule(path)) {
    std::move(done).Run(true, std::string());
    return;
  }
  executor->LoadAddon(
      path, base::BindOnce(
                [](base::OnceCallback<void(bool, const std::string&)> done,
                   bool success, const std::string& error,
                   std::vector<mojom::NodeExportInfoPtr> /*exports*/) {
                  std::move(done).Run(success, error);
                },
                std::move(done)));
}

void XenonServiceImpl::OnNodeCallback(const std::string& context_id,
                                      int32_t client_id,
                                      int32_t callback_id,
                                      std::vector<base::Value> args) {
  const NodeClientKey key =
      {NormalizeIpcContainerId(context_id), client_id};
  const auto renderer_endpoint = renderer_node_endpoints_.find(key);
  if (renderer_endpoint != renderer_node_endpoints_.end()) {
    const auto container = ipc_main_containers_.find(key.first);
    if (container == ipc_main_containers_.end()) {
      return;
    }
    base::ListValue callback_args;
    for (base::Value& arg : args) {
      callback_args.Append(std::move(arg));
    }
    base::ListValue event_args;
    event_args.Append(callback_id);
    event_args.Append(std::move(callback_args));
    container->second->DispatchToRenderer(
        renderer_endpoint->second, kNodeAddonCallbackChannel,
        base::Value(std::move(event_args)));
    return;
  }
  auto observer = node_addon_observers_.find(key);
  if (observer == node_addon_observers_.end() ||
      !observer->second.is_connected()) {
    LOG(WARNING) << "Queue native callback context=" << key.first
                 << " client=" << client_id
                 << " cb=" << callback_id
                 << " (NodeAddonObserver not connected yet)";
    pending_node_callbacks_[key].emplace_back(callback_id, std::move(args));
    return;
  }
  observer->second->OnCallback(callback_id, std::move(args));
}

void XenonServiceImpl::FlushPendingNodeCallbacks(
    const std::string& context_id,
    int32_t client_id) {
  const NodeClientKey key =
      {NormalizeIpcContainerId(context_id), client_id};
  auto pending = pending_node_callbacks_.find(key);
  if (pending == pending_node_callbacks_.end() || pending->second.empty()) {
    return;
  }
  auto observer = node_addon_observers_.find(key);
  if (observer == node_addon_observers_.end() ||
      !observer->second.is_connected()) {
    return;
  }
  LOG(INFO) << "Flushing " << pending->second.size()
            << " queued native callbacks for context=" << key.first
            << " client=" << client_id;
  auto queued = std::move(pending->second);
  pending_node_callbacks_.erase(pending);
  for (auto& item : queued) {
    observer->second->OnCallback(item.first, std::move(item.second));
  }
}

void XenonServiceImpl::OnNodeCallbackReleased(const std::string& context_id,
                                              int32_t client_id,
                                              int32_t callback_id) {
  const NodeClientKey key =
      {NormalizeIpcContainerId(context_id), client_id};
  const auto renderer_endpoint = renderer_node_endpoints_.find(key);
  if (renderer_endpoint != renderer_node_endpoints_.end()) {
    const auto container = ipc_main_containers_.find(key.first);
    if (container == ipc_main_containers_.end()) {
      return;
    }
    base::ListValue event_args;
    event_args.Append(callback_id);
    container->second->DispatchToRenderer(
        renderer_endpoint->second, kNodeAddonCallbackReleasedChannel,
        base::Value(std::move(event_args)));
    return;
  }
  auto observer = node_addon_observers_.find(key);
  if (observer == node_addon_observers_.end() ||
      !observer->second.is_connected()) {
    return;
  }
  observer->second->OnCallbackReleased(callback_id);
}

void XenonServiceImpl::OnNodeAddonObserverDisconnected(
    const std::string& context_id,
    int32_t client_id) {
  const NodeClientKey key =
      {NormalizeIpcContainerId(context_id), client_id};
  node_addon_observers_.erase(key);
  pending_node_callbacks_.erase(key);
}

void XenonServiceImpl::LoadAddon(const std::string& context_id,
                                 const std::string& path,
                                 LoadAddonCallback callback) {
  EnsureNodeExecutor(context_id)->LoadAddon(
      path, mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                std::move(callback), false,
                "LoadAddon callback dropped before completion",
                std::vector<mojom::NodeExportInfoPtr>()));
}

void XenonServiceImpl::InspectExport(const std::string& context_id,
                                     const std::string& module_path,
                                     const std::string& export_path,
                                     InspectExportCallback callback) {
  XenonNodeExecutor* executor = GetNodeExecutor(context_id);
  if (!executor) {
    std::move(callback).Run(false, "No Node addon has been loaded", nullptr);
    return;
  }
  executor->InspectExport(
      module_path, export_path,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false,
          "InspectExport callback dropped before completion",
          mojom::NodeExportInfoPtr()));
}

void XenonServiceImpl::ConstructExport(
    const std::string& context_id,
    int32_t client_id,
    const std::string& module_path,
    const std::string& export_path,
    std::vector<mojom::NodeInvokeArgPtr> args,
    ConstructExportCallback callback) {
  EnsureAddonLoaded(
      context_id, module_path,
      base::BindOnce(
          [](base::WeakPtr<XenonServiceImpl> self, std::string context_id,
             int32_t client_id,
             std::string module_path, std::string export_path,
             std::vector<mojom::NodeInvokeArgPtr> args,
             ConstructExportCallback callback, bool success,
             const std::string& error) {
            XenonNodeExecutor* executor =
                self ? self->GetNodeExecutor(context_id) : nullptr;
            if (!executor) {
              std::move(callback).Run(false, 0, "No Node addon has been loaded");
              return;
            }
            if (!success) {
              std::move(callback).Run(
                  false, 0,
                  error.empty() ? "No Node addon has been loaded" : error);
              return;
            }
            executor->ConstructExport(
                module_path, export_path, client_id, std::move(args),
                mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                    std::move(callback), false, 0,
                    "ConstructExport callback dropped before completion"));
          },
          weak_factory_.GetWeakPtr(), NormalizeIpcContainerId(context_id),
          client_id, module_path, export_path, std::move(args),
          std::move(callback)));
}

void XenonServiceImpl::InvokeInstance(const std::string& context_id,
                                      int32_t client_id,
                                      const std::string& module_path,
                                      int32_t instance_id,
                                      const std::string& method_name,
                                      std::vector<mojom::NodeInvokeArgPtr> args,
                                      InvokeInstanceCallback callback) {
  XenonNodeExecutor* executor = GetNodeExecutor(context_id);
  if (!executor) {
    std::move(callback).Run(false, base::Value(), {},
                            "No Node addon has been loaded");
    return;
  }
  executor->InvokeInstance(
      module_path, instance_id, method_name, client_id, std::move(args),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false, base::Value(),
          std::vector<mojom::NodeCallbackResultPtr>(),
          "InvokeInstance callback dropped before completion"));
}

void XenonServiceImpl::GetInstanceProperty(
    const std::string& context_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name,
    GetInstancePropertyCallback callback) {
  XenonNodeExecutor* executor = GetNodeExecutor(context_id);
  if (!executor) {
    std::move(callback).Run(false, base::Value(),
                            "No Node addon has been loaded");
    return;
  }
  executor->GetInstanceProperty(
      module_path, instance_id, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false, base::Value(),
          "GetInstanceProperty callback dropped before completion"));
}

void XenonServiceImpl::SetInstanceProperty(
    const std::string& context_id,
    const std::string& module_path,
    int32_t instance_id,
    const std::string& property_name,
    base::Value value,
    SetInstancePropertyCallback callback) {
  XenonNodeExecutor* executor = GetNodeExecutor(context_id);
  if (!executor) {
    std::move(callback).Run(false, "No Node addon has been loaded");
    return;
  }
  executor->SetInstanceProperty(
      module_path, instance_id, property_name, std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false,
          "SetInstanceProperty callback dropped before completion"));
}

void XenonServiceImpl::ReleaseInstance(const std::string& context_id,
                                       const std::string& module_path,
                                       int32_t instance_id) {
  if (XenonNodeExecutor* executor = GetNodeExecutor(context_id)) {
    executor->ReleaseInstance(module_path, instance_id);
  }
}

void XenonServiceImpl::InvokeFunction(const std::string& context_id,
                                      int32_t client_id,
                                      const std::string& module_path,
                                      const std::string& function_name,
                                      std::vector<mojom::NodeInvokeArgPtr> args,
                                      InvokeFunctionCallback callback) {
  EnsureAddonLoaded(
      context_id, module_path,
      base::BindOnce(
          [](base::WeakPtr<XenonServiceImpl> self, std::string context_id,
             int32_t client_id,
             std::string module_path, std::string function_name,
             std::vector<mojom::NodeInvokeArgPtr> args,
             InvokeFunctionCallback callback, bool success,
             const std::string& error) {
            XenonNodeExecutor* executor =
                self ? self->GetNodeExecutor(context_id) : nullptr;
            if (!executor) {
              std::move(callback).Run(false, base::Value(), {},
                                      "No Node addon has been loaded");
              return;
            }
            if (!success) {
              std::move(callback).Run(
                  false, base::Value(), {},
                  error.empty() ? "No Node addon has been loaded" : error);
              return;
            }
            executor->InvokeFunction(
                module_path, function_name, client_id, std::move(args),
                mojo::WrapCallbackWithDefaultInvokeIfNotRun(
                    std::move(callback), false, base::Value(),
                    std::vector<mojom::NodeCallbackResultPtr>(),
                    "InvokeFunction callback dropped before completion"));
          },
          weak_factory_.GetWeakPtr(), NormalizeIpcContainerId(context_id),
          client_id, module_path, function_name, std::move(args),
          std::move(callback)));
}

void XenonServiceImpl::GetExportProperty(const std::string& context_id,
                                         const std::string& module_path,
                                         const std::string& object_path,
                                         const std::string& property_name,
                                         GetExportPropertyCallback callback) {
  XenonNodeExecutor* executor = GetNodeExecutor(context_id);
  if (!executor) {
    std::move(callback).Run(false, base::Value(),
                            "No Node addon has been loaded");
    return;
  }
  executor->GetExportProperty(
      module_path, object_path, property_name,
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false, base::Value(),
          "GetExportProperty callback dropped before completion"));
}

void XenonServiceImpl::SetExportProperty(const std::string& context_id,
                                         const std::string& module_path,
                                         const std::string& object_path,
                                         const std::string& property_name,
                                         base::Value value,
                                         SetExportPropertyCallback callback) {
  XenonNodeExecutor* executor = GetNodeExecutor(context_id);
  if (!executor) {
    std::move(callback).Run(false, "No Node addon has been loaded");
    return;
  }
  executor->SetExportProperty(
      module_path, object_path, property_name, std::move(value),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback), false,
          "SetExportProperty callback dropped before completion"));
}

void XenonServiceImpl::InvokeMany(const std::string& context_id,
                                  const std::string& module_path,
                                  std::vector<mojom::NodeInvokeCallPtr> calls,
                                  InvokeManyCallback callback) {
  XenonNodeExecutor* executor = GetNodeExecutor(context_id);
  if (!executor) {
    std::move(callback).Run(
        MakeInvokeFailures(calls.size(), "No Node addon has been loaded"));
    return;
  }
  const size_t call_count = calls.size();
  executor->InvokeMany(
      module_path, std::move(calls),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(
          std::move(callback),
          MakeInvokeFailures(call_count,
                             "InvokeMany callback dropped before completion")));
}

}  // namespace xenon
