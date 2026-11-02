// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_CONNECTION_PROFILE_H_
#define NET_GM_GM_CONNECTION_PROFILE_H_

#include <stdint.h>

#include <compare>
#include <string>

#include "net/base/net_export.h"

namespace net::gm {

enum class TlsProtocol {
  kStandardTls = 0,
  kTlcp = 1,
  kRfc8998 = 2,
  // A negotiation policy for ordinary TLS 1.3 and RFC 8998. This is not an
  // on-wire protocol and must never describe a CertificateRequest.
  kTls13Auto = 3,
};

enum class ProfileValidationError {
  kNone,
  kUnknownProtocol,
  kInvalidTrustGeneration,
  kInvalidIdentityGeneration,
  kMissingGmTrust,
  kMissingPolicyRevision,
};

// An immutable value identifying the effective security policy of a connection.
// IDs are exact, context-local identifiers, not truncated hashes. Generations
// and revisions are nonzero for configured policies and must never be reused
// within that context. The revision covers policy details such as allowed
// cipher suites and resumption rules that are not stored in this value.
//
// This value is an additional connection/cache key. It does not replace the
// NetworkContext, network anonymization key, privacy mode, proxy, or origin.
// It does not enable routing: callers must carry it through all reuse paths
// before exposing a GM policy to browser requests.
class NET_EXPORT ConnectionProfile {
 public:
  // The ordinary Chromium TLS policy, with no additional managed identity.
  ConnectionProfile();
  ConnectionProfile(TlsProtocol protocol,
                    std::string trust_profile_id,
                    uint64_t trust_generation,
                    std::string client_identity_policy_id,
                    uint64_t identity_generation,
                    uint64_t effective_policy_revision);
  ConnectionProfile(const ConnectionProfile&);
  ConnectionProfile(ConnectionProfile&&);
  ConnectionProfile& operator=(const ConnectionProfile&);
  ConnectionProfile& operator=(ConnectionProfile&&);
  ~ConnectionProfile();

  // Construction preserves even invalid input for explicit validation at the
  // configuration boundary. Invalid profiles never advertise capabilities.
  ProfileValidationError Validate() const;
  bool IsValid() const;

  TlsProtocol protocol() const { return protocol_; }
  const std::string& trust_profile_id() const { return trust_profile_id_; }
  uint64_t trust_generation() const { return trust_generation_; }
  const std::string& client_identity_policy_id() const {
    return client_identity_policy_id_;
  }
  uint64_t identity_generation() const { return identity_generation_; }
  uint64_t effective_policy_revision() const {
    return effective_policy_revision_;
  }

  bool AllowsHttp11() const;
  bool AllowsHttp2() const;
  bool AllowsQuic() const;

  // This is only a policy permission, not an authorization to reuse a session.
  // Reuse additionally requires equal profiles, certificate authorization for
  // the new origin, and all of Chromium's ordinary pooling checks.
  bool AllowsCrossOriginHttp2() const;

  // Every field participates, including trust, identity and policy generations.
  bool operator==(const ConnectionProfile&) const = default;
  std::strong_ordering operator<=>(const ConnectionProfile&) const = default;

 private:
  TlsProtocol protocol_ = TlsProtocol::kStandardTls;
  std::string trust_profile_id_;
  uint64_t trust_generation_ = 0;
  std::string client_identity_policy_id_;
  uint64_t identity_generation_ = 0;
  uint64_t effective_policy_revision_ = 0;
};

}  // namespace net::gm

#endif  // NET_GM_GM_CONNECTION_PROFILE_H_
