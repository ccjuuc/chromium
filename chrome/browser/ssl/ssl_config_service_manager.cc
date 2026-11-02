// Copyright 2012 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "chrome/browser/ssl/ssl_config_service_manager.h"

#include <stdint.h>

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "base/check.h"
#include "base/containers/to_vector.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/run_loop.h"
#include "base/strings/string_util.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/pref_names.h"
#include "components/content_settings/core/browser/content_settings_utils.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/prefs/pref_change_registrar.h"
#include "components/prefs/pref_member.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "mojo/public/cpp/bindings/remote_set.h"
#include "net/base/features.h"
#include "net/cert/cert_verifier.h"
#include "net/gm/gm_buildflags.h"
#include "net/ssl/ssl_cipher_suite_names.h"
#include "net/ssl/ssl_config_service.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "services/network/public/mojom/ssl_config.mojom.h"
#include "url/url_canon.h"

#if BUILDFLAG(XENON_GM)
#include "chrome/browser/gm/gm_policy.h"  // nogncheck: XENON_GM conditional dependency.
#endif

#if BUILDFLAG(XENON_GM) && BUILDFLAG(IS_WIN) && defined(ARCH_CPU_X86_64)
#include "chrome/browser/gm/gm_system_trust_win.h"  // nogncheck: XENON_GM conditional dependency.
#endif

#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
#include "net/cert/internal/trust_store_chrome.h"
#endif

namespace {

// Pref value identifying the compliance regime specified by the Commercial
// National Security Algorithm Suite 2.0 (CNSA 2.0).
const char kPrefStringValueCnsa2[] = "cnsa2";

// Pref value identifying the compliance regime specified by the Commercial
// National Security Algorithm Suite versions 1.0 and 2.0 (CNSA 1.0 and 2.0).
const char kPrefStringValueCnsa[] = "cnsa";

// Converts a `base::ListValue` of StringValues into a vector of strings. Any
// values which cannot be converted will be skipped.
std::vector<std::string> ValueListToStringVector(const base::ListValue& list) {
  std::vector<std::string> results;
  results.reserve(list.size());
  for (const auto& entry : list) {
    const std::string* s = entry.GetIfString();
    if (s) {
      results.push_back(*s);
    }
  }
  return results;
}

// Parses a vector of cipher suite strings, returning a sorted vector
// containing the underlying SSL/TLS cipher suites. Unrecognized/invalid
// cipher suites will be ignored.
std::vector<uint16_t> ParseCipherSuites(
    const std::vector<std::string>& cipher_strings) {
  std::vector<uint16_t> cipher_suites;
  cipher_suites.reserve(cipher_strings.size());

  for (auto it = cipher_strings.begin(); it != cipher_strings.end(); ++it) {
    uint16_t cipher_suite = 0;
    if (!net::ParseSSLCipherString(*it, &cipher_suite)) {
      LOG(ERROR) << "Ignoring unrecognized or unparsable cipher suite: " << *it;
      continue;
    }
    cipher_suites.push_back(cipher_suite);
  }
  std::sort(cipher_suites.begin(), cipher_suites.end());
  return cipher_suites;
}

// Writes the SSL protocol version represented by a string to |version|. Returns
// false if the string is not recognized.
bool SSLProtocolVersionFromString(const std::string& version_str,
                                  network::mojom::SSLVersion* version) {
  if (version_str == switches::kSSLVersionTLSv12) {
    *version = network::mojom::SSLVersion::kTLS12;
    return true;
  }
  if (version_str == switches::kSSLVersionTLSv13) {
    *version = network::mojom::SSLVersion::kTLS13;
    return true;
  }
  return false;
}

// Given a vector of hostname patterns |patterns|, returns a vector containing
// the canonical form. Any entries which cannot be parsed are skipped.
std::vector<std::string> CanonicalizeHostnamePatterns(
    const std::vector<std::string>& patterns) {
  std::vector<std::string> out;
  out.reserve(patterns.size());
  for (std::string_view pattern : patterns) {
    std::string canon_pattern;
    url::Component canon_component;
    url::StdStringCanonOutput canon_output(&canon_pattern);
    if (!url::CanonicalizeHost(pattern, url::Component(0, pattern.size()),
                               &canon_output, &canon_component)) {
      continue;
    }
    canon_output.Complete();
    out.push_back(canon_pattern);
  }
  return out;
}

}  // namespace

SSLConfigServiceManager::SSLConfigServiceManager(
    PrefService* local_state,
    GmTrustSnapshotLoader gm_trust_loader_for_testing,
    GmTrustMonitorFactory gm_trust_monitor_for_testing)
#if BUILDFLAG(XENON_GM)
    : gm_trust_loader_(std::move(gm_trust_loader_for_testing))
#endif
{
  DCHECK(local_state);

  PrefChangeRegistrar::NamedChangeCallback local_state_callback =
      base::BindRepeating(&SSLConfigServiceManager::OnPreferenceChanged,
                          base::Unretained(this), local_state);

  rev_checking_enabled_.Init(prefs::kCertRevocationCheckingEnabled, local_state,
                             local_state_callback);
  rev_checking_required_local_anchors_.Init(
      prefs::kCertRevocationCheckingRequiredLocalAnchors, local_state,
      local_state_callback);
  ssl_version_min_.Init(prefs::kSSLVersionMin, local_state,
                        local_state_callback);
  ssl_version_max_.Init(prefs::kSSLVersionMax, local_state,
                        local_state_callback);
  h2_client_cert_coalescing_host_patterns_.Init(
      prefs::kH2ClientCertCoalescingHosts, local_state, local_state_callback);
  ech_enabled_.Init(prefs::kEncryptedClientHelloEnabled, local_state,
                    local_state_callback);
  key_exchange_compliance_.Init(prefs::kPreferSlowKexAlgorithms, local_state,
                                local_state_callback);
  tls13_cipher_compliance_.Init(prefs::kPreferSlowCiphers, local_state,
                                local_state_callback);

  local_state_change_registrar_.Init(local_state);
  local_state_change_registrar_.Add(prefs::kCipherSuiteBlacklist,
                                    local_state_callback);
#if BUILDFLAG(XENON_GM)
  local_state_change_registrar_.Add(gm_browser::kSiteRulesPref,
                                    local_state_callback);
#endif

  // Populate |disabled_cipher_suites_| with the initial pref value.
  OnDisabledCipherSuitesChange(local_state);

#if BUILDFLAG(XENON_GM)
#if BUILDFLAG(IS_WIN) && defined(ARCH_CPU_X86_64)
  if (!gm_trust_loader_) {
    gm_trust_loader_ = base::BindRepeating(&gm_browser::ReadGmSystemTrust);
    gm_trust_monitor_for_testing =
        base::BindRepeating(&gm_browser::StartGmSystemTrustMonitoring);
  }
#endif
  if (gm_trust_loader_) {
    gm_auto_config_ = network::mojom::GmAutoConfig::New();
    gm_auto_config_->enabled = true;
    gm_auto_config_->epoch = ++gm_trust_epoch_;
    net::CertDatabase::GetInstance()->AddObserver(this);
    if (gm_trust_monitor_for_testing) {
      gm_trust_monitor_starting_ = true;
      gm_trust_monitor_stop_.ReplaceClosure(
          gm_trust_monitor_for_testing.Run(base::BindRepeating(
              &SSLConfigServiceManager::OnGmTrustMonitoringChanged,
              weak_factory_.GetWeakPtr())));
    } else {
      StartGmTrustSnapshot();
    }
  }

#endif  // BUILDFLAG(XENON_GM)

#if BUILDFLAG(CHROME_ROOT_STORE_SUPPORTED)
  // Populate initial trust anchor ID state from compiled-in values.
  //
  // Theoretically there should be some check here for whether Chrome Root
  // Store is enabled, on builds where it is optional. That is currently
  // configured through the GetCertVerifierServiceFactory
  // SetUseChromeRootStore() call, so the status is not easily accessible from
  // here. Currently the only platform where CRS is optional and disabled is
  // android webview, which doesn't use this file, so this is okay.
  trust_anchor_ids_ =
      net::TrustStoreChrome::GetTrustAnchorIDsFromCompiledInRootStore();
  if (base::FeatureList::IsEnabled(net::features::kVerifyMTCs)) {
    mtc_trust_anchor_ids_ =
        net::TrustStoreChrome::GetTrustedMtcCaIDsFromCompiledInRootStore();
  }
#endif
}

SSLConfigServiceManager::~SSLConfigServiceManager() {
#if BUILDFLAG(XENON_GM)
  if (gm_auto_config_) {
    net::CertDatabase::GetInstance()->RemoveObserver(this);
  }
#endif
}

// static
void SSLConfigServiceManager::RegisterPrefs(PrefRegistrySimple* registry) {
#if BUILDFLAG(XENON_GM)
  gm_browser::RegisterPrefs(registry);
#endif
  net::CertVerifier::Config default_verifier_config;
  registry->RegisterBooleanPref(prefs::kCertRevocationCheckingEnabled,
                                default_verifier_config.enable_rev_checking);
  registry->RegisterBooleanPref(
      prefs::kCertRevocationCheckingRequiredLocalAnchors,
      default_verifier_config.require_rev_checking_local_anchors);
  net::SSLContextConfig default_context_config;
  registry->RegisterStringPref(prefs::kSSLVersionMin, std::string());
  registry->RegisterStringPref(prefs::kSSLVersionMax, std::string());
  registry->RegisterListPref(prefs::kCipherSuiteBlacklist);
  registry->RegisterListPref(prefs::kH2ClientCertCoalescingHosts);
  registry->RegisterBooleanPref(prefs::kEncryptedClientHelloEnabled,
                                default_context_config.ech_enabled);
  // The following two prefs for SSL compliance policies are used here as
  // local_state prefs, but the same pref names are also used as Profile prefs
  // in certain Profiles. Their value is only used if managed.
  registry->RegisterStringPref(prefs::kPreferSlowKexAlgorithms, std::string());
  registry->RegisterStringPref(prefs::kPreferSlowCiphers, std::string());
}

void SSLConfigServiceManager::AddToNetworkContextParams(
    network::mojom::NetworkContextParams* network_context_params) {
  network_context_params->initial_ssl_config = GetNewSSLConfig();
  mojo::Remote<network::mojom::SSLConfigClient> ssl_config_client;
  network_context_params->ssl_config_client_receiver =
      ssl_config_client.BindNewPipeAndPassReceiver();
  ssl_config_client_set_.Add(std::move(ssl_config_client));
}

void SSLConfigServiceManager::UpdateTrustAnchorIDs(
    std::vector<std::vector<uint8_t>> trust_anchor_ids,
    std::vector<std::vector<uint8_t>> mtc_trust_anchor_ids,
    int64_t mtc_update_time_seconds) {
  trust_anchor_ids_ = std::move(trust_anchor_ids);
  mtc_trust_anchor_ids_ = std::move(mtc_trust_anchor_ids);
  mtc_update_time_seconds_ = mtc_update_time_seconds;
  network::mojom::SSLConfigPtr new_config = GetNewSSLConfig();
  network::mojom::SSLConfig* raw_config = new_config.get();

  for (const auto& client : ssl_config_client_set_) {
    // Mojo calls consume all InterfacePtrs passed to them, so have to
    // clone the config for each call.
    client->OnSSLConfigUpdated(raw_config->Clone());
  }
}

void SSLConfigServiceManager::FlushForTesting() {
#if BUILDFLAG(XENON_GM)
  if (gm_trust_monitor_starting_ || gm_trust_load_in_progress_) {
    CHECK(!gm_trust_flushed_for_testing_);
    base::RunLoop loop;
    gm_trust_flushed_for_testing_ = loop.QuitClosure();
    loop.Run();
  }
#endif
  ssl_config_client_set_.FlushForTesting();
}

void SSLConfigServiceManager::OnPreferenceChanged(
    PrefService* prefs,
    const std::string& pref_name_in) {
  DCHECK(prefs);
  if (pref_name_in == prefs::kCipherSuiteBlacklist) {
    OnDisabledCipherSuitesChange(prefs);
  }

  network::mojom::SSLConfigPtr new_config = GetNewSSLConfig();
  network::mojom::SSLConfig* raw_config = new_config.get();

  for (const auto& client : ssl_config_client_set_) {
    // Mojo calls consume all InterfacePtrs passed to them, so have to
    // clone the config for each call.
    client->OnSSLConfigUpdated(raw_config->Clone());
  }
}

network::mojom::SSLConfigPtr SSLConfigServiceManager::GetNewSSLConfig() const {
  network::mojom::SSLConfigPtr config = network::mojom::SSLConfig::New();
#if BUILDFLAG(XENON_GM)
  config->gm_auto_config = gm_auto_config_.Clone();
  config->gm_tls_rules =
      gm_browser::ReadPolicy(local_state_change_registrar_.prefs())
          .ToNetworkRules();
#endif

  // rev_checking_enabled was formerly a user-settable preference, but now
  // it is managed-only.
  if (rev_checking_enabled_.IsManaged()) {
    config->rev_checking_enabled = rev_checking_enabled_.GetValue();
  } else {
    config->rev_checking_enabled = false;
  }
  config->rev_checking_required_local_anchors =
      rev_checking_required_local_anchors_.GetValue();
  std::string version_min_str = ssl_version_min_.GetValue();
  std::string version_max_str = ssl_version_max_.GetValue();

  network::mojom::SSLVersion version_min;
  if (SSLProtocolVersionFromString(version_min_str, &version_min)) {
    config->version_min = version_min;
  }

  network::mojom::SSLVersion version_max;
  if (SSLProtocolVersionFromString(version_max_str, &version_max)) {
    config->version_max = version_max;
  }

  config->disabled_cipher_suites = disabled_cipher_suites_;
  config->client_cert_pooling_policy = CanonicalizeHostnamePatterns(
      h2_client_cert_coalescing_host_patterns_.GetValue());

  config->ech_enabled = ech_enabled_.GetValue();

  config->trust_anchor_ids = trust_anchor_ids_;
  config->mtc_trust_anchor_ids = mtc_trust_anchor_ids_;
  config->mtc_update_time_seconds = mtc_update_time_seconds_;

  ConfigureSSLComplianceSettings(key_exchange_compliance_,
                                 tls13_cipher_compliance_, config.get());

  return config;
}

void SSLConfigServiceManager::OnTrustStoreChanged() {
#if BUILDFLAG(XENON_GM)
  if (!gm_auto_config_) {
    return;
  }
  // Publish a fresh epoch immediately, invalidating contexts, sessions, pools
  // and discovery hints before doing any asynchronous store enumeration.
  if (gm_trust_epoch_ != std::numeric_limits<uint64_t>::max()) {
    ++gm_trust_epoch_;
  }
  gm_auto_config_ = network::mojom::GmAutoConfig::New();
  gm_auto_config_->enabled =
      !gm_trust_monitor_failed_ &&
      gm_trust_epoch_ != std::numeric_limits<uint64_t>::max();
  gm_auto_config_->epoch = gm_trust_epoch_;
  SendUpdatedSSLConfig();
  if (!gm_auto_config_->enabled || gm_trust_monitor_starting_) {
    return;
  }
  if (gm_trust_load_in_progress_) {
    gm_trust_refresh_pending_ = true;
    return;
  }
  StartGmTrustSnapshot();
#endif
}

#if BUILDFLAG(XENON_GM)
void SSLConfigServiceManager::OnGmTrustMonitoringChanged(bool active) {
  gm_trust_monitor_starting_ = false;
  gm_trust_monitor_failed_ = !active;
  OnTrustStoreChanged();
  if (!gm_trust_load_in_progress_ && gm_trust_flushed_for_testing_) {
    std::move(gm_trust_flushed_for_testing_).Run();
  }
}

void SSLConfigServiceManager::StartGmTrustSnapshot() {
  gm_trust_load_in_progress_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::OnceCallback<network::mojom::GmAutoConfigPtr()>(gm_trust_loader_),
      base::BindOnce(&SSLConfigServiceManager::OnGmTrustSnapshot,
                     weak_factory_.GetWeakPtr()));
}

void SSLConfigServiceManager::OnGmTrustSnapshot(
    network::mojom::GmAutoConfigPtr snapshot) {
  gm_trust_load_in_progress_ = false;
  if (gm_trust_monitor_failed_) {
    gm_trust_refresh_pending_ = false;
    if (gm_trust_flushed_for_testing_) {
      std::move(gm_trust_flushed_for_testing_).Run();
    }
    return;
  }
  if (gm_trust_epoch_ == std::numeric_limits<uint64_t>::max()) {
    gm_trust_refresh_pending_ = false;
    if (gm_trust_flushed_for_testing_) {
      std::move(gm_trust_flushed_for_testing_).Run();
    }
    return;
  }
  if (gm_trust_refresh_pending_) {
    gm_trust_refresh_pending_ = false;
    StartGmTrustSnapshot();
    return;
  }
  if (!snapshot) {
    snapshot = network::mojom::GmAutoConfig::New();
    snapshot->enabled = true;
  }
  snapshot->epoch = gm_trust_epoch_;
  gm_auto_config_ = std::move(snapshot);
  SendUpdatedSSLConfig();
  if (gm_trust_flushed_for_testing_) {
    std::move(gm_trust_flushed_for_testing_).Run();
  }
}

void SSLConfigServiceManager::SendUpdatedSSLConfig() {
  auto config = GetNewSSLConfig();
  for (const auto& client : ssl_config_client_set_) {
    client->OnSSLConfigUpdated(config.Clone());
  }
}

#endif  // BUILDFLAG(XENON_GM)

void SSLConfigServiceManager::OnDisabledCipherSuitesChange(
    PrefService* local_state) {
  const base::ListValue& list =
      local_state->GetList(prefs::kCipherSuiteBlacklist);
  disabled_cipher_suites_ = ParseCipherSuites(ValueListToStringVector(list));
}

// static
void SSLConfigServiceManager::ConfigureSSLComplianceSettings(
    const StringPrefMember& key_exchange_compliance_pref,
    const StringPrefMember& tls13_cipher_compliance_pref,
    network::mojom::SSLConfig* config) {
  if (base::FeatureList::IsEnabled(features::kCryptographyComplianceCnsa)) {
    config->named_groups_preset = network::mojom::SSLNamedGroupsPreset::kCnsa2;
    config->tls13_cipher_prefer_aes_256 = true;
    return;
  }

  if (key_exchange_compliance_pref.IsManaged()) {
    config->named_groups_preset =
        key_exchange_compliance_pref.GetValue() == kPrefStringValueCnsa2
            ? network::mojom::SSLNamedGroupsPreset::kCnsa2
            : network::mojom::SSLNamedGroupsPreset::kDefault;
  }

  if (tls13_cipher_compliance_pref.IsManaged()) {
    config->tls13_cipher_prefer_aes_256 =
        tls13_cipher_compliance_pref.GetValue() == kPrefStringValueCnsa;
  }
}
