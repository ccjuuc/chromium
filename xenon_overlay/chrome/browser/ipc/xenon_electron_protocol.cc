// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_protocol.h"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <utility>

#include "base/byte_size.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/self_deleting.h"
#include "base/memory/weak_ptr.h"
#include "base/no_destructor.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/timer/timer.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/data_pipe_producer.h"
#include "mojo/public/cpp/system/string_data_source.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/cors/cors.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/self_deleting_url_loader_factory.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_guest.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"
#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_host.h"
#include "xenon_overlay/chrome/browser/xenon_manager.h"
#include "xenon_overlay/common/ipc/xenon_privileged_schemes.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMaxBody = 32 * 1024 * 1024;
constexpr size_t kMaxHeaders = 64 * 1024;
struct SessionKey {
  uintptr_t context_id = 0;
  std::string container;
  std::string partition;
};
struct Registration {
  SessionKey session;
  std::string scheme;
  uint64_t generation;
};
auto& Registrations() {
  static base::NoDestructor<std::map<std::string, Registration>> entries;
  return *entries;
}
std::string Key(const SessionKey& session, const std::string& scheme) {
  return base::NumberToString(session.context_id) + '\0' + session.container +
         '\0' + session.partition + '\0' + scheme;
}
const PrivilegedScheme* DeclaredScheme(const std::string& scheme) {
  for (const auto& item : GetStartupPrivilegedSchemes()) {
    if (item.scheme == scheme) {
      return &item;
    }
  }
  return nullptr;
}
std::optional<SessionKey> ResolveSession(content::WebContents* contents) {
  if (!contents) {
    return std::nullopt;
  }
  SessionKey key;
  key.context_id = reinterpret_cast<uintptr_t>(contents->GetBrowserContext());
  const base::DictValue* prefs = nullptr;
  if (auto* guest = XenonElectronGuest::FromWebContents(contents)) {
    key.container = guest->container_id();
    prefs = &guest->preferences();
  } else {
    auto* host = XenonElectronWindowHost::GetInstance();
    if (!host->FindWindowIdForWebContents(contents)) {
      return std::nullopt;
    }
    key.container = host->GetContainerIdForWebContents(contents);
    prefs = host->GetWebPreferencesForWebContents(contents);
  }
  if (key.container.empty()) {
    key.container = "default";
  }
  if (prefs) {
    if (const auto* partition = prefs->FindString("partition")) {
      key.partition = *partition;
    }
  }
  const auto identity = FindElectronSessionIdentity(
      contents->GetPrimaryMainFrame()->GetStoragePartition());
  if (!identity || identity->container_id != key.container ||
      identity->partition != key.partition) {
    return std::nullopt;
  }
  return key;
}
bool StillRegistered(const SessionKey& session,
                     const std::string& scheme,
                     uint64_t generation) {
  const auto it = Registrations().find(Key(session, scheme));
  return it != Registrations().end() && it->second.generation == generation &&
         XenonManager::GetInstance()->service_generation(session.container) ==
             generation;
}

class ProtocolLoader final : public network::mojom::URLLoader {
 public:
  ProtocolLoader(SessionKey session,
                 std::string scheme,
                 uint64_t generation,
                 const network::ResourceRequest& request,
                 mojo::PendingReceiver<network::mojom::URLLoader> receiver,
                 mojo::PendingRemote<network::mojom::URLLoaderClient> client)
      : session_(std::move(session)),
        scheme_(std::move(scheme)),
        generation_(generation),
        request_(request),
        receiver_(this, std::move(receiver)),
        client_(std::move(client)) {
    static uint64_t next_id = 0;
    id_ = base::NumberToString(++next_id);
    receiver_.set_disconnect_handler(
        base::BindOnce(&ProtocolLoader::Abort, weak_factory_.GetWeakPtr()));
    client_.set_disconnect_handler(
        base::BindOnce(&ProtocolLoader::Abort, weak_factory_.GetWeakPtr()));
  }
  void Start() {
    if (!StillRegistered(session_, scheme_, generation_) ||
        !request_.url.is_valid() || request_.url.scheme() != scheme_ ||
        request_.url.has_username() || request_.url.has_password() ||
        request_.url.spec().size() > 2 * 1024 * 1024 ||
        !net::HttpUtil::IsToken(request_.method)) {
      Finish(net::ERR_INVALID_ARGUMENT);
      return;
    }
    const bool same_origin =
        request_.request_initiator &&
        request_.request_initiator->IsSameOriginWith(request_.url);
    if (request_.mode == network::mojom::RequestMode::kSameOrigin &&
        !same_origin) {
      Finish(net::ERR_ACCESS_DENIED);
      return;
    }
    response_type_ =
        network::cors::CalculateResponseType(request_.mode, same_origin);
    // Chromium adds forbidden author headers such as User-Agent and Referer.
    // Like CorsUnsafeNotForbiddenRequestHeaderNames in the network service,
    // exclude those before deciding whether this request needs a preflight.
    auto author_headers = request_.headers.GetHeaderVector();
    std::erase_if(author_headers, [](const auto& header) {
      return !net::HttpUtil::IsSafeHeader(header.key, header.value);
    });
    if (response_type_ == network::mojom::FetchResponseType::kCors &&
        (!request_.request_initiator ||
         !DeclaredScheme(scheme_)->cors_enabled ||
         request_.mode ==
             network::mojom::RequestMode::kCorsWithForcedPreflight ||
         !network::cors::IsCorsSafelistedMethod(request_.method) ||
         !network::cors::CorsUnsafeRequestHeaderNames(author_headers)
              .empty())) {
      Finish(net::ERR_ACCESS_DENIED);
      return;
    }
    base::ListValue headers;
    size_t header_bytes = 0;
    for (const auto& header : request_.headers.GetHeaderVector()) {
      header_bytes += header.key.size() + header.value.size();
      if (header_bytes > kMaxHeaders) {
        Finish(net::ERR_INVALID_ARGUMENT);
        return;
      }
      headers.Append(base::ListValue().Append(header.key).Append(header.value));
    }
    base::Value::BlobStorage body;
    if (request_.request_body) {
      for (const auto& element : *request_.request_body->elements()) {
        if (element.type() != network::DataElement::Tag::kBytes) {
          Finish(net::ERR_NOT_IMPLEMENTED);
          return;
        }
        const auto& bytes = element.As<network::DataElementBytes>().bytes();
        if (bytes.size() > kMaxBody - body.size()) {
          Finish(net::ERR_FILE_TOO_BIG);
          return;
        }
        body.insert(body.end(), bytes.begin(), bytes.end());
      }
    }
    base::DictValue details;
    details.Set("url", request_.url.spec());
    details.Set("method", request_.method);
    details.Set("headers", std::move(headers));
    details.Set("body", base::Value(std::move(body)));
    timer_.Start(
        FROM_HERE, base::Seconds(30),
        base::BindOnce(&ProtocolLoader::TimedOut, weak_factory_.GetWeakPtr()));
    pending_ = true;
    XenonManager::GetInstance()->RequestElectronProtocol(
        session_.container, session_.partition, generation_, id_,
        base::Value(std::move(details)),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&ProtocolLoader::OnResponse,
                           weak_factory_.GetWeakPtr()),
            ipc::mojom::IpcResultPtr()));
  }
  void FollowRedirect(network::HttpRequestHeadersUpdateParams,
                      const std::optional<GURL>&) override {
    Finish(net::ERR_NOT_IMPLEMENTED);
  }
  void SetPriority(net::RequestPriority, int32_t) override {}

 private:
  ~ProtocolLoader() override = default;
  void TimedOut() {
    Cancel();
    Finish(net::ERR_TIMED_OUT);
  }
  void Cancel() {
    if (pending_) {
      XenonManager::GetInstance()->CancelElectronProtocol(session_.container,
                                                          generation_, id_);
    }
    pending_ = false;
  }
  void Abort() {
    Cancel();
    delete this;
  }
  void Finish(int error) {
    Cancel();
    timer_.Stop();
    if (client_.is_bound()) {
      network::URLLoaderCompletionStatus completion(error);
      completion.encoded_data_length = base::ByteSize(body_size_);
      completion.encoded_body_length = base::ByteSize(body_size_);
      completion.decoded_body_length = base::ByteSize(body_size_);
      client_->OnComplete(completion);
    }
    delete this;
  }
  void OnResponse(ipc::mojom::IpcResultPtr result) {
    pending_ = false;
    if (result && !result->success) {
      LOG(ERROR) << "Electron protocol handler failed: " << result->error;
    }
    if (!result || !result->success || !result->value.is_dict() ||
        !StillRegistered(session_, scheme_, generation_)) {
      Finish(net::ERR_FAILED);
      return;
    }
    const auto& value = result->value.GetDict();
    const auto status = value.FindInt("status");
    const auto* status_text = value.FindString("statusText");
    const auto* headers = value.FindList("headers");
    const auto* body = value.FindBlob("body");
    if (!status || *status < 200 || *status > 599 || !status_text ||
        status_text->size() > 1024 ||
        !net::HttpUtil::IsValidHeaderValue(*status_text) || !headers || !body ||
        body->size() > kMaxBody) {
      Finish(net::ERR_INVALID_RESPONSE);
      return;
    }
    auto head = network::mojom::URLResponseHead::New();
    head->headers = base::MakeRefCounted<net::HttpResponseHeaders>(
        "HTTP/1.1 " + base::NumberToString(*status) + " " + *status_text);
    size_t bytes = 0;
    for (const auto& entry : *headers) {
      const auto* pair = entry.GetIfList();
      if (!pair || pair->size() != 2 || !(*pair)[0].is_string() ||
          !(*pair)[1].is_string()) {
        Finish(net::ERR_INVALID_RESPONSE);
        return;
      }
      const auto& name = (*pair)[0].GetString();
      const auto& val = (*pair)[1].GetString();
      bytes += name.size() + val.size();
      if (bytes > kMaxHeaders || !net::HttpUtil::IsValidHeaderName(name) ||
          !net::HttpUtil::IsValidHeaderValue(val)) {
        Finish(net::ERR_INVALID_RESPONSE);
        return;
      }
      head->headers->AddHeader(name, val);
    }
    // Redirects require a new browser-owned URLLoader route. Do not return a
    // misleading successful body until that cross-scheme path is supported.
    std::string redirect;
    if (head->headers->IsRedirect(&redirect)) {
      Finish(net::ERR_NOT_IMPLEMENTED);
      return;
    }
    if (response_type_ == network::mojom::FetchResponseType::kCors) {
      const auto check = network::cors::CheckAccess(
          request_.url,
          head->headers->GetNormalizedHeader("Access-Control-Allow-Origin"),
          head->headers->GetNormalizedHeader(
              "Access-Control-Allow-Credentials"),
          request_.credentials_mode, *request_.request_initiator);
      if (!check.has_value()) {
        Finish(net::ERR_ACCESS_DENIED);
        return;
      }
    }
    if ((*status == 204 || *status == 205 || *status == 304) &&
        !body->empty()) {
      Finish(net::ERR_INVALID_RESPONSE);
      return;
    }
    body_size_ = request_.method == "HEAD" ? 0 : body->size();
    head->content_length = body_size_;
    head->response_type = response_type_;
    head->headers->GetMimeTypeAndCharset(&head->mime_type, &head->charset);
    mojo::ScopedDataPipeProducerHandle producer;
    mojo::ScopedDataPipeConsumerHandle consumer;
    if (mojo::CreateDataPipe(nullptr, producer, consumer) != MOJO_RESULT_OK) {
      Finish(net::ERR_INSUFFICIENT_RESOURCES);
      return;
    }
    client_->OnReceiveResponse(std::move(head), std::move(consumer),
                               std::nullopt);
    if (!body_size_) {
      Finish(net::OK);
      return;
    }
    producer_ = std::make_unique<mojo::DataPipeProducer>(std::move(producer));
    producer_->Write(
        std::make_unique<mojo::StringDataSource>(
            base::as_chars(base::span(*body)),
            mojo::StringDataSource::AsyncWritingMode::
                STRING_MAY_BE_INVALIDATED_BEFORE_COMPLETION),
        base::BindOnce(&ProtocolLoader::Written, weak_factory_.GetWeakPtr()));
  }
  void Written(MojoResult result) {
    Finish(result == MOJO_RESULT_OK ? net::OK : net::ERR_FAILED);
  }
  SessionKey session_;
  std::string scheme_;
  uint64_t generation_;
  network::ResourceRequest request_;
  std::string id_;
  bool pending_ = false;
  size_t body_size_ = 0;
  network::mojom::FetchResponseType response_type_ =
      network::mojom::FetchResponseType::kDefault;
  mojo::Receiver<network::mojom::URLLoader> receiver_;
  mojo::Remote<network::mojom::URLLoaderClient> client_;
  std::unique_ptr<mojo::DataPipeProducer> producer_;
  base::OneShotTimer timer_;
  base::WeakPtrFactory<ProtocolLoader> weak_factory_{this};
};

class ProtocolFactory final : public network::SelfDeletingURLLoaderFactory {
 public:
  ProtocolFactory(
      mojo::PendingReceiver<network::mojom::URLLoaderFactory> receiver,
      SessionKey session,
      std::string scheme,
      uint64_t generation,
      base::SelfDeletingPassKey key)
      : SelfDeletingURLLoaderFactory(std::move(receiver), key),
        session_(std::move(session)),
        scheme_(std::move(scheme)),
        generation_(generation) {}

 private:
  ~ProtocolFactory() override = default;
  void CreateLoaderAndStart(
      mojo::PendingReceiver<network::mojom::URLLoader> receiver,
      int32_t,
      uint32_t,
      const network::ResourceRequest& request,
      mojo::PendingRemote<network::mojom::URLLoaderClient> client,
      const net::MutableNetworkTrafficAnnotationTag&) override {
    auto* loader = new ProtocolLoader(session_, scheme_, generation_, request,
                                      std::move(receiver), std::move(client));
    loader->Start();
  }
  SessionKey session_;
  std::string scheme_;
  uint64_t generation_;
};
}  // namespace

bool SetElectronProtocolHandler(content::BrowserContext* context,
                                const std::string& container_id,
                                uint64_t generation,
                                const base::Value& arguments,
                                std::string* error) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto* dict = arguments.GetIfDict();
  const auto* scheme = dict ? dict->FindString("scheme") : nullptr;
  const auto* partition = dict ? dict->FindString("partition") : nullptr;
  const auto handled = dict ? dict->FindBool("handled") : std::nullopt;
  if (!context || !scheme || !partition || !handled ||
      partition->size() > 1024 || partition->find('\0') != std::string::npos ||
      !DeclaredScheme(*scheme)) {
    *error =
        "ERR_INVALID_ARG_VALUE: invalid or undeclared protocol registration";
    return false;
  }
  SessionKey session{reinterpret_cast<uintptr_t>(context),
                     container_id.empty() ? "default" : container_id,
                     *partition};
  if (!ResolveElectronSessionPartition(context, session.container,
                                       session.partition)) {
    *error = "ERR_INVALID_ARG_VALUE: protocol session is unavailable";
    return false;
  }
  const auto key = Key(session, *scheme);
  if (*handled) {
    if (Registrations().contains(key)) {
      *error = "ERR_INVALID_ARG_VALUE: protocol is already handled";
      return false;
    }
    Registrations().emplace(key, Registration{session, *scheme, generation});
  } else {
    Registrations().erase(key);
  }
  return true;
}

void RemoveElectronProtocolHandlers(const std::string& container_id) {
  const auto normalized = container_id.empty() ? "default" : container_id;
  std::erase_if(Registrations(), [&](const auto& entry) {
    return entry.second.session.container == normalized;
  });
}

std::vector<std::string> GetElectronProtocolSchemes(
    content::WebContents* contents) {
  std::vector<std::string> result;
  const auto session = ResolveSession(contents);
  if (!session) {
    return result;
  }
  for (const auto& scheme : GetStartupPrivilegedSchemes()) {
    auto it = Registrations().find(Key(*session, scheme.scheme));
    if (it != Registrations().end()) {
      result.push_back(scheme.scheme);
    }
  }
  return result;
}

mojo::PendingRemote<network::mojom::URLLoaderFactory>
CreateElectronProtocolURLLoaderFactory(content::WebContents* contents,
                                       const std::string& scheme) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto session = ResolveSession(contents);
  if (!session) {
    return {};
  }
  auto it = Registrations().find(Key(*session, scheme));
  if (it == Registrations().end()) {
    return {};
  }
  mojo::PendingRemote<network::mojom::URLLoaderFactory> remote;
  base::MakeSelfDeleting<ProtocolFactory>(
      remote.InitWithNewPipeAndPassReceiver(), *session, scheme,
      it->second.generation);
  return remote;
}
}  // namespace xenon::ipc
