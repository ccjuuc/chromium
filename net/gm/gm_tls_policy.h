// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_GM_GM_TLS_POLICY_H_
#define NET_GM_GM_TLS_POLICY_H_

#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/containers/flat_set.h"
#include "net/base/host_port_pair.h"
#include "net/base/net_export.h"
#include "net/gm/gm_connection_profile.h"

namespace net {
struct SSLConfig;
namespace gm {

// Immutable after publication through TlsPolicy. Key identifiers and
// generations belong to one NetworkContext; no device path, PIN or private key
// is accepted.
struct NET_EXPORT TlsRule {
  TlsRule();
  TlsRule(const TlsRule&);
  TlsRule(TlsRule&&);
  TlsRule& operator=(const TlsRule&);
  TlsRule& operator=(TlsRule&&);
  ~TlsRule();

  std::string host;
  uint16_t port = 443;
  ConnectionProfile profile;
  std::vector<std::vector<uint8_t>> trust_anchors;
  // System CA-store entries are issuer candidates, never trust anchors.
  std::vector<std::vector<uint8_t>> untrusted_intermediates;
  // Reject any matching certificate in either verified chain.
  std::vector<std::vector<uint8_t>> disallowed_certificates;
  std::vector<std::vector<uint8_t>> crls;
  // Automatic discovery uses a system-trust snapshot and permits an empty
  // root set. The resulting handshake must still fail certificate validation.
  bool automatic = false;
  // 0: disabled by explicit policy; 1: leaf; 2: full chain.
  uint32_t revocation_mode = 0;
  bool enable_session_resumption = true;

  bool IsValid() const;
  bool operator==(const TlsRule&) const = default;
};

// Immutable, NetworkContext-local input to automatic protocol discovery. It
// does not load a backend or enumerate client keys on the ordinary TLS path.
struct NET_EXPORT AutoConfig {
  AutoConfig();
  AutoConfig(const AutoConfig&);
  AutoConfig(AutoConfig&&);
  AutoConfig& operator=(const AutoConfig&);
  AutoConfig& operator=(AutoConfig&&);
  ~AutoConfig();

  bool enabled = false;
  std::vector<std::vector<uint8_t>> trust_anchors;
  std::vector<std::vector<uint8_t>> untrusted_intermediates;
  std::vector<std::vector<uint8_t>> disallowed_certificates;
  std::vector<std::vector<uint8_t>> crls;
  uint64_t epoch = 0;

  bool IsValid() const;
  bool operator==(const AutoConfig&) const = default;
  std::shared_ptr<const AutoConfig> WithEpoch(uint64_t value) const;
  // kTls13Auto is only an entry policy. The routing layer may select it after
  // a destination has successfully authenticated with RFC 8998; this factory
  // neither discovers capability nor relaxes the stored trust snapshot.
  std::shared_ptr<const TlsRule> CreateRule(std::string_view scheme,
                                            std::string_view host,
                                            uint16_t port,
                                            TlsProtocol protocol) const;
  // Compare an existing route without copying the certificate/CRL snapshot.
  bool MatchesRule(std::string_view scheme,
                   std::string_view host,
                   uint16_t port,
                   const TlsRule& rule) const;
};

class NET_EXPORT TlsPolicy {
 public:
  explicit TlsPolicy(std::vector<TlsRule> rules);
  ~TlsPolicy();
  TlsPolicy(const TlsPolicy&) = delete;
  TlsPolicy& operator=(const TlsPolicy&) = delete;

  // Bad configuration is never converted to "no policy". A policy containing
  // malformed/duplicate rules returns an invalid GM rule for all secure
  // origins, so the socket rejects the request instead of silently using
  // ordinary TLS.
  bool IsValid() const { return valid_; }
  std::shared_ptr<const TlsRule> Match(std::string_view scheme,
                                       std::string_view host,
                                       uint16_t port) const;
  bool operator==(const TlsPolicy& other) const;

  // Includes the optional terminal-dot DNS spelling accepted by Match().
  // Invalid policies affect all secure origins; callers must invalidate all
  // pools instead of using this finite set when IsValid() is false.
  base::flat_set<HostPortPair> GetServers() const;

  // NetworkContext-local epochs are assigned at the configuration boundary.
  // Never reuse an epoch, even after removing and re-adding the same policy.
  // This prevents an administrator reusing supplied generations/revisions from
  // reviving connections or resumptions created under old trust settings.
  std::shared_ptr<const TlsPolicy> WithPolicyEpoch(uint64_t epoch) const;

 private:
  bool valid_ = true;
  std::map<std::pair<std::string, uint16_t>, std::shared_ptr<const TlsRule>>
      rules_;
  std::shared_ptr<const TlsRule> invalid_rule_;
};

NET_EXPORT bool TlsRulesEqual(const std::shared_ptr<const TlsRule>& first,
                              const std::shared_ptr<const TlsRule>& second);

NET_EXPORT const ConnectionProfile& ProfileForRule(
    const std::shared_ptr<const TlsRule>& rule);

// Applies the rule selected before looking up connection pools. The caller must
// preserve the same rule/profile through the connection attempt. Proxy TLS is
// configured separately and must never inherit the final destination's rule.
NET_EXPORT void ApplyTlsRule(std::shared_ptr<const TlsRule> rule,
                             SSLConfig* config);

}  // namespace gm
}  // namespace net

#endif  // NET_GM_GM_TLS_POLICY_H_
