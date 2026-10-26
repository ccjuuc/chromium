// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_service.h"

#include <algorithm>
#include <array>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "content/public/browser/network_service_instance.h"
#include "net/base/load_flags.h"
#include "net/base/net_errors.h"
#include "net/http/http_auth_preferences.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/cert_verifier/public/mojom/cert_verifier_service_factory.mojom.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/cert_verifier_service_updater.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/network_service.mojom.h"
#include "services/network/public/mojom/url_loader_factory.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <wincred.h>
#endif

namespace xenon::certificates {
namespace {

constexpr std::array kRetryDelays = {base::Minutes(1), base::Minutes(5),
                                     base::Minutes(15), base::Hours(1)};

std::optional<std::string> ReadCredential(const Config& config) {
  if (!config.auth_bearer) {
    return std::string();
  }
#if BUILDFLAG(IS_WIN)
  PCREDENTIALW credential = nullptr;
  if (!CredReadW(base::UTF8ToWide(config.credential_ref).c_str(),
                 CRED_TYPE_GENERIC, 0, &credential)) {
    return std::nullopt;
  }
  std::string token;
  if (credential->CredentialBlobSize > 0 &&
      credential->CredentialBlobSize <= 4096) {
    token.assign(reinterpret_cast<const char*>(credential->CredentialBlob),
                 credential->CredentialBlobSize);
  }
  CredFree(credential);
  // Bearer credentials must also be safe to place in an HTTP header. Never
  // log the credential or include it in persistent state.
  if (token.empty() || !base::IsStringUTF8(token) ||
      !std::ranges::all_of(
          token, [](unsigned char c) { return c >= 0x21 && c <= 0x7e; })) {
    return std::nullopt;
  }
  return token;
#else
  return std::nullopt;
#endif
}

bool UsableETag(std::string_view etag) {
  return etag.size() <= 1024 && std::ranges::all_of(etag, [](unsigned char c) {
           return c >= 0x20 && c < 0x7f;
         });
}

}  // namespace

struct ServiceReadResult {
  std::optional<Config> config;
  std::optional<PersistentState> state;
  std::optional<std::string> token;
  std::string request_context;
  std::string error;
};

XenonManagedCertificateService* XenonManagedCertificateService::Get() {
  static base::NoDestructor<XenonManagedCertificateService> instance;
  return instance.get();
}

XenonManagedCertificateService::XenonManagedCertificateService()
    : io_runner_(base::ThreadPool::CreateSequencedTaskRunner(
          {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
           base::TaskShutdownBehavior::BLOCK_SHUTDOWN})) {}

XenonManagedCertificateService::~XenonManagedCertificateService() {
  Shutdown();
}

void XenonManagedCertificateService::SetNetworkContextParamsFactory(
    base::RepeatingCallback<network::mojom::NetworkContextParamsPtr()>
        factory) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  CHECK(!started_);
  network_params_factory_ = std::move(factory);
}

void XenonManagedCertificateService::Start(
    const base::FilePath& config_path,
    const base::FilePath& user_data_dir) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (started_) {
    Shutdown();
  }
  started_ = true;
  ready_ = false;
  ++epoch_;
  user_data_dir_ = user_data_dir;
  net::NetworkChangeNotifier::AddNetworkChangeObserver(this);
  observers_.Notify();
  // Configuration, credentials, bootstrap roots, and cache are read once per
  // browser start. Subsequent refreshes use this in-memory configuration.
  io_busy_ = true;
  io_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&XenonManagedCertificateService::ReadInputs, config_path,
                     user_data_dir_),
      base::BindOnce(&XenonManagedCertificateService::OnRead,
                     weak_factory_.GetWeakPtr(), epoch_));
}

void XenonManagedCertificateService::Shutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!started_) {
    return;
  }
  started_ = false;
  ++epoch_;
  weak_factory_.InvalidateWeakPtrs();
  net::NetworkChangeNotifier::RemoveNetworkChangeObserver(this);
  refresh_timer_.Stop();
  CancelNetwork();
  config_.reset();
  state_ = PersistentState();
  published_.reset();
  bearer_token_.clear();
  request_context_.clear();
  configuration_error_.clear();
  refresh_error_.clear();
  last_attempt_ = base::Time();
  last_success_ = base::Time();
  ready_ = true;
  io_busy_ = false;
  fetch_pending_ = false;
  state_usable_ = false;
  failures_ = 0;
  observers_.Notify();
}

bool XenonManagedCertificateService::ready() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return ready_;
}

ServiceStatus XenonManagedCertificateService::GetStatus() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return {.started = started_,
          .enabled = config_ && config_->enabled,
          .ready = ready_,
          .downloading = !!loader_,
          .source_id = config_ && !config_->source_id.empty()
                           ? config_->source_id
                           : state_.source_id,
          .configuration_error = configuration_error_,
          .refresh_error = refresh_error_,
          .last_attempt = last_attempt_,
          .last_success = last_success_};
}

const Snapshot* XenonManagedCertificateService::snapshot() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return published_ ? &*published_ : nullptr;
}

base::CallbackListSubscription XenonManagedCertificateService::AddObserver(
    base::RepeatingClosure observer) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return observers_.Add(std::move(observer));
}

std::unique_ptr<ServiceReadResult> XenonManagedCertificateService::ReadInputs(
    const base::FilePath& config_path,
    const base::FilePath& user_data_dir) {
  auto result = std::make_unique<ServiceReadResult>();
  result->config = LoadConfig(config_path, &result->error);
  if (!result->config || !result->config->enabled) {
    return result;
  }
  result->token = ReadCredential(*result->config);
  if (!result->token) {
    result->error = "credential_unavailable_or_unsupported";
    return result;
  }
  result->request_context =
      Sha256Hex(result->config->revision + ":" + Sha256Hex(*result->token));
  result->state = LoadState(StatePath(user_data_dir, result->config->source_id),
                            result->config->source_id, &result->error);
  return result;
}

void XenonManagedCertificateService::OnRead(
    uint64_t epoch,
    std::unique_ptr<ServiceReadResult> result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!started_ || epoch != epoch_) {
    return;
  }
  config_ = std::move(result->config);
  bearer_token_ = result->token.value_or(std::string());
  request_context_ = std::move(result->request_context);
  state_usable_ = result->state.has_value();
  if (result->state) {
    state_ = std::move(*result->state);
  }
  configuration_error_ = std::move(result->error);
  if (!configuration_error_.empty()) {
    // Parsers return reason codes, never configuration bodies or credentials.
    LOG(WARNING) << "Managed certificate configuration unavailable: "
                 << configuration_error_;
  }
  ready_ = true;
  Publish();
  if (!published_) {
    // Release verification waiters even if there is no usable cached snapshot.
    observers_.Notify();
  }
  fetch_pending_ = true;
  FinishIO();
}

bool XenonManagedCertificateService::EnsureNetworkContext() {
  if (factory_ && factory_.is_connected()) {
    return true;
  }
  factory_.reset();
  network_context_.reset();
  auto verifier = cert_verifier::mojom::CertVerifierCreationParams::New();
  verifier->initial_additional_certificates =
      cert_verifier::mojom::AdditionalCertificates::New();
  verifier->initial_additional_certificates
      ->trust_anchors_with_enforced_constraints =
      config_->bootstrap_certificates;
  auto params = network_params_factory_
                    ? network_params_factory_.Run()
                    : network::mojom::NetworkContextParams::New();
  CHECK(params);
  params->cert_verifier_params =
      content::GetCertVerifierParams(std::move(verifier));
  params->http_cache_enabled = false;
  params->enable_referrers = false;
  params->enable_brotli = false;
  params->enable_zstd = false;
  params->enforce_chrome_ct_policy = true;
  params->http_auth_static_network_context_params =
      network::mojom::HttpAuthStaticNetworkContextParams::New();
  params->http_auth_static_network_context_params->allow_default_credentials =
      net::HttpAuthPreferences::DISALLOW_DEFAULT_CREDENTIALS;
  // The factory may supply system proxy/SSL settings. Trust is independently
  // constructed above and never subscribes to the managed snapshot.
  content::CreateNetworkContextInNetworkService(
      network_context_.BindNewPipeAndPassReceiver(), std::move(params));
  network_context_.set_disconnect_handler(
      base::BindOnce(&XenonManagedCertificateService::OnNetworkDisconnected,
                     weak_factory_.GetWeakPtr(), epoch_));
  auto factory_params = network::mojom::URLLoaderFactoryParams::New();
  factory_params->process_id = network::OriginatingProcessId::browser();
  factory_params->is_trusted = true;
  factory_params->is_orb_enabled = false;
  network_context_->CreateURLLoaderFactory(
      factory_.BindNewPipeAndPassReceiver(), std::move(factory_params));
  factory_.set_disconnect_handler(
      base::BindOnce(&XenonManagedCertificateService::OnNetworkDisconnected,
                     weak_factory_.GetWeakPtr(), epoch_));
  return true;
}

void XenonManagedCertificateService::Fetch() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!started_ || !config_ || !config_->enabled || !state_usable_) {
    return;
  }
  if (io_busy_) {
    fetch_pending_ = true;
    return;
  }
  if (loader_) {
    return;
  }
  if (!test_factory_) {
    EnsureNetworkContext();
  }
  auto request = std::make_unique<network::ResourceRequest>();
  request->url = config_->endpoint;
  request->method = "GET";
  request->credentials_mode = network::mojom::CredentialsMode::kOmit;
  request->do_not_prompt_for_login = true;
  request->redirect_mode = network::mojom::RedirectMode::kError;
  request->load_flags = net::LOAD_BYPASS_CACHE | net::LOAD_DISABLE_CACHE |
                        net::LOAD_DO_NOT_USE_EMBEDDED_IDENTITY;
  request->headers.SetHeader("Accept", "application/json");
  request->headers.SetHeader("Accept-Encoding", "identity");
  if (config_->auth_bearer) {
    request->headers.SetHeader("Authorization", "Bearer " + bearer_token_);
  }
  request_sent_conditional_ = false;
  if (state_.snapshot && state_.request_context == request_context_ &&
      !state_.etag.empty() && UsableETag(state_.etag)) {
    request->headers.SetHeader("If-None-Match", state_.etag);
    request_sent_conditional_ = true;
  }
  constexpr auto annotation =
      net::DefineNetworkTrafficAnnotation("xenon_managed_certificates", R"(
        semantics {
          sender: "Xenon managed certificates"
          description: "Fetches an administrator-configured browser-local CA trust snapshot."
          trigger: "Enabled local configuration, periodic refresh, or network recovery."
          data: "Optional configured bearer credential and cached snapshot ETag."
          destination: OTHER
          destination_other: "HTTPS endpoint explicitly set by the administrator."
        }
        policy {
          cookies_allowed: NO
          setting: "Disabled unless a local managed certificate configuration enables it."
          policy_exception_justification: "Custom browser administrator configuration."
        })");
  loader_ = network::SimpleURLLoader::Create(std::move(request), annotation);
  last_attempt_ = base::Time::Now();
  loader_->SetAllowHttpErrorResults(true);
  loader_->SetTimeoutDuration(config_->request_timeout);
  loader_->DownloadToString(
      test_factory_ ? test_factory_.get() : factory_.get(),
      base::BindOnce(&XenonManagedCertificateService::OnDownloaded,
                     weak_factory_.GetWeakPtr(), epoch_),
      kMaxSnapshotBytes);
}

void XenonManagedCertificateService::OnDownloaded(
    uint64_t epoch,
    std::optional<std::string> body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!started_ || epoch != epoch_ || !loader_) {
    return;
  }
  DCHECK(!io_busy_);
  const auto* info = loader_->ResponseInfo();
  const int code = info && info->headers ? info->headers->response_code() : 0;
  std::string etag;
  bool identity = true;
  bool json_type = false;
  std::optional<base::TimeDelta> retry_after;
  if (info && info->headers) {
    std::string mime_type;
    std::string charset;
    info->headers->GetMimeTypeAndCharset(&mime_type, &charset);
    json_type =
        base::EqualsCaseInsensitiveASCII(mime_type, "application/json") &&
        (charset.empty() || base::EqualsCaseInsensitiveASCII(charset, "utf-8"));
    etag = info->headers->GetNormalizedHeader("etag").value_or(std::string());
    auto encoding = info->headers->GetNormalizedHeader("content-encoding");
    identity =
        !encoding || base::EqualsCaseInsensitiveASCII(*encoding, "identity");
    if (code == 429 || code == 503) {
      auto header = info->headers->GetNormalizedHeader("retry-after");
      base::TimeDelta delay;
      if (header && net::HttpUtil::ParseRetryAfterHeader(
                        *header, base::Time::Now(), &delay)) {
        retry_after = std::clamp(delay, base::Seconds(1), base::Hours(1));
      }
    }
  }
  const int network_error = loader_->NetError();
  const bool transport_ok = network_error == net::OK;
  const bool sent_conditional = std::exchange(request_sent_conditional_, false);
  loader_.reset();
  std::string response_error;
  if (!transport_ok) {
    response_error = "network_error:" + base::NumberToString(network_error);
  } else if (code != 200 && code != 304) {
    response_error = "http_status:" + base::NumberToString(code);
  } else if (!identity) {
    response_error = "unsupported_content_encoding";
  } else if (!UsableETag(etag)) {
    response_error = "invalid_etag";
  } else if (code == 200 && !json_type) {
    response_error = "invalid_content_type";
  } else if (code == 200 && !body) {
    response_error = "missing_body";
  } else if (code == 304 &&
             (!sent_conditional || !state_.snapshot || state_.etag.empty() ||
              state_.request_context != request_context_)) {
    response_error = "unexpected_not_modified";
  }
  if (!response_error.empty()) {
    refresh_error_ = std::move(response_error);
    LOG(WARNING) << "Managed certificate response rejected: status=" << code
                 << ", network_error=" << network_error;
    ScheduleRefresh(false, retry_after);
    FinishIO();
    return;
  }
  if (code == 304) {
    // The persisted snapshot has not changed. No cache rewrite is needed.
    refresh_error_.clear();
    last_success_ = base::Time::Now();
    ScheduleRefresh(true);
    FinishIO();
    return;
  }
  io_busy_ = true;
  io_runner_->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(&XenonManagedCertificateService::ValidateAndSave,
                     StatePath(user_data_dir_, config_->source_id), state_,
                     std::move(*body), std::move(etag), request_context_),
      base::BindOnce(&XenonManagedCertificateService::OnSaved,
                     weak_factory_.GetWeakPtr(), epoch_));
}

std::unique_ptr<ServiceWriteResult>
XenonManagedCertificateService::ValidateAndSave(const base::FilePath& path,
                                                PersistentState state,
                                                std::string body,
                                                std::string etag,
                                                std::string request_context) {
  auto result = std::make_unique<ServiceWriteResult>();
  auto snapshot =
      ParseSnapshot(std::move(body), state.source_id, &result->error);
  if (!snapshot || snapshot->generation < state.highest_generation ||
      (snapshot->generation == state.highest_generation &&
       snapshot->body_hash != state.body_hash)) {
    result->error = "invalid_or_rolled_back_snapshot";
    return result;
  }
  state.highest_generation = snapshot->generation;
  state.body_hash = snapshot->body_hash;
  state.snapshot = std::move(snapshot);
  state.etag = std::move(etag);
  state.request_context = std::move(request_context);
  result->success = SaveState(path, state, &result->error);
  result->state = std::move(state);
  return result;
}

void XenonManagedCertificateService::OnSaved(
    uint64_t epoch,
    std::unique_ptr<ServiceWriteResult> result) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!started_ || epoch != epoch_) {
    return;
  }
  if (result->success) {
    refresh_error_.clear();
    last_success_ = base::Time::Now();
    state_ = std::move(result->state);
    Publish();
    LOG(INFO) << "Managed certificate snapshot accepted: generation="
              << state_.highest_generation
              << ", certificates=" << state_.snapshot->certificates.size();
  } else {
    refresh_error_ = result->error;
    LOG(WARNING) << "Managed certificate refresh rejected: " << result->error;
  }
  ScheduleRefresh(result->success);
  FinishIO();
}

void XenonManagedCertificateService::ScheduleRefresh(
    bool success,
    std::optional<base::TimeDelta> retry_after) {
  if (!started_ || !config_ || !config_->enabled) {
    return;
  }
  base::TimeDelta delay;
  if (success) {
    failures_ = 0;
    delay = config_->refresh_interval * (0.9 + base::RandDouble() * 0.2);
  } else {
    delay =
        kRetryDelays[std::min<size_t>(failures_, std::size(kRetryDelays) - 1)];
    failures_ = std::min<unsigned>(failures_ + 1, std::size(kRetryDelays));
  }
  if (retry_after) {
    delay = *retry_after;
  }
  refresh_timer_.Start(FROM_HERE, delay, this,
                       &XenonManagedCertificateService::Fetch);
}

void XenonManagedCertificateService::Publish() {
  std::optional<Snapshot> next;
  if (config_ && config_->enabled && state_usable_ && state_.snapshot &&
      state_.request_context == request_context_) {
    next = state_.snapshot;
  }
  const bool changed = next.has_value() != published_.has_value() ||
                       (next && published_ &&
                        (next->source_id != published_->source_id ||
                         next->body_hash != published_->body_hash));
  published_ = std::move(next);
  if (changed) {
    observers_.Notify();
  }
}

void XenonManagedCertificateService::CancelNetwork() {
  request_sent_conditional_ = false;
  loader_.reset();
  factory_.reset();
  network_context_.reset();
}

void XenonManagedCertificateService::OnNetworkDisconnected(uint64_t epoch) {
  if (!started_ || epoch != epoch_) {
    return;
  }
  CancelNetwork();
  refresh_error_ = "network_service_disconnected";
  ScheduleRefresh(false);
  if (!io_busy_) {
    FinishIO();
  }
}

void XenonManagedCertificateService::OnNetworkChanged(
    net::NetworkChangeNotifier::ConnectionType type) {
  if (started_ && type != net::NetworkChangeNotifier::CONNECTION_NONE) {
    refresh_timer_.Stop();
    Fetch();
  }
}

void XenonManagedCertificateService::FinishIO() {
  io_busy_ = false;
  if (std::exchange(fetch_pending_, false)) {
    Fetch();
  }
}

}  // namespace xenon::certificates
