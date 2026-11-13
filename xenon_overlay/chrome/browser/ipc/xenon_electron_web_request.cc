// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_web_request.h"

#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/supports_user_data.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/keyed_service/core/keyed_service_shutdown_notifier.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/network_service_instance.h"
#include "content/public/browser/storage_partition.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/receiver_set.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/url_request/redirect_info.h"
#include "net/url_request/redirect_util.h"
#include "services/network/public/cpp/cors/cors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/cpp/url_loader_factory_builder.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/network_service.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_web_request_filter.h"
#include "xenon_overlay/common/ipc/xenon_privileged_schemes.h"

namespace xenon::ipc {
namespace {
constexpr char kRegistryKey[] = "xenon-electron-web-request";
using ResultCallback = base::OnceCallback<void(int, base::DictValue)>;

uint64_t NextId() {
  static uint64_t next = 0;
  return ++next;
}
bool IsCustomProtocol(const GURL& url) {
  for (const auto& scheme : GetStartupPrivilegedSchemes()) {
    if (url.SchemeIs(scheme.scheme)) {
      return true;
    }
  }
  return false;
}
struct Registration {
  ~Registration() {
    if (release) {
      release.Run(generation, id);
    }
  }
  std::string id;
  uint64_t generation = 0;
  ElectronWebRequestFilter filter;
  ElectronWebRequestDispatch dispatch;
  ElectronWebRequestCancel cancel;
  ElectronWebRequestRelease release;
};

class SessionState {
 public:
  explicit SessionState(ElectronSessionIdentity identity)
      : identity_(std::move(identity)) {}
  ~SessionState() {
    weak_factory_.InvalidateWeakPtrs();
    abort_callbacks_.Notify();
    auto pending = std::move(pending_);
    for (auto& [token, item] : pending) {
      item->registration->cancel.Run(item->registration->generation, token);
      std::move(item->callback).Run(net::ERR_ABORTED, {});
    }
  }
  base::WeakPtr<SessionState> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }
  const ElectronSessionIdentity& identity() const { return identity_; }
  bool Matches(const std::string& event,
               const network::ResourceRequest& request) const {
    const auto found = registrations_.find(event);
    return found != registrations_.end() &&
           found->second->filter.Matches(
               request.url, ElectronWebRequestResourceType(request));
  }
  base::CallbackListSubscription ObserveAbort(base::RepeatingClosure callback) {
    return abort_callbacks_.Add(std::move(callback));
  }
  void Set(const std::string& event,
           std::shared_ptr<Registration> registration) {
    if (registration) {
      registrations_[event] = std::move(registration);
    } else {
      registrations_.erase(event);
    }
  }
  void CancelOwner(const std::string& owner) {
    std::vector<std::string> tokens;
    for (const auto& [token, item] : pending_) {
      if (item->owner == owner) {
        tokens.push_back(token);
      }
    }
    for (const auto& token : tokens) {
      Complete(token, net::ERR_ABORTED, {}, true);
    }
  }
  void Run(const std::string& owner,
           uint64_t request_id,
           const std::string& event,
           const network::ResourceRequest& request,
           const net::HttpResponseHeaders* headers,
           ResultCallback callback) {
    const auto found = registrations_.find(event);
    if (found == registrations_.end() ||
        !found->second->filter.Matches(
            request.url, ElectronWebRequestResourceType(request))) {
      std::move(callback).Run(net::OK, {});
      return;
    }
    auto registration = found->second;
    if (pending_.size() >= 1024) {
      std::move(callback).Run(net::ERR_INSUFFICIENT_RESOURCES, {});
      return;
    }
    base::DictValue details;
    details.Set("id", static_cast<double>(request_id));
    details.Set("url", request.url.spec());
    details.Set("method", request.method);
    details.Set("resourceType", ElectronWebRequestResourceType(request));
    details.Set("referrer", request.referrer.spec());
    details.Set("timestamp", base::Time::Now().InMillisecondsFSinceUnixEpoch());
    if (event == "onBeforeSendHeaders") {
      details.Set("requestHeaders", ElectronWebRequestHeaders(request.headers));
    }
    if (headers) {
      details.Set("statusCode", headers->response_code());
      details.Set("statusLine", headers->GetStatusLine());
      details.Set("responseHeaders", ElectronWebRequestHeaders(*headers));
    }
    // Electron exposes uploadData only onBeforeRequest. Inspecting an upload
    // while delivering response headers can reject an otherwise valid stream
    // even when the application never requested its upload metadata.
    if (event == "onBeforeRequest" && request.request_body) {
      base::ListValue upload;
      size_t uploaded_bytes = 0;
      for (const auto& element : *request.request_body->elements()) {
        if (element.type() == network::DataElement::Tag::kBytes) {
          const auto bytes = element.As<network::DataElementBytes>().bytes();
          if (bytes.size() > 32 * 1024 * 1024 - uploaded_bytes) {
            std::move(callback).Run(net::ERR_FILE_TOO_BIG, {});
            return;
          }
          uploaded_bytes += bytes.size();
          upload.Append(base::DictValue().Set(
              "bytes", base::Value::BlobStorage(bytes.begin(), bytes.end())));
        } else if (element.type() == network::DataElement::Tag::kFile) {
          const auto& file = element.As<network::DataElementFile>();
          upload.Append(
              base::DictValue().Set("file", file.path().AsUTF8Unsafe()));
        } else {
          // Reading a data pipe here would consume the application's upload.
          std::move(callback).Run(net::ERR_NOT_IMPLEMENTED, {});
          return;
        }
      }
      details.Set("uploadData", std::move(upload));
    }
    const std::string token = base::NumberToString(NextId());
    auto item = std::make_unique<Pending>();
    item->owner = owner;
    item->registration = registration;
    item->callback = std::move(callback);
    item->timer.Start(
        FROM_HERE, base::Seconds(30),
        base::BindOnce(&SessionState::Complete, weak_factory_.GetWeakPtr(),
                       token, net::ERR_TIMED_OUT, base::DictValue(), true));
    pending_.emplace(token, std::move(item));
    auto wire = base::DictValue()
                    .Set("partition", identity_.partition)
                    .Set("event", event)
                    .Set("requestId", token)
                    .Set("registrationId", registration->id)
                    .Set("details", std::move(details));
    registration->dispatch.Run(
        registration->generation, base::Value(std::move(wire)),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&SessionState::OnReply, weak_factory_.GetWeakPtr(),
                           token),
            base::Value(), std::string("webRequest owner disconnected")));
  }

 private:
  struct Pending {
    std::string owner;
    std::shared_ptr<Registration> registration;
    ResultCallback callback;
    base::OneShotTimer timer;
  };
  void OnReply(const std::string& token,
               base::Value value,
               const std::string& error) {
    if (!error.empty() || !value.is_dict()) {
      LOG(ERROR) << "Electron webRequest callback failed: " << error;
      Complete(token, net::ERR_FAILED, {}, false);
      return;
    }
    const auto* cancel = value.GetDict().Find("cancel");
    if (cancel && !cancel->is_bool()) {
      Complete(token, net::ERR_INVALID_ARGUMENT, {}, false);
      return;
    }
    Complete(token,
             cancel && cancel->GetBool() ? net::ERR_BLOCKED_BY_CLIENT : net::OK,
             std::move(value.GetDict()), false);
  }
  void Complete(const std::string& token,
                int error,
                base::DictValue reply,
                bool cancel) {
    auto found = pending_.find(token);
    if (found == pending_.end()) {
      return;
    }
    auto item = std::move(found->second);
    pending_.erase(found);
    item->timer.Stop();
    if (cancel) {
      item->registration->cancel.Run(item->registration->generation, token);
    }
    std::move(item->callback).Run(error, std::move(reply));
  }
  ElectronSessionIdentity identity_;
  std::map<std::string, std::shared_ptr<Registration>> registrations_;
  std::map<std::string, std::unique_ptr<Pending>> pending_;
  base::RepeatingClosureList abort_callbacks_;
  base::WeakPtrFactory<SessionState> weak_factory_{this};
};

class Registry;
auto& Registries() {
  static base::NoDestructor<std::set<Registry*>> registries;
  return *registries;
}
class Registry : public base::SupportsUserData::Data {
 public:
  explicit Registry(content::BrowserContext* context) {
    shutdown_ = ElectronSessionShutdownNotifierFactory::GetInstance()
                    ->Get(context)
                    ->Subscribe(base::BindOnce(&Registry::Shutdown,
                                               base::Unretained(this)));
    Registries().insert(this);
  }
  ~Registry() override { Registries().erase(this); }
  void Shutdown() {
    shutting_down_ = true;
    sessions.clear();
  }
  SessionState* Get(content::StoragePartition* partition) {
    if (shutting_down_) {
      return nullptr;
    }
    const auto identity = FindElectronSessionIdentity(partition);
    if (!identity) {
      return nullptr;
    }
    auto& state = sessions[partition];
    if (!state) {
      state = std::make_unique<SessionState>(*identity);
    }
    return state.get();
  }
  std::map<content::StoragePartition*, std::unique_ptr<SessionState>> sessions;

 private:
  bool shutting_down_ = false;
  base::CallbackListSubscription shutdown_;
};
SessionState* GetState(content::BrowserContext* context,
                       content::StoragePartition* partition) {
  if (!context || context->ShutdownStarted() || !partition ||
      !FindElectronSessionIdentity(partition)) {
    return nullptr;
  }
  auto* registry = static_cast<Registry*>(context->GetUserData(kRegistryKey));
  if (!registry) {
    auto owned = std::make_unique<Registry>(context);
    registry = owned.get();
    context->SetUserData(kRegistryKey, std::move(owned));
  }
  return registry->Get(partition);
}

// One native network loader's trusted channel. The downstream client runs
// first; the application's modification is applied to its resulting headers.
class HeaderProxy final : public network::mojom::TrustedHeaderClient {
 public:
  using Snapshot =
      base::RepeatingCallback<std::optional<network::ResourceRequest>()>;
  HeaderProxy(
      base::WeakPtr<SessionState> state,
      uint64_t id,
      Snapshot snapshot,
      bool preflight,
      mojo::PendingReceiver<network::mojom::TrustedHeaderClient> receiver,
      mojo::PendingRemote<network::mojom::TrustedHeaderClient> downstream)
      : state_(std::move(state)),
        id_(id),
        snapshot_(std::move(snapshot)),
        preflight_(preflight),
        owner_("header:" + base::NumberToString(NextId())),
        receiver_(this, std::move(receiver)),
        downstream_(std::move(downstream)) {
    receiver_.set_disconnect_handler(
        base::BindOnce(&HeaderProxy::Abort, weak_factory_.GetWeakPtr()));
  }
  ~HeaderProxy() override {
    weak_factory_.InvalidateWeakPtrs();
    if (state_) {
      state_->CancelOwner(owner_);
    }
  }
  void OnBeforeSendHeaders(const GURL& url,
                           const net::HttpRequestHeaders& headers,
                           OnBeforeSendHeadersCallback callback) override {
    if (preflight_) {
      auto request = snapshot_.Run();
      if (!state_ || !request) {
        std::move(callback).Run(net::ERR_ABORTED, std::nullopt, std::nullopt);
        return;
      }
      request->url = url;
      state_->Run(owner_, id_, "onBeforeRequest", *request, nullptr,
                  base::BindOnce(&HeaderProxy::BeforePreflightDone,
                                 weak_factory_.GetWeakPtr(), url, headers,
                                 std::move(callback)));
      return;
    }
    ForwardBeforeSend(url, headers, std::move(callback));
  }
  void ForwardBeforeSend(const GURL& url,
                         const net::HttpRequestHeaders& headers,
                         OnBeforeSendHeadersCallback callback) {
    if (downstream_) {
      downstream_->OnBeforeSendHeaders(
          url, headers,
          mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(&HeaderProxy::AfterDownstreamBeforeSend,
                             weak_factory_.GetWeakPtr(), url, headers,
                             std::move(callback)),
              net::ERR_FAILED, std::optional<net::HttpRequestHeaders>(),
              std::optional<base::DictValue>()));
    } else {
      AfterDownstreamBeforeSend(url, headers, std::move(callback), net::OK,
                                std::nullopt, std::nullopt);
    }
  }
  void OnHeadersReceived(const std::string& headers,
                         const net::IPEndPoint& endpoint,
                         const std::optional<net::SSLInfo>& ssl,
                         OnHeadersReceivedCallback callback) override {
    if (downstream_) {
      downstream_->OnHeadersReceived(
          headers, endpoint, ssl,
          mojo::WrapCallbackWithDefaultInvokeIfNotRun(
              base::BindOnce(&HeaderProxy::AfterDownstream,
                             weak_factory_.GetWeakPtr(), headers,
                             std::move(callback)),
              net::ERR_FAILED, std::optional<std::string>(),
              std::optional<GURL>()));
    } else {
      AfterDownstream(headers, std::move(callback), net::OK, std::nullopt,
                      std::nullopt);
    }
  }

 private:
  void Abort() { delete this; }
  void AfterDownstreamBeforeSend(
      GURL url,
      net::HttpRequestHeaders original,
      OnBeforeSendHeadersCallback callback,
      int error,
      const std::optional<net::HttpRequestHeaders>& replacement,
      std::optional<base::DictValue> net_log) {
    if (error != net::OK) {
      std::move(callback).Run(error, replacement, std::move(net_log));
      return;
    }
    auto request = snapshot_.Run();
    if (!state_ || !request) {
      std::move(callback).Run(net::ERR_ABORTED, std::nullopt, std::move(net_log));
      return;
    }
    // This is the URL and full header set from the actual network transaction,
    // after Chromium has applied redirect/authentication/cookie handling.
    // Never copy modifications back to the loader's original request snapshot:
    // doing so could replay credentials removed on a later redirect.
    request->url = url;
    request->headers = replacement.value_or(original);
    state_->Run(owner_, id_, "onBeforeSendHeaders", *request, nullptr,
                base::BindOnce(&HeaderProxy::AfterApplicationBeforeSend,
                               weak_factory_.GetWeakPtr(), replacement,
                               std::move(net_log), std::move(callback)));
  }
  void AfterApplicationBeforeSend(
      std::optional<net::HttpRequestHeaders> downstream,
      std::optional<base::DictValue> net_log,
      OnBeforeSendHeadersCallback callback,
      int error,
      base::DictValue reply) {
    std::optional<net::HttpRequestHeaders> replacement;
    std::string diagnostic;
    if (error == net::OK && !BuildElectronWebRequestRequestHeaders(
                                reply, &replacement, &diagnostic)) {
      LOG(ERROR) << diagnostic;
      error = net::ERR_INVALID_ARGUMENT;
    }
    std::move(callback).Run(error, replacement ? replacement : downstream,
                            std::move(net_log));
  }
  void BeforePreflightDone(GURL url,
                           net::HttpRequestHeaders headers,
                           OnBeforeSendHeadersCallback callback,
                           int error,
                           base::DictValue reply) {
    if (reply.contains("redirectURL")) {
      error = net::ERR_NOT_IMPLEMENTED;
    }
    if (error != net::OK) {
      std::move(callback).Run(error, std::nullopt, std::nullopt);
      return;
    }
    ForwardBeforeSend(url, headers, std::move(callback));
  }
  void AfterDownstream(std::string original,
                       OnHeadersReceivedCallback callback,
                       int error,
                       const std::optional<std::string>& replacement,
                       const std::optional<GURL>& fragment) {
    if (error != net::OK) {
      std::move(callback).Run(error, replacement, fragment);
      return;
    }
    auto request = snapshot_.Run();
    if (!state_ || !request) {
      std::move(callback).Run(net::ERR_ABORTED, std::nullopt, fragment);
      return;
    }
    if (!state_->Matches("onHeadersReceived", *request)) {
      std::move(callback).Run(net::OK, replacement, fragment);
      return;
    }
    auto headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        replacement.value_or(original));
    state_->Run(owner_, id_, "onHeadersReceived", *request, headers.get(),
                base::BindOnce(&HeaderProxy::AfterApplication,
                               weak_factory_.GetWeakPtr(), headers, replacement,
                               fragment, std::move(callback)));
  }
  void AfterApplication(scoped_refptr<net::HttpResponseHeaders> original,
                        std::optional<std::string> downstream,
                        std::optional<GURL> fragment,
                        OnHeadersReceivedCallback callback,
                        int error,
                        base::DictValue reply) {
    scoped_refptr<net::HttpResponseHeaders> replacement;
    std::string diagnostic;
    if (error == net::OK && !BuildElectronWebRequestHeaders(
                                *original, reply, &replacement, &diagnostic)) {
      LOG(ERROR) << diagnostic;
      error = net::ERR_INVALID_RESPONSE;
    }
    std::move(callback).Run(error,
                            replacement
                                ? std::make_optional(replacement->raw_headers())
                                : downstream,
                            fragment);
  }
  base::WeakPtr<SessionState> state_;
  uint64_t id_;
  Snapshot snapshot_;
  bool preflight_;
  std::string owner_;
  mojo::Receiver<network::mojom::TrustedHeaderClient> receiver_;
  mojo::Remote<network::mojom::TrustedHeaderClient> downstream_;
  base::WeakPtrFactory<HeaderProxy> weak_factory_{this};
};

class ProxyFactory;
class ProxyLoader final : public network::mojom::URLLoader,
                          public network::mojom::URLLoaderClient {
 public:
  ProxyLoader(ProxyFactory* factory,
              base::WeakPtr<SessionState> state,
              int32_t network_id,
              int32_t original_id,
              uint32_t options,
              network::ResourceRequest request,
              bool network,
              bool custom,
              mojo::PendingReceiver<network::mojom::URLLoader> loader,
              mojo::PendingRemote<network::mojom::URLLoaderClient> client,
              net::MutableNetworkTrafficAnnotationTag annotation);
  ~ProxyLoader() override;
  void Start();
  void Abort();
  void BindHeaders(
      mojo::PendingReceiver<network::mojom::TrustedHeaderClient> receiver,
      mojo::PendingRemote<network::mojom::TrustedHeaderClient> downstream) {
    new HeaderProxy(state_, id_,
                    base::BindRepeating(
                        [](base::WeakPtr<ProxyLoader> self)
                            -> std::optional<network::ResourceRequest> {
                          return self ? std::make_optional(self->request_)
                                      : std::nullopt;
                        },
                        weak_factory_.GetWeakPtr()),
                    false, std::move(receiver), std::move(downstream));
  }
  int32_t original_id() const { return original_id_; }
  void FollowRedirect(network::HttpRequestHeadersUpdateParams headers,
                      const std::optional<GURL>& new_url) override;
  void SetPriority(net::RequestPriority priority, int32_t intra) override {
    request_.priority = priority;
    if (target_loader_) {
      target_loader_->SetPriority(priority, intra);
    }
  }
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr hints) override {
    client_->OnReceiveEarlyHints(std::move(hints));
  }
  void OnReceiveResponse(network::mojom::URLResponseHeadPtr head,
                         mojo::ScopedDataPipeConsumerHandle body,
                         std::optional<mojo_base::BigBuffer> metadata) override;
  void OnReceiveRedirect(const net::RedirectInfo& redirect,
                         network::mojom::URLResponseHeadPtr head) override;
  void OnUploadProgress(int64_t current,
                        int64_t total,
                        OnUploadProgressCallback callback) override {
    client_->OnUploadProgress(current, total, std::move(callback));
  }
  void OnTransferSizeUpdated(int32_t size) override {
    client_->OnTransferSizeUpdated(size);
  }
  void OnComplete(const network::URLLoaderCompletionStatus& status) override;

 private:
  void BeforeDone(int error, base::DictValue reply);
  void BeforeSendDone(int error, base::DictValue reply);
  void StartTarget();
  void SendRedirect(const GURL& url);
  void HeadersDone(network::mojom::URLResponseHeadPtr head,
                   mojo::ScopedDataPipeConsumerHandle body,
                   std::optional<mojo_base::BigBuffer> metadata,
                   std::optional<net::RedirectInfo> redirect,
                   int error,
                   base::DictValue reply);
  void Parsed(network::mojom::URLResponseHeadPtr head,
              mojo::ScopedDataPipeConsumerHandle body,
              std::optional<mojo_base::BigBuffer> metadata,
              std::optional<net::RedirectInfo> redirect,
              network::mojom::ParsedHeadersPtr parsed);
  void Deliver(network::mojom::URLResponseHeadPtr head,
               mojo::ScopedDataPipeConsumerHandle body,
               std::optional<mojo_base::BigBuffer> metadata,
               std::optional<net::RedirectInfo> redirect);
  void Finish(int error);
  raw_ptr<ProxyFactory> factory_;
  base::WeakPtr<SessionState> state_;
  uint64_t id_ = NextId();
  std::string owner_ = "loader:" + base::NumberToString(id_);
  int32_t network_id_;
  int32_t original_id_;
  uint32_t options_;
  network::ResourceRequest request_;
  bool network_;
  bool custom_;
  bool waiting_redirect_ = false;
  int redirect_count_ = 0;
  std::optional<net::RedirectInfo> redirect_;
  std::optional<network::HttpRequestHeadersUpdateParams> follow_headers_;
  std::optional<GURL> follow_url_;
  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  mojo::Remote<network::mojom::URLLoader> target_loader_;
  mojo::Receiver<network::mojom::URLLoaderClient> target_client_{this};
  net::MutableNetworkTrafficAnnotationTag annotation_;
  base::CallbackListSubscription abort_subscription_;
  base::WeakPtrFactory<ProxyLoader> weak_factory_{this};
};

class ProxyFactory final : public network::mojom::URLLoaderFactory,
                           public network::mojom::TrustedURLLoaderHeaderClient {
 public:
  ProxyFactory(
      base::WeakPtr<SessionState> state,
      bool network,
      bool custom,
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> target,
      mojo::PendingReceiver<network::mojom::TrustedURLLoaderHeaderClient>
          headers,
      mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>
          downstream)
      : state_(std::move(state)),
        network_(network),
        custom_(custom),
        target_(std::move(target)),
        downstream_(std::move(downstream)) {
    receivers_.Add(this, std::move(receiver));
    receivers_.set_disconnect_handler(base::BindRepeating(
        &ProxyFactory::MaybeDelete, base::Unretained(this)));
    target_.set_disconnect_handler(base::BindOnce(
        &ProxyFactory::TargetDisconnected, base::Unretained(this)));
    if (headers) {
      header_receiver_.Bind(std::move(headers));
    }
  }
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> loader,
      int32_t original_id,
      uint32_t options,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag& annotation) override {
    if (network_ && !header_receiver_.is_bound()) {
      // A response-only proxy cannot affect CORS safely. Never pretend it can
      // implement onHeadersReceived when Chromium offers no trusted channel.
      mojo::Remote<network::mojom::URLLoaderClient> failed(std::move(client));
      failed->OnComplete(
          network::URLLoaderCompletionStatus(net::ERR_NOT_IMPLEMENTED));
      return;
    }
    if (!custom_ && IsCustomProtocol(request.url)) {
      // The custom factory is permanently wrapped before its CORS decision.
      target_->CreateLoaderAndStart(std::move(loader), original_id, options,
                                    request, std::move(client), annotation);
      return;
    }
    // Request IDs from multiple factory clones are not assumed globally unique.
    static int32_t next_network_id = 0;
    do {
      if (next_network_id == std::numeric_limits<int32_t>::max()) {
        next_network_id = 0;
      }
      ++next_network_id;
    } while (loaders_.contains(next_network_id));
    const int32_t id = next_network_id;
    auto owned = std::make_unique<ProxyLoader>(
        this, state_, id, original_id, options, request, network_, custom_,
        std::move(loader), std::move(client), annotation);
    auto* value = owned.get();
    loaders_.emplace(id, std::move(owned));
    value->Start();
  }
  void Clone(mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver)
      override {
    receivers_.Add(this, std::move(receiver));
  }
  void OnLoaderCreated(
      int32_t request_id,
      mojo::PendingReceiver<network::mojom::TrustedHeaderClient> receiver)
      override {
    const auto found = loaders_.find(request_id);
    if (found == loaders_.end()) {
      if (downstream_) {
        downstream_->OnLoaderCreated(request_id, std::move(receiver));
      }
      return;
    }
    mojo::PendingRemote<network::mojom::TrustedHeaderClient> target;
    if (downstream_) {
      downstream_->OnLoaderCreated(found->second->original_id(),
                                   target.InitWithNewPipeAndPassReceiver());
    }
    found->second->BindHeaders(std::move(receiver), std::move(target));
  }
  void OnLoaderForCorsPreflightCreated(
      const network::ResourceRequest& request,
      mojo::PendingReceiver<network::mojom::TrustedHeaderClient> receiver)
      override {
    mojo::PendingRemote<network::mojom::TrustedHeaderClient> target;
    if (downstream_) {
      downstream_->OnLoaderForCorsPreflightCreated(
          request, target.InitWithNewPipeAndPassReceiver());
    }
    new HeaderProxy(state_, NextId(),
                    base::BindRepeating(
                        [](network::ResourceRequest value) {
                          return std::make_optional(value);
                        },
                        request),
                    true, std::move(receiver), std::move(target));
  }
  network::mojom::URLLoaderFactory* target() { return target_.get(); }
  void Remove(int32_t id) {
    loaders_.erase(id);
    MaybeDelete();
  }

 private:
  void TargetDisconnected() {
    receivers_.Clear();
    // Suppress self deletion until all per-request callbacks have run.
    disconnecting_ = true;
    while (!loaders_.empty()) {
      loaders_.begin()->second->Abort();
    }
    disconnecting_ = false;
    MaybeDelete();
  }
  void MaybeDelete() {
    if (!disconnecting_ && receivers_.empty() && loaders_.empty()) {
      delete this;
    }
  }
  base::WeakPtr<SessionState> state_;
  bool network_;
  bool custom_;
  bool disconnecting_ = false;
  mojo::Remote<network::mojom::URLLoaderFactory> target_;
  mojo::ReceiverSet<network::mojom::URLLoaderFactory> receivers_;
  mojo::Receiver<network::mojom::TrustedURLLoaderHeaderClient> header_receiver_{
      this};
  mojo::Remote<network::mojom::TrustedURLLoaderHeaderClient> downstream_;
  std::map<int32_t, std::unique_ptr<ProxyLoader>> loaders_;
};

ProxyLoader::ProxyLoader(
    ProxyFactory* factory,
    base::WeakPtr<SessionState> state,
    int32_t network_id,
    int32_t original_id,
    uint32_t options,
    network::ResourceRequest request,
    bool network,
    bool custom,
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    net::MutableNetworkTrafficAnnotationTag annotation)
    : factory_(factory),
      state_(std::move(state)),
      network_id_(network_id),
      original_id_(original_id),
      options_(options),
      request_(std::move(request)),
      network_(network),
      custom_(custom),
      receiver_(this, std::move(loader)),
      client_(std::move(client)),
      annotation_(annotation) {
  receiver_.set_disconnect_handler(
      base::BindOnce(&ProxyLoader::Abort, weak_factory_.GetWeakPtr()));
  client_.set_disconnect_handler(
      base::BindOnce(&ProxyLoader::Abort, weak_factory_.GetWeakPtr()));
  if (state_) {
    abort_subscription_ = state_->ObserveAbort(
        base::BindRepeating(&ProxyLoader::Abort, weak_factory_.GetWeakPtr()));
  }
}
ProxyLoader::~ProxyLoader() {
  weak_factory_.InvalidateWeakPtrs();
  if (state_) {
    state_->CancelOwner(owner_);
  }
}
void ProxyLoader::Start() {
  if (!state_) {
    Finish(net::ERR_ABORTED);
    return;
  }
  state_->Run(
      owner_, id_, "onBeforeRequest", request_, nullptr,
      base::BindOnce(&ProxyLoader::BeforeDone, weak_factory_.GetWeakPtr()));
}
void ProxyLoader::BeforeDone(int error, base::DictValue reply) {
  if (error != net::OK) {
    Finish(error);
    return;
  }
  if (const auto* value = reply.Find("redirectURL")) {
    if (!value->is_string()) {
      Finish(net::ERR_INVALID_ARGUMENT);
      return;
    }
    GURL url(value->GetString());
    if (!url.is_valid() || url.has_username() || url.has_password()) {
      Finish(net::ERR_INVALID_URL);
      return;
    }
    SendRedirect(url);
    return;
  }
  if (!network_) {
    if (!state_) {
      Finish(net::ERR_ABORTED);
      return;
    }
    auto request = request_;
    request.headers.MergeFrom(request.cors_exempt_headers);
    state_->Run(owner_, id_, "onBeforeSendHeaders", request, nullptr,
                base::BindOnce(&ProxyLoader::BeforeSendDone,
                               weak_factory_.GetWeakPtr()));
    return;
  }
  StartTarget();
}
void ProxyLoader::BeforeSendDone(int error, base::DictValue reply) {
  std::optional<net::HttpRequestHeaders> replacement;
  std::string diagnostic;
  if (error == net::OK && !BuildElectronWebRequestRequestHeaders(
                              reply, &replacement, &diagnostic)) {
    LOG(ERROR) << diagnostic;
    error = net::ERR_INVALID_ARGUMENT;
  }
  if (error != net::OK) {
    Finish(error);
    return;
  }
  if (replacement) {
    if (follow_headers_) {
      // Existing non-network factories receive the same complete replacement
      // through their redirect update. No new header gains CORS exemption.
      for (const auto& header : request_.headers.GetHeaderVector()) {
        follow_headers_->removed_headers.push_back(header.key);
      }
      for (const auto& header :
           request_.cors_exempt_headers.GetHeaderVector()) {
        follow_headers_->removed_headers.push_back(header.key);
      }
      follow_headers_->modified_headers = *replacement;
      follow_headers_->modified_cors_exempt_headers.Clear();
    }
    request_.headers = std::move(*replacement);
    request_.cors_exempt_headers.Clear();
  }
  StartTarget();
}
void ProxyLoader::StartTarget() {
  if (target_loader_) {
    target_loader_->FollowRedirect(std::move(*follow_headers_), follow_url_);
    follow_headers_.reset();
    follow_url_.reset();
    target_client_.Resume();
    return;
  }
  if (network_) {
    options_ |= network::mojom::kURLLoadOptionUseHeaderClient;
  }
  factory_->target()->CreateLoaderAndStart(
      target_loader_.BindNewPipeAndPassReceiver(), network_id_, options_,
      request_, target_client_.BindNewPipeAndPassRemote(), annotation_);
  // Completion and disconnect are ordered on the client pipe. The separate
  // URLLoader pipe may close before its already-sent OnComplete is delivered.
  target_client_.set_disconnect_handler(
      base::BindOnce(&ProxyLoader::Abort, weak_factory_.GetWeakPtr()));
}
void ProxyLoader::SendRedirect(const GURL& url) {
  if (url.scheme() != request_.url.scheme() &&
      !(url.SchemeIsHTTPOrHTTPS() && request_.url.SchemeIsHTTPOrHTTPS())) {
    Finish(net::ERR_NOT_IMPLEMENTED);
    return;
  }
  target_loader_.reset();
  target_client_.reset();
  waiting_redirect_ = true;
  redirect_ = net::RedirectInfo::ComputeRedirectInfo(
      request_.method, request_.url, request_.site_for_cookies,
      request_.update_first_party_url_on_redirect
          ? net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT
          : net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL,
      request_.referrer_policy, request_.referrer.spec(),
      request_.request_initiator, 307, url, std::nullopt, false, false);
  auto head = network::mojom::URLResponseHead::New();
  head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
      "HTTP/1.1 307 Internal Redirect");
  head->headers->AddHeader("Location", url.spec());
  client_->OnReceiveRedirect(*redirect_, std::move(head));
}
void ProxyLoader::FollowRedirect(
    network::HttpRequestHeadersUpdateParams headers,
    const std::optional<GURL>& new_url) {
  if (!waiting_redirect_ || !redirect_) {
    Finish(net::ERR_UNEXPECTED);
    return;
  }
  if (++redirect_count_ > 20) {
    Finish(net::ERR_TOO_MANY_REDIRECTS);
    return;
  }
  const auto target = new_url.value_or(redirect_->new_url);
  const bool cross_origin = !url::IsSameOriginWith(target, request_.url);
  if (new_url &&
      !url::Origin::Create(*new_url).IsSameOriginWith(redirect_->new_url)) {
    Finish(net::ERR_UNSAFE_REDIRECT);
    return;
  }
  bool clear_upload = false;
  net::RedirectUtil::UpdateHttpRequest(
      request_.url, request_.method, *redirect_, headers.removed_headers,
      headers.modified_headers, &request_.headers, &clear_upload);
  if (clear_upload) {
    request_.request_body.reset();
  }
  request_.url = target;
  request_.method = redirect_->new_method;
  request_.referrer = GURL(redirect_->new_referrer);
  request_.referrer_policy = redirect_->new_referrer_policy;
  request_.site_for_cookies = redirect_->new_site_for_cookies;
  for (const auto& header : headers.removed_headers) {
    request_.cors_exempt_headers.RemoveHeader(header);
  }
  request_.cors_exempt_headers.MergeFrom(headers.modified_cors_exempt_headers);
  if (!network_ && cross_origin) {
    // Custom/file factories do not have URLRequest's redirect credential
    // handling. A previous hop's credentials must not reach another origin;
    // the application can explicitly add credentials for the new URL in its
    // next onBeforeSendHeaders callback.
    for (const char* name : {"Authorization", "Cookie"}) {
      request_.headers.RemoveHeader(name);
      request_.cors_exempt_headers.RemoveHeader(name);
      headers.modified_headers.RemoveHeader(name);
      headers.modified_cors_exempt_headers.RemoveHeader(name);
      headers.removed_headers.emplace_back(name);
    }
  }
  follow_headers_ = std::move(headers);
  follow_url_ = new_url;
  waiting_redirect_ = false;
  redirect_.reset();
  Start();
}
void ProxyLoader::OnReceiveResponse(
    network::mojom::URLResponseHeadPtr head,
    mojo::ScopedDataPipeConsumerHandle body,
    std::optional<mojo_base::BigBuffer> metadata) {
  if (network_) {
    client_->OnReceiveResponse(std::move(head), std::move(body),
                               std::move(metadata));
    return;
  }
  if (!state_) {
    Finish(net::ERR_ABORTED);
    return;
  }
  if (!custom_ && !state_->Matches("onHeadersReceived", request_)) {
    client_->OnReceiveResponse(std::move(head), std::move(body),
                               std::move(metadata));
    return;
  }
  target_client_.Pause();
  if (!head->headers) {
    head->headers =
        base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 200 OK");
    if (!head->mime_type.empty()) {
      head->headers->AddHeader(
          "Content-Type",
          head->mime_type +
              (head->charset.empty() ? "" : "; charset=" + head->charset));
    }
    if (head->content_length >= 0) {
      head->headers->AddHeader("Content-Length",
                               base::NumberToString(head->content_length));
    }
  }
  auto headers = head->headers;
  state_->Run(
      owner_, id_, "onHeadersReceived", request_, headers.get(),
      base::BindOnce(&ProxyLoader::HeadersDone, weak_factory_.GetWeakPtr(),
                     std::move(head), std::move(body), std::move(metadata),
                     std::nullopt));
}
void ProxyLoader::OnReceiveRedirect(const net::RedirectInfo& redirect,
                                    network::mojom::URLResponseHeadPtr head) {
  target_client_.Pause();
  if (network_) {
    Deliver(std::move(head), {}, std::nullopt, redirect);
    return;
  }
  if (!state_) {
    Finish(net::ERR_ABORTED);
    return;
  }
  if (!custom_ && !state_->Matches("onHeadersReceived", request_)) {
    redirect_ = redirect;
    waiting_redirect_ = true;
    client_->OnReceiveRedirect(redirect, std::move(head));
    return;
  }
  auto headers = head->headers;
  state_->Run(
      owner_, id_, "onHeadersReceived", request_, headers.get(),
      base::BindOnce(&ProxyLoader::HeadersDone, weak_factory_.GetWeakPtr(),
                     std::move(head), mojo::ScopedDataPipeConsumerHandle(),
                     std::optional<mojo_base::BigBuffer>(), redirect));
}
void ProxyLoader::HeadersDone(network::mojom::URLResponseHeadPtr head,
                              mojo::ScopedDataPipeConsumerHandle body,
                              std::optional<mojo_base::BigBuffer> metadata,
                              std::optional<net::RedirectInfo> redirect,
                              int error,
                              base::DictValue reply) {
  if (error != net::OK) {
    Finish(error);
    return;
  }
  scoped_refptr<net::HttpResponseHeaders> replacement;
  std::string diagnostic;
  if (!BuildElectronWebRequestHeaders(*head->headers, reply, &replacement,
                                      &diagnostic)) {
    LOG(ERROR) << diagnostic;
    Finish(net::ERR_INVALID_RESPONSE);
    return;
  }
  if (replacement) {
    head->headers = std::move(replacement);
    head->headers->GetMimeTypeAndCharset(&head->mime_type, &head->charset);
  }
  if (reply.contains("responseHeaders") || reply.contains("statusLine") ||
      !head->parsed_headers) {
    // Parsing untrusted CSP/COOP headers belongs in the sandboxed network
    // service.
    auto headers = head->headers;
    content::GetNetworkService()->ParseHeaders(
        request_.url, headers,
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&ProxyLoader::Parsed, weak_factory_.GetWeakPtr(),
                           std::move(head), std::move(body),
                           std::move(metadata), redirect),
            network::mojom::ParsedHeadersPtr()));
    return;
  }
  Deliver(std::move(head), std::move(body), std::move(metadata), redirect);
}
void ProxyLoader::Parsed(network::mojom::URLResponseHeadPtr head,
                         mojo::ScopedDataPipeConsumerHandle body,
                         std::optional<mojo_base::BigBuffer> metadata,
                         std::optional<net::RedirectInfo> redirect,
                         network::mojom::ParsedHeadersPtr parsed) {
  if (!parsed) {
    Finish(net::ERR_FAILED);
    return;
  }
  head->parsed_headers = std::move(parsed);
  Deliver(std::move(head), std::move(body), std::move(metadata), redirect);
}
void ProxyLoader::Deliver(network::mojom::URLResponseHeadPtr head,
                          mojo::ScopedDataPipeConsumerHandle body,
                          std::optional<mojo_base::BigBuffer> metadata,
                          std::optional<net::RedirectInfo> redirect) {
  if (custom_ &&
      head->response_type == network::mojom::FetchResponseType::kCors) {
    if (!request_.request_initiator ||
        !network::cors::CheckAccess(
             request_.url,
             head->headers->GetNormalizedHeader("Access-Control-Allow-Origin"),
             head->headers->GetNormalizedHeader(
                 "Access-Control-Allow-Credentials"),
             request_.credentials_mode, *request_.request_initiator)
             .has_value()) {
      Finish(net::ERR_ACCESS_DENIED);
      return;
    }
  }
  if (!network_) {
    std::string location;
    if (head->headers->IsRedirect(&location)) {
      GURL url = request_.url.Resolve(location);
      if (!url.is_valid()) {
        Finish(net::ERR_INVALID_URL);
        return;
      }
      if (url.scheme() != request_.url.scheme()) {
        Finish(net::ERR_NOT_IMPLEMENTED);
        return;
      }
      redirect = net::RedirectInfo::ComputeRedirectInfo(
          request_.method, request_.url, request_.site_for_cookies,
          request_.update_first_party_url_on_redirect
              ? net::RedirectInfo::FirstPartyURLPolicy::UPDATE_URL_ON_REDIRECT
              : net::RedirectInfo::FirstPartyURLPolicy::NEVER_CHANGE_URL,
          request_.referrer_policy, request_.referrer.spec(),
          request_.request_initiator, head->headers->response_code(), url,
          net::RedirectUtil::GetReferrerPolicyHeader(head->headers.get()),
          false, true);
      // A file/custom factory may already be writing the response body. A
      // header-generated redirect consumes none of it and starts a fresh load.
      target_loader_.reset();
      target_client_.reset();
    } else if (redirect) {
      Finish(net::ERR_NOT_IMPLEMENTED);
      return;
    }
  }
  if (redirect) {
    redirect_ = redirect;
    waiting_redirect_ = true;
    client_->OnReceiveRedirect(*redirect, std::move(head));
  } else {
    client_->OnReceiveResponse(std::move(head), std::move(body),
                               std::move(metadata));
    if (target_client_.is_bound()) {
      target_client_.Resume();
    }
  }
}
void ProxyLoader::OnComplete(const network::URLLoaderCompletionStatus& status) {
  client_->OnComplete(status);
  factory_->Remove(network_id_);
}
void ProxyLoader::Finish(int error) {
  client_->OnComplete(network::URLLoaderCompletionStatus(error));
  factory_->Remove(network_id_);
}
void ProxyLoader::Abort() {
  Finish(net::ERR_ABORTED);
}
}  // namespace

bool SetElectronWebRequestHandler(content::BrowserContext* context,
                                  const std::string& container,
                                  uint64_t generation,
                                  const base::Value& arguments,
                                  ElectronWebRequestDispatch dispatch,
                                  ElectronWebRequestCancel cancel,
                                  ElectronWebRequestRelease release,
                                  std::string* error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto* dict = arguments.GetIfDict();
  const auto* partition = dict ? dict->FindString("partition") : nullptr;
  const auto* event = dict ? dict->FindString("event") : nullptr;
  const auto* id = dict ? dict->FindString("registrationId") : nullptr;
  auto enabled = dict ? dict->FindBool("enabled") : std::nullopt;
  const auto* filter = dict ? dict->FindDict("filter") : nullptr;
  if (!partition || partition->size() > 1024 ||
      partition->find('\0') != std::string::npos || !event ||
      (*event != "onBeforeRequest" && *event != "onBeforeSendHeaders" &&
       *event != "onHeadersReceived") ||
      !enabled ||
      (*enabled && (!id || id->empty() || id->size() > 128 || !filter ||
                    !dispatch || !cancel || !release))) {
    *error = "ERR_INVALID_ARG_VALUE: invalid webRequest registration";
    return false;
  }
  std::shared_ptr<Registration> registration;
  if (*enabled) {
    registration = std::make_shared<Registration>();
    if (!registration->filter.Parse(*filter, error)) {
      return false;
    }
    registration->id = *id;
    registration->generation = generation;
    registration->dispatch = std::move(dispatch);
    registration->cancel = std::move(cancel);
    registration->release = std::move(release);
  }
  auto* storage =
      ResolveElectronSessionPartition(context, container, *partition);
  auto* state = GetState(context, storage);
  if (!state) {
    *error = "ERR_NOT_SUPPORTED: Electron Session is unavailable";
    return false;
  }
  state->Set(*event, std::move(registration));
  return true;
}
void RemoveElectronWebRequestHandlers(const std::string& container) {
  for (auto* registry : Registries()) {
    std::erase_if(registry->sessions, [&](const auto& item) {
      return item.second->identity().container_id == container;
    });
  }
}
bool AppendElectronWebRequestProxy(
    content::BrowserContext* context,
    content::StoragePartition* partition,
    network::URLLoaderFactoryBuilder& builder,
    bool network,
    mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>*
        headers) {
  auto* state = GetState(context, partition);
  if (!state) {
    return false;
  }
  auto [receiver, target] = builder.Append();
  mojo::PendingReceiver<network::mojom::TrustedURLLoaderHeaderClient>
      header_receiver;
  mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient> downstream;
  if (network && headers) {
    downstream = std::move(*headers);
    *headers = header_receiver.InitWithNewPipeAndPassRemote();
  }
  new ProxyFactory(state->GetWeakPtr(), network, false, std::move(receiver),
                   std::move(target), std::move(header_receiver),
                   std::move(downstream));
  return true;
}
mojo::PendingRemote<network::mojom::URLLoaderFactory>
WrapElectronWebRequestFactory(
    content::BrowserContext* context,
    content::StoragePartition* partition,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target,
    ElectronWebRequestFactoryKind kind) {
  auto* state = GetState(context, partition);
  if (!target) {
    return target;
  }
  if (!state) {
    // A custom protocol's security check must never be bypassed on teardown.
    if (kind == ElectronWebRequestFactoryKind::kCustomProtocol) {
      return {};
    }
    return target;
  }
  mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
  new ProxyFactory(state->GetWeakPtr(), false,
                   kind == ElectronWebRequestFactoryKind::kCustomProtocol,
                   remote.InitWithNewPipeAndPassReceiver(), std::move(target),
                   {}, {});
  return remote;
}
}  // namespace xenon::ipc
