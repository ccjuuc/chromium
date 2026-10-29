// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_key_manager.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/check.h"
#include "base/check_is_test.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "chrome/browser/gm/gm_client_cert_chain_win.h"
#include "chrome/browser/gm/gm_policy.h"
#include "chrome/grit/generated_resources.h"
#include "content/public/browser/service_process_host.h"
#include "content/public/browser/web_contents.h"
#include "crypto/sha2.h"
#include "net/base/net_errors.h"
#include "net/cert/x509_certificate.h"
#include "net/gm/gm_client_cert_constraints.h"
#include "services/gm_key/public/cpp/secure_pin.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/views/widget/widget.h"

namespace gm_browser {
namespace {
bool Ok(const gm_key::mojom::KeyResultPtr& result) {
  return result && result->error == gm_key::mojom::KeyError::kOk;
}

std::string Fingerprint(const std::vector<uint8_t>& certificate) {
  return base::ToLowerASCII(base::HexEncode(crypto::SHA256Hash(certificate)));
}

net::gm::TlsProtocol ConvertProtocol(network::mojom::GmTlsProtocol protocol) {
  switch (protocol) {
    case network::mojom::GmTlsProtocol::kTlcp:
      return net::gm::TlsProtocol::kTlcp;
    case network::mojom::GmTlsProtocol::kRfc8998:
      return net::gm::TlsProtocol::kRfc8998;
    case network::mojom::GmTlsProtocol::kTls13Auto:
      return net::gm::TlsProtocol::kTls13Auto;
  }
  return static_cast<net::gm::TlsProtocol>(-1);
}
}  // namespace

KeyManager::KeyManager(PrefService* local_state) : prefs_(local_state) {
  registrar_.Init(local_state);
  for (const char* pref : {kSiteRulesPref, kDriverPref}) {
    registrar_.Add(pref, base::BindRepeating(&KeyManager::OnPolicyChanged,
                                             base::Unretained(this)));
  }
  service_factory_ = base::BindRepeating([] {
    return content::ServiceProcessHost::Launch<gm_key::mojom::KeyService>(
               content::ServiceProcessHost::Options()
                   .WithDisplayName(l10n_util::GetStringUTF16(IDS_GM_PIN_TITLE))
                   .Pass())
        .Unbind();
  });
  brokers_.set_disconnect_handler(base::BindRepeating(
      &KeyManager::OnBrokerDisconnected, base::Unretained(this)));
}

KeyManager::~KeyManager() {
  Reset(net::ERR_ABORTED);
}

void KeyManager::SetTestHooks(ServiceFactory factory, Prompt prompt) {
  CHECK_IS_TEST();
  CHECK(!service_.is_bound());
  test_hooks_ = true;
  service_factory_ = std::move(factory);
  prompt_ = std::move(prompt);
}

content::WebContents* KeyManager::active_web_contents_for_testing() const {
  CHECK_IS_TEST();
  return requests_.empty() ? nullptr : requests_.front().web_contents.get();
}

mojo::PendingRemote<network::mojom::GmIdentityBroker> KeyManager::Bind() {
  mojo::PendingRemote<network::mojom::GmIdentityBroker> remote;
  brokers_.Add(this, remote.InitWithNewPipeAndPassReceiver(), BrokerContext{});
  return remote;
}

bool KeyManager::Allowed(const Request& request) const {
  if (request.origin.opaque() || request.origin.scheme() != "https" ||
      !net::gm::IsValidClientCertificateRequest(request.certificate_request)) {
    return false;
  }
  auto policy = ReadPolicy(prefs_);
  if (!policy.valid) {
    return false;
  }
  std::string host = request.origin.host();
  if (host.ends_with('.')) {
    host.pop_back();
  }
  for (const auto& site : policy.sites) {
    const auto& rule = *site.rule;
    if (host == rule.host && request.origin.port() == rule.port) {
      const net::gm::ConnectionProfile profile(
          ConvertProtocol(rule.protocol), rule.trust_profile_id,
          rule.trust_generation, rule.identity_policy_id,
          rule.identity_generation, rule.policy_revision);
      return request.policy_id == rule.identity_policy_id &&
             request.generation == rule.identity_generation &&
             request.certificate_sha256 == site.certificate_sha256 &&
             net::gm::IsClientCertificateRequestForProfile(
                 profile, request.certificate_request);
    }
  }
  // Automatic GM mTLS still requires a managed driver and explicit user
  // authorization. A conflicting managed site rule can never be bypassed.
  return request.policy_id.empty() && request.generation == 0 &&
         request.certificate_sha256.empty();
}

bool KeyManager::MatchesRequest(
    const gm_key::mojom::KeyIdentity& identity) const {
  const auto& request = requests_.front();
  return !identity.identity_id.empty() && identity.identity_id.size() <= 1024 &&
         identity.generation &&
         (request.certificate_sha256.empty() ||
          request.certificate_sha256 ==
              Fingerprint(identity.signing_certificate)) &&
         net::gm::MatchesClientCertificateRequest(
             identity.signing_certificate, identity.encryption_certificate,
             identity.intermediates, request.certificate_request);
}

void KeyManager::AcquireIdentity(
    uint64_t request_id,
    const url::Origin& origin,
    const std::string& policy_id,
    uint64_t generation,
    network::mojom::GmClientCertificateRequestPtr constraints,
    AcquireIdentityCallback callback) {
  auto& context = brokers_.current_context();
  if (!request_id || request_id <= context.last_request_id) {
    std::move(callback).Run(net::ERR_INVALID_ARGUMENT, nullptr);
    return;
  }
  context.last_request_id = request_id;
  // A profile-wide broker does not establish that a request may show UI.
  // Production interaction must enter through the verified page observer.
  if (!test_hooks_) {
    std::move(callback).Run(net::ERR_ACCESS_DENIED, nullptr);
    return;
  }
  QueueRequest(brokers_.current_receiver(), request_id, nullptr, origin,
               policy_id, generation, std::move(constraints),
               std::move(callback));
}

base::OnceClosure KeyManager::AcquireIdentityForWebContents(
    content::WebContents* web_contents,
    const url::Origin& origin,
    const std::string& policy_id,
    uint64_t generation,
    network::mojom::GmClientCertificateRequestPtr constraints,
    AcquireIdentityCallback callback) {
  if (!web_contents || next_browser_request_id_ == UINT64_MAX) {
    std::move(callback).Run(net::ERR_ACCESS_DENIED, nullptr);
    return {};
  }
  const uint64_t id = ++next_browser_request_id_;
  auto cancel = base::BindOnce(&KeyManager::CancelRequest,
                               weak_factory_.GetWeakPtr(), std::nullopt, id);
  QueueRequest(std::nullopt, id, web_contents, origin, policy_id, generation,
               std::move(constraints), std::move(callback));
  return cancel;
}

void KeyManager::QueueRequest(
    std::optional<mojo::ReceiverId> broker_id,
    uint64_t request_id,
    content::WebContents* web_contents,
    const url::Origin& origin,
    const std::string& policy_id,
    uint64_t generation,
    network::mojom::GmClientCertificateRequestPtr constraints,
    AcquireIdentityCallback callback) {
  if (!constraints) {
    std::move(callback).Run(net::ERR_INVALID_ARGUMENT, nullptr);
    return;
  }
  net::gm::ClientCertificateRequest certificate_request;
  certificate_request.protocol = ConvertProtocol(constraints->protocol);
  certificate_request.required_roles = constraints->required_roles;
  certificate_request.certificate_authorities =
      std::move(constraints->certificate_authorities);
  certificate_request.signature_algorithms =
      std::move(constraints->signature_algorithms);
  certificate_request.certificate_types =
      std::move(constraints->certificate_types);
  Request request{broker_id,
                  request_id,
                  origin,
                  policy_id,
                  generation,
                  {},
                  std::move(certificate_request),
                  std::move(callback),
                  web_contents ? web_contents->GetWeakPtr() : nullptr};
  auto policy = ReadPolicy(prefs_);
  std::string host = origin.host();
  if (host.ends_with('.')) {
    host.pop_back();
  }
  for (const auto& site : policy.sites) {
    if (site.rule->host == host && site.rule->port == origin.port()) {
      request.certificate_sha256 = site.certificate_sha256;
      break;
    }
  }
  if (!Allowed(request) || requests_.size() >= 32) {
    std::move(request.callback)
        .Run(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED, nullptr);
    return;
  }
  requests_.push_back(std::move(request));
  Process();
}

void KeyManager::Cancel(uint64_t request_id) {
  CancelRequest(brokers_.current_receiver(), request_id);
}

void KeyManager::CancelRequest(std::optional<mojo::ReceiverId> broker,
                               uint64_t request_id) {
  auto found = std::ranges::find_if(requests_, [&](const Request& request) {
    return request.broker_id == broker && request.request_id == request_id;
  });
  if (found == requests_.end()) {
    return;
  }
  if (found == requests_.begin()) {
    StopActiveRequest();
  }
  auto callback = std::move(found->callback);
  requests_.erase(found);
  ScheduleNextRequest();
  std::move(callback).Run(net::ERR_ABORTED, nullptr);
}

void KeyManager::OnBrokerDisconnected() {
  const auto broker = brokers_.current_receiver();
  if (!requests_.empty() && requests_.front().broker_id == broker) {
    StopActiveRequest();
  }
  // Its response pipe is gone, so discard callbacks instead of completing them.
  std::erase_if(requests_, [broker](const Request& request) {
    return request.broker_id == broker;
  });
  ScheduleNextRequest();
}

void KeyManager::Process() {
  if (processing_ || requests_.empty()) {
    return;
  }
  if (!Allowed(requests_.front())) {
    Finish(net::ERR_ACCESS_DENIED);
    return;
  }
  if (!service_.is_bound()) {
    auto config = ReadDriver(prefs_);
    if (!config) {
      Finish(requests_.front().policy_id.empty()
                 ? net::OK
                 : net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
      return;
    }
    service_.Bind(service_factory_.Run());
    if (!service_.is_bound()) {
      Finish(net::ERR_FAILED);
      return;
    }
    service_.set_disconnect_handler(
        base::BindOnce(&KeyManager::Reset, weak_factory_.GetWeakPtr(),
                       net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED));
    configuring_ = true;
    service_->Configure(
        std::move(config), observer_.BindNewPipeAndPassRemote(),
        base::BindOnce(&KeyManager::OnConfigured, weak_factory_.GetWeakPtr()));
    return;
  }
  if (!configured_) {
    if (!configuring_) {
      Reset(net::ERR_FAILED);
    }
    return;
  }
  processing_ = true;
  service_->Enumerate(base::BindOnce(&KeyManager::OnEnumerated,
                                     operation_weak_factory_.GetWeakPtr()));
}

void KeyManager::OnConfigured(gm_key::mojom::KeyResultPtr result) {
  if (!Ok(result)) {
    Reset(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return;
  }
  configured_ = true;
  configuring_ = false;
  Process();
}

std::string KeyManager::RequestKey() const {
  const auto& r = requests_.front();
  return r.origin.Serialize() + "\n" + r.policy_id + "\n" +
         base::NumberToString(r.generation) + "\n" +
         base::NumberToString(static_cast<int>(r.certificate_request.protocol));
}

void KeyManager::OnEnumerated(
    gm_key::mojom::KeyResultPtr result,
    std::vector<gm_key::mojom::KeyIdentityPtr> identities) {
  if (!Ok(result)) {
    Finish(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(
          [](std::vector<gm_key::mojom::KeyIdentityPtr> identities) {
            std::erase_if(identities, [](auto& identity) {
              if (!identity) {
                return true;
              }
              auto chain = FindGmClientCertificateIntermediates(
                  identity->signing_certificate,
                  identity->encryption_certificate, identity->intermediates);
              if (!chain) {
                return true;
              }
              identity->intermediates = std::move(*chain);
              return false;
            });
            return identities;
          },
          std::move(identities)),
      base::BindOnce(&KeyManager::OnCertificatesCompleted,
                     operation_weak_factory_.GetWeakPtr()));
}

void KeyManager::OnCertificatesCompleted(
    std::vector<gm_key::mojom::KeyIdentityPtr> identities) {
  candidates_.clear();
  for (auto& identity : identities) {
    if (identity && MatchesRequest(*identity)) {
      candidates_.push_back(std::move(identity));
    }
  }
  if (candidates_.empty()) {
    Finish(requests_.front().policy_id.empty()
               ? net::OK
               : net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return;
  }
  auto found = selected_.find(RequestKey());
  if (found != selected_.end()) {
    for (const auto& identity : candidates_) {
      if (found->second ==
          std::make_pair(identity->identity_id, identity->generation)) {
        selected_identity_ = identity->Clone();
        auto receiver = pending_signer_.InitWithNewPipeAndPassReceiver();
        service_->GrantAuthorized(
            identity->identity_id, identity->generation, std::move(receiver),
            base::BindOnce(&KeyManager::OnGranted,
                           operation_weak_factory_.GetWeakPtr(), false));
        return;
      }
    }
    selected_.erase(found);
  }
  PromptForPin({});
}

void KeyManager::PromptForPin(const std::u16string& message) {
  if (!Allowed(requests_.front()) ||
      (!test_hooks_ && !requests_.front().web_contents)) {
    Finish(net::ERR_ACCESS_DENIED);
    return;
  }
  std::vector<std::u16string> labels;
  for (const auto& identity : candidates_) {
    auto cert =
        net::X509Certificate::CreateFromBytes(identity->signing_certificate);
    std::string label = cert ? cert->subject().GetDisplayName() : "USBKey";
    label +=
        " [" + Fingerprint(identity->signing_certificate).substr(0, 16) + "]";
    labels.push_back(base::UTF8ToUTF16(label));
  }
  prompt_timeout_.Start(
      FROM_HERE, base::Minutes(2),
      base::BindOnce(&KeyManager::Finish, operation_weak_factory_.GetWeakPtr(),
                     net::ERR_TIMED_OUT, nullptr));
  auto callback =
      base::BindOnce(&KeyManager::OnPin, operation_weak_factory_.GetWeakPtr());
  if (test_hooks_) {
    dialog_ = prompt_.Run(requests_.front().origin.Serialize(), labels, message,
                          std::move(callback));
  } else {
    dialog_ = ShowPinDialogForWebContents(requests_.front().web_contents.get(),
                                          requests_.front().origin.Serialize(),
                                          labels, message, std::move(callback));
  }
}

void KeyManager::OnPin(size_t selected,
                       std::unique_ptr<gm_key::SecurePin> pin) {
  prompt_timeout_.Stop();
  dialog_.reset();
  if (!pin || selected >= candidates_.size() || !Allowed(requests_.front()) ||
      (!test_hooks_ && !requests_.front().web_contents)) {
    // The browser has already handled national certificate selection. Never
    // invoke Chromium's separate BoringSSL client-certificate retry flow.
    Finish(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return;
  }
  const auto& identity = candidates_[selected];
  if (!MatchesRequest(*identity)) {
    Finish(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return;
  }
  selected_identity_ = identity->Clone();
  if (next_authorization_id_ == UINT64_MAX) {
    Finish(net::ERR_FAILED);
    return;
  }
  active_authorization_id_ = ++next_authorization_id_;
  auto receiver = pending_signer_.InitWithNewPipeAndPassReceiver();
  // Move-only PIN ownership. The service clears its decoded bytes immediately
  // after the SKF call; no browser preference, cache or logging receives it.
  service_->Authorize(
      active_authorization_id_, identity->identity_id, identity->generation,
      std::move(*pin), std::move(receiver),
      base::BindOnce(&KeyManager::OnGranted,
                     operation_weak_factory_.GetWeakPtr(), true));
}

void KeyManager::OnGranted(bool from_pin,
                           gm_key::mojom::KeyResultPtr result,
                           gm_key::mojom::KeyIdentityPtr identity) {
  if (!Ok(result) || !identity) {
    if (active_authorization_id_) {
      service_->CancelAuthorization(std::exchange(active_authorization_id_, 0));
    }
    pending_signer_.reset();
    if (result && result->error == gm_key::mojom::KeyError::kPinLocked &&
        !test_hooks_ && requests_.front().web_contents) {
      auto finish = base::BindOnce(
          &KeyManager::Finish, operation_weak_factory_.GetWeakPtr(),
          net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED, nullptr);
      prompt_timeout_.Start(FROM_HERE, base::Minutes(2),
                            base::BindOnce(&KeyManager::Finish,
                                           operation_weak_factory_.GetWeakPtr(),
                                           net::ERR_TIMED_OUT, nullptr));
      dialog_ = ShowPinLockedDialogForWebContents(
          requests_.front().web_contents.get(),
          requests_.front().origin.Serialize(), std::move(finish));
      return;
    }
    if (result && ((!from_pin &&
                    result->error == gm_key::mojom::KeyError::kPinRequired) ||
                   result->error == gm_key::mojom::KeyError::kPinIncorrect)) {
      std::u16string message;
      if (result->error == gm_key::mojom::KeyError::kPinIncorrect) {
        message = l10n_util::GetStringUTF16(IDS_GM_PIN_INCORRECT);
        if (result->pin_retries != UINT32_MAX) {
          message += u" " + l10n_util::GetStringFUTF16(
                                IDS_GM_PIN_RETRIES,
                                base::NumberToString16(result->pin_retries));
        }
      }
      PromptForPin(message);
      return;
    }
    Finish(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return;
  }
  if (!Allowed(requests_.front()) ||
      (!test_hooks_ && !requests_.front().web_contents)) {
    Finish(net::ERR_ACCESS_DENIED);
    return;
  }
  // The driver only owns the leaf certificates. Retain the chain discovered
  // by the browser; require all device identity fields to match the selection.
  if (selected_identity_) {
    identity->intermediates = selected_identity_->intermediates;
  }
  if (!selected_identity_ || !identity->Equals(*selected_identity_) ||
      !MatchesRequest(*identity)) {
    Finish(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return;
  }
  selected_[RequestKey()] = {identity->identity_id, identity->generation};
  auto grant = network::mojom::GmIdentityGrant::New();
  grant->identity = std::move(identity);
  grant->signer = std::move(pending_signer_);
  if (from_pin) {
    CHECK(active_authorization_id_);
    service_->CommitAuthorization(std::exchange(active_authorization_id_, 0));
  }
  Finish(net::OK, std::move(grant));
}

void KeyManager::Finish(int error, network::mojom::GmIdentityGrantPtr grant) {
  CHECK(!requests_.empty());
  auto callback = std::move(requests_.front().callback);
  requests_.pop_front();
  StopActiveRequest();
  ScheduleNextRequest();
  std::move(callback).Run(error, std::move(grant));
}

void KeyManager::StopActiveRequest() {
  if (active_authorization_id_ && service_.is_bound()) {
    service_->CancelAuthorization(std::exchange(active_authorization_id_, 0));
  }
  active_authorization_id_ = 0;
  operation_weak_factory_.InvalidateWeakPtrs();
  prompt_timeout_.Stop();
  if (dialog_) {
    dialog_->Close();
  }
  dialog_.reset();
  pending_signer_.reset();
  candidates_.clear();
  selected_identity_.reset();
  processing_ = false;
}

void KeyManager::ScheduleNextRequest() {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(&KeyManager::Process, weak_factory_.GetWeakPtr()));
}

void KeyManager::Reset(int error) {
  weak_factory_.InvalidateWeakPtrs();
  StopActiveRequest();
  observer_.reset();
  service_.reset();
  configured_ = configuring_ = false;
  selected_.clear();
  candidates_.clear();
  auto requests = std::move(requests_);
  requests_.clear();
  for (auto& request : requests) {
    std::move(request.callback).Run(error, nullptr);
  }
}

void KeyManager::OnPolicyChanged() {
  Reset(net::ERR_NETWORK_CHANGED);
}
void KeyManager::OnIdentitiesChanged() {
  Reset(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
}
void KeyManager::OnServiceFailure(gm_key::mojom::KeyError) {
  Reset(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
}

}  // namespace gm_browser
