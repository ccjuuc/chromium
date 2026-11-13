// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_crash_reporter_annotations.h"

#include <string>

#include "components/crash/core/common/crash_key.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::ipc {
namespace {

class ElectronCrashReporterAnnotationsTest : public testing::Test {
 protected:
  void SetUp() override { crash_reporter::InitializeCrashKeysForTesting(); }
  // Annotation list nodes must never be removed. No dump is generated and no
  // Crashpad client, handler, database or uploader is initialized by these
  // tests.
  ElectronCrashReporterAnnotations annotations_{true};
};

TEST_F(ElectronCrashReporterAnnotationsTest,
       StartRecordsApplicationWithoutReplacingHostKeys) {
  const auto host_product = crash_reporter::GetCrashKeyValue("prod");
  const auto host_version = crash_reporter::GetCrashKeyValue("ver");
  const auto host_type = crash_reporter::GetCrashKeyValue("ptype");
  auto result = annotations_.Call(base::Value(
      base::DictValue()
          .Set("operation", "start")
          .Set("productName", "Fixture app")
          .Set("companyName", "Fixture company")
          .Set("appVersion", "1.2.3")
          .Set("extra", base::DictValue().Set("prod", "application extra"))));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ("Fixture app",
            crash_reporter::GetCrashKeyValue("xenon-electron-productName"));
  EXPECT_EQ("Fixture company",
            crash_reporter::GetCrashKeyValue("xenon-electron-companyName"));
  EXPECT_EQ("1.2.3",
            crash_reporter::GetCrashKeyValue("xenon-electron-appVersion"));
  EXPECT_EQ("main", crash_reporter::GetCrashKeyValue("xenon-electron-role"));
  EXPECT_EQ("application extra",
            crash_reporter::GetCrashKeyValue("xenon-electron-extra-prod"));
  EXPECT_EQ(host_product, crash_reporter::GetCrashKeyValue("prod"));
  EXPECT_EQ(host_version, crash_reporter::GetCrashKeyValue("ver"));
  EXPECT_EQ(host_type, crash_reporter::GetCrashKeyValue("ptype"));

  result = annotations_.Call(
      base::Value(base::DictValue()
                      .Set("operation", "start")
                      .Set("productName", "ignored second start")));
  ASSERT_TRUE(result->success);
  EXPECT_EQ("Fixture app",
            crash_reporter::GetCrashKeyValue("xenon-electron-productName"));
  annotations_.Clear();
  EXPECT_TRUE(
      crash_reporter::GetCrashKeyValue("xenon-electron-productName").empty());
  EXPECT_EQ(host_product, crash_reporter::GetCrashKeyValue("prod"));
}

TEST_F(ElectronCrashReporterAnnotationsTest,
       StoresFullElectronValueLimitAndRemovesIt) {
  std::string value(20319, 'x');
  value +=
      "\xe4\xb8\xad";  // Truncation must not keep half of a UTF-8 character.
  auto result =
      annotations_.Call(base::Value(base::DictValue()
                                        .Set("operation", "addExtraParameter")
                                        .Set("key", "limit")
                                        .Set("value", value)));
  ASSERT_TRUE(result->success) << result->error;
  EXPECT_EQ(std::string(20319, 'x'),
            crash_reporter::GetCrashKeyValue("xenon-electron-extra-limit"));
  result = annotations_.Call(
      base::Value(base::DictValue()
                      .Set("operation", "addExtraParameter")
                      .Set("key", "limit")
                      .Set("value", std::string(20321, 'y'))));
  ASSERT_TRUE(result->success);
  EXPECT_EQ(std::string(20320, 'y'),
            crash_reporter::GetCrashKeyValue("xenon-electron-extra-limit"));
  result = annotations_.Call(
      base::Value(base::DictValue().Set("operation", "getParameters")));
  ASSERT_TRUE(result->success);
  ASSERT_TRUE(result->value.GetDict().FindString("limit"));
  EXPECT_EQ(20320u, result->value.GetDict().FindString("limit")->size());
  result = annotations_.Call(
      base::Value(base::DictValue()
                      .Set("operation", "removeExtraParameter")
                      .Set("key", "limit")));
  ASSERT_TRUE(result->success);
  EXPECT_TRUE(
      crash_reporter::GetCrashKeyValue("xenon-electron-extra-limit").empty());
}

TEST_F(ElectronCrashReporterAnnotationsTest,
       RejectsInvalidNamesAndUnimplementedGlobalPropagation) {
  for (const auto& name :
       {std::string(), std::string(40, 'x'), std::string("a\0b", 3)}) {
    auto result =
        annotations_.Call(base::Value(base::DictValue()
                                          .Set("operation", "addExtraParameter")
                                          .Set("key", name)
                                          .Set("value", "data")));
    EXPECT_FALSE(result->success);
  }
  auto invalid_extra = annotations_.Call(base::Value(
      base::DictValue()
          .Set("operation", "start")
          .Set("productName", "must not be applied")
          .Set("extra",
               base::DictValue().Set("valid", "data").Set("invalid", 42))));
  EXPECT_FALSE(invalid_extra->success);
  EXPECT_TRUE(
      crash_reporter::GetCrashKeyValue("xenon-electron-productName").empty());
  EXPECT_TRUE(
      crash_reporter::GetCrashKeyValue("xenon-electron-extra-valid").empty());
  auto global = annotations_.Call(base::Value(
      base::DictValue()
          .Set("operation", "start")
          .Set("globalExtra", base::DictValue().Set("global", "data"))));
  EXPECT_FALSE(global->success);
  EXPECT_TRUE(global->error.starts_with("ERR_NOT_SUPPORTED:"));
  auto upload = annotations_.Call(
      base::Value(base::DictValue().Set("operation", "setUploadToServer")));
  EXPECT_FALSE(upload->success);
}

TEST_F(ElectronCrashReporterAnnotationsTest,
       ScopeCleanupDoesNotClearAnotherOwnersLatestValue) {
  ElectronCrashReporterAnnotations renderer;
  auto result =
      annotations_.Call(base::Value(base::DictValue()
                                        .Set("operation", "addExtraParameter")
                                        .Set("key", "ownership")
                                        .Set("value", "first")));
  ASSERT_TRUE(result->success);
  result = renderer.Call(base::Value(base::DictValue()
                                         .Set("operation", "addExtraParameter")
                                         .Set("key", "ownership")
                                         .Set("value", "second")));
  ASSERT_TRUE(result->success);
  annotations_.Clear();
  EXPECT_EQ("second",
            crash_reporter::GetCrashKeyValue("xenon-electron-extra-ownership"));
  result =
      renderer.Call(base::Value(base::DictValue().Set("operation", "start")));
  EXPECT_FALSE(result->success);
  EXPECT_TRUE(result->error.starts_with("ERR_NOT_SUPPORTED:"));
  renderer.Clear();
  EXPECT_TRUE(crash_reporter::GetCrashKeyValue("xenon-electron-extra-ownership")
                  .empty());
}

}  // namespace
}  // namespace xenon::ipc
