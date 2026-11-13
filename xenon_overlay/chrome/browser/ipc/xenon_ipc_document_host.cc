// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_document_host.h"

#include <utility>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_split.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "base/uuid.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/global_routing_id.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/base/filename_util.h"
#include "third_party/blink/public/common/tokens/tokens.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_document_registration.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_guest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_sandbox_policy.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_file_system_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_network_request_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_os_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_sqlite_bridge.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

namespace xenon::ipc {

namespace {

constexpr base::TimeDelta kSynchronousIpcTimeout = base::Seconds(30);

xenon::ipc::mojom::IpcResultPtr SynchronousIpcTimeoutResult() {
  auto result = xenon::ipc::mojom::IpcResult::New();
  result->success = false;
  result->value = base::Value();
  result->error = "Synchronous ipcMain request timed out after 30 seconds";
  return result;
}

// A synchronous renderer call is represented by an asynchronous Browser to
// Utility hop. Completion, pipe failure and timeout may race; only the first
// result may unblock the renderer, and late results must be harmless.
class SynchronousIpcReplyState
    : public base::RefCounted<SynchronousIpcReplyState> {
 public:
  explicit SynchronousIpcReplyState(
      xenon::ipc::mojom::IpcHost::SendSyncCallback callback)
      : callback_(std::move(callback)) {}

  void StartTimeout() {
    timeout_.Start(FROM_HERE, kSynchronousIpcTimeout, this,
                   &SynchronousIpcReplyState::OnTimeout);
  }

  void Reply(xenon::ipc::mojom::IpcResultPtr result) {
    if (callback_) {
      timeout_.Stop();
      std::move(callback_).Run(std::move(result));
    }
  }

 private:
  friend class base::RefCounted<SynchronousIpcReplyState>;
  ~SynchronousIpcReplyState() = default;

  void OnTimeout() { Reply(SynchronousIpcTimeoutResult()); }

  xenon::ipc::mojom::IpcHost::SendSyncCallback callback_;
  base::OneShotTimer timeout_;
};

}  // namespace

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
    : DocumentService(render_frame_host, std::move(receiver)),
      content::WebContentsObserver(
          content::WebContents::FromRenderFrameHost(&render_frame_host)) {}

XenonIpcDocumentHost::~XenonIpcDocumentHost() {
  if (!endpoint_id_.empty()) {
    RevokeElectronDocumentRegistration(&render_frame_host(), endpoint_id_);
    XenonManager::GetInstance()->RemoveElectronIpcRenderer(container_id_,
                                                           endpoint_id_);
  }
}

void XenonIpcDocumentHost::GetRuntimeConfig(GetRuntimeConfigCallback callback) {
  if (!IsAllowedDocument() || !InitializeSandboxPolicy()) {
    std::move(callback).Run(nullptr);
    return;
  }
  auto config =
      XenonManager::GetInstance()->GetElectronIpcRendererConfigForContainer(
          ResolveContainerId(),
          render_frame_host().GetLastCommittedURL().spec());
  if (config) {
    content::WebContents* web_contents =
        content::WebContents::FromRenderFrameHost(&render_frame_host());
    config->is_guest =
        XenonElectronGuest::FromWebContents(web_contents) != nullptr;
    config->is_main_frame = render_frame_host().IsInPrimaryMainFrame();
    config->web_preferences = base::Value(effective_preferences_->Clone());
    auto* partition = render_frame_host().GetStoragePartition();
    auto identity = FindElectronSessionIdentity(partition);
    if (identity && identity->container_id == ResolveContainerId()) {
      for (const auto& script :
           GetElectronSessionPreloadScripts(partition, "frame")) {
        config->session_preloads.push_back(mojom::IpcPreloadScript::New(
            script.id, script.file_path, script.source));
      }
    }
    const bool hosted =
        config->is_guest ||
        XenonElectronWindowHost::GetInstance()->FindWindowIdForWebContents(
            web_contents) != 0;
    config->can_load_mapped_file_resources =
        hosted &&
        XenonManager::GetInstance()->IsDeclaredFileRendererURL(
            ResolveContainerId(), render_frame_host().GetLastCommittedURL());
    const auto* preload = effective_preferences_->FindString("preload");
    if (effective_preferences_->FindBool("sandbox").value_or(false) && preload &&
        !preload->empty()) {
      const std::string container = ResolveContainerId();
      const uint64_t generation =
          XenonManager::GetInstance()->service_generation(container);
      base::ListValue request;
      request.Append(base::DictValue()
                         .Set("operation", "read_file")
                         .Set("path", *preload)
                         .Set("returnBytes", true));
      // Only the Browser-approved preload is read. The renderer's generic
      // filesystem/addon IPC remains unavailable throughout initialization.
      base::ThreadPool::PostTaskAndReplyWithResult(
          FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
          base::BindOnce(&PerformFileSystemCall, base::Value(std::move(request))),
          base::BindOnce(
              [](base::WeakPtr<XenonIpcDocumentHost> self,
                 std::string container, uint64_t generation,
                 mojom::IpcRendererConfigPtr config,
                 GetRuntimeConfigCallback callback, mojom::IpcResultPtr result) {
                if (!self || !self->IsAllowedDocument() ||
                    XenonManager::GetInstance()->service_generation(container) !=
                        generation) {
                  std::move(callback).Run(nullptr);
                  return;
                }
                if (result && result->success && result->value.is_blob()) {
                  const auto& bytes = result->value.GetBlob();
                  config->window_preload_source =
                      std::string(bytes.begin(), bytes.end());
                } else {
                  config->window_preload_error =
                      result && !result->error.empty()
                          ? result->error
                          : "Unable to read sandboxed preload script";
                }
                std::move(callback).Run(std::move(config));
              },
              weak_factory_.GetWeakPtr(), container, generation,
              std::move(config), std::move(callback)));
      return;
    }
  }
  std::move(callback).Run(std::move(config));
}

void XenonIpcDocumentHost::AttachGuest(
    const base::UnguessableToken& frame_token,
    base::Value preferences,
    AttachGuestCallback callback) {
  callback = mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback),
                                                         UnavailableResult());
  auto fail = [&callback](const std::string& error) {
    auto result = xenon::ipc::mojom::IpcResult::New();
    result->success = false;
    result->error = error;
    std::move(callback).Run(std::move(result));
  };
  if (!IsAllowedDocument() || !IsCurrentRenderer() || !preferences.is_dict() ||
      !InitializeSandboxPolicy() ||
      (effective_preferences_->FindBool("sandbox").value_or(false) &&
       !effective_preferences_->FindBool("webviewTag").value_or(false))) {
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
  if (prefs.contains("partition") && !prefs.FindString("partition")) {
    fail("Guest partition must be a string");
    return;
  }
  auto normalized =
      XenonManager::GetInstance()->NormalizeElectronWebPreferences(
          ResolveContainerId(), prefs);
  if (!normalized.has_value()) {
    fail(normalized.error());
    return;
  }
  frame->PrepareForInnerWebContentsAttach(base::BindOnce(
      &XenonIpcDocumentHost::OnGuestFramePrepared, weak_factory_.GetWeakPtr(),
      std::move(*normalized), std::move(callback)));
}

void XenonIpcDocumentHost::OnGuestFramePrepared(
    base::DictValue preferences,
    AttachGuestCallback callback,
    content::RenderFrameHost* frame) {
  if (!IsCurrentRenderer() || !frame ||
      frame->GetParent() != &render_frame_host()) {
    std::move(callback).Run(UnavailableResult());
    return;
  }
  int guest_id = XenonElectronGuest::Attach(frame, ResolveContainerId(),
                                            preferences.Clone());
  if (guest_id == 0) {
    auto result = mojom::IpcResult::New();
    result->success = false;
    result->error = "Unable to create the guest WebContents";
    std::move(callback).Run(std::move(result));
    return;
  }
  base::ListValue args;
  args.Append(guest_id);
  args.Append(std::move(preferences));
  // Register the real WebContents in ipcMain before its first navigation and
  // preload. The reply contains the public, container-local WebContents ID.
  XenonManager::GetInstance()->ElectronIpcInvoke(
      container_id_, endpoint_id_, "__xenon:register-guest",
      base::Value(std::move(args)), std::move(callback), service_generation_);
}

bool XenonIpcDocumentHost::IsCurrentRenderer() const {
  return !endpoint_id_.empty() &&
         XenonManager::GetInstance()->IsElectronIpcGenerationCurrent(
             container_id_, service_generation_);
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
  auto* window_preferences = XenonElectronWindowHost::GetInstance()
                                 ->GetWebPreferencesForWebContents(contents);
  if (window_preferences) {
    return render_frame_host().IsInPrimaryMainFrame() ||
           window_preferences->FindBool("nodeIntegrationInSubFrames")
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
  if (XenonElectronWindowHost::GetInstance()->FindWindowIdForWebContents(
          web_contents) > 0) {
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
    mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer,
    BindRendererCallback callback) {
  if (!IsAllowedDocument() || !InitializeSandboxPolicy()) {
    // Treat an origin-policy rejection as unavailable transport rather than a
    // compromised renderer. This closes only the document-scoped Mojo pipe.
    std::move(callback).Run(false);
    ResetAndDeleteThis();
    return;
  }
  if (!endpoint_id_.empty()) {
    // A retained document must not rebind itself into a restarted main process.
    // A new document receives a new DocumentService and can register normally.
    if (!IsCurrentRenderer()) {
      std::move(callback).Run(false);
      ResetAndDeleteThis();
      return;
    }
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
  renderer_frame_url_ = render_frame_host().GetLastCommittedURL().spec();
  LOG(INFO) << "RegisterElectronIpcRenderer origin="
            << render_frame_host().GetLastCommittedOrigin().Serialize()
            << " window_id=" << window_id << " endpoint=" << endpoint_id_;
  if (window_id == 0) {
    LOG(WARNING) << "RegisterElectronIpcRenderer: window_id=0; "
                    "BrowserWindow.fromWebContents will not resolve";
  }
  const bool accepted = XenonManager::GetInstance()->RegisterElectronIpcRenderer(
      container_id_, endpoint_id_, std::move(renderer),
      frame_id.child_id.GetUnsafeValue(), frame_id.frame_routing_id, window_id,
      renderer_frame_url_, render_frame_host().IsInPrimaryMainFrame(),
      BeginElectronDocumentRegistration(
          &render_frame_host(), endpoint_id_,
          base::BindRepeating(
              [](base::WeakPtr<XenonIpcDocumentHost> self,
                 const std::string& endpoint) {
                return self && self->endpoint_id_ == endpoint &&
                       self->IsCurrentRenderer() && self->IsAllowedDocument();
              },
              weak_factory_.GetWeakPtr(), endpoint_id_)));
  service_generation_ =
      XenonManager::GetInstance()->service_generation(container_id_);
  // This ack covers Browser authorization and the document's pending identity.
  // Utility acknowledges its actual registration separately; waiting for that
  // here could deadlock a main process synchronously creating this window.
  std::move(callback).Run(accepted);
}

void XenonIpcDocumentHost::UpdateRendererFrameUrl() {
  const std::string url = render_frame_host().GetLastCommittedURL().spec();
  if (url == renderer_frame_url_) {
    return;
  }
  renderer_frame_url_ = url;
  // Queue this trusted update before the IPC using the same Utility pipe.
  // DocumentService recreation already handles cross-document navigation.
  XenonManager::GetInstance()->UpdateElectronIpcRendererFrame(
      container_id_, endpoint_id_, url);
}

void XenonIpcDocumentHost::DidFinishNavigation(
    content::NavigationHandle* navigation) {
  if (!navigation->HasCommitted() || !navigation->IsSameDocument() ||
      navigation->GetRenderFrameHost() != &render_frame_host() ||
      !IsCurrentRenderer() || !IsAllowedDocument()) {
    return;
  }
  // Hash/history navigation keeps this document and its endpoint alive. Keep
  // the trusted URL current even when the page sends no application IPC.
  UpdateRendererFrameUrl();
}

void XenonIpcDocumentHost::BindNodeAddonHost(
    mojo::PendingReceiver<xenon::ipc::mojom::NodeAddonHost> receiver,
    mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> callback_renderer) {
  if (!IsAllowedDocument() || !IsCurrentRenderer()) {
    ResetAndDeleteThis();
    return;
  }
  if (!InitializeSandboxPolicy() ||
      effective_preferences_->FindBool("sandbox").value_or(false)) {
    return;
  }
  XenonManager::GetInstance()->BindNodeAddonHost(container_id_, endpoint_id_,
                                                 std::move(receiver),
                                                 std::move(callback_renderer));
}

void XenonIpcDocumentHost::DidFirstNonEmptyLayout() {
  if (!IsAllowedDocument() || !IsCurrentRenderer() ||
      !render_frame_host().IsActive()) {
    return;
  }
  XenonElectronWindowHost::GetInstance()->OnFirstNonEmptyLayout(
      &render_frame_host());
}

void XenonIpcDocumentHost::Send(const std::string& channel,
                                base::Value arguments) {
  if (!IsAllowedDocument() || !IsCurrentRenderer() ||
      !AllowsSandboxChannel(channel, arguments)) {
    return;
  }
  UpdateRendererFrameUrl();
  XenonManager::GetInstance()->ElectronIpcSend(container_id_, endpoint_id_,
                                               channel, std::move(arguments),
                                               service_generation_);
}

void XenonIpcDocumentHost::PostMessage(const std::string& channel,
                                     blink::TransferableMessage message) {
  if (!IsAllowedDocument() || !IsCurrentRenderer() ||
      !AllowsSandboxChannel(channel, base::Value())) {
    return;
  }
  UpdateRendererFrameUrl();
  XenonManager::GetInstance()->ElectronIpcPostMessage(
      container_id_, endpoint_id_, channel, std::move(message),
      service_generation_);
}

void XenonIpcDocumentHost::Invoke(const std::string& channel,
                                  base::Value arguments,
                                  InvokeCallback callback) {
  callback = mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(callback),
                                                         UnavailableResult());
  if (!IsAllowedDocument() || !IsCurrentRenderer() ||
      channel == "__xenon:register-guest") {
    std::move(callback).Run(UnavailableResult());
    return;
  }
  if (!AllowsSandboxChannel(channel, arguments)) {
    std::move(callback).Run(SandboxDeniedResult());
    return;
  }
  UpdateRendererFrameUrl();
  if (channel == "__xenon:electron-api") {
    CallElectronApi(std::move(arguments), std::move(callback));
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
    PerformNetworkRequest(std::move(url_loader_factory), std::move(arguments),
                          std::move(callback));
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
      std::move(callback), service_generation_);
}

void XenonIpcDocumentHost::SendSync(const std::string& channel,
                                    base::Value arguments,
                                    SendSyncCallback callback) {
  if (!IsAllowedDocument() || !IsCurrentRenderer()) {
    std::move(callback).Run(UnavailableResult());
    return;
  }
  if (!AllowsSandboxChannel(channel, arguments)) {
    std::move(callback).Run(SandboxDeniedResult());
    return;
  }
  UpdateRendererFrameUrl();
  auto reply_state =
      base::MakeRefCounted<SynchronousIpcReplyState>(std::move(callback));
  reply_state->StartTimeout();
  callback = mojo::WrapCallbackWithDefaultInvokeIfNotRun(
      base::BindOnce(&SynchronousIpcReplyState::Reply, reply_state),
      UnavailableResult());
  if (channel == "__xenon:renderer-web-preferences") {
    arguments = base::Value(
        base::ListValue().Append(render_frame_host().IsInPrimaryMainFrame()));
  }
  if (channel == "__xenon:electron-api") {
    CallElectronApi(std::move(arguments), std::move(callback));
    return;
  }
  if (channel == "__xenon:os-network-interfaces") {
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&GetNetworkInterfaces), std::move(callback));
    return;
  }
  if (channel == "__xenon:os") {
    if (!arguments.is_list() || arguments.GetList().size() != 1 ||
        !arguments.GetList().front().is_dict()) {
      auto result = xenon::ipc::mojom::IpcResult::New();
      result->success = false;
      result->error =
          "ERR_INVALID_ARG_TYPE: os call expects one request object";
      std::move(callback).Run(std::move(result));
      return;
    }
    base::Value request = std::move(arguments.GetList().front());
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(&PerformOsCall, std::move(request)),
        std::move(callback));
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
      std::move(callback), service_generation_);
}

bool XenonIpcDocumentHost::InitializeSandboxPolicy() {
  if (effective_preferences_) {
    return true;
  }
  auto* contents = content::WebContents::FromRenderFrameHost(&render_frame_host());
  const base::DictValue* preferences = nullptr;
  if (auto* guest = XenonElectronGuest::FromWebContents(contents)) {
    preferences = &guest->preferences();
  } else {
    preferences = XenonElectronWindowHost::GetInstance()
                      ->GetWebPreferencesForWebContents(contents);
  }
  const base::DictValue empty;
  const std::string container = ResolveContainerId();
  auto* manager = XenonManager::GetInstance();
  auto normalized = manager->NormalizeElectronWebPreferences(
      container, preferences ? *preferences : empty, preferences != nullptr);
  if (!normalized.has_value()) {
    LOG(ERROR) << "Electron document sandbox policy rejected: "
               << normalized.error();
    return false;
  }
  effective_preferences_ = std::move(*normalized);
  // Ordinary authorized WebUI documents can expose the same full runtime.
  // They also prevent a later opt-in from claiming to revoke escaped handles.
  manager->NoteElectronSandboxDocument(container);
  return true;
}

bool XenonIpcDocumentHost::AllowsSandboxChannel(
    const std::string& channel,
    const base::Value& arguments) {
  if (!InitializeSandboxPolicy()) {
    return false;
  }
  return !effective_preferences_->FindBool("sandbox").value_or(false) ||
         ElectronSandboxPolicy::AllowsChannel(
             channel, arguments,
             effective_preferences_->FindBool("webviewTag").value_or(false));
}

xenon::ipc::mojom::IpcResultPtr XenonIpcDocumentHost::SandboxDeniedResult() const {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = "ERR_ACCESS_DENIED: sandboxed renderer cannot access the "
                  "privileged Node or application broker";
  return result;
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
