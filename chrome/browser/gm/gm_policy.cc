// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_policy.h"

#include <set>
#include <utility>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/pref_service.h"
#include "net/base/host_port_pair.h"
#include "net/base/url_util.h"
#include "url/gurl.h"

namespace gm_browser {
namespace {

constexpr size_t kMaxPolicyBytes = 16 * 1024 * 1024;

bool DecodeList(const base::ListValue* list,
                std::vector<std::vector<uint8_t>>* out,
                size_t* total) {
  if (!list || list->size() > 128) {
    return false;
  }
  for (const auto& value : *list) {
    if (!value.is_string() || value.GetString().size() > 350 * 1024) {
      return false;
    }
    auto data = base::Base64Decode(value.GetString());
    if (!data || data->empty() || data->size() > 256 * 1024) {
      return false;
    }
    *total += data->size();
    if (*total > kMaxPolicyBytes) {
      return false;
    }
    out->emplace_back(data->begin(), data->end());
  }
  return true;
}

bool ParseSite(const base::Value& value,
               SitePolicy* site,
               size_t* total,
               const char** invalid_field) {
  const auto fail = [invalid_field](const char* field) {
    *invalid_field = field;
    return false;
  };
  const auto* dict = value.GetIfDict();
  if (!dict) {
    return fail("");
  }
  const auto* host = dict->FindString("host");
  const auto* protocol = dict->FindString("protocol");
  const auto* trust = dict->FindString("trust_profile_id");
  const int port = dict->FindInt("port").value_or(443);
  const int trust_generation = dict->FindInt("trust_generation").value_or(0);
  const int revision = dict->FindInt("policy_revision").value_or(0);
  if ((dict->contains("port") && !dict->FindInt("port")) || port < 1 ||
      port > 65535) {
    return fail("port");
  }
  if (dict->contains("identity_policy_id") &&
      !dict->FindString("identity_policy_id")) {
    return fail("identity_policy_id");
  }
  if (dict->contains("certificate_sha256") &&
      !dict->FindString("certificate_sha256")) {
    return fail("certificate_sha256");
  }
  if (!host || host->empty() || host->size() > 253) {
    return fail("host");
  }
  if (!protocol) {
    return fail("protocol");
  }
  if (!trust || trust->empty() || trust->size() > 128) {
    return fail("trust_profile_id");
  }
  if (trust_generation <= 0) {
    return fail("trust_generation");
  }
  if (revision <= 0) {
    return fail("policy_revision");
  }
  // Accept an exact canonical DNS name or IP literal, with no URL components.
  net::HostPortPair pair(*host, port);
  GURL origin("https://" + pair.ToString() + "/");
  if (!origin.is_valid() || origin.has_username() || origin.has_password() ||
      origin.path() != "/" || origin.has_query() || origin.has_ref() ||
      origin.HostNoBrackets() != *host || host->back() == '.' ||
      (!origin.HostIsIPAddress() &&
       !net::IsCanonicalizedHostCompliant(*host))) {
    return fail("host");
  }
  auto rule = network::mojom::GmTlsRule::New();
  rule->host = *host;
  rule->port = port;
  if (*protocol == "tlcp") {
    rule->protocol = network::mojom::GmTlsProtocol::kTlcp;
  } else if (*protocol == "rfc8998") {
    rule->protocol = network::mojom::GmTlsProtocol::kRfc8998;
  } else if (*protocol == "tls13_auto") {
    rule->protocol = network::mojom::GmTlsProtocol::kTls13Auto;
  } else {
    return fail("protocol");
  }
  rule->trust_profile_id = *trust;
  rule->trust_generation = trust_generation;
  rule->policy_revision = revision;
  if (const auto* identity = dict->FindString("identity_policy_id")) {
    if (identity->empty() || identity->size() > 128) {
      return fail("identity_policy_id");
    }
    rule->identity_policy_id = *identity;
    const int generation = dict->FindInt("identity_generation").value_or(0);
    if (generation <= 0) {
      return fail("identity_generation");
    }
    rule->identity_generation = generation;
  } else if (dict->contains("identity_generation")) {
    return fail("identity_generation");
  }
  if (const auto* fingerprint = dict->FindString("certificate_sha256")) {
    std::vector<uint8_t> bytes;
    if (rule->identity_policy_id.empty() || fingerprint->size() != 64 ||
        !base::HexStringToBytes(*fingerprint, &bytes) || bytes.size() != 32) {
      return fail("certificate_sha256");
    }
    site->certificate_sha256 = base::ToLowerASCII(*fingerprint);
  }
  if (!DecodeList(dict->FindList("trust_anchors"), &rule->trust_anchors,
                  total) ||
      rule->trust_anchors.empty()) {
    return fail("trust_anchors");
  }
  if (dict->contains("crls") &&
      !DecodeList(dict->FindList("crls"), &rule->crls, total)) {
    return fail("crls");
  }
  // Revocation is an explicit deployment decision, including "disabled".
  const auto* revocation = dict->FindString("revocation");
  if (!revocation) {
    return fail("revocation");
  }
  if (*revocation == "disabled") {
    rule->revocation_mode = 0;
  } else if (*revocation == "leaf") {
    rule->revocation_mode = 1;
  } else if (*revocation == "chain") {
    rule->revocation_mode = 2;
  } else {
    return fail("revocation");
  }
  if (rule->revocation_mode && rule->crls.empty()) {
    return fail("crls");
  }
  if (dict->contains("enable_session_resumption")) {
    auto resume = dict->FindBool("enable_session_resumption");
    if (!resume) {
      return fail("enable_session_resumption");
    }
    rule->enable_session_resumption = *resume;
  }
  site->rule = std::move(rule);
  return true;
}

}  // namespace

std::vector<network::mojom::GmTlsRulePtr> Policy::ToNetworkRules() const {
  std::vector<network::mojom::GmTlsRulePtr> result;
  if (!valid) {
    result.push_back(network::mojom::GmTlsRule::New());
    return result;
  }
  for (const auto& site : sites) {
    result.push_back(site.rule->Clone());
  }
  return result;
}

void RegisterPrefs(PrefRegistrySimple* registry) {
  registry->RegisterListPref(kSiteRulesPref);
  registry->RegisterDictionaryPref(kDriverPref);
}

Policy ParsePolicy(const base::ListValue& value, PolicyValidationError* error) {
  if (error) {
    *error = {};
  }
  Policy policy;
  if (value.size() > 256) {
    policy.valid = false;
    return policy;
  }
  size_t total = 0;
  std::set<std::pair<std::string, uint16_t>> origins;
  for (size_t index = 0; index < value.size(); ++index) {
    SitePolicy site;
    const char* invalid_field = "";
    const bool valid = ParseSite(value[index], &site, &total, &invalid_field);
    if (!valid || !origins.emplace(site.rule->host, site.rule->port).second) {
      policy.valid = false;
      policy.sites.clear();
      if (error) {
        error->rule_index = index;
        error->field = valid ? "host" : invalid_field;
      }
      return policy;
    }
    policy.sites.push_back(std::move(site));
  }
  return policy;
}

Policy ReadPolicy(const PrefService* prefs) {
  if (!prefs->IsManagedPreference(kSiteRulesPref)) {
    return {};
  }
  return ParsePolicy(prefs->GetList(kSiteRulesPref));
}

gm_key::mojom::DriverConfigPtr ReadDriver(const PrefService* prefs) {
  if (!prefs->IsManagedPreference(kDriverPref)) {
    return nullptr;
  }
  const auto& dict = prefs->GetDict(kDriverPref);
  const auto* path = dict.FindString("path");
  const auto* hash = dict.FindString("sha256");
  if (!path || !hash || hash->size() != 64) {
    return nullptr;
  }
  auto config = gm_key::mojom::DriverConfig::New();
  config->library_path = base::FilePath::FromUTF8Unsafe(*path);
  if (!config->library_path.IsAbsolute() ||
      config->library_path.ReferencesParent() ||
      config->library_path.value().starts_with(FILE_PATH_LITERAL("\\\\")) ||
      !base::HexStringToBytes(*hash, &config->sha256) ||
      config->sha256.size() != 32) {
    return nullptr;
  }
  return config;
}

}  // namespace gm_browser
