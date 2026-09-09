// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_document_host.h"

#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/string_split.h"
#include "base/task/thread_pool.h"
#include "base/uuid.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "net/base/filename_util.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_guest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_network_request_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_sqlite_bridge.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

namespace xenon::ipc {

// static
void XenonIpcDocumentHost::Create(
    content::RenderFrameHost* render_frame_host,
    mojo::PendingReceiver<xenon::ipc::mojom::IpcHost> receiver) {
  if (!render_frame_host) {
    return;
  }
  new XenonIpcDocumentHost(*render_frame_host, std::move(receiver));
}

XenonIpcDocumentHost::XenonIpcDocumentHost(
    content::RenderFrameHost& render_frame_host,
    mojo::PendingReceiver<xenon::ipc::mojom::IpcHost> receiver)
    : DocumentService(render_frame_host, std::move(receiver)) {}

XenonIpcDocumentHost::~XenonIpcDocumentHost() {
  if (!endpoint_id_.empty()) {
    XenonManager::GetInstance()->RemoveElectronIpcRenderer(container_id_,
                                                            endpoint_id_);
  }
}

void XenonIpcDocumentHost::GetRuntimeConfig(
    GetRuntimeConfigCallback callback) {
  if (!IsAllowedDocument()) {
    std::move(callback).Run(nullptr);
    return;
  }
  auto config =
      XenonManager::GetInstance()->GetElectronIpcRendererConfigForContainer(
          ResolveContainerId(),
          render_frame_host().GetLastCommittedURL().spec());
  if (config) {
    config->is_guest =
        XenonElectronGuest::FromWebContents(
            content::WebContents::FromRenderFrameHost(&render_frame_host())) !=
        nullptr;
    config->is_main_frame = render_frame_host().IsInPrimaryMainFrame();
  }
  std::move(callback).Run(std::move(config));
}

void XenonIpcDocumentHost::AttachGuest(
    const base::UnguessableToken& frame_token,
    base::Value preferences,
    AttachGuestCallback callback) {
  auto fail = [&callback](const std::string& error) {
    auto result = xenon::ipc::mojom::IpcResult::New();
    result->success = false;
    result->error = error;
    std::move(callback).Run(std::move(result));
  };
  if (!IsAllowedDocument() || endpoint_id_.empty() || !preferences.is_dict()) {
    fail("Invalid guest owner or preferences");
    return;
  }
  // Only the owner may attach to its own empty direct child. In particular,
  // remote websites cannot claim arbitrary frames or hosted-app identities.
  auto* frame = content::RenderFrameHost::FromFrameToken(
      {render_frame_host().GetProcess()->GetID(),
       blink::LocalFrameToken(frame_token)});
  if (!frame || frame->GetParent() != &render_frame_host() ||
      !frame->GetLastCommittedURL().IsAboutBlank()) {
    fail("Guest attachment requires an about:blank child of the caller");
    return;
  }
  const auto& prefs = preferences.GetDict();
  if (prefs.FindBool("contextIsolation").value_or(true) ||
      prefs.FindBool("sandbox").value_or(true) || prefs.contains("partition")) {
    fail(
        "Guest isolated worlds, sandboxed preloads and partitions are not "
        "supported yet");
    return;
  }
  frame->PrepareForInnerWebContentsAttach(base::BindOnce(
      &XenonIpcDocumentHost::OnGuestFramePrepared, weak_factory_.GetWeakPtr(),
      std::move(preferences).TakeDict(), std::move(callback)));
}

void XenonIpcDocumentHost::OnGuestFramePrepared(
    base::DictValue preferences,
    AttachGuestCallback callback,
    content::RenderFrameHost* frame) {
  if (!frame || frame->GetParent() != &render_frame_host()) {
    std::move(callback).Run(UnavailableResult());
    return;
  }
  int guest_id = XenonElectronGuest::Attach(frame, ResolveContainerId(),
                                            preferences.Clone());
  base::ListValue args;
  args.Append(guest_id);
  args.Append(std::move(preferences));
  // Register the real WebContents in ipcMain before its first navigation and
  // preload. The reply contains the public, container-local WebContents ID.
  XenonManager::GetInstance()->ElectronIpcInvoke(
      container_id_, endpoint_id_, "__xenon:register-guest",
      base::Value(std::move(args)), std::move(callback));
}

bool XenonIpcDocumentHost::IsAllowedDocument() const {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  if (!command_line->HasSwitch(switches::kEnable)) {
    return false;
  }
  auto* contents =
      content::WebContents::FromRenderFrameHost(&render_frame_host());
  if (auto* guest = XenonElectronGuest::FromWebContents(contents)) {
    return render_frame_host().IsInPrimaryMainFrame() ||
           guest->preferences()
               .FindBool("nodeIntegrationInSubFrames")
               .value_or(false);
  }
  if (!render_frame_host().IsInPrimaryMainFrame()) {
    return false;
  }
  if (command_line->HasSwitch(switches::kAllowAllOrigins)) {
    return true;
  }

  // Hosted BrowserWindows are trusted by identity, not by URL. This is
  // required for standard Electron file:/ASAR renderers, whose origins are
  // opaque and cannot be represented in an origin allowlist.
  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(&render_frame_host());
  if (XenonElectronWindowHost::GetInstance()
          ->FindWindowIdForWebContents(web_contents) > 0) {
    return true;
  }

  const url::Origin& document_origin =
      render_frame_host().GetLastCommittedOrigin();
  if (document_origin.opaque()) {
    return false;
  }

  for (const std::string& allowed_origin : base::SplitString(
           command_line->GetSwitchValueASCII(switches::kAllowedOrigins), ",",
           base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    const url::Origin configured_origin =
        url::Origin::Create(GURL(allowed_origin));
    if (!configured_origin.opaque() && configured_origin == document_origin) {
      return true;
    }
  }
  return false;
}

std::string XenonIpcDocumentHost::ResolveContainerId() const {
  content::WebContents* web_contents =
      content::WebContents::FromRenderFrameHost(&render_frame_host());
  if (auto* guest = XenonElectronGuest::FromWebContents(web_contents)) {
    return guest->container_id();
  }
  std::string hosted_container =
      XenonElectronWindowHost::GetInstance()->GetContainerIdForWebContents(
          web_contents);
  if (!hosted_container.empty()) {
    return hosted_container;
  }
  return XenonManager::GetInstance()->GetElectronIpcContainerForOrigin(
      render_frame_host().GetLastCommittedOrigin().Serialize());
}

void XenonIpcDocumentHost::BindRenderer(
    mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer) {
  if (!IsAllowedDocument()) {
    // Treat an origin-policy rejection as unavailable transport rather than a
    // compromised renderer. This closes only the document-scoped Mojo pipe.
    ResetAndDeleteThis();
    return;
  }
  if (!endpoint_id_.empty()) {
    XenonManager::GetInstance()->RemoveElectronIpcRenderer(container_id_,
                                                            endpoint_id_);
  }
  const content::GlobalRenderFrameHostId frame_id =
      render_frame_host().GetGlobalId();
  int32_t window_id =
      XenonElectronWindowHost::GetInstance()->FindWindowIdForWebContents(
          content::WebContents::FromRenderFrameHost(&render_frame_host()));
  if (auto* guest = XenonElectronGuest::FromWebContents(
          content::WebContents::FromRenderFrameHost(&render_frame_host()))) {
    window_id = guest->id();
  }
  endpoint_id_ = base::Uuid::GenerateRandomV4().AsLowercaseString();
  container_id_ = ResolveContainerId();
  LOG(INFO) << "RegisterElectronIpcRenderer origin="
            << render_frame_host().GetLastCommittedOrigin().Serialize()
            << " window_id=" << window_id << " endpoint=" << endpoint_id_;
  if (window_id == 0) {
    LOG(WARNING) << "RegisterElectronIpcRenderer: window_id=0; "
                    "BrowserWindow.fromWebContents will not resolve";
  }
  XenonManager::GetInstance()->RegisterElectronIpcRenderer(
      container_id_, endpoint_id_, std::move(renderer),
      frame_id.child_id.GetUnsafeValue(), frame_id.frame_routing_id,
      window_id);
}

void XenonIpcDocumentHost::BindNodeAddonHost(
    mojo::PendingReceiver<xenon::ipc::mojom::NodeAddonHost> receiver) {
  if (!IsAllowedDocument()) {
    ResetAndDeleteThis();
    return;
  }
  XenonManager::GetInstance()->BindNodeAddonHost(ResolveContainerId(),
                                                 std::move(receiver));
}

void XenonIpcDocumentHost::Send(const std::string& channel,
                                base::Value arguments) {
  if (!IsAllowedDocument() || endpoint_id_.empty()) {
    return;
  }
  XenonManager::GetInstance()->ElectronIpcSend(
      container_id_, endpoint_id_, channel, std::move(arguments));
}

void XenonIpcDocumentHost::Invoke(const std::string& channel,
                                  base::Value arguments,
                                  InvokeCallback callback) {
  if (!IsAllowedDocument() || endpoint_id_.empty() ||
      channel == "__xenon:register-guest") {
    std::move(callback).Run(UnavailableResult());
    return;
  }
  if (channel == "__xenon:fs") {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&PerformFileSystemCall, std::move(arguments)),
        std::move(callback));
    return;
  }
  if (channel == "__xenon:net-request") {
    scoped_refptr<network::SharedURLLoaderFactory> url_loader_factory =
        render_frame_host()
            .GetBrowserContext()
            ->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess();
    PerformNetworkRequest(std::move(url_loader_factory),
                          std::move(arguments), std::move(callback));
    return;
  }
  if (channel == "__xenon:sqlite") {
    if (sqlite_bridge_.is_null()) {
      sqlite_bridge_.emplace(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN}));
    }
    sqlite_bridge_.AsyncCall(&XenonSqliteBridge::Call)
        .WithArgs(std::move(arguments))
        .Then(std::move(callback));
    return;
  }
  XenonManager::GetInstance()->ElectronIpcInvoke(
      container_id_, endpoint_id_, channel, std::move(arguments),
      std::move(callback));
}

void XenonIpcDocumentHost::SendSync(const std::string& channel,
                                    base::Value arguments,
                                    SendSyncCallback callback) {
  if (!IsAllowedDocument() || endpoint_id_.empty()) {
    std::move(callback).Run(UnavailableResult());
    return;
  }
  if (channel == "__xenon:renderer-web-preferences") {
    arguments = base::Value(
        base::ListValue().Append(render_frame_host().IsInPrimaryMainFrame()));
  }
  if (channel == "__xenon:os-network-interfaces") {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&GetNetworkInterfaces), std::move(callback));
    return;
  }
  if (channel == "__xenon:fs") {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&PerformFileSystemCall, std::move(arguments)),
        std::move(callback));
    return;
  }
  // The renderer-facing method remains [Sync], but the Browser -> Utility
  // hop must be asynchronous. Utility handlers can call native window APIs;
  // those APIs may synchronously deliver a message to this Browser UI thread.
  // Waiting for Utility here would deadlock both sides.
  XenonManager::GetInstance()->ElectronIpcSendSync(
      container_id_, endpoint_id_, channel, std::move(arguments),
      std::move(callback));
}

xenon::ipc::mojom::IpcResultPtr XenonIpcDocumentHost::UnavailableResult()
    const {
  auto result = xenon::ipc::mojom::IpcResult::New();
  result->success = false;
  result->value = base::Value();
  result->error = "Utility ipcMain container is unavailable";
  return result;
}

}  // namespace xenon::ipc
