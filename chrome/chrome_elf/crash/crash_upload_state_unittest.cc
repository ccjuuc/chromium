// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <optional>

#include "base/files/scoped_temp_dir.h"
#include "base/scoped_environment_variable_override.h"
#include "chrome/app/chrome_crash_reporter_client_win.h"
#include "components/crash/core/app/crash_export_thunks.h"
#include "components/crash/core/app/crashpad.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/crashpad/crashpad/client/crash_report_database.h"
#include "third_party/crashpad/crashpad/client/settings.h"

namespace {

// Keep the real product-consent resolver, but never consult the user's
// registry.
class TestCrashReporterClient : public ChromeCrashReporterClient {
 public:
  bool GetCollectStatsConsent() override { return false; }
  bool GetCollectStatsInSample() override { return in_sample; }
  bool IsRunningUnattended() override { return unattended; }
  bool ReportingIsEnforcedByPolicy(bool* enabled) override {
    if (!policy.has_value()) {
      return false;
    }
    *enabled = *policy;
    return true;
  }

  bool in_sample = true;
  bool unattended = false;
  std::optional<bool> policy;
};

class CrashpadUploadStateTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_FALSE(crash_reporter::internal::GetCrashReportDatabase());
    ASSERT_TRUE(database_dir_.CreateUniqueTempDir());
    database_path_ = database_dir_.GetPath();
    database_ = crashpad::CrashReportDatabase::Initialize(database_path_);
    ASSERT_TRUE(database_);
    // This test executable does not initialize Crashpad. Only a temporary
    // database and a local client are installed; no handler is started.
    crash_reporter::SetCrashReporterClient(&client_);
    crash_reporter::internal::SetCrashReportDatabaseForTesting(database_.get(),
                                                               &database_path_);
    crash_reporter::internal::SetCrashpadUploadConfigured(true);
  }

  void TearDown() override {
    crash_reporter::internal::SetCrashReportDatabaseForTesting(nullptr,
                                                               nullptr);
    crash_reporter::internal::SetCrashpadUploadConfigured(false);
    crash_reporter::SetCrashReporterClient(nullptr);
  }

  TestCrashReporterClient client_;
  base::ScopedTempDir database_dir_;
  base::FilePath database_path_;
  std::unique_ptr<crashpad::CrashReportDatabase> database_;
};

TEST_F(CrashpadUploadStateTest, ExplicitConsentSurvivesMetricsUpdates) {
  base::ScopedEnvironmentVariableOverride enabled("XENON_CRASH_UPLOAD_ENABLED",
                                                  "1");
  ASSERT_TRUE(enabled.IsOverridden());
  crash_reporter::SetUploadConsent(false);
  EXPECT_TRUE(crash_reporter::IsCrashUploadEnabled());
  // General metrics/JS-error consent remains unchanged by the crash setting.
  EXPECT_FALSE(GetUploadConsent_ExportThunk());
  crash_reporter::SetUploadConsent(true);
  crash_reporter::SetUploadConsent(false);
  EXPECT_TRUE(crash_reporter::IsCrashUploadEnabled());
}

TEST_F(CrashpadUploadStateTest, ExplicitOptOutOverridesMetricsConsent) {
  base::ScopedEnvironmentVariableOverride disabled("XENON_CRASH_UPLOAD_ENABLED",
                                                   "0");
  ASSERT_TRUE(disabled.IsOverridden());
  crash_reporter::SetUploadConsent(true);
  EXPECT_FALSE(crash_reporter::IsCrashUploadEnabled());
}

TEST_F(CrashpadUploadStateTest, PolicyCanDisableExplicitConsent) {
  base::ScopedEnvironmentVariableOverride enabled("XENON_CRASH_UPLOAD_ENABLED",
                                                  "1");
  ASSERT_TRUE(enabled.IsOverridden());
  client_.policy = false;
  crash_reporter::SetUploadConsent(true);
  EXPECT_FALSE(crash_reporter::IsCrashUploadEnabled());
}

TEST_F(CrashpadUploadStateTest, PolicyCanEnableButSamplingStillApplies) {
  base::ScopedEnvironmentVariableOverride disabled("XENON_CRASH_UPLOAD_ENABLED",
                                                   "0");
  ASSERT_TRUE(disabled.IsOverridden());
  client_.policy = true;
  crash_reporter::SetUploadConsent(false);
  EXPECT_TRUE(crash_reporter::IsCrashUploadEnabled());
  client_.in_sample = false;
  crash_reporter::SetUploadConsent(false);
  EXPECT_FALSE(crash_reporter::IsCrashUploadEnabled());
}

TEST_F(CrashpadUploadStateTest, UnattendedBlocksConsentUnlessPolicyEnforces) {
  base::ScopedEnvironmentVariableOverride enabled("XENON_CRASH_UPLOAD_ENABLED",
                                                  "1");
  ASSERT_TRUE(enabled.IsOverridden());
  client_.unattended = true;
  crash_reporter::SetUploadConsent(false);
  EXPECT_FALSE(crash_reporter::IsCrashUploadEnabled());
  // Preserve the existing precedence of enterprise policy over unattended mode.
  client_.policy = true;
  crash_reporter::SetUploadConsent(false);
  EXPECT_TRUE(crash_reporter::IsCrashUploadEnabled());
}

TEST_F(CrashpadUploadStateTest, ReadsDatabaseSettingInsteadOfClientConsent) {
  ASSERT_TRUE(database_->GetSettings()->SetUploadsEnabled(true));
  EXPECT_TRUE(crash_reporter::IsCrashUploadEnabled());
  ASSERT_TRUE(database_->GetSettings()->SetUploadsEnabled(false));
  EXPECT_FALSE(crash_reporter::IsCrashUploadEnabled());
}

TEST_F(CrashpadUploadStateTest, MissingUploadEndpointIsDisabled) {
  ASSERT_TRUE(database_->GetSettings()->SetUploadsEnabled(true));
  crash_reporter::internal::SetCrashpadUploadConfigured(false);
  EXPECT_FALSE(crash_reporter::IsCrashUploadEnabled());
}

TEST_F(CrashpadUploadStateTest, MissingDatabaseIsDisabled) {
  crash_reporter::internal::SetCrashReportDatabaseForTesting(nullptr, nullptr);
  EXPECT_FALSE(crash_reporter::IsCrashUploadEnabled());
}

}  // namespace
