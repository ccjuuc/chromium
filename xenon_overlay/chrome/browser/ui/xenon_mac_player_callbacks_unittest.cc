// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_callbacks.h"

#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/ui/adapters/xunlei_mac_player_adapter.h"
#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_adapter.h"

namespace xenon {
namespace {

base::Value Callback(int id) {
  return base::Value(base::DictValue()
                         .Set("__xenon_node_wire_type__", "callback")
                         .Set("callback_id", id));
}

base::Value Completion() {
  return base::Value(base::DictValue()
                         .Set("__xenon_node_wire_type__", "native_function")
                         .Set("instance_id", 71)
                         .Set("owner_token", "owner-a"));
}

base::ListValue NativeArgs(size_t count) {
  base::ListValue args;
  for (size_t i = 1; i < count; ++i) {
    args.Append("");
  }
  args.Append(Completion());
  return args;
}

TEST(XenonMacPlayerCallbacksTest, UsesSelectedAdapterCompletionContract) {
  const MacPlayerCallbackContract contracts[] = {
      {"registerLookup", "lookup", 2,
       +[]() { return base::ListValue().Append("cancelled").Append(-1); }},
      {"registerNotice", "", 0, nullptr}};
  std::vector<MacPlayerCallbackRegistration> registrations;
  std::string error;
  ASSERT_TRUE(ReadMacPlayerCallbackRegistrations(
      contracts, "registerLookup",
      base::ListValue().Append(base::ListValue().Append(
          base::DictValue().Set("lookup", Callback(4)))),
      &registrations, &error));
  ASSERT_EQ(1u, registrations.size());
  std::optional<MacPlayerCompletion> completion;
  ASSERT_TRUE(ReadMacPlayerCompletion(contracts, registrations[0],
                                      NativeArgs(2), &completion, &error));
  ASSERT_TRUE(completion);
  EXPECT_EQ(base::ListValue().Append("cancelled").Append(-1),
            completion->failure_args);
  ASSERT_TRUE(ReadMacPlayerCompletion(contracts, {5, "registerNotice", ""}, {},
                                      &completion, &error));
  EXPECT_FALSE(completion);
  EXPECT_FALSE(ReadMacPlayerCallbackRegistrations(
      contracts, "registerPanApi", base::ListValue().Append(base::ListValue()),
      &registrations, &error));
}

TEST(XenonMacPlayerCallbacksTest, ReadsNestedProvidersAtomically) {
  base::ListValue providers;
  providers.Append(base::DictValue().Set("GetFileInfo", Callback(2)));
  providers.Append(base::DictValue().Set("ReportParam", Callback(3)));
  base::ListValue args;
  args.Append(providers.Clone());
  std::vector<MacPlayerCallbackRegistration> registrations;
  std::string error;
  ASSERT_TRUE(ReadMacPlayerCallbackRegistrations(
      XunleiMacPlayerAdapterV1().callbacks, "registerPanApi", args,
      &registrations, &error));
  ASSERT_EQ(registrations.size(), 2u);
  EXPECT_EQ(registrations[0].callback_id, 2);
  EXPECT_EQ(registrations[0].provider, "GetFileInfo");
  EXPECT_EQ(registrations[1].provider, "ReportParam");

  providers.Append(base::DictValue().Set("UnknownFutureAPI", Callback(4)));
  args[0] = base::Value(providers.Clone());
  EXPECT_FALSE(ReadMacPlayerCallbackRegistrations(
      XunleiMacPlayerAdapterV1().callbacks, "registerPanApi", args,
      &registrations, &error));
  EXPECT_EQ(registrations.size(), 2u);

  args[0] = base::Value(base::ListValue().Append(
      base::DictValue().Set("GetFileInfo", Callback(0))));
  EXPECT_FALSE(ReadMacPlayerCallbackRegistrations(
      XunleiMacPlayerAdapterV1().callbacks, "registerPanApi", args,
      &registrations, &error));
  EXPECT_FALSE(ReadMacPlayerCallbackRegistrations(
      XunleiMacPlayerAdapterV1().callbacks, "registerUniversalApi", args,
      &registrations, &error));
}

TEST(XenonMacPlayerCallbacksTest, PreservesNativeFailureParameterTypes) {
  const struct {
    const char* method;
    const char* provider;
    size_t count;
    int failure_kind;
  } cases[] = {
      {"setGlobalHttpRequest", "", 6, 0},
      {"registerPanApi", "GetFileInfo", 3, 1},
      {"registerUniversalApi", "GetSpeedStrategy", 8, 2},
      {"registerUniversalApi", "GetDefaultSpeedStrategy", 2, 2},
      {"registerMediaStorageOnlineApi", "GetPlayInfoByUrl", 2, 2},
      {"registerPanApi", "NasGetFileLastPostion", 3, 3},
      {"registerPanApi", "NasGetFileLastPostion", 4, 4},
      {"registerPanApi", "GetNasSubtitleFilesInSameDirectory", 4, 5},
  };
  for (const auto& item : cases) {
    SCOPED_TRACE(item.provider);
    MacPlayerCallbackRegistration registration{2, item.method, item.provider};
    std::optional<MacPlayerCompletion> completion;
    std::string error;
    ASSERT_TRUE(ReadMacPlayerCompletion(XunleiMacPlayerAdapterV1().callbacks,
                                        registration, NativeArgs(item.count),
                                        &completion, &error));
    ASSERT_TRUE(completion);
    EXPECT_EQ(completion->instance_id, 71);
    EXPECT_EQ(completion->owner_token, "owner-a");
    const auto& failure = completion->failure_args;
    ASSERT_EQ(failure.size(), item.failure_kind == 2 ? 2u : 1u);
    switch (item.failure_kind) {
      case 0:
        EXPECT_EQ(failure[0], base::Value(""));
        break;
      case 1:
        ASSERT_TRUE(failure[0].is_dict());
        EXPECT_EQ(failure[0].GetDict().FindInt("success"), 0);
        break;
      case 2:
        EXPECT_EQ(failure[0], base::Value(false));
        EXPECT_EQ(failure[1], base::Value(""));
        break;
      case 3:
        EXPECT_EQ(failure[0], base::Value(0));
        break;
      case 4:
        EXPECT_EQ(failure[0], base::Value(false));
        break;
      case 5:
        ASSERT_TRUE(failure[0].is_dict());
        ASSERT_TRUE(failure[0].GetDict().FindList("files"));
        EXPECT_TRUE(failure[0].GetDict().FindList("files")->empty());
        break;
    }
  }
}

TEST(XenonMacPlayerCallbacksTest, RejectsUnexpectedCompletionsAndOwners) {
  MacPlayerCallbackRegistration registration{2, "registerPanApi",
                                             "GetFileInfo"};
  std::optional<MacPlayerCompletion> completion;
  std::string error;
  EXPECT_FALSE(ReadMacPlayerCompletion(XunleiMacPlayerAdapterV1().callbacks,
                                       registration, NativeArgs(2), &completion,
                                       &error));
  auto args = NativeArgs(3);
  args[2].GetDict().Remove("owner_token");
  EXPECT_FALSE(ReadMacPlayerCompletion(XunleiMacPlayerAdapterV1().callbacks,
                                       registration, args, &completion,
                                       &error));
  args = NativeArgs(3);
  args[0] = Completion();
  EXPECT_FALSE(ReadMacPlayerCompletion(XunleiMacPlayerAdapterV1().callbacks,
                                       registration, args, &completion,
                                       &error));
  args = NativeArgs(3);
  args[2] = Callback(4);
  EXPECT_FALSE(ReadMacPlayerCompletion(XunleiMacPlayerAdapterV1().callbacks,
                                       registration, args, &completion,
                                       &error));
  EXPECT_FALSE(completion);
}

TEST(XenonMacPlayerCallbacksTest, NotificationsDoNotFabricateCompletions) {
  std::optional<MacPlayerCompletion> completion;
  std::string error;
  EXPECT_TRUE(ReadMacPlayerCompletion(
      XunleiMacPlayerAdapterV1().callbacks, {1, "addStatCallback", ""},
      base::ListValue().Append("event").Append("value"), &completion, &error));
  EXPECT_FALSE(completion);
  EXPECT_TRUE(ReadMacPlayerCompletion(
      XunleiMacPlayerAdapterV1().callbacks,
      {2, "registerPanApi", "ReportParam"},
      base::ListValue().Append(base::DictValue().Set("event", "play")),
      &completion, &error));
  EXPECT_FALSE(completion);
  EXPECT_FALSE(ReadMacPlayerCompletion(
      XunleiMacPlayerAdapterV1().callbacks,
      {2, "registerPanApi", "ReportParam"},
      base::ListValue().Append(base::DictValue().Set("callback", Completion())),
      &completion, &error));
}

}  // namespace
}  // namespace xenon
