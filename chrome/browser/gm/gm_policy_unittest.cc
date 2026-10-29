// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_policy.h"

#include <algorithm>
#include <memory>
#include <string_view>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/gm/gm_site_rules_policy_handler.h"
#include "components/policy/core/browser/configuration_policy_handler_list.h"
#include "components/policy/core/browser/policy_error_map.h"
#include "components/policy/core/common/policy_map.h"
#include "components/policy/policy_constants.h"
#include "components/prefs/pref_value_map.h"
#include "components/prefs/testing_pref_service.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include "base/json/json_writer.h"
#include "base/test/task_environment.h"
#include "base/test/test_reg_util_win.h"
#include "base/win/registry.h"
#include "components/policy/core/common/chrome_schema.h"
#include "components/policy/core/common/management/platform_management_service.h"
#include "components/policy/core/common/policy_bundle.h"
#include "components/policy/core/common/policy_loader_common.h"
#include "components/policy/core/common/policy_loader_win.h"
#include "components/policy/core/common/schema_registry.h"
#endif

namespace gm_browser {
namespace {
base::DictValue Rule() {
  return base::DictValue()
      .Set("host", "gm.example")
      .Set("protocol", "tlcp")
      .Set("trust_profile_id", "company-root")
      .Set("trust_generation", 1)
      .Set("policy_revision", 1)
      .Set("revocation", "disabled")
      .Set("trust_anchors", base::ListValue().Append("AQID"));
}

TEST(GmBrowserPolicy, AcceptsExplicitProtocolsAndPreservesTrust) {
  for (const char* protocol : {"tlcp", "rfc8998", "tls13_auto"}) {
    auto policy =
        ParsePolicy(base::ListValue().Append(Rule().Set("protocol", protocol)));
    ASSERT_TRUE(policy.valid);
    ASSERT_EQ(policy.sites.size(), 1u);
    EXPECT_EQ(policy.sites[0].rule->trust_anchors[0],
              (std::vector<uint8_t>{1, 2, 3}));
    EXPECT_EQ(policy.sites[0].rule->port, 443);
    EXPECT_TRUE(policy.sites[0].rule->enable_session_resumption);
    const auto expected = std::string_view(protocol) == "tlcp"
                              ? network::mojom::GmTlsProtocol::kTlcp
                          : std::string_view(protocol) == "rfc8998"
                              ? network::mojom::GmTlsProtocol::kRfc8998
                              : network::mojom::GmTlsProtocol::kTls13Auto;
    EXPECT_EQ(expected, policy.sites[0].rule->protocol);
    EXPECT_EQ(expected, policy.ToNetworkRules()[0]->protocol);
  }
}

TEST(GmBrowserPolicy, JointNegotiationStillRequiresExplicitTrustAndRevocation) {
  auto joint = Rule().Set("protocol", "tls13_auto");
  ASSERT_TRUE(ParsePolicy(base::ListValue().Append(joint.Clone())).valid);
  joint.Set("trust_anchors", base::ListValue());
  // A caller cannot turn a managed joint rule into a system-trust rule by
  // supplying the internal automatic marker as an extra policy property.
  joint.Set("automatic", true);
  EXPECT_FALSE(ParsePolicy(base::ListValue().Append(joint.Clone())).valid);
  joint = Rule().Set("protocol", "tls13_auto");
  joint.Remove("revocation");
  EXPECT_FALSE(ParsePolicy(base::ListValue().Append(std::move(joint))).valid);
}

TEST(GmBrowserPolicy, RejectsAmbiguousOriginsAndDuplicateRules) {
  for (const char* host :
       {"*.example", "example/a", "example?x", "example#x", "user@example",
        "UPPER.example", "example.", "a b.example"}) {
    EXPECT_FALSE(
        ParsePolicy(base::ListValue().Append(Rule().Set("host", host))).valid)
        << host;
  }
  EXPECT_FALSE(
      ParsePolicy(base::ListValue().Append(Rule()).Append(Rule())).valid);
}

TEST(GmBrowserPolicy, InvalidConfigurationBecomesRejectingSentinel) {
  auto policy =
      ParsePolicy(base::ListValue().Append(Rule().Set("protocol", "tls")));
  EXPECT_FALSE(policy.valid);
  auto wire = policy.ToNetworkRules();
  ASSERT_EQ(wire.size(), 1u);
  EXPECT_TRUE(wire[0]->host.empty());
  EXPECT_TRUE(wire[0]->trust_anchors.empty());
}

TEST(GmBrowserPolicy, RequiresExplicitRevocationAndCurrentInput) {
  auto rule = Rule();
  rule.Remove("revocation");
  EXPECT_FALSE(ParsePolicy(base::ListValue().Append(std::move(rule))).valid);
  EXPECT_FALSE(
      ParsePolicy(base::ListValue().Append(Rule().Set("revocation", "chain")))
          .valid);
  EXPECT_TRUE(
      ParsePolicy(base::ListValue().Append(
                      Rule()
                          .Set("revocation", "leaf")
                          .Set("crls", base::ListValue().Append("AQID"))))
          .valid);
}

TEST(GmBrowserPolicy, EnforcesGenerationAndIdentityBinding) {
  EXPECT_FALSE(
      ParsePolicy(base::ListValue().Append(Rule().Set("port", "443"))).valid);
  EXPECT_FALSE(
      ParsePolicy(base::ListValue().Append(Rule().Set("identity_policy_id", 1)))
          .valid);
  EXPECT_FALSE(
      ParsePolicy(base::ListValue().Append(Rule().Set("certificate_sha256", 1)))
          .valid);
  EXPECT_FALSE(
      ParsePolicy(base::ListValue().Append(Rule().Set("trust_generation", 0)))
          .valid);
  EXPECT_FALSE(
      ParsePolicy(base::ListValue().Append(Rule().Set("policy_revision", -1)))
          .valid);
  EXPECT_FALSE(ParsePolicy(base::ListValue().Append(
                               Rule().Set("identity_policy_id", "key")))
                   .valid);
  EXPECT_FALSE(ParsePolicy(base::ListValue().Append(
                               Rule().Set("identity_generation", 1)))
                   .valid);
  EXPECT_TRUE(
      ParsePolicy(base::ListValue().Append(
                      Rule()
                          .Set("identity_policy_id", "key")
                          .Set("identity_generation", 2)
                          .Set("certificate_sha256", std::string(64, 'a'))))
          .valid);
}

TEST(GmBrowserPolicy, RejectsMalformedAndOversizedCertificateInput) {
  EXPECT_FALSE(
      ParsePolicy(base::ListValue().Append(Rule().Set(
                      "trust_anchors", base::ListValue().Append("!invalid"))))
          .valid);
  EXPECT_FALSE(
      ParsePolicy(base::ListValue().Append(Rule().Set(
                      "trust_anchors",
                      base::ListValue().Append(std::string(351 * 1024, 'A')))))
          .valid);
  base::ListValue many;
  for (size_t i = 0; i < 257; ++i) {
    many.Append(Rule());
  }
  EXPECT_FALSE(ParsePolicy(many).valid);
}

TEST(GmBrowserPolicy, IgnoresUnmanagedSettings) {
  TestingPrefServiceSimple prefs;
  RegisterPrefs(prefs.registry());
  prefs.SetList(kSiteRulesPref, base::ListValue().Append(Rule()));
  EXPECT_TRUE(ReadPolicy(&prefs).sites.empty());
  prefs.SetManagedPref(kSiteRulesPref,
                       base::Value(base::ListValue().Append(Rule())));
  EXPECT_EQ(ReadPolicy(&prefs).sites.size(), 1u);
}

class GmSiteRulesPolicyHandlerTest : public testing::Test {
 protected:
  GmSiteRulesPolicyHandlerTest() : handlers_({}, {}, false) {
    RegisterPrefs(prefs_.registry());
    handlers_.AddHandler(std::make_unique<SiteRulesPolicyHandler>());
  }

  void SetPolicy(base::Value value) {
    policies_.Set(policy::key::kGmTlsSiteRules, policy::POLICY_LEVEL_MANDATORY,
                  policy::POLICY_SCOPE_MACHINE, policy::POLICY_SOURCE_PLATFORM,
                  std::move(value), nullptr);
  }

  Policy ApplyPolicies() {
    PrefValueMap mapped;
    errors_ = std::make_unique<policy::PolicyErrorMap>();
    // Exercise the real CheckPolicySettings -> ApplyPolicySettings dispatch.
    // A false validation result here would drop the managed preference.
    handlers_.ApplyPolicySettings(policies_, &mapped, errors_.get(), nullptr,
                                  nullptr);
    const base::Value* value = nullptr;
    if (mapped.GetValue(kSiteRulesPref, &value)) {
      prefs_.SetManagedPref(kSiteRulesPref, value->Clone());
    } else {
      prefs_.RemoveManagedPref(kSiteRulesPref);
    }
    return ReadPolicy(&prefs_);
  }

  void ExpectRejected(const Policy& result) {
    EXPECT_TRUE(prefs_.IsManagedPreference(kSiteRulesPref));
    EXPECT_FALSE(result.valid);
    EXPECT_TRUE(result.sites.empty());
    const auto wire = result.ToNetworkRules();
    ASSERT_EQ(wire.size(), 1u);
    EXPECT_TRUE(wire[0]->host.empty());
    EXPECT_TRUE(wire[0]->trust_anchors.empty());
    EXPECT_TRUE(errors_->HasFatalError(policy::key::kGmTlsSiteRules));
  }

  TestingPrefServiceSimple prefs_;
  policy::PolicyMap policies_;
  policy::ConfigurationPolicyHandlerList handlers_;
  std::unique_ptr<policy::PolicyErrorMap> errors_;
};

TEST_F(GmSiteRulesPolicyHandlerTest,
       AbsentAndExplicitEmptyRulesRemainDistinct) {
  auto absent = ApplyPolicies();
  EXPECT_TRUE(absent.valid);
  EXPECT_TRUE(absent.ToNetworkRules().empty());
  EXPECT_FALSE(prefs_.IsManagedPreference(kSiteRulesPref));
  EXPECT_TRUE(errors_->empty());

  SetPolicy(base::Value(base::ListValue()));
  auto empty = ApplyPolicies();
  EXPECT_TRUE(empty.valid);
  EXPECT_TRUE(empty.ToNetworkRules().empty());
  EXPECT_TRUE(prefs_.IsManagedPreference(kSiteRulesPref));
  EXPECT_TRUE(errors_->empty());
}

TEST_F(GmSiteRulesPolicyHandlerTest, ValidRulesPreserveNetworkConstraints) {
  SetPolicy(base::Value(
      base::ListValue().Append(Rule()
                                   .Set("protocol", "rfc8998")
                                   .Set("enable_session_resumption", false))));
  auto result = ApplyPolicies();
  ASSERT_TRUE(result.valid);
  EXPECT_TRUE(errors_->empty());
  const auto wire = result.ToNetworkRules();
  ASSERT_EQ(wire.size(), 1u);
  EXPECT_EQ(wire[0]->host, "gm.example");
  EXPECT_EQ(wire[0]->protocol, network::mojom::GmTlsProtocol::kRfc8998);
  EXPECT_EQ(wire[0]->trust_anchors,
            (std::vector<std::vector<uint8_t>>{{1, 2, 3}}));
  EXPECT_FALSE(wire[0]->enable_session_resumption);
}

TEST_F(GmSiteRulesPolicyHandlerTest, EveryWrongTopLevelTypeFailsClosed) {
  std::vector<base::Value> values;
  values.emplace_back();
  values.emplace_back(true);
  values.emplace_back(1);
  values.emplace_back(1.5);
  values.emplace_back("[]");
  values.emplace_back(Rule());
  for (const auto& value : values) {
    SCOPED_TRACE(base::Value::GetTypeName(value.type()));
    SetPolicy(value.Clone());
    ExpectRejected(ApplyPolicies());
  }
}

TEST_F(GmSiteRulesPolicyHandlerTest,
       InvalidRuleReportsItsFieldAndRejectsAllRules) {
  for (const char* field : {"host", "protocol", "port", "trust_anchors",
                            "identity_policy_id", "revocation"}) {
    SCOPED_TRACE(field);
    SetPolicy(base::Value(
        base::ListValue().Append(Rule()).Append(Rule().Set(field, false))));
    ExpectRejected(ApplyPolicies());
    const auto expected_path = base::UTF8ToUTF16(std::string("[1].") + field);
    EXPECT_NE(errors_->GetErrorMessages(policy::key::kGmTlsSiteRules)
                  .find(expected_path),
              std::u16string::npos);
  }
}

TEST_F(GmSiteRulesPolicyHandlerTest, InvalidListEntryIsNotSilentlyFiltered) {
  SetPolicy(base::Value(base::ListValue().Append(Rule()).Append(false)));
  ExpectRejected(ApplyPolicies());
  EXPECT_NE(
      errors_->GetErrorMessages(policy::key::kGmTlsSiteRules).find(u"[1]"),
      std::u16string::npos);
}

TEST_F(GmSiteRulesPolicyHandlerTest, SemanticErrorsReportTheAffectedField) {
  SetPolicy(base::Value(base::ListValue().Append(
      Rule().Set("trust_anchors", base::ListValue().Append("!not-base64")))));
  ExpectRejected(ApplyPolicies());
  EXPECT_NE(errors_->GetErrorMessages(policy::key::kGmTlsSiteRules)
                .find(u"[0].trust_anchors"),
            std::u16string::npos);

  SetPolicy(base::Value(base::ListValue().Append(Rule()).Append(Rule())));
  ExpectRejected(ApplyPolicies());
  EXPECT_NE(
      errors_->GetErrorMessages(policy::key::kGmTlsSiteRules).find(u"[1].host"),
      std::u16string::npos);
}

TEST_F(GmSiteRulesPolicyHandlerTest,
       RefreshFailsClosedAndRecoversWithoutRestart) {
  for (int cycle = 0; cycle < 2; ++cycle) {
    SetPolicy(base::Value(base::ListValue().Append(Rule())));
    auto valid = ApplyPolicies();
    ASSERT_TRUE(valid.valid);
    ASSERT_EQ(valid.sites.size(), 1u);
    EXPECT_TRUE(errors_->empty());

    SetPolicy(base::Value(Rule()));
    ExpectRejected(ApplyPolicies());
    SetPolicy(base::Value(base::ListValue().Append(Rule().Set("port", "443"))));
    ExpectRejected(ApplyPolicies());
  }
  SetPolicy(base::Value(base::ListValue().Append(Rule())));
  EXPECT_TRUE(ApplyPolicies().valid);
  EXPECT_TRUE(errors_->empty());

  policies_.Clear();
  auto removed = ApplyPolicies();
  EXPECT_TRUE(removed.valid);
  EXPECT_TRUE(removed.ToNetworkRules().empty());
  EXPECT_FALSE(prefs_.IsManagedPreference(kSiteRulesPref));
  EXPECT_TRUE(errors_->empty());
}

#if BUILDFLAG(IS_WIN)
class GmPolicyLoaderWinTest : public GmSiteRulesPolicyHandlerTest {
 protected:
  static constexpr wchar_t kRegistryPath[] =
      L"Software\\Policies\\ChromiumGmPolicyTest";

  void SetUp() override {
    // Every registry access below is redirected to temporary test hives.
    ASSERT_NO_FATAL_FAILURE(
        registry_override_.OverrideRegistry(HKEY_LOCAL_MACHINE));
    ASSERT_NO_FATAL_FAILURE(
        registry_override_.OverrideRegistry(HKEY_CURRENT_USER));
    schema_registry_.RegisterComponent(
        policy::PolicyNamespace(policy::POLICY_DOMAIN_CHROME, ""),
        policy::GetChromeSchema());
  }

  void WriteJsonPolicy(const wchar_t* value,
                       HKEY hive = HKEY_LOCAL_MACHINE,
                       const wchar_t* name = L"GmTlsSiteRules") {
    base::win::RegKey key(hive, kRegistryPath, KEY_ALL_ACCESS);
    ASSERT_TRUE(key.Valid());
    ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(name, value));
  }

  Policy LoadAndApplyPolicies() {
    policy::PolicyLoaderWin loader(
        task_environment_.GetMainThreadTaskRunner(),
        policy::PlatformManagementService::GetInstance(), kRegistryPath);
    auto bundle = loader.InitialLoad(schema_registry_.schema_map());
    policies_ =
        bundle.Get(policy::PolicyNamespace(policy::POLICY_DOMAIN_CHROME, ""))
            .Clone();
    return ApplyPolicies();
  }

  std::wstring ValidRulesJson() {
    return base::UTF8ToWide(
        base::WriteJson(base::ListValue().Append(Rule())).value());
  }

  base::test::TaskEnvironment task_environment_;
  registry_util::RegistryOverrideManager registry_override_;
  policy::SchemaRegistry schema_registry_;
};

TEST_F(GmPolicyLoaderWinTest, MalformedRegistryJsonNeverBecomesAbsentPolicy) {
  for (const wchar_t* raw :
       {L"{}", L"null", L"true", L"\"[]\"", L"not JSON", L""}) {
    SCOPED_TRACE(base::WideToUTF8(raw));
    ASSERT_NO_FATAL_FAILURE(WriteJsonPolicy(raw));
    ExpectRejected(LoadAndApplyPolicies());
    ASSERT_TRUE(policies_.Get(policy::key::kGmTlsSiteRules));
  }
}

TEST_F(GmPolicyLoaderWinTest, WrongRegistryTypeRemainsARejectingPolicy) {
  base::win::RegKey key(HKEY_LOCAL_MACHINE, kRegistryPath, KEY_ALL_ACCESS);
  ASSERT_TRUE(key.Valid());
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"GmTlsSiteRules", DWORD{1}));
  ExpectRejected(LoadAndApplyPolicies());
}

TEST_F(GmPolicyLoaderWinTest, UnsupportedNativeTopLevelTypesRemainPresent) {
  base::win::RegKey key(HKEY_LOCAL_MACHINE, kRegistryPath, KEY_ALL_ACCESS);
  ASSERT_TRUE(key.Valid());
  const uint64_t bytes = 1;
  for (DWORD type : {REG_BINARY, REG_QWORD_LITTLE_ENDIAN, REG_NONE}) {
    SCOPED_TRACE(type);
    ASSERT_EQ(ERROR_SUCCESS,
              key.WriteValue(L"GmTlsSiteRules", &bytes, sizeof(bytes), type));
    ExpectRejected(LoadAndApplyPolicies());
  }
}

TEST_F(GmPolicyLoaderWinTest, UnsupportedNativeNumberedTypesAreNotFiltered) {
  const std::wstring path = std::wstring(kRegistryPath) + L"\\GmTlsSiteRules";
  base::win::RegKey key(HKEY_LOCAL_MACHINE, path.c_str(), KEY_ALL_ACCESS);
  ASSERT_TRUE(key.Valid());
  const auto valid_rule = base::UTF8ToWide(base::WriteJson(Rule()).value());
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"1", valid_rule.c_str()));
  const uint64_t bytes = 1;
  for (DWORD type : {REG_BINARY, REG_QWORD_LITTLE_ENDIAN, REG_NONE}) {
    SCOPED_TRACE(type);
    ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"2", &bytes, sizeof(bytes), type));
    ExpectRejected(LoadAndApplyPolicies());
    EXPECT_NE(
        errors_->GetErrorMessages(policy::key::kGmTlsSiteRules).find(u"[1]"),
        std::u16string::npos);
  }
}

TEST_F(GmPolicyLoaderWinTest, RegistryRefreshRecoversAndRemovalIsExplicit) {
  const auto valid = ValidRulesJson();
  ASSERT_NO_FATAL_FAILURE(WriteJsonPolicy(valid.c_str()));
  ASSERT_TRUE(LoadAndApplyPolicies().valid);
  ASSERT_EQ(ReadPolicy(&prefs_).sites.size(), 1u);
  EXPECT_TRUE(errors_->empty());

  // Case-insensitive policy lookup must preserve malformed values too.
  ASSERT_NO_FATAL_FAILURE(
      WriteJsonPolicy(L"{}", HKEY_LOCAL_MACHINE, L"gmtlssiterules"));
  ExpectRejected(LoadAndApplyPolicies());

  ASSERT_NO_FATAL_FAILURE(WriteJsonPolicy(valid.c_str()));
  ASSERT_TRUE(LoadAndApplyPolicies().valid);
  EXPECT_EQ(ReadPolicy(&prefs_).sites.size(), 1u);
  EXPECT_TRUE(errors_->empty());

  base::win::RegKey key(HKEY_LOCAL_MACHINE, kRegistryPath, KEY_ALL_ACCESS);
  ASSERT_EQ(ERROR_SUCCESS, key.DeleteValue(L"GmTlsSiteRules"));
  EXPECT_TRUE(LoadAndApplyPolicies().ToNetworkRules().empty());
  EXPECT_FALSE(prefs_.IsManagedPreference(kSiteRulesPref));
}

TEST_F(GmPolicyLoaderWinTest, InvalidMachinePolicyDoesNotYieldToUserPolicy) {
  const auto valid = ValidRulesJson();
  ASSERT_NO_FATAL_FAILURE(WriteJsonPolicy(valid.c_str(), HKEY_CURRENT_USER));
  ASSERT_NO_FATAL_FAILURE(WriteJsonPolicy(L"{}"));
  ExpectRejected(LoadAndApplyPolicies());
  ASSERT_TRUE(policies_.Get(policy::key::kGmTlsSiteRules));
  EXPECT_EQ(policies_.Get(policy::key::kGmTlsSiteRules)->scope,
            policy::POLICY_SCOPE_MACHINE);
}

TEST_F(GmPolicyLoaderWinTest, InvalidNumberedEntryIsNotDropped) {
  const std::wstring path = std::wstring(kRegistryPath) + L"\\GmTlsSiteRules";
  base::win::RegKey key(HKEY_LOCAL_MACHINE, path.c_str(), KEY_ALL_ACCESS);
  ASSERT_TRUE(key.Valid());
  const auto valid_rule = base::UTF8ToWide(base::WriteJson(Rule()).value());
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"1", valid_rule.c_str()));
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"2", DWORD{1}));
  ExpectRejected(LoadAndApplyPolicies());
  EXPECT_NE(
      errors_->GetErrorMessages(policy::key::kGmTlsSiteRules).find(u"[1]"),
      std::u16string::npos);
}

TEST_F(GmPolicyLoaderWinTest, InvalidOptionalIdentityFieldIsNotDropped) {
  const std::wstring path =
      std::wstring(kRegistryPath) + L"\\GmTlsSiteRules\\1";
  base::win::RegKey key(HKEY_LOCAL_MACHINE, path.c_str(), KEY_ALL_ACCESS);
  ASSERT_TRUE(key.Valid());
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"host", L"gm.example"));
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"protocol", L"tlcp"));
  ASSERT_EQ(ERROR_SUCCESS,
            key.WriteValue(L"trust_profile_id", L"company-root"));
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"trust_generation", DWORD{1}));
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"policy_revision", DWORD{1}));
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"revocation", L"disabled"));
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"trust_anchors", L"[\"AQID\"]"));
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"identity_policy_id", DWORD{1}));
  ExpectRejected(LoadAndApplyPolicies());
  EXPECT_NE(errors_->GetErrorMessages(policy::key::kGmTlsSiteRules)
                .find(u"[0].identity_policy_id"),
            std::u16string::npos);
}

TEST_F(GmPolicyLoaderWinTest, NonnumericListEntriesCannotBecomeAnEmptyPolicy) {
  const std::wstring path = std::wstring(kRegistryPath) + L"\\GmTlsSiteRules";
  base::win::RegKey key(HKEY_LOCAL_MACHINE, path.c_str(), KEY_ALL_ACCESS);
  ASSERT_TRUE(key.Valid());
  ASSERT_EQ(ERROR_SUCCESS, key.WriteValue(L"host", L"gm.example"));
  ExpectRejected(LoadAndApplyPolicies());
}

TEST_F(GmPolicyLoaderWinTest,
       OtherPoliciesKeepTheirRegistryConversionBehavior) {
  ASSERT_FALSE(
      policy::IsPolicyNameSensitive(policy::key::kApplicationLocaleValue));
  ASSERT_NO_FATAL_FAILURE(
      WriteJsonPolicy(L"{}", HKEY_LOCAL_MACHINE, L"URLBlocklist"));
  ASSERT_NO_FATAL_FAILURE(
      WriteJsonPolicy(L"en-US", HKEY_LOCAL_MACHINE, L"ApplicationLocaleValue"));
  ASSERT_NO_FATAL_FAILURE(WriteJsonPolicy(
      L"https://example.test/", HKEY_LOCAL_MACHINE, L"HomepageLocation"));

  // Establish the ordinary-policy result without any national-TLS setting.
  // HomepageLocation is sensitive: unmanaged devices mark it ignored, although
  // its string was successfully converted. Do not assume this test machine is
  // managed or bypass Chromium's existing sensitive-policy filter.
  EXPECT_TRUE(LoadAndApplyPolicies().valid);
  const auto baseline = policies_.Clone();
  const auto homepage = std::ranges::find_if(baseline, [](const auto& entry) {
    return entry.first == policy::key::kHomepageLocation;
  });
  ASSERT_NE(homepage, baseline.end());
  ASSERT_TRUE(homepage->second.value(base::Value::Type::STRING));
  EXPECT_EQ(homepage->second.value(base::Value::Type::STRING)->GetString(),
            "https://example.test/");

  ASSERT_NO_FATAL_FAILURE(WriteJsonPolicy(L"{}"));
  ExpectRejected(LoadAndApplyPolicies());
  EXPECT_FALSE(policies_.Get(policy::key::kURLBlocklist));
  const auto* locale = policies_.GetValue(policy::key::kApplicationLocaleValue,
                                          base::Value::Type::STRING);
  ASSERT_TRUE(locale);
  EXPECT_EQ(locale->GetString(), "en-US");
  policies_.Erase(policy::key::kGmTlsSiteRules);
  EXPECT_TRUE(policies_.Equals(baseline));
}
#endif  // BUILDFLAG(IS_WIN)

TEST(GmBrowserPolicy, DriverRequiresManagedAbsoluteLocalPathAndExactHash) {
  TestingPrefServiceSimple prefs;
  RegisterPrefs(prefs.registry());
  auto driver = base::DictValue()
                    .Set("path", "C:\\Program Files\\Feitian\\SKF.dll")
                    .Set("sha256", std::string(64, '1'));
  prefs.SetDict(kDriverPref, driver.Clone());
  EXPECT_FALSE(ReadDriver(&prefs));
  prefs.SetManagedPref(kDriverPref, base::Value(driver.Clone()));
  ASSERT_TRUE(ReadDriver(&prefs));
  EXPECT_EQ(ReadDriver(&prefs)->sha256.size(), 32u);
  for (const char* path :
       {"SKF.dll", "\\\\server\\SKF.dll", "C:\\..\\SKF.dll"}) {
    prefs.SetManagedPref(kDriverPref,
                         base::Value(driver.Clone().Set("path", path)));
    EXPECT_FALSE(ReadDriver(&prefs)) << path;
  }
  prefs.SetManagedPref(kDriverPref,
                       base::Value(driver.Clone().Set("sha256", "0011")));
  EXPECT_FALSE(ReadDriver(&prefs));
}
}  // namespace
}  // namespace gm_browser
