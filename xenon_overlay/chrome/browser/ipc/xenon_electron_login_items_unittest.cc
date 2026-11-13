// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_login_items.h"

#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(IS_WIN)
#include "base/test/file_path_reparse_point_win.h"
#include "base/test/test_reg_util_win.h"
#include "base/win/registry.h"
#endif

namespace xenon::ipc {
namespace {
class ElectronLoginItemsTest : public testing::Test {
 protected:
#if BUILDFLAG(IS_WIN)
  void SetUp() override {
    ASSERT_TRUE(directory_.CreateUniqueTempDir(L"li"));
    ASSERT_TRUE(
        base::CreateDirectory(directory_.GetPath().AppendASCII("profile")));
    ASSERT_TRUE(base::WriteFile(directory_.GetPath().AppendASCII("app.asar"),
                                "fixture"));
  }
  ElectronLoginItemContext Context() {
    base::FilePath profile;
    EXPECT_TRUE(base::NormalizeFilePath(
        directory_.GetPath().AppendASCII("profile"), &profile));
    return {.application_path = directory_.GetPath().AppendASCII("app.asar"),
            .launch_path = base::FilePath(L"C:\\Apps\\Example Launch.json"),
            .executable_path = base::FilePath(L"C:\\Host One\\browser.exe"),
            .user_data_directory = profile};
  }
  base::ScopedTempDir directory_;
#endif
  std::tuple<base::Value, std::string> Call(
      const ElectronLoginItemContext& context,
      const char* method,
      base::DictValue options = {}) {
    base::test::TestFuture<base::Value, std::string> future;
    CallElectronLoginItemSettings(
        context,
        base::Value(base::DictValue()
                        .Set("method", method)
                        .Set("options", std::move(options))),
        future.GetCallback());
    return future.Take();
  }
  base::test::TaskEnvironment task_environment_;
};

TEST_F(ElectronLoginItemsTest, MissingTrustedLaunchContextFailsExplicitly) {
  auto [value, error] =
      Call({}, "set", base::DictValue().Set("openAtLogin", false));
  EXPECT_TRUE(error.starts_with("ERR_NOT_SUPPORTED:"));
  EXPECT_TRUE(value.is_none());
}

#if BUILDFLAG(IS_WIN)
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

TEST_F(ElectronLoginItemsTest, DisablingAbsentItemDoesNotCreateRunKey) {
  registry_util::RegistryOverrideManager registry;
  ASSERT_NO_FATAL_FAILURE(registry.OverrideRegistry(HKEY_CURRENT_USER));
  auto [value, error] =
      Call(Context(), "set", base::DictValue().Set("openAtLogin", false));
  EXPECT_TRUE(error.empty()) << error;
  base::win::RegKey run;
  EXPECT_EQ(ERROR_FILE_NOT_FOUND,
            run.Open(HKEY_CURRENT_USER, kRunKey, KEY_READ));
  auto [state, query_error] = Call(Context(), "get");
  ASSERT_TRUE(query_error.empty()) << query_error;
  EXPECT_EQ(false, state.GetDict().FindBool("openAtLogin"));
  EXPECT_EQ(false, state.GetDict().FindBool("executableWillLaunchAtLogin"));
}

TEST_F(ElectronLoginItemsTest, RoundTripsRealRunCommandAndApprovalState) {
  registry_util::RegistryOverrideManager registry;
  ASSERT_NO_FATAL_FAILURE(registry.OverrideRegistry(HKEY_CURRENT_USER));
  base::ListValue args;
  args.Append("--opened-at-login=1");
  args.Append("quoted \"argument\"");
  args.Append("--user-data-dir=untrusted");
  auto options =
      base::DictValue().Set("openAtLogin", true).Set("args", args.Clone());
  auto [value, error] = Call(Context(), "set", options.Clone());
  ASSERT_TRUE(error.empty()) << error;
  auto [state, query_error] =
      Call(Context(), "get", base::DictValue().Set("args", args.Clone()));
  ASSERT_TRUE(query_error.empty()) << query_error;
  EXPECT_EQ(true, state.GetDict().FindBool("openAtLogin"));
  EXPECT_EQ(true, state.GetDict().FindBool("executableWillLaunchAtLogin"));
  const auto* items = state.GetDict().FindList("launchItems");
  ASSERT_TRUE(items);
  ASSERT_EQ(1u, items->size());
  const auto& item = (*items)[0].GetDict();
  base::win::RegKey run(HKEY_CURRENT_USER, kRunKey, KEY_READ);
  std::wstring text;
  ASSERT_EQ(
      ERROR_SUCCESS,
      run.ReadValue(base::UTF8ToWide(*item.FindString("name")).c_str(), &text));
  const auto command = base::CommandLine::FromString(text);
  EXPECT_EQ(Context().executable_path, command.GetProgram());
  EXPECT_EQ(Context().launch_path,
            command.GetSwitchValuePath("xenon-electron-app"));
  EXPECT_EQ(Context().user_data_directory,
            command.GetSwitchValuePath("user-data-dir"));
  ASSERT_EQ(3u, command.GetArgs().size());
  EXPECT_EQ(L"quoted \"argument\"", command.GetArgs()[1]);
  EXPECT_EQ(L"--user-data-dir=untrusted", command.GetArgs()[2]);
  auto [different_args, different_error] = Call(Context(), "get");
  ASSERT_TRUE(different_error.empty()) << different_error;
  EXPECT_EQ(false, different_args.GetDict().FindBool("openAtLogin"));
  EXPECT_EQ(true,
            different_args.GetDict().FindBool("executableWillLaunchAtLogin"));
  ASSERT_EQ(1u, different_args.GetDict().FindList("launchItems")->size());
  base::ListValue actual_launch_args;
  for (size_t i = 1; i < command.argv().size(); ++i) {
    actual_launch_args.Append(base::WideToUTF8(command.argv()[i]));
  }
  EXPECT_EQ(actual_launch_args, *different_args.GetDict()
                                     .FindList("launchItems")
                                     ->front()
                                     .GetDict()
                                     .FindList("args"));
  options.Set("enabled", false);
  auto [disabled, disable_error] = Call(Context(), "set", options.Clone());
  ASSERT_TRUE(disable_error.empty()) << disable_error;
  auto [disabled_state, disabled_query_error] =
      Call(Context(), "get", base::DictValue().Set("args", args.Clone()));
  ASSERT_TRUE(disabled_query_error.empty()) << disabled_query_error;
  EXPECT_EQ(true, disabled_state.GetDict().FindBool("openAtLogin"));
  EXPECT_EQ(false,
            disabled_state.GetDict().FindBool("executableWillLaunchAtLogin"));
  options.Remove("enabled");
  auto [reenabled, reenable_error] = Call(Context(), "set", options.Clone());
  ASSERT_TRUE(reenable_error.empty()) << reenable_error;
  auto [reenabled_state, reenabled_query_error] =
      Call(Context(), "get", base::DictValue().Set("args", args.Clone()));
  ASSERT_TRUE(reenabled_query_error.empty()) << reenabled_query_error;
  EXPECT_EQ(true,
            reenabled_state.GetDict().FindBool("executableWillLaunchAtLogin"));
  auto [removed, remove_error] =
      Call(Context(), "set", base::DictValue().Set("openAtLogin", false));
  EXPECT_TRUE(remove_error.empty()) << remove_error;
  EXPECT_EQ(0u, run.GetValueCount().value());
}

TEST_F(ElectronLoginItemsTest, ApplicationAndHostProfileOwnIndependentItems) {
  registry_util::RegistryOverrideManager registry;
  ASSERT_NO_FATAL_FAILURE(registry.OverrideRegistry(HKEY_CURRENT_USER));
  base::win::RegKey run(HKEY_CURRENT_USER, kRunKey, KEY_ALL_ACCESS);
  ASSERT_EQ(ERROR_SUCCESS, run.WriteValue(L"Unrelated", L"keep.exe"));
  auto first = Context(), second = Context(), third = Context();
  second.application_path = directory_.GetPath().AppendASCII("other.asar");
  ASSERT_TRUE(base::WriteFile(second.application_path, "other"));
  second.launch_path = base::FilePath(L"C:\\Apps\\Another");
  third.user_data_directory = directory_.GetPath().AppendASCII("other-profile");
  ASSERT_TRUE(base::CreateDirectory(third.user_data_directory));
  for (const auto& context : {first, second, third}) {
    auto [value, error] =
        Call(context, "set", base::DictValue().Set("openAtLogin", true));
    ASSERT_TRUE(error.empty()) << error;
  }
  EXPECT_EQ(4u, run.GetValueCount().value());
  auto [removed, error] =
      Call(first, "set", base::DictValue().Set("openAtLogin", false));
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(3u, run.GetValueCount().value());
  std::wstring unrelated;
  EXPECT_EQ(ERROR_SUCCESS, run.ReadValue(L"Unrelated", &unrelated));
  EXPECT_EQ(L"keep.exe", unrelated);
  for (const auto& context : {second, third}) {
    auto [value, query_error] = Call(context, "get");
    ASSERT_TRUE(query_error.empty()) << query_error;
    EXPECT_EQ(true, value.GetDict().FindBool("openAtLogin"));
  }
}

TEST_F(ElectronLoginItemsTest, UnsupportedOptionsFailBeforeChangingRegistry) {
  registry_util::RegistryOverrideManager registry;
  ASSERT_NO_FATAL_FAILURE(registry.OverrideRegistry(HKEY_CURRENT_USER));
  std::vector<base::DictValue> invalid;
  invalid.push_back(
      base::DictValue().Set("openAtLogin", true).Set("path", "C:\\other.exe"));
  invalid.push_back(
      base::DictValue().Set("openAtLogin", true).Set("name", "Another"));
  invalid.push_back(base::DictValue().Set("openAtLogin", "yes"));
  invalid.push_back(base::DictValue().Set("args", base::ListValue().Append(1)));
  for (auto& options : invalid) {
    auto [value, error] = Call(Context(), "set", std::move(options));
    EXPECT_FALSE(error.empty());
  }
  base::win::RegKey run;
  EXPECT_EQ(ERROR_FILE_NOT_FOUND,
            run.Open(HKEY_CURRENT_USER, kRunKey, KEY_READ));
}

TEST_F(ElectronLoginItemsTest, JunctionAndCaseAliasesKeepTheSameOwner) {
  registry_util::RegistryOverrideManager registry;
  ASSERT_NO_FATAL_FAILURE(registry.OverrideRegistry(HKEY_CURRENT_USER));
  auto [value, error] =
      Call(Context(), "set", base::DictValue().Set("openAtLogin", true));
  ASSERT_TRUE(error.empty()) << error;
  auto uppercase = Context();
  uppercase.application_path =
      base::FilePath(base::ToUpperASCII(uppercase.application_path.value()));
  uppercase.user_data_directory =
      base::FilePath(base::ToUpperASCII(uppercase.user_data_directory.value()));
  auto [upper_state, upper_error] = Call(uppercase, "get");
  ASSERT_TRUE(upper_error.empty()) << upper_error;
  EXPECT_EQ(true, upper_state.GetDict().FindBool("openAtLogin"));
  base::ScopedTempDir alias;
  ASSERT_TRUE(alias.CreateUniqueTempDir(L"li-alias"));
  auto reparse = base::test::FilePathReparsePoint::Create(alias.GetPath(),
                                                          directory_.GetPath());
  ASSERT_TRUE(reparse);
  auto linked = Context();
  linked.application_path = alias.GetPath().AppendASCII("app.asar");
  linked.user_data_directory = alias.GetPath().AppendASCII("profile");
  auto [linked_state, linked_error] = Call(linked, "get");
  ASSERT_TRUE(linked_error.empty()) << linked_error;
  EXPECT_EQ(true, linked_state.GetDict().FindBool("openAtLogin"));
  auto [removed, remove_error] =
      Call(linked, "set", base::DictValue().Set("openAtLogin", false));
  ASSERT_TRUE(remove_error.empty()) << remove_error;
  base::win::RegKey run(HKEY_CURRENT_USER, kRunKey, KEY_READ);
  EXPECT_EQ(0u, run.GetValueCount().value());
}

TEST_F(ElectronLoginItemsTest, LongCommandOnlyPreventsEnabling) {
  registry_util::RegistryOverrideManager registry;
  ASSERT_NO_FATAL_FAILURE(registry.OverrideRegistry(HKEY_CURRENT_USER));
  auto context = Context();
  context.launch_path = base::FilePath(L"C:\\" + std::wstring(300, L'x'));
  auto [enabled, enable_error] =
      Call(context, "set", base::DictValue().Set("openAtLogin", true));
  EXPECT_TRUE(enable_error.starts_with("ERR_NOT_SUPPORTED:"));
  auto [state, query_error] = Call(context, "get");
  EXPECT_TRUE(query_error.empty()) << query_error;
  auto [disabled, disable_error] =
      Call(context, "set", base::DictValue().Set("openAtLogin", false));
  EXPECT_TRUE(disable_error.empty()) << disable_error;
}
#else
TEST_F(ElectronLoginItemsTest, OtherPlatformsRejectUnsupportedRegistration) {
  ElectronLoginItemContext context{
      base::FilePath("/app/resources/app.asar"), base::FilePath("/app"),
      base::FilePath("/host/browser"), base::FilePath("/profile")};
  auto [value, error] =
      Call(context, "set", base::DictValue().Set("openAtLogin", false));
  EXPECT_TRUE(error.starts_with("ERR_NOT_SUPPORTED:"));
}
#endif
}  // namespace
}  // namespace xenon::ipc
