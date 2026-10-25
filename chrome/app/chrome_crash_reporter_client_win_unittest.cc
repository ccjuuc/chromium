// Copyright 2022 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/app/chrome_crash_reporter_client_win.h"

#include <string>

#include "base/environment.h"
#include "base/files/file_path.h"
#include "base/scoped_environment_variable_override.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/buildflags/crash_buildflags.h"

namespace {

TEST(ChromeCrashReporterClientWin, GetWerModule) {
  ChromeCrashReporterClient client;

  std::wstring wer_module = client.GetWerRuntimeExceptionModule();
  ASSERT_FALSE(wer_module.empty());
  base::FilePath as_file_path(wer_module);
  ASSERT_EQ(as_file_path.Extension(), FILE_PATH_LITERAL(".dll"));
}

TEST(ChromeCrashReporterClientTest,
     ExplicitConsentSurvivesMetricsConsentChanges) {
  base::ScopedEnvironmentVariableOverride enabled("XENON_CRASH_UPLOAD_ENABLED",
                                                  "1");
  ASSERT_TRUE(enabled.IsOverridden());
  ChromeCrashReporterClient client;

  // Browser startup can subsequently report metrics consent as false. That
  // update must not undo an explicit crash upload setting.
  EXPECT_TRUE(client.GetCrashUploadConsent(true));
  EXPECT_TRUE(client.GetCrashUploadConsent(false));
}

TEST(ChromeCrashReporterClientTest, ExplicitOptOutOverridesMetricsConsent) {
  base::ScopedEnvironmentVariableOverride disabled("XENON_CRASH_UPLOAD_ENABLED",
                                                   "0");
  ASSERT_TRUE(disabled.IsOverridden());
  ChromeCrashReporterClient client;

  EXPECT_FALSE(client.GetCrashUploadConsent(false));
  EXPECT_FALSE(client.GetCrashUploadConsent(true));
}

TEST(ChromeCrashReporterClientTest,
     UnsetConsentUsesBuildSettingOrMetricsConsent) {
  base::ScopedEnvironmentVariableOverride unset("XENON_CRASH_UPLOAD_ENABLED");
  ASSERT_FALSE(unset.GetEnv()->HasVar("XENON_CRASH_UPLOAD_ENABLED"));
  ChromeCrashReporterClient client;

  EXPECT_EQ(static_cast<bool>(BUILDFLAG(XENON_CRASH_UPLOAD_ENABLED)),
            client.GetCrashUploadConsent(false));
  EXPECT_TRUE(client.GetCrashUploadConsent(true));
}

TEST(ChromeCrashReporterClientTest,
     InvalidConsentUsesBuildSettingOrMetricsConsent) {
  base::ScopedEnvironmentVariableOverride invalid("XENON_CRASH_UPLOAD_ENABLED",
                                                  "invalid");
  ASSERT_TRUE(invalid.IsOverridden());
  ChromeCrashReporterClient client;

  EXPECT_EQ(static_cast<bool>(BUILDFLAG(XENON_CRASH_UPLOAD_ENABLED)),
            client.GetCrashUploadConsent(false));
  EXPECT_TRUE(client.GetCrashUploadConsent(true));
}

TEST(ChromeCrashReporterClientTest, BinaryFormatOverride) {
  base::ScopedEnvironmentVariableOverride binary("XENON_CRASH_UPLOAD_FORMAT",
                                                 "binary");
  ASSERT_TRUE(binary.IsOverridden());
  ChromeCrashReporterClient client;

  EXPECT_TRUE(client.ShouldUploadDumpAsBinary());
}

TEST(ChromeCrashReporterClientTest, MultipartFormatOverride) {
  base::ScopedEnvironmentVariableOverride multipart("XENON_CRASH_UPLOAD_FORMAT",
                                                    "multipart");
  ASSERT_TRUE(multipart.IsOverridden());
  ChromeCrashReporterClient client;

  EXPECT_FALSE(client.ShouldUploadDumpAsBinary());
}

TEST(ChromeCrashReporterClientTest, UnsetFormatUsesBuildSetting) {
  base::ScopedEnvironmentVariableOverride unset("XENON_CRASH_UPLOAD_FORMAT");
  ASSERT_FALSE(unset.GetEnv()->HasVar("XENON_CRASH_UPLOAD_FORMAT"));
  ChromeCrashReporterClient client;

  EXPECT_EQ(static_cast<bool>(BUILDFLAG(XENON_CRASH_UPLOAD_BINARY)),
            client.ShouldUploadDumpAsBinary());
}

TEST(ChromeCrashReporterClientTest, InvalidFormatUsesBuildSetting) {
  base::ScopedEnvironmentVariableOverride invalid("XENON_CRASH_UPLOAD_FORMAT",
                                                  "invalid");
  ASSERT_TRUE(invalid.IsOverridden());
  ChromeCrashReporterClient client;

  EXPECT_EQ(static_cast<bool>(BUILDFLAG(XENON_CRASH_UPLOAD_BINARY)),
            client.ShouldUploadDumpAsBinary());
}

}  // namespace
