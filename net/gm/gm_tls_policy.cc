// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_tls_policy.h"

#include <algorithm>
#include <utility>

#include "base/no_destructor.h"
#include "net/base/url_util.h"
#include "net/socket/next_proto.h"
#include "net/ssl/ssl_config.h"
#include "url/scheme_host_port.h"
#include "url/url_canon.h"

namespace net::gm {
namespace {

std::string NormalizeHost(std::string_view input) {
  url::CanonHostInfo info;
  std::string host = input.starts_with('[')
                         ? CanonicalizeHost(input, &info)
                         : CanonicalizeHostSupportsBareIPV6(input, &info);
  if (info.family == url::CanonHostInfo::BROKEN || host.empty() ||
      (!info.IsIPAddress() && !IsCanonicalizedHostCompliant(host))) {
    return {};
  }
  if (host.back() == '.') {
    host.pop_back();
  }
  return host;
}

}  // namespace

TlsRule::TlsRule() = default;
TlsRule::TlsRule(const TlsRule&) = default;
TlsRule::TlsRule(TlsRule&&) = default;
TlsRule& TlsRule::operator=(const TlsRule&) = default;
TlsRule& TlsRule::operator=(TlsRule&&) = default;
TlsRule::~TlsRule() = default;

bool TlsRule::IsValid() const {
  if (host.empty() || !port || !profile.IsValid() ||
      profile.protocol() == TlsProtocol::kStandardTls ||
      (!automatic && trust_anchors.empty()) ||
      trust_anchors.size() > (automatic ? 4096u : 128u) ||
      untrusted_intermediates.size() > 4096 ||
      disallowed_certificates.size() > 4096 || crls.size() > 128 ||
      revocation_mode > 2 || (revocation_mode && crls.empty())) {
    return false;
  }
  const auto valid_blob = [](const auto& blob) {
    return !blob.empty() && blob.size() <= 256 * 1024;
  };
  return std::ranges::all_of(trust_anchors, valid_blob) &&
         std::ranges::all_of(untrusted_intermediates, valid_blob) &&
         std::ranges::all_of(disallowed_certificates, valid_blob) &&
         std::ranges::all_of(crls, valid_blob);
}

AutoConfig::AutoConfig() = default;
AutoConfig::AutoConfig(const AutoConfig&) = default;
AutoConfig::AutoConfig(AutoConfig&&) = default;
AutoConfig& AutoConfig::operator=(const AutoConfig&) = default;
AutoConfig& AutoConfig::operator=(AutoConfig&&) = default;
AutoConfig::~AutoConfig() = default;

bool AutoConfig::IsValid() const {
  if (!epoch || trust_anchors.size() > 4096 ||
      untrusted_intermediates.size() > 4096 ||
      disallowed_certificates.size() > 4096 || crls.size() > 128) {
    return false;
  }
  size_t bytes = 0;
  for (const auto* list : {&trust_anchors, &untrusted_intermediates,
                           &disallowed_certificates, &crls}) {
    for (const auto& blob : *list) {
      if (blob.empty() || blob.size() > 256 * 1024) {
        return false;
      }
      bytes += blob.size();
      if (bytes > 64 * 1024 * 1024) {
        return false;
      }
    }
  }
  return true;
}

std::shared_ptr<const AutoConfig> AutoConfig::WithEpoch(uint64_t value) const {
  auto copy = std::make_shared<AutoConfig>(*this);
  copy->epoch = value;
  return copy;
}

std::shared_ptr<const TlsRule> AutoConfig::CreateRule(
    std::string_view scheme,
    std::string_view host,
    uint16_t port,
    TlsProtocol protocol) const {
  if (!enabled || !IsValid() || (scheme != "https" && scheme != "wss") ||
      !port ||
      (protocol != TlsProtocol::kTlcp && protocol != TlsProtocol::kRfc8998 &&
       protocol != TlsProtocol::kTls13Auto)) {
    return nullptr;
  }
  auto rule = std::make_shared<TlsRule>();
  rule->host = NormalizeHost(host);
  if (rule->host.empty()) {
    return nullptr;
  }
  rule->port = port;
  rule->automatic = true;
  rule->profile =
      ConnectionProfile(protocol, "windows-system", epoch, {}, 0, epoch);
  rule->trust_anchors = trust_anchors;
  rule->untrusted_intermediates = untrusted_intermediates;
  rule->disallowed_certificates = disallowed_certificates;
  rule->crls = crls;
  return rule;
}

bool AutoConfig::MatchesRule(std::string_view scheme,
                             std::string_view host,
                             uint16_t port,
                             const TlsRule& rule) const {
  if (!enabled || !IsValid() || (scheme != "https" && scheme != "wss") ||
      !rule.automatic || !rule.IsValid() || rule.port != port ||
      rule.host != NormalizeHost(host)) {
    return false;
  }
  const auto& profile = rule.profile;
  return profile.protocol() != TlsProtocol::kStandardTls &&
         (profile.protocol() == TlsProtocol::kTlcp ||
          profile.protocol() == TlsProtocol::kRfc8998 ||
          profile.protocol() == TlsProtocol::kTls13Auto) &&
         profile.trust_profile_id() == "windows-system" &&
         profile.trust_generation() == epoch &&
         profile.client_identity_policy_id().empty() &&
         profile.identity_generation() == 0 &&
         profile.effective_policy_revision() == epoch &&
         rule.trust_anchors == trust_anchors &&
         rule.untrusted_intermediates == untrusted_intermediates &&
         rule.disallowed_certificates == disallowed_certificates &&
         rule.crls == crls && rule.revocation_mode == 0 &&
         rule.enable_session_resumption;
}

TlsPolicy::TlsPolicy(std::vector<TlsRule> rules) {
  TlsRule invalid;
  invalid.profile = ConnectionProfile(TlsProtocol::kTlcp, {}, 0, {}, 0, 0);
  invalid_rule_ = std::make_shared<const TlsRule>(std::move(invalid));
  if (rules.size() > 256) {
    valid_ = false;
    return;
  }
  size_t bytes = 0;
  for (auto& rule : rules) {
    rule.host = NormalizeHost(rule.host);
    if (rule.automatic || !rule.IsValid()) {
      valid_ = false;
      return;
    }
    for (const auto* list : {&rule.trust_anchors, &rule.untrusted_intermediates,
                             &rule.disallowed_certificates, &rule.crls}) {
      for (const auto& blob : *list) {
        bytes += blob.size();
      }
    }
    if (bytes > 16 * 1024 * 1024) {
      valid_ = false;
      return;
    }
    const auto key = std::make_pair(rule.host, rule.port);
    if (!rules_.emplace(key, std::make_shared<const TlsRule>(std::move(rule)))
             .second) {
      valid_ = false;
      return;
    }
  }
}

TlsPolicy::~TlsPolicy() = default;

std::shared_ptr<const TlsRule> TlsPolicy::Match(std::string_view scheme,
                                                std::string_view host,
                                                uint16_t port) const {
  if (scheme != "https" && scheme != "wss") {
    return nullptr;
  }
  if (!valid_) {
    return invalid_rule_;
  }
  auto entry = rules_.find({NormalizeHost(host), port});
  return entry == rules_.end() ? nullptr : entry->second;
}

bool TlsPolicy::operator==(const TlsPolicy& other) const {
  if (valid_ != other.valid_ || rules_.size() != other.rules_.size()) {
    return false;
  }
  auto first = rules_.begin();
  auto second = other.rules_.begin();
  for (; first != rules_.end(); ++first, ++second) {
    if (first->first != second->first || *first->second != *second->second) {
      return false;
    }
  }
  return true;
}

base::flat_set<HostPortPair> TlsPolicy::GetServers() const {
  base::flat_set<HostPortPair> servers;
  for (const auto& [key, rule] : rules_) {
    servers.insert(HostPortPair::FromSchemeHostPort(
        url::SchemeHostPort("https", rule->host, rule->port)));
    url::CanonHostInfo host_info;
    CanonicalizeHost(rule->host, &host_info);
    if (!host_info.IsIPAddress()) {
      servers.insert(HostPortPair(rule->host + ".", rule->port));
    }
  }
  return servers;
}

std::shared_ptr<const TlsPolicy> TlsPolicy::WithPolicyEpoch(
    uint64_t epoch) const {
  if (!valid_ || !epoch) {
    return std::make_shared<const TlsPolicy>(std::vector<TlsRule>{TlsRule()});
  }
  std::vector<TlsRule> rules;
  rules.reserve(rules_.size());
  for (const auto& [key, value] : rules_) {
    TlsRule rule = *value;
    const auto& p = rule.profile;
    rule.profile = ConnectionProfile(
        p.protocol(), p.trust_profile_id(), p.trust_generation(),
        p.client_identity_policy_id(), p.identity_generation(), epoch);
    rules.push_back(std::move(rule));
  }
  return std::make_shared<const TlsPolicy>(std::move(rules));
}

bool TlsRulesEqual(const std::shared_ptr<const TlsRule>& first,
                   const std::shared_ptr<const TlsRule>& second) {
  return first == second || (first && second && *first == *second);
}

const ConnectionProfile& ProfileForRule(
    const std::shared_ptr<const TlsRule>& rule) {
  static const base::NoDestructor<ConnectionProfile> standard;
  return rule ? rule->profile : *standard;
}

void ApplyTlsRule(std::shared_ptr<const TlsRule> rule, SSLConfig* config) {
  config->gm_rule = std::move(rule);
  if (!config->gm_rule) {
    return;
  }
  config->early_data_enabled = false;
  config->renego_allowed_default = false;
  config->renego_allowed_for_protos.clear();
  if (config->gm_rule->profile.protocol() == TlsProtocol::kTls13Auto) {
    // The joint socket retains ordinary TLS settings and evaluates them only
    // for standard suites. Its RFC 8998 branch never accepts bad-cert exceptions.
    return;
  }
  config->ignore_certificate_errors = false;
  config->allowed_bad_certs.clear();
  config->ech_config_list.clear();
  config->trust_anchor_ids.reset();
  config->server_padding_to_request.reset();
  config->application_settings.clear();
  config->renego_allowed_default = false;
  config->renego_allowed_for_protos.clear();
  if (!config->gm_rule->profile.AllowsHttp2()) {
    config->alpn_protos = {NextProto::kProtoHTTP11};
  }
}

}  // namespace net::gm
