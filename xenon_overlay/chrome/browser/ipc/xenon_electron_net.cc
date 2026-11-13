// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_net.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/timer/timer.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/system/simple_watcher.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/cookies/site_for_cookies.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/redirect_info.h"
#include "services/network/public/cpp/http_request_headers_update_params.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/resource_request_body.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/early_hints.mojom.h"
#include "services/network/public/mojom/url_loader.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_net_observer.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMaximumUpload = 32 * 1024 * 1024;
constexpr size_t kMaximumChunk = 64 * 1024;

base::DictValue DescribeHeaders(const network::mojom::URLResponseHead& head) {
  base::DictValue result;
  base::DictValue headers;
  base::ListValue raw;
  if (head.headers) {
    result.Set("statusCode", head.headers->response_code());
    result.Set("statusMessage", head.headers->GetStatusText());
    const auto version = head.headers->GetHttpVersion();
    result.Set("httpVersion", base::NumberToString(version.major_value()) +
                                  "." +
                                  base::NumberToString(version.minor_value()));
    size_t iterator = 0;
    std::string name, value;
    while (head.headers->EnumerateHeaderLines(&iterator, &name, &value)) {
      raw.Append(name);
      raw.Append(value);
      const auto lower = base::ToLowerASCII(name);
      auto* values = headers.FindList(lower);
      if (!values) {
        values = headers.Set(lower, base::ListValue())->GetIfList();
      }
      values->Append(value);
    }
  }
  result.Set("headers", std::move(headers));
  result.Set("rawHeaders", std::move(raw));
  return result;
}
}  // namespace

class ElectronNetEngine::Request final
    : public network::mojom::URLLoaderClient,
      public mojom::ElectronNetworkHeadersObserver {
 public:
  Request(ElectronNetEngine* owner, uint64_t id)
      : owner_(owner),
        id_(id),
        observer_(base::BindRepeating(&ElectronNetEngine::Emit,
                                      base::Unretained(owner),
                                      id)),
        reader_(FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::MANUAL) {}
  bool Configure(const base::DictValue& options, std::string* error) {
    const auto fail = [&](const std::string& message) {
      *error = message;
      return false;
    };
    const auto* url = options.FindString("url");
    request_.url = GURL(url ? *url : "");
    if (!request_.url.is_valid() || request_.url.has_username() ||
        request_.url.has_password()) {
      return fail(
          "ERR_INVALID_URL: a valid URL without credentials is required");
    }
    if (!request_.url.SchemeIsHTTPOrHTTPS()) {
      return fail("ERR_NOT_SUPPORTED: electron.net currently supports HTTP(S)");
    }
    if (const auto* partition = options.Find("partition");
        partition && !partition->is_string()) {
      return fail("ERR_INVALID_ARG_TYPE: partition must be a string");
    }
    if (const auto* partition = options.FindString("partition")) {
      partition_ = *partition;
    }
    if (partition_.size() > 1024 ||
        partition_.find('\0') != std::string::npos) {
      return fail("ERR_INVALID_ARG_VALUE: invalid Session partition");
    }
    if (const auto* method = options.Find("method");
        method && !method->is_string()) {
      return fail("ERR_INVALID_ARG_TYPE: method must be a string");
    }
    request_.method =
        options.FindString("method") ? *options.FindString("method") : "GET";
    if (request_.method.empty() ||
        !net::HttpUtil::IsValidHeaderName(request_.method)) {
      return fail("ERR_INVALID_ARG_VALUE: invalid HTTP method");
    }
    if (const auto* credentials = options.Find("credentials");
        credentials && !credentials->is_string()) {
      return fail("ERR_INVALID_ARG_TYPE: credentials must be a string");
    }
    if (const auto* cookies = options.Find("useSessionCookies");
        cookies && !cookies->is_bool()) {
      return fail("ERR_INVALID_ARG_TYPE: useSessionCookies must be boolean");
    }
    const auto* credentials = options.FindString("credentials");
    if (credentials && *credentials != "omit" && *credentials != "include") {
      return fail("ERR_NOT_SUPPORTED: credentials must be include or omit");
    }
    request_.mode = network::mojom::RequestMode::kNoCors;
    // Electron's default sends cached Session authentication but no cookies.
    // Explicit include/omit controls both; useSessionCookies applies only when
    // credentials is omitted. Blocking cookies is separate from HTTP auth.
    request_.credentials_mode = credentials && *credentials == "omit"
                                    ? network::mojom::CredentialsMode::kOmit
                                    : network::mojom::CredentialsMode::kInclude;
    const bool cookies =
        credentials ? *credentials == "include"
                    : options.FindBool("useSessionCookies").value_or(false);
    if (!cookies) {
      request_options_ |= network::mojom::kURLLoadOptionBlockAllCookies;
    }
    request_.site_for_cookies = net::SiteForCookies::FromUrl(request_.url);
    if (const auto* redirect = options.Find("redirect");
        redirect && !redirect->is_string()) {
      return fail("ERR_INVALID_ARG_TYPE: redirect must be a string");
    }
    if (const auto* redirect = options.FindString("redirect");
        redirect && *redirect != "follow" && *redirect != "manual" &&
        *redirect != "error") {
      return fail("ERR_INVALID_ARG_VALUE: invalid redirect policy");
    }
    if (const auto* cache = options.Find("cache")) {
      if (!cache->is_string()) {
        return fail("ERR_INVALID_ARG_TYPE: cache must be a string");
      }
      const auto& mode = cache->GetString();
      if (mode == "no-store") {
        request_.load_flags |= net::LOAD_DISABLE_CACHE;
      } else if (mode == "reload") {
        request_.load_flags |= net::LOAD_BYPASS_CACHE;
      } else if (mode == "no-cache") {
        request_.load_flags |= net::LOAD_VALIDATE_CACHE;
      } else if (mode == "force-cache") {
        request_.load_flags |= net::LOAD_SKIP_CACHE_VALIDATION;
      } else if (mode == "only-if-cached") {
        request_.load_flags |=
            net::LOAD_ONLY_FROM_CACHE | net::LOAD_SKIP_CACHE_VALIDATION;
      } else if (mode != "default") {
        return fail("ERR_INVALID_ARG_VALUE: invalid cache mode");
      }
    }
    if (const auto* bypass = options.Find("bypassCustomProtocolHandlers");
        bypass && !bypass->is_bool()) {
      return fail(
          "ERR_INVALID_ARG_TYPE: bypassCustomProtocolHandlers must be boolean");
    }
    if (const auto* timeout = options.Find("timeoutMs")) {
      if (!timeout->is_int() || timeout->GetInt() < 0) {
        return fail(
            "ERR_INVALID_ARG_VALUE: timeoutMs must be a nonnegative integer");
      }
      timeout_ms_ = timeout->GetInt();
    }
    std::string body;
    if (const auto* encoded = options.Find("bodyBase64")) {
      if (!encoded->is_string()) {
        return fail("ERR_INVALID_ARG_TYPE: bodyBase64 must be a string");
      }
      if (encoded->GetString().size() > ((kMaximumUpload + 2) / 3) * 4) {
        return fail("ERR_BUFFER_TOO_LARGE: electron.net upload exceeds 32 MiB");
      }
      if (!base::Base64Decode(encoded->GetString(), &body)) {
        return fail("ERR_INVALID_ARG_VALUE: invalid base64 request body");
      }
      if (body.size() > kMaximumUpload) {
        return fail("ERR_BUFFER_TOO_LARGE: electron.net upload exceeds 32 MiB");
      }
      request_.request_body = network::ResourceRequestBody::CreateFromBytes(
          std::vector<uint8_t>(body.begin(), body.end()));
    }
    if (const auto* header_values = options.Find("headers")) {
      if (!header_values->is_dict()) {
        return fail("ERR_INVALID_ARG_TYPE: headers must be an object");
      }
      for (const auto [name, value] : header_values->GetDict()) {
        if (!net::HttpUtil::IsValidHeaderName(name)) {
          return fail("ERR_INVALID_ARG_VALUE: invalid HTTP header name");
        }
        const std::string lower = base::ToLowerASCII(name);
        if (lower == "host" || lower == "connection" ||
            lower == "proxy-connection" || lower == "transfer-encoding") {
          return fail("ERR_NOT_SUPPORTED: transport-managed HTTP header " +
                      name);
        }
        std::string combined;
        auto append = [&](const base::Value& item) {
          if (!item.is_string() ||
              !net::HttpUtil::IsValidHeaderValue(item.GetString())) {
            return false;
          }
          if (!combined.empty()) {
            combined += lower == "cookie" ? "; " : ", ";
          }
          combined += item.GetString();
          return true;
        };
        if (value.is_list()) {
          for (const auto& item : value.GetList()) {
            if (!append(item)) {
              return fail("ERR_INVALID_ARG_VALUE: invalid HTTP header value");
            }
          }
        } else if (!append(value)) {
          return fail("ERR_INVALID_ARG_VALUE: invalid HTTP header value");
        }
        if (lower == "content-length") {
          uint64_t size = 0;
          if (!base::StringToUint64(combined, &size) || size != body.size()) {
            return fail(
                "ERR_INVALID_ARG_VALUE: Content-Length does not match body");
          }
        }
        request_.headers.SetHeader(name, combined);
      }
    }
    return true;
  }
  void Begin() {
    if (timeout_ms_) {
      timer_.Start(FROM_HERE, base::Milliseconds(timeout_ms_),
                   base::BindOnce(&Request::Fail, weak_factory_.GetWeakPtr(),
                                  net::ERR_TIMED_OUT));
    }
    owner_->provider_.Run(
        partition_, headers_receiver_.BindNewPipeAndPassRemote(),
        mojo::WrapCallbackWithDefaultInvokeIfNotRun(
            base::BindOnce(&Request::OnFactory, weak_factory_.GetWeakPtr()),
            mojo::PendingRemote<network::mojom::URLLoaderFactory>(),
            std::string("ERR_ABORTED: Electron Session factory disconnected")));
  }
  bool Control(const std::string& command, std::string* error) {
    if (command == "resume") {
      credit_ = true;
      Read();
      return true;
    }
    if (command == "follow-redirect") {
      if (!waiting_redirect_) {
        *error = "ERR_INVALID_STATE: no redirect is pending";
        return false;
      }
      waiting_redirect_ = false;
      loader_->FollowRedirect(network::HttpRequestHeadersUpdateParams(),
                              std::nullopt);
      return true;
    }
    *error = "ERR_INVALID_ARG_VALUE: unknown electron.net command";
    return false;
  }
  bool Auth(uint64_t challenge,
            const std::optional<std::string>& username,
            const std::optional<std::string>& password) {
    return observer_.Reply(challenge, username, password);
  }
  void OnResponseHeaders(const std::string& raw_headers,
                         OnResponseHeadersCallback callback) override {
    final_headers_ =
        base::MakeRefCounted<net::HttpResponseHeaders>(raw_headers);
    std::move(callback).Run();
  }
  void OnReceiveEarlyHints(network::mojom::EarlyHintsPtr) override {}
  void OnReceiveResponse(network::mojom::URLResponseHeadPtr head,
                         mojo::ScopedDataPipeConsumerHandle body,
                         std::optional<mojo_base::BigBuffer>) override {
    if (response_received_) {
      Fail(net::ERR_UNEXPECTED);
      return;
    }
    response_received_ = true;
    if (final_headers_ && head->headers &&
        final_headers_->response_code() == head->headers->response_code()) {
      head->headers = std::move(final_headers_);
    }
    auto event = DescribeHeaders(*head);
    event.Set("type", "response");
    event.Set("url", request_.url.spec());
    body_ = std::move(body);
    body_eof_ = !body_;
    if (body_) {
      reader_.Watch(
          body_.get(), MOJO_HANDLE_SIGNAL_READABLE,
          base::BindRepeating(&Request::Readable, weak_factory_.GetWeakPtr()));
    }
    owner_->Emit(id_, std::move(event));
    Read();
  }
  void OnReceiveRedirect(const net::RedirectInfo& redirect,
                         network::mojom::URLResponseHeadPtr head) override {
    if (!redirect.new_url.SchemeIsHTTPOrHTTPS()) {
      Fail(net::ERR_NOT_IMPLEMENTED);
      return;
    }
    if (++redirects_ > 20) {
      Fail(net::ERR_TOO_MANY_REDIRECTS);
      return;
    }
    waiting_redirect_ = true;
    if (final_headers_ && head->headers &&
        final_headers_->response_code() == head->headers->response_code()) {
      head->headers = std::move(final_headers_);
    }
    request_.url = redirect.new_url;
    request_.method = redirect.new_method;
    auto event = DescribeHeaders(*head);
    event.Set("type", "redirect");
    event.Set("url", redirect.new_url.spec());
    event.Set("method", redirect.new_method);
    owner_->Emit(id_, std::move(event));
  }
  void OnUploadProgress(int64_t,
                        int64_t,
                        OnUploadProgressCallback callback) override {
    std::move(callback).Run();
  }
  void OnTransferSizeUpdated(int32_t) override {}
  void OnComplete(const network::URLLoaderCompletionStatus& status) override {
    if (status.error_code != net::OK) {
      Fail(status.error_code);
      return;
    }
    if (!response_received_) {
      Fail(net::ERR_INVALID_RESPONSE);
      return;
    }
    completed_ = true;
    client_.reset();
    FinishIfDrained();
  }

 private:
  void OnFactory(mojo::PendingRemote<network::mojom::URLLoaderFactory> factory,
                 std::string error) {
    if (!factory) {
      auto event =
          base::DictValue()
              .Set("type", "error")
              .Set("code", "ERR_FAILED")
              .Set("message", error.empty()
                                  ? "Electron Session factory is unavailable"
                                  : error);
      owner_->Emit(id_, std::move(event));
      owner_->Remove(id_);
      return;
    }
    static const auto annotation = net::DefineNetworkTrafficAnnotation(
        "xenon_electron_session_network_request", R"(
          semantics {
            sender: "Hosted Electron application network API"
            description: "Sends an application HTTP(S) request through its Electron Session."
            trigger: "The application calls electron.net or session.fetch."
            data: "Application-supplied URL, headers and optional request body."
            destination: OTHER
            destination_other: "The server selected by the application."
          }
          policy {
            cookies_allowed: YES
            cookies_store: "The application's own Session when explicitly enabled."
            setting: "Available to explicitly hosted Electron applications."
            policy_exception_justification: "An application runtime capability."
          })");
    mojo::Remote<network::mojom::URLLoaderFactory> remote(std::move(factory));
    request_.trusted_params.emplace();
    request_.trusted_params->url_loader_network_observer = observer_.Bind();
    remote->CreateLoaderAndStart(
        loader_.BindNewPipeAndPassReceiver(), 1, request_options_, request_,
        client_.BindNewPipeAndPassRemote(),
        net::MutableNetworkTrafficAnnotationTag(annotation));
    // The client pipe orders disconnect after its completion messages. Never
    // use the separate URLLoader pipe closing as proof of request failure.
    client_.set_disconnect_handler(base::BindOnce(
        &Request::Fail, weak_factory_.GetWeakPtr(), net::ERR_ABORTED));
  }
  void Readable(MojoResult) { Read(); }
  void Read() {
    if (!credit_ || !body_ || body_eof_) {
      FinishIfDrained();
      return;
    }
    base::span<const uint8_t> bytes;
    const MojoResult result =
        body_->BeginReadData(MOJO_READ_DATA_FLAG_NONE, bytes);
    if (result == MOJO_RESULT_SHOULD_WAIT) {
      reader_.ArmOrNotify();
      return;
    }
    if (result == MOJO_RESULT_FAILED_PRECONDITION) {
      body_eof_ = true;
      reader_.Cancel();
      FinishIfDrained();
      return;
    }
    if (result != MOJO_RESULT_OK) {
      Fail(net::ERR_FAILED);
      return;
    }
    const size_t length = std::min(bytes.size(), kMaximumChunk);
    std::string encoded = base::Base64Encode(bytes.first(length));
    body_->EndReadData(length);
    credit_ = false;
    owner_->Emit(id_, base::DictValue()
                          .Set("type", "data")
                          .Set("bodyBase64", std::move(encoded)));
  }
  void FinishIfDrained() {
    if (completed_ && body_eof_) {
      owner_->Emit(id_, base::DictValue().Set("type", "end"));
      owner_->Remove(id_);
    }
  }
  void Fail(int error) {
    owner_->Emit(id_, base::DictValue()
                          .Set("type", "error")
                          .Set("code", net::ErrorToShortString(error))
                          .Set("message", net::ErrorToString(error)));
    owner_->Remove(id_);
  }
  raw_ptr<ElectronNetEngine> owner_;
  uint64_t id_;
  ElectronNetObserver observer_;
  mojo::Receiver<mojom::ElectronNetworkHeadersObserver> headers_receiver_{this};
  scoped_refptr<net::HttpResponseHeaders> final_headers_;
  std::string partition_;
  network::ResourceRequest request_;
  int timeout_ms_ = 0;
  int redirects_ = 0;
  uint32_t request_options_ = 0;
  bool credit_ = false;
  bool waiting_redirect_ = false;
  bool response_received_ = false;
  bool body_eof_ = false;
  bool completed_ = false;
  mojo::Remote<network::mojom::URLLoader> loader_;
  mojo::Receiver<network::mojom::URLLoaderClient> client_{this};
  mojo::ScopedDataPipeConsumerHandle body_;
  mojo::SimpleWatcher reader_;
  base::OneShotTimer timer_;
  base::WeakPtrFactory<Request> weak_factory_{this};
};

ElectronNetEngine::ElectronNetEngine(FactoryProvider provider,
                                     EventCallback event)
    : provider_(std::move(provider)), event_(std::move(event)) {}
ElectronNetEngine::~ElectronNetEngine() {
  Shutdown();
}
uint64_t ElectronNetEngine::Start(const base::DictValue& options,
                                  std::string* error) {
  if (!provider_ || !event_) {
    *error = "ERR_NOT_SUPPORTED: Electron network host is unavailable";
    return 0;
  }
  if (requests_.size() >= 128) {
    *error = "ERR_RESOURCE_BUSY: Electron network request limit reached";
    return 0;
  }
  const uint64_t id = ++next_id_;
  auto request = std::make_unique<Request>(this, id);
  if (!request->Configure(options, error)) {
    return 0;
  }
  requests_.emplace(id, std::move(request));
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&ElectronNetEngine::Begin,
                                weak_factory_.GetWeakPtr(), id));
  return id;
}
void ElectronNetEngine::Begin(uint64_t id) {
  auto found = requests_.find(id);
  if (found != requests_.end()) {
    found->second->Begin();
  }
}
bool ElectronNetEngine::Control(uint64_t id,
                                const std::string& command,
                                std::string* error) {
  auto found = requests_.find(id);
  if (found == requests_.end()) {
    return false;
  }
  if (command == "abort") {
    requests_.erase(found);
    return true;
  }
  return found->second->Control(command, error);
}
void ElectronNetEngine::Emit(uint64_t id, base::DictValue event) {
  event.Set("id", static_cast<double>(id));
  base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          [](base::WeakPtr<ElectronNetEngine> self, base::Value event) {
            if (self && self->event_) {
              self->event_.Run(std::move(event));
            }
          },
          weak_factory_.GetWeakPtr(), base::Value(std::move(event))));
}
bool ElectronNetEngine::Auth(uint64_t id,
                             uint64_t challenge,
                             const std::optional<std::string>& username,
                             const std::optional<std::string>& password) {
  auto found = requests_.find(id);
  return found != requests_.end() &&
         found->second->Auth(challenge, username, password);
}
void ElectronNetEngine::Remove(uint64_t id) {
  requests_.erase(id);
}
void ElectronNetEngine::Shutdown() {
  weak_factory_.InvalidateWeakPtrs();
  requests_.clear();
  provider_.Reset();
  event_.Reset();
}
}  // namespace xenon::ipc
