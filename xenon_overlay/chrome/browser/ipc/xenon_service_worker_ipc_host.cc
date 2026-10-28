// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_service_worker_ipc_host.h"

#include <memory>
#include <utility>

#include "base/uuid.h"
#include "content/public/browser/render_process_host.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/self_owned_associated_receiver.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"

namespace xenon::ipc {

namespace {
mojom::IpcResultPtr UnavailableResult() {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = "Service worker IPC is unavailable";
  return result;
}
}  // namespace

// static
void XenonServiceWorkerIpcHost::Create(
    content::BrowserContext* browser_context,
    const content::ServiceWorkerVersionBaseInfo& worker,
    mojo::PendingAssociatedReceiver<mojom::ServiceWorkerIpcHost> receiver) {
  auto* process = content::RenderProcessHost::FromID(worker.process_id);
  if (!process || process->GetBrowserContext() != browser_context) {
    return;
  }
  mojo::MakeSelfOwnedAssociatedReceiver(
      std::make_unique<XenonServiceWorkerIpcHost>(worker), std::move(receiver));
}

XenonServiceWorkerIpcHost::XenonServiceWorkerIpcHost(
    const content::ServiceWorkerVersionBaseInfo& worker)
    : worker_(worker) {}

XenonServiceWorkerIpcHost::~XenonServiceWorkerIpcHost() {
  if (!endpoint_id_.empty()) {
    XenonManager::GetInstance()->RemoveElectronIpcRenderer(container_id_,
                                                           endpoint_id_);
  }
}

void XenonServiceWorkerIpcHost::Initialize(
    mojo::PendingRemote<mojom::IpcRenderer> renderer,
    InitializeCallback callback) {
  auto* process = content::RenderProcessHost::FromID(worker_.process_id);
  if (!process || !endpoint_id_.empty()) {
    std::move(callback).Run(nullptr);
    return;
  }
  auto identity = FindElectronSessionIdentity(process->GetStoragePartition());
  auto scripts = GetElectronSessionPreloadScripts(
      process->GetStoragePartition(), "service-worker");
  if (!identity || scripts.empty()) {
    std::move(callback).Run(nullptr);
    return;
  }
  container_id_ = identity->container_id;
  partition_ = identity->partition;
  service_generation_ =
      XenonManager::GetInstance()->service_generation(container_id_);
  if (!service_generation_) {
    std::move(callback).Run(nullptr);
    return;
  }
  endpoint_id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
  auto config = mojom::IpcServiceWorkerConfig::New();
  config->identity = mojom::IpcServiceWorkerInfo::New();
  config->identity->version_id = worker_.version_id;
  config->identity->scope = worker_.scope.spec();
  config->identity->script_url = worker_.script_url.spec();
  config->identity->partition = partition_;
  for (const auto& script : scripts) {
    auto preload = mojom::IpcPreloadScript::New();
    preload->id = script.id;
    preload->file_path = script.file_path;
    preload->source = script.source;
    config->preloads.push_back(std::move(preload));
  }
  XenonManager::GetInstance()->RegisterElectronIpcServiceWorker(
      container_id_, endpoint_id_, std::move(renderer),
      worker_.process_id.GetUnsafeValue(), config->identity.Clone());
  std::move(callback).Run(std::move(config));
}

bool XenonServiceWorkerIpcHost::IsAvailable(const std::string& channel) const {
  // The document bridge has privileged internal operations. Worker preloads
  // receive application IPC only, with no route to those native operations.
  if (endpoint_id_.empty() || channel.starts_with("__xenon:")) {
    return false;
  }
  auto* process = content::RenderProcessHost::FromID(worker_.process_id);
  if (!process) {
    return false;
  }
  auto identity = FindElectronSessionIdentity(process->GetStoragePartition());
  return identity && identity->container_id == container_id_ &&
         identity->partition == partition_;
}

void XenonServiceWorkerIpcHost::Send(const std::string& channel,
                                     base::Value arguments) {
  if (IsAvailable(channel)) {
    XenonManager::GetInstance()->ElectronIpcSend(container_id_, endpoint_id_,
                                                 channel, std::move(arguments),
                                                 service_generation_);
  }
}

void XenonServiceWorkerIpcHost::Invoke(const std::string& channel,
                                       base::Value arguments,
                                       InvokeCallback callback) {
  if (!IsAvailable(channel)) {
    std::move(callback).Run(UnavailableResult());
    return;
  }
  XenonManager::GetInstance()->ElectronIpcInvoke(
      container_id_, endpoint_id_, channel, std::move(arguments),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback),
                                                  UnavailableResult()),
      service_generation_);
}

}  // namespace xenon::ipc
