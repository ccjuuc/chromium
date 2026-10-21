// Copyright 2020 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/updater/external_constants_override.h"

#include <memory>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/memory/scoped_refptr.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "base/values.h"
#include "chrome/updater/configurator.h"
#include "chrome/updater/constants.h"
#include "chrome/updater/event_logger.h"
#include "chrome/updater/external_constants.h"
#include "chrome/updater/external_constants_default.h"
#include "chrome/updater/persisted_data.h"
#include "chrome/updater/prefs_impl.h"
#include "chrome/updater/test/test_scope.h"
#include "chrome/updater/update_service_impl_impl.h"
#include "chrome/updater/updater_branding.h"
#include "components/crx_file/crx_verifier.h"
#include "components/policy/core/common/policy_types.h"
#include "components/prefs/pref_registry_simple.h"
#include "components/prefs/testing_pref_service.h"
#include "components/update_client/update_client.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace updater {

class ExternalConstantsOverriderTest : public ::testing::Test {};

#if UPDATER_IS_XENON_BRANDED
TEST(XenonUpdaterTrustTest, UsesOwnPinnedCrxKeyWithoutGoogleServices) {
  const auto constants = CreateDefaultExternalConstants();
  EXPECT_FALSE(constants->UseCUP());
  EXPECT_EQ(constants->CrxVerifierFormat(), crx_file::VerifierFormat::CRX3);
  const auto pin = constants->CrxPublicKeyHash();
  ASSERT_TRUE(pin);
  EXPECT_EQ(pin->size(), 32u);
  EXPECT_EQ(pin, base::Base64Decode(CRX_PKHASH));
  EXPECT_TRUE(constants->CrashUploadURL().is_empty());
  EXPECT_TRUE(constants->EventLoggingURL().is_empty());
  EXPECT_TRUE(constants->AppLogoURL().is_empty());
}

TEST(XenonUpdaterTrustTest, OverridesCannotChangeTrustOrStartCloudServices) {
  base::test::TaskEnvironment environment;
  auto prefs = std::make_unique<TestingPrefServiceSimple>();
  update_client::RegisterPrefs(prefs->registry());
  RegisterPersistedDataPrefs(prefs->registry());
  base::DictValue overrides;
  overrides.Set(kDevOverrideKeyUrl, "http://untrusted.example.test/omaha");
  overrides.Set(kDevOverrideKeyUseCUP, true);
  overrides.Set(kDevOverrideKeyCrxPublicKeyHash, "");
  overrides.Set(
      kDevOverrideKeyCrxVerifierFormat,
      static_cast<int>(crx_file::VerifierFormat::CRX3_WITH_PUBLISHER_PROOF));
  overrides.Set(kDevOverrideKeyEventLoggingUrl,
                "https://untrusted.example.test/events");
  auto configurator = base::MakeRefCounted<Configurator>(
      base::MakeRefCounted<UpdaterPrefsImpl>(base::FilePath(), nullptr,
                                             std::move(prefs)),
      base::MakeRefCounted<ExternalConstantsOverrider>(
          std::move(overrides), CreateDefaultExternalConstants()),
      GetUpdaterScopeForTesting());
  EXPECT_EQ(configurator->UpdateUrl(),
            CreateDefaultExternalConstants()->UpdateURL());
  EXPECT_FALSE(configurator->EnabledCupSigning());
  EXPECT_EQ(configurator->GetCrxVerifierFormat(),
            crx_file::VerifierFormat::CRX3);
  EXPECT_EQ(configurator->GetCrxPublicKeyHash(),
            base::Base64Decode(CRX_PKHASH));
  EXPECT_FALSE(configurator->GetEventLogger());

  auto service = base::MakeRefCounted<UpdateServiceImplImpl>(configurator);
  base::test::TestFuture<int> completed;
  service->FetchPolicies(policy::PolicyFetchReason::kUserRequest,
                         completed.GetCallback());
  EXPECT_FALSE(completed.IsReady());
  EXPECT_EQ(completed.Get(), 0);
}
#endif

TEST_F(ExternalConstantsOverriderTest, TestEmptyDictValue) {
  auto overrider = base::MakeRefCounted<ExternalConstantsOverrider>(
      base::DictValue(), CreateDefaultExternalConstants());

  EXPECT_EQ(overrider->UseCUP(), !UPDATER_IS_XENON_BRANDED);

  std::vector<GURL> urls = overrider->UpdateURL();
  ASSERT_EQ(urls.size(), 1ul);
  EXPECT_EQ(urls[0], GURL(UPDATE_CHECK_URL));
  EXPECT_TRUE(urls[0].is_valid());

  EXPECT_EQ(overrider->CrashUploadURL(), GURL(CRASH_UPLOAD_URL));
  EXPECT_EQ(overrider->CrashUploadURL().is_valid(), !UPDATER_IS_XENON_BRANDED);
  EXPECT_EQ(overrider->AppLogoURL(), GURL(APP_LOGO_URL));
  EXPECT_EQ(overrider->AppLogoURL().is_valid(), !UPDATER_IS_XENON_BRANDED);

  EXPECT_EQ(overrider->InitialDelay(), kInitialDelay);
  EXPECT_EQ(overrider->ServerKeepAliveTime(), kServerKeepAliveTime);
  EXPECT_EQ(overrider->DictPolicies().size(), 0U);
  EXPECT_EQ(overrider->CecaConnectionTimeout(), kCecaConnectionTimeout);
}

TEST_F(ExternalConstantsOverriderTest, TestFullOverrides) {
  base::DictValue overrides;
  base::ListValue url_list;
  url_list.Append("https://localhost/1/www");
  url_list.Append("https://localhost/2/www");
  base::DictValue dict_policies;
  dict_policies.Set("a", 1);
  dict_policies.Set("b", 2);

  overrides.Set(kDevOverrideKeyUseCUP, false);
  overrides.Set(kDevOverrideKeyUrl, std::move(url_list));
  overrides.Set(kDevOverrideKeyCrashUploadUrl,
                "https://localhost/2/crash_test");
  overrides.Set(kDevOverrideKeyAppLogoUrl, "https://localhost/2/applogo/");
  overrides.Set(kDevOverrideKeyInitialDelay, 137.1);
  overrides.Set(kDevOverrideKeyServerKeepAliveSeconds, 1);
  overrides.Set(kDevOverrideKeyDictPolicies, std::move(dict_policies));
  overrides.Set(kDevOverrideKeyOverinstallTimeout, 3);
  overrides.Set(kDevOverrideKeyIdleCheckPeriodSeconds, 4);
  overrides.Set(kDevOverrideKeyCecaConnectionTimeout, 27);
  auto overrider = base::MakeRefCounted<ExternalConstantsOverrider>(
      std::move(overrides), CreateDefaultExternalConstants());

  EXPECT_FALSE(overrider->UseCUP());

  std::vector<GURL> urls = overrider->UpdateURL();
  ASSERT_EQ(urls.size(), 2ul);
  EXPECT_EQ(urls[0], GURL("https://localhost/1/www"));
  EXPECT_TRUE(urls[0].is_valid());
  EXPECT_EQ(urls[1], GURL("https://localhost/2/www"));
  EXPECT_TRUE(urls[1].is_valid());

  EXPECT_EQ(overrider->CrashUploadURL(),
            GURL("https://localhost/2/crash_test"));
  EXPECT_TRUE(overrider->CrashUploadURL().is_valid());
  EXPECT_EQ(overrider->AppLogoURL(), GURL("https://localhost/2/applogo/"));
  EXPECT_TRUE(overrider->AppLogoURL().is_valid());

  EXPECT_EQ(overrider->InitialDelay(), base::Seconds(137.1));
  EXPECT_EQ(overrider->ServerKeepAliveTime(), base::Seconds(1));
  EXPECT_EQ(overrider->DictPolicies().size(), 2U);
  EXPECT_EQ(overrider->OverinstallTimeout(), base::Seconds(3));
  EXPECT_EQ(overrider->IdleCheckPeriod(), base::Seconds(4));
  EXPECT_EQ(overrider->CecaConnectionTimeout(), base::Seconds(27));
}

TEST_F(ExternalConstantsOverriderTest, TestOverrideUnwrappedURL) {
  base::DictValue overrides;
  overrides.Set(kDevOverrideKeyUrl, "https://localhost/1/www");
  auto overrider = base::MakeRefCounted<ExternalConstantsOverrider>(
      std::move(overrides), CreateDefaultExternalConstants());

  std::vector<GURL> urls = overrider->UpdateURL();
  ASSERT_EQ(urls.size(), 1ul);
  EXPECT_EQ(urls[0], GURL("https://localhost/1/www"));
  EXPECT_TRUE(urls[0].is_valid());

  // Non-overridden items should fall back to defaults
  EXPECT_EQ(overrider->UseCUP(), !UPDATER_IS_XENON_BRANDED);
  EXPECT_EQ(overrider->InitialDelay(), kInitialDelay);
  EXPECT_EQ(overrider->ServerKeepAliveTime(), kServerKeepAliveTime);
  EXPECT_EQ(overrider->DictPolicies().size(), 0U);
  EXPECT_EQ(overrider->CecaConnectionTimeout(), kCecaConnectionTimeout);
}

}  // namespace updater
