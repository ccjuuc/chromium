// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_crash_reporter_bridge.h"

#include <array>

#include "base/strings/string_util.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "components/crash/core/app/crashpad.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

TEST(ElectronCrashReporterBridgeTest,
     FormatsOnlyUploadedReportsFromSharedDatabaseResults) {
  std::array<crash_reporter::Report, 4> reports{};
  reports[0].state = crash_reporter::ReportUploadState::Pending;
  reports[0].capture_time = 500;
  reports[1].state = crash_reporter::ReportUploadState::Uploaded;
  reports[1].capture_time = 100;
  base::strlcpy(reports[1].remote_id, "older", sizeof(reports[1].remote_id));
  reports[2].state = crash_reporter::ReportUploadState::NotUploaded;
  reports[2].capture_time = 400;
  reports[3].state = crash_reporter::ReportUploadState::Uploaded;
  reports[3].capture_time = 200;
  base::strlcpy(reports[3].remote_id, "newer", sizeof(reports[3].remote_id));
  const auto result = UploadedCrashReportsToValue(reports);
  ASSERT_EQ(2u, result.size());
  EXPECT_EQ("newer", *result[0].GetDict().FindString("id"));
  EXPECT_EQ(200000.0, result[0].GetDict().FindDouble("date"));
  EXPECT_EQ("older", *result[1].GetDict().FindString("id"));
}

TEST(ElectronCrashReporterBridgeTest,
     RejectsConfigurationWritesAndReadsActualHostState) {
  base::test::TaskEnvironment environment;
  base::test::TestFuture<mojom::IpcResultPtr> future;
  CallCrashReporterApi("crashReporter.setUploadToServer", future.GetCallback());
  auto result = future.Take();
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->error.starts_with("ERR_NOT_SUPPORTED:"));
  CallCrashReporterApi("crashReporter.getUploadToServer", future.GetCallback());
  result = future.Take();
  ASSERT_TRUE(result->success);
  // This standalone test never initializes a handler or a database. The result
  // is still derived from the host API, not a JS option cached by the shim.
  EXPECT_EQ(crash_reporter::IsCrashUploadEnabled(), result->value.GetBool());
  CallCrashReporterApi("crashReporter.getUploadedReports",
                       future.GetCallback());
  result = future.Take();
  ASSERT_TRUE(result->success);
  EXPECT_TRUE(result->value.is_list());
}

}  // namespace
}  // namespace xenon::ipc
