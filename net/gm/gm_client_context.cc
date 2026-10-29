// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_client_context.h"

#include <utility>

#include "base/check.h"
#include "base/rand_util.h"
#include "base/threading/thread_restrictions.h"
#include "net/base/url_util.h"
#include "net/gm/gm_backend_loader.h"
#include "net/gm/gm_buildflags.h"
#include "net/gm/gm_client_identity.h"
#include "net/gm/gm_tls_policy.h"
#include "net/ssl/ssl_config.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "url/url_canon.h"

#if BUILDFLAG(XENON_GM)
#include "net/gm/gm_certificate_verifier.h"
#endif

namespace net::gm {
namespace {
void Wipe(std::vector<uint8_t>& bytes) {
  if (!bytes.empty()) {
    OPENSSL_cleanse(bytes.data(), bytes.size());
  }
}

HostPortPair CanonicalServer(const HostPortPair& server) {
  url::CanonHostInfo info;
  std::string host =
      server.host().starts_with('[')
          ? CanonicalizeHost(server.host(), &info)
          : CanonicalizeHostSupportsBareIPV6(server.host(), &info);
  if (!host.empty() && host.back() == '.') {
    host.pop_back();
  }
  return HostPortPair(host, server.port());
}
}  // namespace

BackendContext::BackendContext(const GmBackendApi* api,
                               GmContext* handle,
                               std::shared_ptr<const TlsRule> rule)
    : api_(api), handle_(handle), rule_(std::move(rule)) {}

BackendContext::~BackendContext() {
  CHECK_EQ(CallBackendFunction(api_->destroy_context, handle_), GM_OK);
}

scoped_refptr<BackendContext> BackendContext::Create(
    std::shared_ptr<const TlsRule> rule) {
  const auto* api = GetBackendApi();
  if (!api || !rule || !rule->IsValid() ||
      (rule->profile.protocol() != TlsProtocol::kTlcp &&
       rule->profile.protocol() != TlsProtocol::kRfc8998)) {
    return nullptr;
  }
  std::vector<GmBytes> roots;
  std::vector<GmBytes> intermediates;
  std::vector<GmBytes> disallowed;
  std::vector<GmBytes> crls;
  for (const auto& der : rule->trust_anchors) {
    roots.push_back({der.data(), der.size()});
  }
  for (const auto& der : rule->crls) {
    crls.push_back({der.data(), der.size()});
  }
  for (const auto& der : rule->untrusted_intermediates) {
    intermediates.push_back({der.data(), der.size()});
  }
  for (const auto& der : rule->disallowed_certificates) {
    disallowed.push_back({der.data(), der.size()});
  }
  GmContextConfig config = {};
  config.struct_size = sizeof(config);
  config.protocol = rule->profile.protocol() == TlsProtocol::kTlcp
                        ? GM_PROTOCOL_TLCP
                        : GM_PROTOCOL_RFC8998;
  config.trust_anchors = roots.data();
  config.trust_anchor_count = roots.size();
  config.crls = crls.data();
  config.crl_count = crls.size();
  config.revocation_mode = rule->revocation_mode;
  config.untrusted_intermediates = intermediates.data();
  config.untrusted_intermediate_count = intermediates.size();
  config.disallowed_certificates = disallowed.data();
  config.disallowed_certificate_count = disallowed.size();
  GmContext* handle = nullptr;
  if (CallBackendFunction(api->create_context, &config, &handle) != GM_OK) {
    return nullptr;
  }
  return base::WrapRefCounted(new BackendContext(api, handle, std::move(rule)));
}

ClientContext::ClientContext() = default;
ClientContext::~ClientContext() {
  Clear();
}

bool ClientContext::SessionKey::operator<(const SessionKey& other) const {
  return std::tie(server, profile, network_anonymization_key, privacy_mode,
                  proxy_chain, proxy_chain_index, session_usage, identity_id,
                  identity_generation, alpn) <
         std::tie(other.server, other.profile, other.network_anonymization_key,
                  other.privacy_mode, other.proxy_chain,
                  other.proxy_chain_index, other.session_usage,
                  other.identity_id, other.identity_generation, other.alpn);
}

scoped_refptr<BackendContext> ClientContext::GetContext(
    std::shared_ptr<const TlsRule> rule) {
  auto found = contexts_.find(rule);
  if (found != contexts_.end()) {
    return found->second;
  }
  auto context = BackendContext::Create(rule);
  if (context) {
    if (contexts_.size() >= 32) {
      contexts_.erase(contexts_.begin());
    }
    contexts_.emplace(std::move(rule), context);
  }
  return context;
}

CachedCertificateVerifier::CachedCertificateVerifier(
    std::shared_ptr<const TlsRule> rule)
    : rule_(std::move(rule)) {}

CachedCertificateVerifier::~CachedCertificateVerifier() = default;

const GmCertificateVerifier* CachedCertificateVerifier::GetForWorker() {
#if BUILDFLAG(XENON_GM)
  base::AssertLongCPUWorkAllowed();
  std::call_once(initialized_, [this] {
    if (!rule_ || rule_->profile.protocol() != TlsProtocol::kTls13Auto ||
        !rule_->IsValid()) {
      return;
    }
    const auto views = [](const std::vector<std::vector<uint8_t>>& values) {
      std::vector<GmBytes> result;
      result.reserve(values.size());
      for (const auto& value : values) {
        result.push_back({value.data(), value.size()});
      }
      return result;
    };
    const auto roots = views(rule_->trust_anchors);
    const auto issuers = views(rule_->untrusted_intermediates);
    const auto disallowed = views(rule_->disallowed_certificates);
    const auto crls = views(rule_->crls);
    GmContextConfig config = {};
    config.struct_size = sizeof(config);
    config.protocol = GM_PROTOCOL_RFC8998;
    config.trust_anchors = roots.data();
    config.trust_anchor_count = roots.size();
    config.untrusted_intermediates = issuers.data();
    config.untrusted_intermediate_count = issuers.size();
    config.disallowed_certificates = disallowed.data();
    config.disallowed_certificate_count = disallowed.size();
    config.crls = crls.data();
    config.crl_count = crls.size();
    config.revocation_mode = rule_->revocation_mode;
    verifier_ = GmCertificateVerifier::Create(config);
  });
  return verifier_.get();
#else
  return nullptr;
#endif
}

std::shared_ptr<CachedCertificateVerifier>
ClientContext::GetCertificateVerifier(std::shared_ptr<const TlsRule> rule) {
#if BUILDFLAG(XENON_GM)
  if (!rule || rule->profile.protocol() != TlsProtocol::kTls13Auto) {
    return nullptr;
  }
  const auto found = certificate_verifiers_.find(rule);
  if (found != certificate_verifiers_.end()) {
    return found->second;
  }
  auto verifier = std::make_shared<CachedCertificateVerifier>(rule);
  if (certificate_verifiers_.size() >= 32) {
    certificate_verifiers_.erase(certificate_verifiers_.begin());
  }
  certificate_verifiers_.emplace(std::move(rule), verifier);
  return verifier;
#else
  return nullptr;
#endif
}

ClientContext::SessionKey ClientContext::MakeSessionKey(
    const HostPortPair& server,
    const SSLConfig& config,
    const ClientIdentity* identity,
    base::span<const uint8_t> effective_alpn) {
  CHECK(config.gm_rule);
  SessionKey key;
  key.server = server;
  key.profile = config.gm_rule->profile;
  key.network_anonymization_key = config.network_anonymization_key;
  key.privacy_mode = config.privacy_mode;
  key.proxy_chain = config.proxy_chain;
  key.proxy_chain_index = config.proxy_chain_index;
  key.session_usage = config.session_usage;
  key.alpn.assign(effective_alpn.begin(), effective_alpn.end());
  if (identity) {
    key.identity_id = identity->identity_id();
    key.identity_generation = identity->generation();
  }
  return key;
}

ClientContext::Session ClientContext::GetSession(const SessionKey& key) {
  auto found = sessions_.find(key);
  if (found != sessions_.end()) {
    return found->second;
  }
  if (sessions_.size() >= 256) {
    Wipe(sessions_.begin()->second.blob);
    sessions_.erase(sessions_.begin());
  }
  Session session;
  session.binding.resize(32);
  base::RandBytes(session.binding);
  sessions_.emplace(key, session);
  return session;
}

void ClientContext::StoreSession(const SessionKey& key,
                                 const std::vector<uint8_t>& binding,
                                 std::vector<uint8_t> blob) {
  const auto found = sessions_.find(key);
  if (found == sessions_.end() || found->second.binding != binding) {
    Wipe(blob);
    return;
  }
  Wipe(found->second.blob);
  found->second.blob = std::move(blob);
}

void ClientContext::InvalidateIdentity(const std::string& identity_id) {
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->first.identity_id == identity_id) {
      Wipe(it->second.blob);
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
}

void ClientContext::Clear() {
  for (auto& [key, session] : sessions_) {
    Wipe(session.blob);
  }
  sessions_.clear();
  contexts_.clear();
  certificate_verifiers_.clear();
}

void ClientContext::ClearForServers(
    const base::flat_set<HostPortPair>& servers) {
  base::flat_set<HostPortPair> normalized;
  for (const auto& server : servers) {
    normalized.insert(CanonicalServer(server));
  }
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (normalized.contains(CanonicalServer(it->first.server))) {
      Wipe(it->second.blob);
      it = sessions_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = contexts_.begin(); it != contexts_.end();) {
    if (normalized.contains(
            CanonicalServer(HostPortPair(it->first->host, it->first->port)))) {
      it = contexts_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = certificate_verifiers_.begin();
       it != certificate_verifiers_.end();) {
    if (normalized.contains(
            CanonicalServer(HostPortPair(it->first->host, it->first->port)))) {
      it = certificate_verifiers_.erase(it);
    } else {
      ++it;
    }
  }
}

void ClientContext::EraseSession(const SessionKey& key) {
  const auto found = sessions_.find(key);
  if (found != sessions_.end()) {
    Wipe(found->second.blob);
    sessions_.erase(found);
  }
}

}  // namespace net::gm
