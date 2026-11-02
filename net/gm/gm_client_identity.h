// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_CLIENT_IDENTITY_H_
#define NET_GM_GM_CLIENT_IDENTITY_H_

#include <stdint.h>

#include <array>
#include <string>
#include <vector>

#include "base/callback_list.h"
#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/unguessable_token.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_export.h"
#include "net/gm/gm_connection_profile.h"

namespace net::gm {

inline constexpr uint32_t kClientCertificateSigning = 1u << 0;
inline constexpr uint32_t kClientCertificateEncryption = 1u << 1;

// A copy of the authenticated server's CertificateRequest. CA names are DER
// X509_NAME values; signature algorithms use their two-byte IANA identifiers.
struct NET_EXPORT ClientCertificateRequest {
  uint64_t request_id = 0;
  base::UnguessableToken gm_client_auth_context;
  // Resolved only after an authorization-cache miss. Ordinary TLS and cached
  // GM identities never register an interactive request.
  base::RepeatingCallback<base::UnguessableToken()>
      gm_client_auth_context_resolver;
  // The protocol actually negotiated: TLCP or RFC 8998, never kTls13Auto.
  TlsProtocol protocol = TlsProtocol::kRfc8998;
  uint32_t required_roles = kClientCertificateSigning;
  std::vector<std::vector<uint8_t>> certificate_authorities;
  std::vector<uint16_t> signature_algorithms;
  std::vector<uint8_t> certificate_types;
};

// A profile-local authorization to use a device identity. It contains public
// certificates only; a process-isolated implementation performs private-key
// operations. All calls and callbacks belong to the network OS thread.
class NET_EXPORT ClientIdentity : public base::RefCounted<ClientIdentity> {
 public:
  using SignCallback = base::OnceCallback<void(int, std::vector<uint8_t>)>;

  ClientIdentity(std::string identity_id,
                 uint64_t generation,
                 std::vector<uint8_t> signing_certificate,
                 std::vector<uint8_t> encryption_certificate,
                 std::vector<std::vector<uint8_t>> intermediates,
                 uint64_t signing_key_id = 0,
                 uint64_t encryption_key_id = 0);

  const std::string& identity_id() const { return identity_id_; }
  uint64_t generation() const { return generation_; }
  uint64_t signing_key_id() const { return signing_key_id_; }
  uint64_t encryption_key_id() const { return encryption_key_id_; }
  const std::vector<uint8_t>& signing_certificate() const {
    return signing_certificate_;
  }
  const std::vector<uint8_t>& encryption_certificate() const {
    return encryption_certificate_;
  }
  const std::vector<std::vector<uint8_t>>& intermediates() const {
    return intermediates_;
  }
  bool is_valid() const { return valid_; }
  base::CallbackListSubscription AddInvalidationCallback(
      base::RepeatingClosure callback);
  void Invalidate();

  // Implementations must reply asynchronously, never on the calling stack.
  virtual void SignDigest(uint64_t request_id,
                          const std::array<uint8_t, 32>& digest,
                          SignCallback callback) = 0;
  virtual void Cancel(uint64_t request_id) = 0;

 protected:
  friend class base::RefCounted<ClientIdentity>;
  virtual ~ClientIdentity();

 private:
  std::string identity_id_;
  uint64_t generation_;
  uint64_t signing_key_id_;
  uint64_t encryption_key_id_;
  std::vector<uint8_t> signing_certificate_;
  std::vector<uint8_t> encryption_certificate_;
  std::vector<std::vector<uint8_t>> intermediates_;
  bool valid_ = true;
  base::RepeatingClosureList invalidation_callbacks_;
};

class NET_EXPORT ClientIdentityProvider {
 public:
  using AcquireCallback =
      base::OnceCallback<void(int, scoped_refptr<ClientIdentity>)>;
  virtual ~ClientIdentityProvider() = default;
  // Never prompts or obtains new authorization. Only a still-authorized live
  // identity may be used to offer a session authenticated with that identity.
  virtual scoped_refptr<ClientIdentity> GetCachedIdentity(
      const HostPortPair& server,
      const ConnectionProfile& profile);
  // The browser must independently validate the origin and policy before
  // prompting the user or handing out an authorized signer. Replies are async.
  // Run the returned closure to withdraw an outstanding request. Completion
  // releases it without running; cancellation does not invoke the callback.
  virtual base::OnceClosure AcquireIdentity(
      const HostPortPair& server,
      const ConnectionProfile& profile,
      const ClientCertificateRequest& request,
      AcquireCallback callback) = 0;
};

}  // namespace net::gm

#endif  // NET_GM_GM_CLIENT_IDENTITY_H_
