// Copyright 2018 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/ssl_config_type_converter.h"

#include <functional>
#include <optional>

#include "base/check_op.h"
#include "base/notreached.h"
#include "net/gm/gm_buildflags.h"
#include "net/gm/gm_tls_policy.h"
#include "net/ssl/ssl_config_service.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace mojo {
#if BUILDFLAG(XENON_GM)
namespace {

net::gm::TlsProtocol ConvertGmTlsProtocol(
    network::mojom::GmTlsProtocol protocol) {
  switch (protocol) {
    case network::mojom::GmTlsProtocol::kTlcp:
      return net::gm::TlsProtocol::kTlcp;
    case network::mojom::GmTlsProtocol::kRfc8998:
      return net::gm::TlsProtocol::kRfc8998;
    case network::mojom::GmTlsProtocol::kTls13Auto:
      return net::gm::TlsProtocol::kTls13Auto;
  }
  // Mojo rejects unknown wire values. Also fail closed when an in-process
  // caller supplies an invalid enum, instead of reinterpreting it as RFC 8998.
  return static_cast<net::gm::TlsProtocol>(-1);
}

}  // namespace

#endif  // BUILDFLAG(XENON_GM)
int MojoSSLVersionToNetSSLVersion(network::mojom::SSLVersion mojo_version) {
  switch (mojo_version) {
    case network::mojom::SSLVersion::kTLS12:
      return net::SSL_PROTOCOL_VERSION_TLS1_2;
    case network::mojom::SSLVersion::kTLS13:
      return net::SSL_PROTOCOL_VERSION_TLS1_3;
  }
  NOTREACHED();
}

net::SSLContextConfig MojoSSLConfigToSSLContextConfig(
    const network::mojom::SSLConfigPtr& mojo_config) {
  net::SSLContextConfig net_config;

#if BUILDFLAG(XENON_GM)
  if (mojo_config->gm_auto_config) {
    auto automatic = std::make_shared<net::gm::AutoConfig>();
    const auto& source = *mojo_config->gm_auto_config;
    automatic->enabled = source.enabled;
    automatic->trust_anchors = source.trust_anchors;
    automatic->untrusted_intermediates = source.untrusted_intermediates;
    automatic->disallowed_certificates = source.disallowed_certificates;
    automatic->crls = source.crls;
    automatic->epoch = source.epoch;
    net_config.gm_auto_config = std::move(automatic);
  }

  if (!mojo_config->gm_tls_rules.empty()) {
    std::vector<net::gm::TlsRule> rules;
    for (const auto& source : mojo_config->gm_tls_rules) {
      net::gm::TlsRule rule;
      rule.host = source->host;
      rule.port = source->port;
      rule.profile = net::gm::ConnectionProfile(
          ConvertGmTlsProtocol(source->protocol), source->trust_profile_id,
          source->trust_generation, source->identity_policy_id,
          source->identity_generation, source->policy_revision);
      rule.trust_anchors = source->trust_anchors;
      rule.crls = source->crls;
      rule.revocation_mode = source->revocation_mode;
      rule.enable_session_resumption = source->enable_session_resumption;
      rules.push_back(std::move(rule));
    }
    net_config.gm_tls_policy =
        std::make_shared<const net::gm::TlsPolicy>(std::move(rules));
  }

#endif  // BUILDFLAG(XENON_GM)
  net_config.version_min =
      MojoSSLVersionToNetSSLVersion(mojo_config->version_min);
  net_config.version_max =
      MojoSSLVersionToNetSSLVersion(mojo_config->version_max);
  DCHECK_LE(net_config.version_min, net_config.version_max);

  net_config.disabled_cipher_suites = mojo_config->disabled_cipher_suites;
  net_config.tls13_cipher_prefer_aes_256 =
      mojo_config->tls13_cipher_prefer_aes_256;
  net_config.ech_enabled = mojo_config->ech_enabled;

  // Translate the configuration options related to named groups.
  switch (mojo_config->named_groups_preset) {
    case network::mojom::SSLNamedGroupsPreset::kDefault:
      // Do nothing, the `net::SSLContextConfig` constructor starts with the
      // default list.
      break;
    case network::mojom::SSLNamedGroupsPreset::kCnsa2:
      net_config.supported_named_groups = {
          {.group_id = SSL_GROUP_MLKEM1024, .send_key_share = false},
          {.group_id = SSL_GROUP_X25519_MLKEM768, .send_key_share = true},
          {.group_id = SSL_GROUP_SECP384R1, .send_key_share = false},
          {.group_id = SSL_GROUP_SECP256R1, .send_key_share = false},
          {.group_id = SSL_GROUP_X25519, .send_key_share = true},
      };
      break;
  }

  for (const auto& tai : mojo_config->trust_anchor_ids) {
    net_config.trust_anchor_ids.insert(tai);
  }

  for (const auto& tai : mojo_config->mtc_trust_anchor_ids) {
    net_config.mtc_trust_anchor_ids.push_back(tai);
  }
  net_config.mtc_update_time_seconds = mojo_config->mtc_update_time_seconds;

  return net_config;
}

net::CertVerifier::Config MojoSSLConfigToCertVerifierConfig(
    const network::mojom::SSLConfigPtr& mojo_config) {
  net::CertVerifier::Config net_config;
  net_config.enable_rev_checking = mojo_config->rev_checking_enabled;
  net_config.require_rev_checking_local_anchors =
      mojo_config->rev_checking_required_local_anchors;

  return net_config;
}

}  // namespace mojo
