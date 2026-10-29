// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GM_GM_SITE_RULES_POLICY_HANDLER_H_
#define CHROME_BROWSER_GM_GM_SITE_RULES_POLICY_HANDLER_H_

#include "components/policy/core/browser/configuration_policy_handler.h"

namespace gm_browser {

// Invalid configured rules must reach the network layer as a rejecting policy,
// rather than being discarded and interpreted as an absent policy.
class SiteRulesPolicyHandler : public policy::TypeCheckingPolicyHandler {
 public:
  SiteRulesPolicyHandler();
  ~SiteRulesPolicyHandler() override;

  bool CheckPolicySettings(const policy::PolicyMap& policies,
                           policy::PolicyErrorMap* errors) override;
  void ApplyPolicySettings(const policy::PolicyMap& policies,
                           PrefValueMap* prefs) override;
};

}  // namespace gm_browser

#endif  // CHROME_BROWSER_GM_GM_SITE_RULES_POLICY_HANDLER_H_
