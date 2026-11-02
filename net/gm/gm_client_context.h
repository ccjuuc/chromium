// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_CLIENT_CONTEXT_H_
#define NET_GM_GM_CLIENT_CONTEXT_H_

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "base/containers/flat_set.h"
#include "base/containers/span.h"
#include "base/memory/ref_counted.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_export.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/privacy_mode.h"
#include "net/base/proxy_chain.h"
#include "net/base/session_usage.h"
#include "net/gm/gm_backend.h"
#include "net/gm/gm_connection_profile.h"

namespace net {
struct SSLConfig;
namespace gm {
class ClientIdentity;
class GmCertificateVerifier;
struct TlsRule;

// A lazily parsed, immutable trust snapshot. Creation and cache lookup are
// cheap on the network sequence; parsing and any initialization wait happen
// exclusively on worker threads. This caches trust inputs, never decisions.
class NET_EXPORT CachedCertificateVerifier {
 public:
  explicit CachedCertificateVerifier(std::shared_ptr<const TlsRule> rule);
  ~CachedCertificateVerifier();

  // May be called concurrently on worker threads. Never call on the network
  // sequence. The returned verifier remains alive as long as this entry does.
  const GmCertificateVerifier* GetForWorker();

 private:
  const std::shared_ptr<const TlsRule> rule_;
  std::once_flag initialized_;
  std::shared_ptr<const GmCertificateVerifier> verifier_;
};

class NET_EXPORT BackendContext : public base::RefCounted<BackendContext> {
 public:
  static scoped_refptr<BackendContext> Create(
      std::shared_ptr<const TlsRule> rule);
  GmContext* handle() const { return handle_; }
  const GmBackendApi& api() const { return *api_; }

 private:
  friend class base::RefCounted<BackendContext>;
  BackendContext(const GmBackendApi* api,
                 GmContext* handle,
                 std::shared_ptr<const TlsRule> rule);
  ~BackendContext();
  const GmBackendApi* api_;
  GmContext* handle_;
  std::shared_ptr<const TlsRule> rule_;
};

// Owned by one SSLClientContext/NetworkContext on one OS thread. Sessions are
// memory-only and never enter Chromium's BoringSSL session cache.
class NET_EXPORT ClientContext {
 public:
  struct SessionKey {
    HostPortPair server;
    ConnectionProfile profile;
    NetworkAnonymizationKey network_anonymization_key;
    PrivacyMode privacy_mode = PRIVACY_MODE_DISABLED;
    ProxyChain proxy_chain = ProxyChain::Direct();
    size_t proxy_chain_index = 0;
    SessionUsage session_usage = SessionUsage::kDestination;
    std::string identity_id;
    uint64_t identity_generation = 0;
    std::vector<uint8_t> alpn;
    bool operator<(const SessionKey& other) const;
  };
  struct Session {
    std::vector<uint8_t> binding;
    std::vector<uint8_t> blob;
  };

  ClientContext();
  ~ClientContext();
  scoped_refptr<BackendContext> GetContext(std::shared_ptr<const TlsRule> rule);
  // Only joint TLS 1.3 policies use this cache. The immutable rule owns the
  // trust snapshot, so no result crosses policy or trust generations. Chain
  // verification (including current time and CRLs) still runs per connection.
  std::shared_ptr<CachedCertificateVerifier> GetCertificateVerifier(
      std::shared_ptr<const TlsRule> rule);
  static SessionKey MakeSessionKey(const HostPortPair& server,
                                   const SSLConfig& config,
                                   const ClientIdentity* identity,
                                   base::span<const uint8_t> effective_alpn);
  Session GetSession(const SessionKey& key);
  void StoreSession(const SessionKey& key,
                    const std::vector<uint8_t>& binding,
                    std::vector<uint8_t> blob);
  void InvalidateIdentity(const std::string& identity_id);
  void EraseSession(const SessionKey& key);
  void Clear();
  void ClearForServers(const base::flat_set<HostPortPair>& servers);

 private:
  std::map<std::shared_ptr<const TlsRule>, scoped_refptr<BackendContext>>
      contexts_;
  std::map<std::shared_ptr<const TlsRule>,
           std::shared_ptr<CachedCertificateVerifier>>
      certificate_verifiers_;
  std::map<SessionKey, Session> sessions_;
};

}  // namespace gm
}  // namespace net

#endif  // NET_GM_GM_CLIENT_CONTEXT_H_
