// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Opt-in diagnostic for installed application payloads. Use
// tools/smoke_real_apps.cjs to isolate environment paths and capture full logs.
// No commercial JavaScript or native addon is copied into this repository.
#include <memory>
#include <string>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/scoped_path_override.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "chrome/common/chrome_paths.h"
#include "mojo/core/embedder/embedder.h"
#include "net/base/filename_util.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"
#include "v8/include/v8.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_config.h"
#include "xenon_overlay/services/xenon_service_impl.h"

namespace xenon {
namespace {

class XenonRealAppSmokeTest : public testing::Test {
 protected:
  static void SetUpTestSuite() { mojo::core::Init(); }

  void SetUp() override {
    if (!base::CommandLine::ForCurrentProcess()->HasSwitch(
            "xenon-app-smoke-root")) {
      GTEST_SKIP() << "Opt-in real payload smoke: run smoke_real_apps.cjs";
    }
    v8::V8::SetFlagsFromString("--no-freeze-flags-after-init");
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    base::FilePath profile =
        base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
            "xenon-app-smoke-profile");
    if (profile.empty()) {
      profile = temp_dir_.GetPath().AppendASCII("profile");
    }
    user_data_override_ = std::make_unique<base::ScopedPathOverride>(
        chrome::DIR_USER_DATA, profile);
    service_ = std::make_unique<XenonServiceImpl>(
        browser_.BindNewPipeAndPassReceiver());
  }

  void TearDown() override {
    service_.reset();
    browser_.reset();
    task_environment_.RunUntilIdle();
  }

  void WriteReport(const base::DictValue& report) {
    std::string json;
    ASSERT_TRUE(base::JSONWriter::Write(report, &json));
    LOG(ERROR) << "[xenon-app-smoke] " << json;
    const base::FilePath result_path =
        base::CommandLine::ForCurrentProcess()->GetSwitchValuePath(
            "xenon-app-smoke-result");
    if (!result_path.empty()) {
      ASSERT_TRUE(base::WriteFile(result_path, json));
    }
  }

  void RunApplication(bool thunder) {
    const auto* command_line = base::CommandLine::ForCurrentProcess();
    const base::FilePath root =
        command_line->GetSwitchValuePath("xenon-app-smoke-root");
    ASSERT_TRUE(root.IsAbsolute());
    ipc::mojom::IpcMainConfigPtr config;
    std::string configuration_error;
    ASSERT_TRUE(ipc::LoadElectronAppConfig(
        root.AppendASCII(thunder ? "thunder_2025.xenon.json"
                                 : "xenon_player.xenon.json"),
        thunder ? "thunder-2025" : "xenon-player-test", &config,
        &configuration_error))
        << configuration_error;

    base::DictValue report;
    report.Set("application", thunder ? "TH" : "PL-E");
    report.Set("main_override", config->main_script_path);
    report.Set("app_path", config->app_path);
    report.Set("runtime_directory", config->runtime_directory);
    report.Set("executable_identity", config->executable_path);
    report.Set("native_addons",
               "real packaged addons through XenonNodeExecutor");
    report.Set("browser_boundary",
               "No BrowserObserver: BrowserWindow creation fails explicitly; "
               "no renderer or HWND is simulated");
    report.Set("network_boundary",
               "No network success responses are mocked; actual runtime "
               "support or failure is retained");
    report.Set("business_startup_verified", false);
    report.Set("phase", "evaluating-main");
    WriteReport(report);

    base::test::TestFuture<bool, const std::string&> initialized;
    browser_->InitializeElectronIpc(std::move(config),
                                    initialized.GetCallback());
    ASSERT_TRUE(initialized.Wait());
    const bool loaded = initialized.Get<0>();
    const std::string error = initialized.Get<1>();
    report.Set("main_evaluated", loaded);
    report.Set("startup_error", error);
    report.Set("phase", loaded ? "observing-ready" : "main-failed");
    WriteReport(report);
    ASSERT_TRUE(loaded) << error;

    int observe_ms = 1000;
    if (command_line->HasSwitch("xenon-app-smoke-observe-ms")) {
      ASSERT_TRUE(base::StringToInt(
          command_line->GetSwitchValueASCII("xenon-app-smoke-observe-ms"),
          &observe_ms));
      ASSERT_GE(observe_ms, 0);
      ASSERT_LE(observe_ms, 10000);
    }
    // Run real ready listeners, timers, native async work and microtasks for a
    // bounded observation period. Logged errors remain failures in the runner;
    // reaching this point is deliberately not an assertion of UI readiness.
    base::RunLoop observation;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE, observation.QuitClosure(), base::Milliseconds(observe_ms));
    observation.Run();
    report.Set("phase", "observation-complete");
    report.Set("observation_ms", observe_ms);
    WriteReport(report);
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
  base::ScopedTempDir temp_dir_;
  std::unique_ptr<base::ScopedPathOverride> user_data_override_;
  mojo::Remote<mojom::XenonMainService> browser_;
  std::unique_ptr<XenonServiceImpl> service_;
};

TEST_F(XenonRealAppSmokeTest, ThunderMain) {
  RunApplication(true);
}
TEST_F(XenonRealAppSmokeTest, PlayerMain) {
  RunApplication(false);
}

}  // namespace
}  // namespace xenon
