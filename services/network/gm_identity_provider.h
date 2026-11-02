// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_NETWORK_GM_IDENTITY_PROVIDER_H_
#define SERVICES_NETWORK_GM_IDENTITY_PROVIDER_H_

#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "base/component_export.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/weak_ptr.h"
#include "base/unguessable_token.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/gm/gm_client_identity.h"
#include "services/network/public/mojom/gm_identity.mojom.h"

namespace network {

// One NetworkContext owns the grants and coalesces identical authorization
// requests. Every socket receives a separate lease so overlapping backend
// request IDs cannot cancel or complete another socket's private-key operation.
class COMPONENT_EXPORT(NETWORK_SERVICE) GmIdentityProvider final
    : public net::gm::ClientIdentityProvider {
 public:
  explicit GmIdentityProvider(
      mojo::PendingRemote<mojom::GmIdentityBroker> broker = {});
  ~GmIdentityProvider() override;
  using IdentityRequestCallback = base::RepeatingCallback<base::OnceClosure(
      const url::Origin&,
      const std::string& identity_policy_id,
      uint64_t generation,
      mojom::GmClientCertificateRequestPtr,
      mojom::GmIdentityBroker::AcquireIdentityCallback)>;

  // Only a live URLLoader can authorize new user interaction. Tokens identify
  // the owner of a prompt, not a connection/session/grant partition.
  base::UnguessableToken RegisterRequest(IdentityRequestCallback callback);
  void UnregisterRequest(const base::UnguessableToken& context);
  scoped_refptr<net::gm::ClientIdentity> GetCachedIdentity(
      const net::HostPortPair& server,
      const net::gm::ConnectionProfile& profile) override;
  base::OnceClosure AcquireIdentity(
      const net::HostPortPair& server,
      const net::gm::ConnectionProfile& profile,
      const net::gm::ClientCertificateRequest& request,
      AcquireCallback callback) override;

 private:
  class Grant;
  class Identity;
  using Key = std::tuple<std::string,
                         net::gm::ConnectionProfile,
                         uint32_t,
                         std::vector<std::vector<uint8_t>>,
                         std::vector<uint16_t>,
                         std::vector<uint8_t>>;
  static net::gm::ClientCertificateRequest RequestForKey(const Key& key);
  using PendingKey = std::pair<Key, base::UnguessableToken>;
  struct Pending {
    uint64_t broker_request_id = 0;
    std::map<uint64_t, AcquireCallback> waiters;
    base::ScopedClosureRunner cancel;
  };
  void DispatchRequest(PendingKey key, uint64_t request_id);
  void CancelAcquire(PendingKey key, uint64_t request_id);
  void OnAcquired(PendingKey key,
                  uint64_t request_id,
                  int net_error,
                  mojom::GmIdentityGrantPtr grant);
  void OnBrokerDisconnected();

  mojo::Remote<mojom::GmIdentityBroker> broker_;
  std::map<Key, scoped_refptr<Grant>> grants_;
  std::vector<base::WeakPtr<Grant>> live_grants_;
  std::map<base::UnguessableToken, IdentityRequestCallback> requests_;
  std::map<PendingKey, Pending> pending_;
  size_t pending_count_ = 0;
  uint64_t next_request_id_ = 0;
  base::WeakPtrFactory<GmIdentityProvider> weak_factory_{this};
};

}  // namespace network

#endif  // SERVICES_NETWORK_GM_IDENTITY_PROVIDER_H_
