// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/service_host/utility_sandbox_delegate.h"

#include <memory>

#include "base/environment.h"
#include "base/strings/strcat.h"
#include "base/test/scoped_feature_list.h"
#include "base/win/windows_version.h"
#include "build/build_config.h"
#include "content/public/browser/child_process_host.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_switches.h"
#include "sandbox/policy/features.h"
#include "sandbox/policy/mojom/sandbox.mojom.h"
#include "sandbox/policy/sandbox_type.h"
#include "sandbox/policy/switches.h"
#include "sandbox/policy/win/sandbox_win.h"
#include "sandbox/win/src/app_container_base.h"
#include "sandbox/win/src/sandbox_factory.h"
#include "sandbox/win/src/sandbox_policy.h"
#include "sandbox/win/src/sandbox_policy_base.h"
#include "testing/gtest/include/gtest/gtest.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include "base/files/scoped_temp_dir.h"
#include "base/process/launch.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"
#endif

namespace content {

class UtilitySandboxDelegateWinTest
    : public ::testing::Test,
      public ::testing::WithParamInterface<
          std::tuple</*AppContainerDisabled=*/bool,
                     /*kPrintCompositorLPAC=*/bool>> {
 public:
  UtilitySandboxDelegateWinTest() = default;

  void SetUp() override {
    scoped_feature_list_.InitWithFeatureState(
        sandbox::policy::features::kPrintCompositorLPAC,
        IsLPACFeatureEnabledForTest());
  }

 protected:
  static bool IsAppContainerDisabledForTest() {
    return std::get<0>(GetParam());
  }
  static bool IsLPACFeatureEnabledForTest() { return std::get<1>(GetParam()); }

 private:
  base::test::ScopedFeatureList scoped_feature_list_;
};

TEST_P(UtilitySandboxDelegateWinTest, IsAppContainerDisabled) {
  class MaybeDisableAppCongtainerBrowserClient : public ContentBrowserClient {
    bool IsAppContainerDisabled(sandbox::mojom::Sandbox sandbox_type) override {
      return IsAppContainerDisabledForTest();
    }
  } test_browser_client;

  auto* old_browser_client = SetBrowserClientForTesting(&test_browser_client);

  // Print compositor is chosen as a sandbox type, because it is a utility
  // process that has App Container enabled by default.
  constexpr auto kSandboxType = sandbox::mojom::Sandbox::kPrintCompositor;

  auto cmd_line = base::CommandLine(
      ChildProcessHost::GetChildPath(ChildProcessHost::CHILD_NORMAL));
  cmd_line.AppendSwitchASCII(switches::kProcessType, switches::kUtilityProcess);
  sandbox::policy::SetCommandLineFlagsForSandboxType(&cmd_line, kSandboxType);

  base::EnvironmentMap env;
  auto utility_delegate =
      std::make_unique<UtilitySandboxedProcessLauncherDelegate>(kSandboxType,
                                                                env, cmd_line);
  base::HandlesToInheritVector handles_to_inherit;
  sandbox::BrokerServices* broker =
      sandbox::SandboxFactory::GetBrokerServices();
  auto policy = broker->CreatePolicy();

  sandbox::ResultCode result =
      sandbox::policy::SandboxWin::GeneratePolicyForSandboxedProcess(
          cmd_line, handles_to_inherit, utility_delegate.get(), policy.get());
  ASSERT_EQ(sandbox::ResultCode::SBOX_ALL_OK, result);
  SetBrowserClientForTesting(old_browser_client);

  bool expected_app_container_enabled = IsLPACFeatureEnabledForTest();

  // The content client can override the state to disable it, but not to enable
  // it if the feature is disabled.
  if (IsAppContainerDisabledForTest()) {
    expected_app_container_enabled = false;
  }

  if (expected_app_container_enabled) {
    EXPECT_TRUE(policy->GetConfig()->GetAppContainer());
    EXPECT_EQ(policy->GetConfig()->GetIntegrityLevel(),
              sandbox::IntegrityLevel::INTEGRITY_LEVEL_LAST);
    EXPECT_EQ(policy->GetConfig()->GetInitialTokenLevel(),
              sandbox::TokenLevel::USER_UNPROTECTED);
    EXPECT_EQ(policy->GetConfig()->GetLockdownTokenLevel(),
              sandbox::TokenLevel::USER_UNPROTECTED);
  } else {
    EXPECT_FALSE(policy->GetConfig()->GetAppContainer());
    EXPECT_EQ(policy->GetConfig()->GetIntegrityLevel(),
              sandbox::IntegrityLevel::INTEGRITY_LEVEL_LOW);
    EXPECT_EQ(policy->GetConfig()->GetInitialTokenLevel(),
              sandbox::TokenLevel::USER_RESTRICTED_SAME_ACCESS);
    EXPECT_EQ(policy->GetConfig()->GetLockdownTokenLevel(),
              sandbox::TokenLevel::USER_LOCKDOWN);
  }
}

INSTANTIATE_TEST_SUITE_P(
    /* no prefix */,
    UtilitySandboxDelegateWinTest,
// TODO(crbug.com/353583970):  Enable `kPrintCompositorLPAC` for ARM when
// flakiness is resolved.
#if defined(ARCH_CPU_ARM_FAMILY)
    ::testing::Combine(/*AppContainerDisabled=*/::testing::Bool(),
                       /*kPrintCompositorLPAC=*/::testing::Values(false)),
#else
    ::testing::Combine(/*AppContainerDisabled=*/::testing::Bool(),
                       /*kPrintCompositorLPAC=*/::testing::Bool()),
#endif
    [](const auto& info) {
      return base::StrCat(
          {std::get<0>(info.param) ? "ACDisabled" : "ACEnabled",
           std::get<1>(info.param) ? "FeatureEnabled" : "FeatureDisabled"});
    });

#if BUILDFLAG(ENABLE_XENON_SERVICE)
class XenonUtilityLaunchOptionsTest : public testing::Test {
 public:
  void SetUp() override {
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    previous_client_ = SetBrowserClientForTesting(&client_);
  }
  void TearDown() override { SetBrowserClientForTesting(previous_client_); }

  std::unique_ptr<xenon::UtilityProcessLaunchOptions> Options() {
    auto options = std::make_unique<xenon::UtilityProcessLaunchOptions>();
    options->working_directory = directory_.GetPath();
    options->environment.emplace(L"XENON_TEST_VALUE", L"isolated");
    for (auto& file : options->stdio) {
      file = base::File(directory_.GetPath().AppendASCII("stdio"),
                        base::File::FLAG_OPEN_ALWAYS | base::File::FLAG_READ |
                            base::File::FLAG_WRITE);
      EXPECT_TRUE(file.IsValid());
    }
    return options;
  }

  base::CommandLine Command() {
    base::CommandLine command(base::CommandLine::NO_PROGRAM);
    command.AppendSwitchASCII(switches::kProcessType,
                              switches::kUtilityProcess);
    command.AppendSwitchASCII(switches::kUtilitySubType,
                              "xenon.ipc.mojom.UtilityProcessService");
    command.AppendSwitch(xenon::ipc::switches::kUtilityProcess);
    return command;
  }

 private:
  base::ScopedTempDir directory_;
  ContentBrowserClient client_;
  raw_ptr<ContentBrowserClient> previous_client_ = nullptr;
};

TEST_F(XenonUtilityLaunchOptionsTest, DefaultDelegateLeavesLaunchUntouched) {
  UtilitySandboxedProcessLauncherDelegate delegate(
      sandbox::mojom::Sandbox::kNoSandbox, {}, Command());
  base::LaunchOptions options;
  options.environment.emplace(L"UNCHANGED", L"original");
  EXPECT_TRUE(delegate.ApplyXenonUtilityProcessOptions(&options));
  EXPECT_FALSE(options.clear_environment);
  EXPECT_FALSE(options.preserve_empty_environment_values);
  EXPECT_TRUE(options.current_directory.empty());
  EXPECT_EQ(options.environment.at(L"UNCHANGED"), L"original");
  EXPECT_EQ(options.stdout_handle, nullptr);
}

TEST_F(XenonUtilityLaunchOptionsTest, RequiresDedicatedUnsandboxedUtility) {
  UtilitySandboxedProcessLauncherDelegate sandboxed(
      sandbox::mojom::Sandbox::kUtility, {}, Command());
  EXPECT_FALSE(sandboxed.SetXenonUtilityProcessOptions(Options()));
  auto command = Command();
  command.RemoveSwitch(xenon::ipc::switches::kUtilityProcess);
  UtilitySandboxedProcessLauncherDelegate unmarked(
      sandbox::mojom::Sandbox::kNoSandbox, {}, command);
  EXPECT_FALSE(unmarked.SetXenonUtilityProcessOptions(Options()));
  command = Command();
  command.RemoveSwitch(switches::kUtilitySubType);
  command.AppendSwitchASCII(switches::kUtilitySubType, "other.service");
  UtilitySandboxedProcessLauncherDelegate unrelated(
      sandbox::mojom::Sandbox::kNoSandbox, {}, command);
  EXPECT_FALSE(unrelated.SetXenonUtilityProcessOptions(Options()));
}

TEST_F(XenonUtilityLaunchOptionsTest,
       AppliesOwnedHandlesWithoutLosingIpcHandles) {
  UtilitySandboxedProcessLauncherDelegate delegate(
      sandbox::mojom::Sandbox::kNoSandbox, {}, Command());
  auto owned = Options();
  owned->environment.emplace(L"EMPTY", L"");
  const auto cwd = owned->working_directory;
  const HANDLE in = owned->stdio[0].GetPlatformFile();
  const HANDLE out = owned->stdio[1].GetPlatformFile();
  const HANDLE err = owned->stdio[2].GetPlatformFile();
  ASSERT_TRUE(delegate.SetXenonUtilityProcessOptions(std::move(owned)));
  auto ipc_file = Options()->stdio[0].Duplicate();
  base::LaunchOptions options;
  options.handles_to_inherit.push_back(ipc_file.GetPlatformFile());
  options.environment.emplace(L"MUST_NOT_LEAK", L"parent");
  ASSERT_TRUE(delegate.ApplyXenonUtilityProcessOptions(&options));
  EXPECT_TRUE(options.clear_environment);
  EXPECT_TRUE(options.preserve_empty_environment_values);
  EXPECT_TRUE(options.start_hidden);
  EXPECT_EQ(options.current_directory, cwd);
  EXPECT_EQ(options.environment.size(), 2u);
  EXPECT_EQ(options.environment.at(L"XENON_TEST_VALUE"), L"isolated");
  EXPECT_TRUE(options.environment.contains(L"EMPTY"));
  EXPECT_TRUE(options.environment.at(L"EMPTY").empty());
  EXPECT_EQ(options.stdin_handle, in);
  EXPECT_EQ(options.stdout_handle, out);
  EXPECT_EQ(options.stderr_handle, err);
  ASSERT_EQ(options.handles_to_inherit.size(), 4u);
  EXPECT_EQ(options.handles_to_inherit.front(), ipc_file.GetPlatformFile());
  for (HANDLE handle : {in, out, err}) {
    DWORD flags = 0;
    ASSERT_TRUE(::GetHandleInformation(handle, &flags));
    EXPECT_TRUE(flags & HANDLE_FLAG_INHERIT);
  }
}

TEST_F(XenonUtilityLaunchOptionsTest,
       InvalidEnvironmentAndStdioFailExplicitly) {
  std::string error;
  auto options = Options();
  ASSERT_TRUE(options->Validate(&error));
  options->environment.emplace(L"EMPTY", L"");
  EXPECT_TRUE(options->Validate(&error));
  options->environment.erase(L"EMPTY");
  options->environment.emplace(L"INVALID=KEY", L"value");
  EXPECT_FALSE(options->Validate(&error));
  EXPECT_TRUE(error.starts_with("ERR_INVALID_ARG_VALUE:"));
  options->environment.erase(L"INVALID=KEY");
  options->stdio[2].Close();
  EXPECT_FALSE(options->Validate(&error));
}
#endif

}  // namespace content
