// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GM_GM_POLICY_H_
#define CHROME_BROWSER_GM_GM_POLICY_H_

#include <stddef.h>

#include <optional>
#include <string>
#include <vector>

#include "base/values.h"
#include "services/gm_key/public/mojom/gm_key_service.mojom.h"
#include "services/network/public/mojom/ssl_config.mojom.h"

class PrefRegistrySimple;
class PrefService;

namespace gm_browser {

inline constexpr char kSiteRulesPref[] = "gm_tls.site_rules";
inline constexpr char kDriverPref[] = "gm_tls.usb_key_driver";

struct SitePolicy {
  network::mojom::GmTlsRulePtr rule;
  // Optional exact SHA-256 of the signing certificate, lowercase hex.
  std::string certificate_sha256;
};

struct Policy {
  bool valid = true;
  std::vector<SitePolicy> sites;
  // An invalid configuration emits a rejecting sentinel, never an empty list
  // that could accidentally enable ordinary TLS for a required GM origin.
  std::vector<network::mojom::GmTlsRulePtr> ToNetworkRules() const;
};

// Location of the first invalid setting. A missing rule index identifies an
// error in the whole list, such as exceeding its size limit.
struct PolicyValidationError {
  std::optional<size_t> rule_index;
  std::string field;
};

void RegisterPrefs(PrefRegistrySimple* registry);
Policy ParsePolicy(const base::ListValue& value,
                   PolicyValidationError* error = nullptr);
Policy ReadPolicy(const PrefService* prefs);
gm_key::mojom::DriverConfigPtr ReadDriver(const PrefService* prefs);

}  // namespace gm_browser

#endif  // CHROME_BROWSER_GM_GM_POLICY_H_
