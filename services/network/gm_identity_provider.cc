// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/gm_identity_provider.h"

#include <algorithm>
#include <limits>
#include <utility>

#include "base/functional/bind.h"
#include "base/rand_util.h"
#include "base/task/bind_post_task.h"
#include "base/task/single_thread_task_runner.h"
#include "net/base/net_errors.h"
#include "net/gm/gm_client_cert_constraints.h"
#include "services/gm_key/public/mojom/gm_key_service.mojom.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace network {
namespace {
constexpr size_t kMaxPending = 64;
constexpr size_t kMaxCachedGrants = 64;

int MapKeyError(gm_key::mojom::KeyError error) {
  using Error = gm_key::mojom::KeyError;
  switch (error) {
    case Error::kOk:
      return net::OK;
    case Error::kCancelled:
      return net::ERR_ABORTED;
    case Error::kTimedOut:
      return net::ERR_TIMED_OUT;
    case Error::kQueueFull:
      return net::ERR_INSUFFICIENT_RESOURCES;
    case Error::kInvalidArgument:
      return net::ERR_INVALID_ARGUMENT;
    default:
      return net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
  }
}

bool ValidGrant(const mojom::GmIdentityGrantPtr& grant,
                const net::gm::ClientCertificateRequest& request) {
  if (!grant || !grant->identity || !grant->signer.is_valid()) {
    return false;
  }
  const auto& identity = *grant->identity;
  const auto valid_certificate = [](const std::vector<uint8_t>& der) {
    return !der.empty() && der.size() <= 256 * 1024;
  };
  return !identity.identity_id.empty() && identity.identity_id.size() <= 1024 &&
         identity.generation &&
         valid_certificate(identity.signing_certificate) &&
         (identity.encryption_certificate.empty() ||
          valid_certificate(identity.encryption_certificate)) &&
         identity.intermediates.size() <= 32 &&
         std::ranges::all_of(identity.intermediates, valid_certificate) &&
         net::gm::MatchesClientCertificateRequest(
             identity.signing_certificate, identity.encryption_certificate,
             identity.intermediates, request);
}

void Reply(net::gm::ClientIdentityProvider::AcquireCallback callback,
           int error,
           scoped_refptr<net::gm::ClientIdentity> identity = nullptr) {
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(std::move(callback), error, std::move(identity)));
}
}  // namespace

class GmIdentityProvider::Grant : public base::RefCounted<Grant> {
 public:
  using SignCallback = net::gm::ClientIdentity::SignCallback;
  Grant(mojom::GmIdentityGrantPtr grant,
        std::string origin,
        net::gm::ConnectionProfile profile)
      : origin_(std::move(origin)),
        profile_(std::move(profile)),
        identity_(std::move(grant->identity)),
        signer_(std::move(grant->signer)),
        signing_key_id_(base::RandUint64() | 1),
        encryption_key_id_(base::RandUint64() | 1) {
    signer_.set_disconnect_handler(
        base::BindOnce(&Grant::Revoke, weak_factory_.GetWeakPtr()));
  }
  const gm_key::mojom::KeyIdentity& identity() const { return *identity_; }
  bool MatchesScope(const std::string& origin,
                    const net::gm::ConnectionProfile& profile) const {
    return origin_ == origin && profile_ == profile;
  }
  uint64_t signing_key_id() const { return signing_key_id_; }
  uint64_t encryption_key_id() const { return encryption_key_id_; }
  bool valid() const { return signer_.is_bound(); }
  base::WeakPtr<Grant> GetWeakPtr() { return weak_factory_.GetWeakPtr(); }
  base::CallbackListSubscription Subscribe(base::RepeatingClosure callback) {
    return invalidated_.Add(std::move(callback));
  }
  uint64_t Sign(const std::array<uint8_t, 32>& digest, SignCallback callback) {
    if (!valid() || next_request_ == std::numeric_limits<uint64_t>::max() ||
        pending_.size() >= kMaxPending) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(
              std::move(callback),
              valid() ? net::ERR_INSUFFICIENT_RESOURCES
                      : net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED,
              std::vector<uint8_t>()));
      return 0;
    }
    const uint64_t request = ++next_request_;
    pending_.emplace(request, std::move(callback));
    signer_->Sign(
        request, std::vector<uint8_t>(digest.begin(), digest.end()),
        base::BindOnce(&Grant::OnSigned, weak_factory_.GetWeakPtr(), request));
    return request;
  }
  void Cancel(uint64_t request) {
    if (pending_.erase(request) && valid()) {
      signer_->Cancel(request);
    }
  }
  void Revoke() {
    scoped_refptr<Grant> keep_alive(this);
    signer_.reset();
    auto pending = std::move(pending_);
    pending_.clear();
    invalidated_.Notify();
    for (auto& [id, callback] : pending) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(std::move(callback),
                         net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED,
                         std::vector<uint8_t>()));
    }
  }

 private:
  friend class base::RefCounted<Grant>;
  ~Grant() = default;
  void OnSigned(uint64_t request,
                gm_key::mojom::KeyResultPtr result,
                const std::vector<uint8_t>& signature) {
    auto found = pending_.find(request);
    if (found == pending_.end()) {
      return;
    }
    auto callback = std::move(found->second);
    pending_.erase(found);
    int error = result ? MapKeyError(result->error)
                       : net::ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED;
    if (error == net::OK && (signature.empty() || signature.size() > 256)) {
      error = net::ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED;
    }
    std::move(callback).Run(
        error, error == net::OK ? signature : std::vector<uint8_t>());
  }
  const std::string origin_;
  const net::gm::ConnectionProfile profile_;
  gm_key::mojom::KeyIdentityPtr identity_;
  mojo::Remote<gm_key::mojom::KeySigner> signer_;
  const uint64_t signing_key_id_;
  const uint64_t encryption_key_id_;
  uint64_t next_request_ = 0;
  std::map<uint64_t, SignCallback> pending_;
  base::RepeatingClosureList invalidated_;
  base::WeakPtrFactory<Grant> weak_factory_{this};
};

class GmIdentityProvider::Identity final : public net::gm::ClientIdentity {
 public:
  explicit Identity(scoped_refptr<Grant> grant)
      : ClientIdentity(grant->identity().identity_id,
                       grant->identity().generation,
                       grant->identity().signing_certificate,
                       grant->identity().encryption_certificate,
                       grant->identity().intermediates,
                       grant->signing_key_id(),
                       grant->encryption_key_id()),
        grant_(std::move(grant)) {
    subscription_ = grant_->Subscribe(
        base::BindRepeating(&Identity::Invalidate, weak_factory_.GetWeakPtr()));
  }
  void SignDigest(uint64_t request,
                  const std::array<uint8_t, 32>& digest,
                  SignCallback callback) override {
    if (!is_valid() || !request || pending_.contains(request)) {
      base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
          FROM_HERE,
          base::BindOnce(std::move(callback),
                         net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED,
                         std::vector<uint8_t>()));
      return;
    }
    const uint64_t wire = grant_->Sign(
        digest, base::BindOnce(&Identity::OnSigned, weak_factory_.GetWeakPtr(),
                               request, std::move(callback)));
    pending_.emplace(request, wire);
  }
  void Cancel(uint64_t request) override {
    auto found = pending_.find(request);
    if (found == pending_.end()) {
      return;
    }
    grant_->Cancel(found->second);
    pending_.erase(found);
  }

 private:
  ~Identity() override {
    for (auto& [local, wire] : pending_) {
      grant_->Cancel(wire);
    }
  }
  void OnSigned(uint64_t request,
                SignCallback callback,
                int error,
                std::vector<uint8_t> signature) {
    if (!pending_.erase(request)) {
      return;
    }
    std::move(callback).Run(error, std::move(signature));
  }
  scoped_refptr<Grant> grant_;
  base::CallbackListSubscription subscription_;
  std::map<uint64_t, uint64_t> pending_;
  base::WeakPtrFactory<Identity> weak_factory_{this};
};

GmIdentityProvider::GmIdentityProvider(
    mojo::PendingRemote<mojom::GmIdentityBroker> broker)
    : broker_(std::move(broker)) {
  if (broker_.is_bound()) {
    broker_.set_disconnect_handler(base::BindOnce(
        &GmIdentityProvider::OnBrokerDisconnected, weak_factory_.GetWeakPtr()));
  }
}
GmIdentityProvider::~GmIdentityProvider() {
  OnBrokerDisconnected();
}

base::UnguessableToken GmIdentityProvider::RegisterRequest(
    IdentityRequestCallback callback) {
  CHECK(callback);
  const auto context = base::UnguessableToken::Create();
  requests_.emplace(context, std::move(callback));
  return context;
}

void GmIdentityProvider::UnregisterRequest(
    const base::UnguessableToken& context) {
  requests_.erase(context);
  std::vector<Pending> cancelled;
  for (auto found = pending_.begin(); found != pending_.end();) {
    if (found->first.second != context) {
      ++found;
      continue;
    }
    pending_count_ -= found->second.waiters.size();
    cancelled.push_back(std::move(found->second));
    found = pending_.erase(found);
  }
  // Remove all state before cancelling UI: a cancellation closure may reenter.
  for (auto& pending : cancelled) {
    pending.cancel.RunAndReset();
    for (auto& [id, callback] : pending.waiters) {
      Reply(std::move(callback), net::ERR_ABORTED);
    }
  }
}

net::gm::ClientCertificateRequest GmIdentityProvider::RequestForKey(
    const Key& key) {
  net::gm::ClientCertificateRequest request;
  // AcquireIdentity only admits an RFC 8998 request for a joint TLS 1.3
  // profile. Retain that concrete protocol when reconstructing cached requests.
  const auto protocol = std::get<1>(key).protocol();
  request.protocol = protocol == net::gm::TlsProtocol::kTls13Auto
                         ? net::gm::TlsProtocol::kRfc8998
                         : protocol;
  request.required_roles = std::get<2>(key);
  request.certificate_authorities = std::get<3>(key);
  request.signature_algorithms = std::get<4>(key);
  request.certificate_types = std::get<5>(key);
  return request;
}

scoped_refptr<net::gm::ClientIdentity> GmIdentityProvider::GetCachedIdentity(
    const net::HostPortPair& server,
    const net::gm::ConnectionProfile& profile) {
  const auto origin = url::Origin::Create(GURL("https://" + server.ToString()));
  scoped_refptr<Grant> selected;
  for (const auto& [key, grant] : grants_) {
    if (std::get<0>(key) != origin.Serialize() || std::get<1>(key) != profile ||
        !grant->valid()) {
      continue;
    }
    const auto& identity = grant->identity();
    if (!net::gm::MatchesClientCertificateRequest(
            identity.signing_certificate, identity.encryption_certificate,
            identity.intermediates, RequestForKey(key))) {
      continue;
    }
    // A new CertificateRequest is needed if earlier connections selected
    // different identities. Never guess which identity should resume.
    if (selected && !selected->identity().Equals(identity)) {
      return nullptr;
    }
    selected = grant;
  }
  return selected ? base::MakeRefCounted<Identity>(selected) : nullptr;
}

base::OnceClosure GmIdentityProvider::AcquireIdentity(
    const net::HostPortPair& server,
    const net::gm::ConnectionProfile& profile,
    const net::gm::ClientCertificateRequest& request,
    AcquireCallback callback) {
  const auto origin = url::Origin::Create(GURL("https://" + server.ToString()));
  if (origin.opaque() ||
      !net::gm::IsClientCertificateRequestForProfile(profile, request)) {
    Reply(std::move(callback),
          net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return {};
  }
  Key key(origin.Serialize(), profile, request.required_roles,
          request.certificate_authorities, request.signature_algorithms,
          request.certificate_types);
  auto found = grants_.find(key);
  if (found != grants_.end() && found->second->valid() &&
      net::gm::MatchesClientCertificateRequest(
          found->second->identity().signing_certificate,
          found->second->identity().encryption_certificate,
          found->second->identity().intermediates, request)) {
    Reply(std::move(callback), net::OK,
          base::MakeRefCounted<Identity>(found->second));
    return {};
  }
  auto request_context = request.gm_client_auth_context;
  if (request_context.is_empty() && request.gm_client_auth_context_resolver) {
    request_context = request.gm_client_auth_context_resolver.Run();
  }
  if (request_context.is_empty() || !requests_.contains(request_context)) {
    Reply(std::move(callback),
          profile.client_identity_policy_id().empty()
              ? net::OK
              : net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    return {};
  }
  if (pending_count_ >= kMaxPending ||
      next_request_id_ == std::numeric_limits<uint64_t>::max()) {
    Reply(std::move(callback), net::ERR_INSUFFICIENT_RESOURCES);
    return {};
  }
  const uint64_t request_id = ++next_request_id_;
  PendingKey pending_key(std::move(key), request_context);
  auto& pending = pending_[pending_key];
  pending.waiters.emplace(request_id, std::move(callback));
  ++pending_count_;
  if (pending.waiters.size() == 1) {
    pending.broker_request_id = request_id;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&GmIdentityProvider::DispatchRequest,
                       weak_factory_.GetWeakPtr(), pending_key, request_id));
  }
  return base::BindOnce(&GmIdentityProvider::CancelAcquire,
                        weak_factory_.GetWeakPtr(), std::move(pending_key),
                        request_id);
}

void GmIdentityProvider::DispatchRequest(PendingKey key, uint64_t request_id) {
  auto pending = pending_.find(key);
  auto registered = requests_.find(key.second);
  if (pending == pending_.end() ||
      pending->second.broker_request_id != request_id ||
      registered == requests_.end()) {
    return;
  }
  const auto request = RequestForKey(key.first);
  auto constraints = mojom::GmClientCertificateRequest::New();
  constraints->protocol = request.protocol == net::gm::TlsProtocol::kTlcp
                              ? mojom::GmTlsProtocol::kTlcp
                              : mojom::GmTlsProtocol::kRfc8998;
  constraints->required_roles = request.required_roles;
  constraints->certificate_authorities = request.certificate_authorities;
  constraints->signature_algorithms = request.signature_algorithms;
  constraints->certificate_types = request.certificate_types;
  auto callback = registered->second;
  const auto& profile = std::get<1>(key.first);
  auto weak = weak_factory_.GetWeakPtr();
  auto cancel = callback.Run(
      url::Origin::Create(GURL(std::get<0>(key.first))),
      profile.client_identity_policy_id(), profile.identity_generation(),
      std::move(constraints),
      base::BindPostTaskToCurrentDefault(base::BindOnce(
          &GmIdentityProvider::OnAcquired, weak, key, request_id)));
  // A callback may cancel or destroy us inline; completion is always posted.
  if (!weak) {
    if (cancel) {
      std::move(cancel).Run();
    }
    return;
  }
  pending = pending_.find(key);
  if (pending != pending_.end() &&
      pending->second.broker_request_id == request_id) {
    pending->second.cancel.ReplaceClosure(std::move(cancel));
  } else if (cancel) {
    std::move(cancel).Run();
  }
}

void GmIdentityProvider::CancelAcquire(PendingKey key, uint64_t request_id) {
  const auto found = pending_.find(key);
  if (found == pending_.end() || !found->second.waiters.erase(request_id)) {
    return;
  }
  --pending_count_;
  if (found->second.waiters.empty()) {
    auto cancel = found->second.cancel.Release();
    pending_.erase(found);
    if (cancel) {
      std::move(cancel).Run();
    }
  }
}

void GmIdentityProvider::OnAcquired(PendingKey pending_key,
                                    uint64_t request_id,
                                    int error,
                                    mojom::GmIdentityGrantPtr granted) {
  auto found = pending_.find(pending_key);
  if (found == pending_.end() ||
      found->second.broker_request_id != request_id) {
    return;
  }
  auto callbacks = std::move(found->second.waiters);
  auto key = std::move(pending_key.first);
  found->second.cancel.Release().Reset();
  pending_count_ -= callbacks.size();
  pending_.erase(found);
  scoped_refptr<Grant> grant;
  if (error == net::OK && !granted &&
      std::get<1>(key).client_identity_policy_id().empty()) {
    // An optional CertificateRequest may be answered without a certificate.
    // Do not cache this decision: a Key may be inserted before the next
    // request.
  } else if (error == net::OK && ValidGrant(granted, RequestForKey(key))) {
    std::erase_if(live_grants_, [](const auto& weak) { return !weak; });
    for (const auto& existing : live_grants_) {
      if (existing->valid() &&
          existing->MatchesScope(std::get<0>(key), std::get<1>(key)) &&
          existing->identity().Equals(*granted->identity)) {
        // The browser has approved the new CertificateRequest. Keep the live
        // capability's key IDs stable when its entire public identity agrees:
        // session tickets bind these IDs as well as identity_id/generation.
        // Dropping the redundant signer closes only that separate capability;
        // it neither replaces nor revives the existing authorization.
        grant = base::WrapRefCounted(existing.get());
        break;
      }
    }
    if (!grant) {
      grant = base::MakeRefCounted<Grant>(std::move(granted), std::get<0>(key),
                                          std::get<1>(key));
      live_grants_.push_back(grant->GetWeakPtr());
    }
    if (grants_.size() >= kMaxCachedGrants) {
      grants_.erase(grants_.begin());
    }
    grants_.insert_or_assign(std::move(key), grant);
  } else if (error >= net::OK ||
             error == net::ERR_SSL_CLIENT_AUTH_CERT_NEEDED) {
    // GM authorization is completed by the browser broker. It cannot enter
    // the standard BoringSSL certificate-selection/restart path.
    error = net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
  }
  for (auto& [id, callback] : callbacks) {
    Reply(std::move(callback), error,
          grant ? base::MakeRefCounted<Identity>(grant) : nullptr);
  }
}

void GmIdentityProvider::OnBrokerDisconnected() {
  broker_.reset();
  requests_.clear();
  for (auto& weak : live_grants_) {
    if (weak) {
      weak->Revoke();
    }
  }
  live_grants_.clear();
  grants_.clear();
  auto pending = std::move(pending_);
  pending_.clear();
  pending_count_ = 0;
  for (auto& [key, request] : pending) {
    for (auto& [id, callback] : request.waiters) {
      Reply(std::move(callback),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
    }
  }
}

}  // namespace network
