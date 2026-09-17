// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_child_process_bridge.h"

#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/environment.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/test/run_until.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace xenon {
namespace {

#if BUILDFLAG(IS_WIN)
MULTIPROCESS_TEST_MAIN(XenonChildProcessWindowState) {
  STARTUPINFOW startup_info{};
  startup_info.cb = sizeof(startup_info);
  ::GetStartupInfoW(&startup_info);
  std::cout << "console=" << (::GetConsoleWindow() != nullptr) << '\n'
            << "use-show-window="
            << ((startup_info.dwFlags & STARTF_USESHOWWINDOW) != 0) << '\n'
            << "show-window=" << startup_info.wShowWindow << '\n';
  return 0;
}
#endif

MULTIPROCESS_TEST_MAIN(XenonChildProcessEcho) {
  auto environment = base::Environment::Create();
  const auto expected = environment->GetVar("XENON_CHILD_TEST_CWD");
  base::FilePath directory;
  if (expected && (!base::GetCurrentDirectory(&directory) ||
                   directory.AsUTF8Unsafe() != *expected)) {
    return 90;
  }
  const auto marker = environment->GetVar("XENON_CHILD_TEST_MARKER");
  if (marker && *marker != "child-fixture") {
    return 91;
  }
  std::string line;
  if (!std::getline(std::cin, line)) {
    return 92;
  }
  std::cout << line << '\n';
  std::cerr << "fixture-stderr\n";
  return 7;
}

class XenonChildProcessBridgeTest : public testing::Test {
 protected:
  XenonChildProcessBridgeTest()
      : timeout_(FROM_HERE, base::Seconds(15)),
        bridge_(base::BindRepeating(&XenonChildProcessBridgeTest::OnEvent,
                                    base::Unretained(this))) {}

  base::DictValue SpawnRequest() {
    base::FilePath executable;
    EXPECT_TRUE(base::PathService::Get(base::FILE_EXE, &executable));
    return base::DictValue()
        .Set("op", "spawn")
        .Set("id", "fixture")
        .Set("file", executable.AsUTF8Unsafe())
        .Set("args", base::ListValue().Append(
                         "--test-child-process=XenonChildProcessEcho"))
        .Set("stdio", base::ListValue().Append("pipe").Append("pipe").Append("pipe"))
        .Set("windowsHide", true);
  }

#if BUILDFLAG(IS_WIN)
  void ExpectHiddenConsole(std::optional<bool> windows_hide) {
    auto request = SpawnRequest();
    request.Set("args", base::ListValue().Append(
                            "--test-child-process=XenonChildProcessWindowState"));
    if (windows_hide.has_value()) {
      request.Set("windowsHide", *windows_hide);
    } else {
      request.Remove("windowsHide");
    }
    const auto result = bridge_.Call("container", "renderer", request);
    ASSERT_TRUE(result.FindBool("ok").value_or(false));
    ASSERT_GT(result.FindInt("pid").value_or(0), 0);
    EXPECT_TRUE(Call("resume", 1).FindBool("ok").value_or(false));
    EXPECT_TRUE(Call("resume", 2).FindBool("ok").value_or(false));
    ASSERT_TRUE(base::test::RunUntil([&] { return closed_; }));
    EXPECT_NE(std::string::npos, stdout_.find("console=0"));
    EXPECT_NE(std::string::npos, stdout_.find("use-show-window=1"));
    // Electron's default suppresses consoles without hiding GUI children.
    const int expected_show_window =
        windows_hide.value_or(false) ? SW_HIDE : SW_SHOWDEFAULT;
    EXPECT_NE(std::string::npos,
              stdout_.find("show-window=" +
                           std::to_string(expected_show_window)));
    EXPECT_TRUE(stderr_.empty());
    EXPECT_EQ(0, exit_code_);
    ASSERT_FALSE(events_.empty());
    EXPECT_EQ("close", events_.back());
  }
#endif

  base::DictValue Call(std::string operation, int fd = 0) {
    return bridge_.Call("container", "renderer",
                        base::DictValue().Set("id", "fixture")
                            .Set("op", operation).Set("fd", fd));
  }

  void OnEvent(const std::string& container, const std::string& endpoint,
               base::Value value) {
    EXPECT_EQ("container", container);
    EXPECT_EQ("renderer", endpoint);
    const auto& event = value.GetDict();
    events_.push_back(*event.FindString("event"));
    if (*event.FindString("event") == "data") {
      std::string bytes;
      EXPECT_TRUE(base::Base64Decode(*event.FindString("data"), &bytes));
      (event.FindInt("fd") == 1 ? stdout_ : stderr_) += bytes;
    }
    if (*event.FindString("event") == "exit") {
      exit_code_ = event.FindDouble("exitCode").value_or(-99);
    }
    if (*event.FindString("event") == "close") {
      closed_ = true;
    }
  }

  base::test::TaskEnvironment environment_;
  base::test::ScopedRunLoopTimeout timeout_;
  XenonChildProcessBridge bridge_;
  std::vector<std::string> events_;
  std::string stdout_;
  std::string stderr_;
  double exit_code_ = -99;
  bool closed_ = false;
};

#if BUILDFLAG(IS_WIN)
TEST_F(XenonChildProcessBridgeTest, ConsoleIsHiddenWhenWindowsHideIsOmitted) {
  ExpectHiddenConsole(std::nullopt);
}

TEST_F(XenonChildProcessBridgeTest, ConsoleIsHiddenWhenWindowsHideIsFalse) {
  ExpectHiddenConsole(false);
}

TEST_F(XenonChildProcessBridgeTest, WindowsHideAlsoHidesGuiStartupWindow) {
  ExpectHiddenConsole(true);
}
#endif

TEST_F(XenonChildProcessBridgeTest, RealPipesEnvironmentCwdAndExitOrdering) {
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  auto request = SpawnRequest();
  request.Set("cwd", directory.GetPath().AsUTF8Unsafe());
  request.Set("env", base::DictValue()
                         .Set("XENON_CHILD_TEST_CWD", directory.GetPath().AsUTF8Unsafe())
                         .Set("XENON_CHILD_TEST_MARKER", "child-fixture"));
  auto result = bridge_.Call("container", "renderer", request);
  ASSERT_TRUE(result.FindBool("ok").value_or(false));
  ASSERT_GT(result.FindInt("pid").value_or(0), 0);
  EXPECT_TRUE(Call("resume", 1).FindBool("ok").value_or(false));
  EXPECT_TRUE(Call("resume", 2).FindBool("ok").value_or(false));
  result = bridge_.Call(
      "container", "renderer", base::DictValue()
          .Set("id", "fixture").Set("op", "write").Set("fd", 0)
          .Set("token", 1).Set("data", base::Base64Encode("fixture-stdin\n")));
  EXPECT_TRUE(result.FindBool("ok").value_or(false));
  EXPECT_TRUE(Call("end").FindBool("ok").value_or(false));
  ASSERT_TRUE(base::test::RunUntil([&] { return closed_; }));
  EXPECT_TRUE(stdout_.find("fixture-stdin") != std::string::npos);
  EXPECT_TRUE(stderr_.find("fixture-stderr") != std::string::npos);
  EXPECT_EQ(7, exit_code_);
  ASSERT_FALSE(events_.empty());
  EXPECT_EQ("close", events_.back());
  EXPECT_EQ("ESRCH", *Call("kill").FindString("code"));
}

TEST_F(XenonChildProcessBridgeTest, MissingExecutableReturnsRealError) {
  auto request = SpawnRequest();
  request.Set("file", "xenon-child-process-fixture-does-not-exist");
  const auto result = bridge_.Call("container", "renderer", request);
  EXPECT_FALSE(result.FindBool("ok").value_or(true));
  EXPECT_EQ("ENOENT", *result.FindString("code"));
  EXPECT_FALSE(result.contains("pid"));
  environment_.RunUntilIdle();
  EXPECT_TRUE(events_.empty());
}

TEST(XenonChildProcessBridgeIdleTest, NoPollingBeforeSpawnOrAfterFailureCleanup) {
  base::test::TaskEnvironment environment{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  XenonChildProcessBridge bridge(base::BindRepeating(
      [](const std::string&, const std::string&, base::Value) {
        ADD_FAILURE() << "A failed spawn must not dispatch lifecycle events";
      }));
  EXPECT_EQ(0u, environment.GetPendingMainThreadTaskCount());
  environment.FastForwardBy(base::Seconds(1));
  EXPECT_EQ(0u, environment.GetPendingMainThreadTaskCount());

  auto request = base::DictValue()
                     .Set("op", "spawn")
                     .Set("id", "fixture")
                     .Set("file", "xenon-child-process-fixture-does-not-exist")
                     .Set("args", base::ListValue())
                     .Set("stdio", base::ListValue()
                                       .Append("pipe")
                                       .Append("pipe")
                                       .Append("pipe"));
  // The second attempt also proves polling restarts after the idle interval,
  // and that the first attempt's asynchronously closed handles were removed.
  for (int attempt = 0; attempt < 2; ++attempt) {
    const auto result = bridge.Call("container", "renderer", request);
    ASSERT_FALSE(result.FindBool("ok").value_or(true));
    EXPECT_EQ("ENOENT", *result.FindString("code"));
    EXPECT_GT(environment.GetPendingMainThreadTaskCount(), 0u);
    environment.FastForwardBy(base::Milliseconds(20));
    EXPECT_EQ(0u, environment.GetPendingMainThreadTaskCount());
    environment.FastForwardBy(base::Seconds(1));
    EXPECT_EQ(0u, environment.GetPendingMainThreadTaskCount());
  }
}

TEST_F(XenonChildProcessBridgeTest, EndpointOwnershipAndDisconnectCleanup) {
  auto result = bridge_.Call("container", "renderer", SpawnRequest());
  ASSERT_TRUE(result.FindBool("ok").value_or(false));
  auto kill = base::DictValue().Set("id", "fixture").Set("op", "kill").Set("signal", 0);
  EXPECT_EQ("ESRCH", *bridge_.Call("container", "other", kill).FindString("code"));
  EXPECT_EQ("ESRCH", *bridge_.Call("other", "renderer", kill).FindString("code"));
  EXPECT_TRUE(bridge_.Call("container", "renderer", kill).FindBool("ok").value_or(false));
  bridge_.RemoveEndpoint("container", "renderer");
  ASSERT_TRUE(base::test::RunUntil([&] {
    return !bridge_.Call("container", "renderer", kill).FindBool("ok").value_or(false);
  }));
  EXPECT_TRUE(events_.empty());
}

TEST_F(XenonChildProcessBridgeTest, InvalidNativeRequestCannotSpawn) {
  auto request = SpawnRequest();
  request.Set("env", base::DictValue().Set("INVALID=KEY", "value"));
  EXPECT_EQ("EINVAL", *bridge_.Call("container", "renderer", request).FindString("code"));
  request = SpawnRequest();
  request.Set("stdio", base::ListValue().Append("pipe").Append("ipc").Append("pipe"));
  EXPECT_EQ("EINVAL", *bridge_.Call("container", "renderer", request).FindString("code"));
}

TEST_F(XenonChildProcessBridgeTest, UnrefChildIsReapedDuringBridgeDestruction) {
  int callbacks = 0;
  auto owner = std::make_unique<XenonChildProcessBridge>(base::BindRepeating(
      [](int* count, const std::string&, const std::string&, base::Value) {
        ++*count;
      }, base::Unretained(&callbacks)));
  auto result = owner->Call("container", "renderer", SpawnRequest());
  ASSERT_TRUE(result.FindBool("ok").value_or(false));
  const int pid = result.FindInt("pid").value_or(0);
  ASSERT_GT(pid, 0);
  EXPECT_TRUE(owner->Call("container", "renderer",
                          base::DictValue().Set("id", "fixture").Set("op", "unref"))
                  .FindBool("ok").value_or(false));
  owner.reset();
  EXPECT_EQ(UV_ESRCH, uv_kill(pid, 0));
  EXPECT_EQ(0, callbacks);
}

TEST_F(XenonChildProcessBridgeTest, DisconnectCancelsPendingWriteAndShutdownSafely) {
  int callbacks = 0;
  auto owner = std::make_unique<XenonChildProcessBridge>(base::BindRepeating(
      [](int* count, const std::string&, const std::string&, base::Value) {
        ++*count;
      }, base::Unretained(&callbacks)));
  ASSERT_TRUE(owner->Call("container", "renderer", SpawnRequest())
                  .FindBool("ok").value_or(false));
  auto result = owner->Call(
      "container", "renderer", base::DictValue()
          .Set("id", "fixture").Set("op", "write").Set("fd", 0)
          .Set("token", 1).Set("data", base::Base64Encode(std::string(2 * 1024 * 1024, 'x'))));
  ASSERT_TRUE(result.FindBool("ok").value_or(false));
  EXPECT_TRUE(owner->Call("container", "renderer",
                          base::DictValue().Set("id", "fixture").Set("op", "end").Set("fd", 0))
                  .FindBool("ok").value_or(false));
  owner->RemoveEndpoint("container", "renderer");
  owner.reset();
  EXPECT_EQ(0, callbacks);
}

}  // namespace
}  // namespace xenon
