// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_site_rules_policy_handler.h"

#include <utility>

#include "base/numerics/safe_conversions.h"
#include "chrome/browser/gm/gm_policy.h"
#include "components/policy/core/browser/policy_error_map.h"
#include "components/policy/policy_constants.h"
#include "components/prefs/pref_value_map.h"
#include "components/strings/grit/components_strings.h"

namespace gm_browser {

SiteRulesPolicyHandler::SiteRulesPolicyHandler()
    : TypeCheckingPolicyHandler(policy::key::kGmTlsSiteRules,
                                base::Value::Type::LIST) {}

SiteRulesPolicyHandler::~SiteRulesPolicyHandler() = default;

bool SiteRulesPolicyHandler::CheckPolicySettings(
    const policy::PolicyMap& policies,
    policy::PolicyErrorMap* errors) {
  const base::Value* value = nullptr;
  if (!CheckAndGetValue(policies, errors, &value)) {
    // Returning false would suppress ApplyPolicySettings and erase the managed
    // constraint. Keep the type error and apply a rejecting sentinel instead.
    return true;
  }
  if (!value) {
    if (policies.Get(policy_name()) && errors) {
      errors->AddError(policy_name(), IDS_POLICY_VALUE_FORMAT_ERROR);
    }
    return true;
  }
  PolicyValidationError error;
  if (!ParsePolicy(value->GetList(), &error).valid && errors) {
    policy::PolicyErrorPath path;
    if (error.rule_index) {
      path.emplace_back(base::checked_cast<int>(*error.rule_index));
    }
    if (!error.field.empty()) {
      path.emplace_back(error.field);
    }
    errors->AddError(policy_name(), IDS_POLICY_VALUE_FORMAT_ERROR,
                     std::move(path));
  }
  return true;
}

void SiteRulesPolicyHandler::ApplyPolicySettings(
    const policy::PolicyMap& policies,
    PrefValueMap* prefs) {
  const auto* entry = policies.Get(policy_name());
  if (!entry) {
    return;
  }
  // Inspect the type here because this handler deliberately accepts invalid
  // input for conversion to the fail-closed representation.
  const auto* value = entry->value_unsafe();
  prefs->SetValue(
      kSiteRulesPref,
      value && value->is_list()
          ? value->Clone()
          : base::Value(base::ListValue().Append(base::DictValue())));
}

}  // namespace gm_browser
