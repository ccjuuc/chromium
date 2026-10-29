// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_connection_profile.h"

#include <utility>

namespace net::gm {

ConnectionProfile::ConnectionProfile() = default;

ConnectionProfile::ConnectionProfile(TlsProtocol protocol,
                                     std::string trust_profile_id,
                                     uint64_t trust_generation,
                                     std::string client_identity_policy_id,
                                     uint64_t identity_generation,
                                     uint64_t effective_policy_revision)
    : protocol_(protocol),
      trust_profile_id_(std::move(trust_profile_id)),
      trust_generation_(trust_generation),
      client_identity_policy_id_(std::move(client_identity_policy_id)),
      identity_generation_(identity_generation),
      effective_policy_revision_(effective_policy_revision) {}

ConnectionProfile::ConnectionProfile(const ConnectionProfile&) = default;
ConnectionProfile::ConnectionProfile(ConnectionProfile&&) = default;
ConnectionProfile& ConnectionProfile::operator=(const ConnectionProfile&) =
    default;
ConnectionProfile& ConnectionProfile::operator=(ConnectionProfile&&) = default;
ConnectionProfile::~ConnectionProfile() = default;

ProfileValidationError ConnectionProfile::Validate() const {
  switch (protocol_) {
    case TlsProtocol::kStandardTls:
    case TlsProtocol::kTlcp:
    case TlsProtocol::kRfc8998:
    case TlsProtocol::kTls13Auto:
      break;
    default:
      return ProfileValidationError::kUnknownProtocol;
  }
  if (trust_profile_id_.empty() != (trust_generation_ == 0)) {
    return ProfileValidationError::kInvalidTrustGeneration;
  }
  if (client_identity_policy_id_.empty() != (identity_generation_ == 0)) {
    return ProfileValidationError::kInvalidIdentityGeneration;
  }
  if (protocol_ != TlsProtocol::kStandardTls && trust_profile_id_.empty()) {
    return ProfileValidationError::kMissingGmTrust;
  }
  if (effective_policy_revision_ == 0 &&
      (protocol_ != TlsProtocol::kStandardTls || !trust_profile_id_.empty() ||
       !client_identity_policy_id_.empty())) {
    return ProfileValidationError::kMissingPolicyRevision;
  }
  return ProfileValidationError::kNone;
}

bool ConnectionProfile::IsValid() const {
  return Validate() == ProfileValidationError::kNone;
}

bool ConnectionProfile::AllowsHttp11() const {
  return IsValid();
}

bool ConnectionProfile::AllowsHttp2() const {
  return IsValid() && protocol_ != TlsProtocol::kTlcp;
}

bool ConnectionProfile::AllowsQuic() const {
  // A configured client identity must not be bypassed through a QUIC path
  // that does not implement this identity policy.
  return IsValid() && protocol_ == TlsProtocol::kStandardTls &&
         client_identity_policy_id_.empty();
}

bool ConnectionProfile::AllowsCrossOriginHttp2() const {
  return AllowsHttp2() && protocol_ != TlsProtocol::kTls13Auto &&
         client_identity_policy_id_.empty();
}

}  // namespace net::gm
