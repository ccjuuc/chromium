// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_single_instance.h"

#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/process/launch.h"
#include "base/task/bind_post_task.h"
#include "base/test/multiprocess_test.h"
#include "base/test/task_environment.h"
#include "base/threading/platform_thread.h"
#include "base/time/time.h"
#include "base/unguessable_token.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/multiprocess_func_list.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_single_instance_transport.h"

namespace xenon::ipc {
namespace {

using Notification = XenonAppSingleInstanceBroker::Notification;

Notification ExampleNotification(const base::FilePath& cwd) {
  return {base::ListValue().Append("host").Append("note with space.md"), cwd,
          base::Value(base::DictValue()
                          .Set("argv", base::ListValue().Append("exact argv"))
                          .Set("nested", base::DictValue().Set("key", true))
                          .Set("__proto__", "ordinary data"))};
}

class XenonAppSingleInstanceTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(root_.CreateUniqueTempDir());
    app_ = root_.GetPath().AppendASCII("application");
    profile_ = root_.GetPath().AppendASCII("host-profile");
    ASSERT_TRUE(base::CreateDirectory(app_));
    ASSERT_TRUE(base::CreateDirectory(profile_));
  }

  base::expected<bool, XenonAppSingleInstanceBroker::Error> Request(
      std::string owner) {
    return broker_.Request(owner, app_, profile_, ExampleNotification(app_));
  }

  base::Process Spawn(const char* child) {
    auto command = base::GetMultiProcessTestChildBaseCommandLine();
    command.AppendSwitchPath("singleton-app", app_);
    command.AppendSwitchPath("singleton-profile", profile_);
    command.AppendSwitchPath("singleton-root", root_.GetPath());
    base::LaunchOptions options;
#if BUILDFLAG(IS_WIN)
    options.start_hidden = true;
#endif
    return base::SpawnMultiProcessTestChild(child, command, options);
  }

  void OnNotification(const std::string& owner, Notification notification) {
    owners_.push_back(owner);
    notifications_.push_back(std::move(notification));
  }

  base::test::TaskEnvironment environment_;
  base::ScopedTempDir root_;
  base::FilePath app_;
  base::FilePath profile_;
  std::vector<std::string> owners_;
  std::vector<Notification> notifications_;
  XenonAppSingleInstanceBroker broker_{
      base::BindRepeating(&XenonAppSingleInstanceTest::OnNotification,
                          base::Unretained(this))};
};

TEST_F(XenonAppSingleInstanceTest,
       AcquiresOnceAndForwardsCompleteSecondArguments) {
  auto first = Request("container:1");
  ASSERT_TRUE(first.has_value()) << first.error().message;
  EXPECT_TRUE(*first);
  EXPECT_TRUE(Request("container:1").value());
  EXPECT_TRUE(broker_.Has("container:1"));
  EXPECT_FALSE(broker_.Has("other:1"));
  auto second = Request("other:1");
  ASSERT_TRUE(second.has_value()) << second.error().message;
  EXPECT_FALSE(*second);
  ASSERT_EQ(1u, notifications_.size());
  EXPECT_EQ("container:1", owners_[0]);
  EXPECT_EQ(ExampleNotification(app_).argv, notifications_[0].argv);
  EXPECT_EQ(app_, notifications_[0].working_directory);
  EXPECT_EQ(ExampleNotification(app_).additional_data,
            notifications_[0].additional_data);
}

TEST_F(XenonAppSingleInstanceTest,
       NotificationUsesNativeLaunchStateDespiteMutatedJsArguments) {
  ASSERT_TRUE(Request("primary").value());
  const std::vector<std::string> launch_argv = {
      "trusted-launcher", "--new-window", "文档 名称.md", ""};
  auto user_argv =
      base::ListValue().Append(base::Value()).Append("runtime-edit");
  auto request = base::DictValue()
                     .Set("method", "request")
                     .Set("argv", user_argv.Clone())
                     .Set("cwd", "fake-relative-js-cwd")
                     .Set("additionalData",
                          base::DictValue().Set("argv", user_argv.Clone()));
  auto notification = XenonAppSingleInstanceBroker::NotificationFromRequest(
      launch_argv, app_.AppendASCII("launcher"), profile_, request);
  ASSERT_TRUE(notification.has_value()) << notification.error().message;
  auto acquired =
      broker_.Request("secondary", app_, profile_, std::move(*notification));
  ASSERT_TRUE(acquired.has_value()) << acquired.error().message;
  EXPECT_FALSE(*acquired);
  ASSERT_EQ(1u, notifications_.size());
  EXPECT_EQ(base::ListValue()
                .Append("trusted-launcher")
                .Append("--new-window")
                .Append("文档 名称.md")
                .Append(""),
            notifications_[0].argv);
  EXPECT_EQ(profile_, notifications_[0].working_directory);
  EXPECT_EQ(user_argv,
            *notifications_[0].additional_data.GetDict().FindList("argv"));
}

TEST_F(XenonAppSingleInstanceTest,
       NotificationFallsBackToTrustedExecutableAndRequiresAdditionalObject) {
  const auto executable = app_.AppendASCII("launcher");
  const auto request =
      base::DictValue().Set("additionalData", base::DictValue());
  auto notification = XenonAppSingleInstanceBroker::NotificationFromRequest(
      {}, executable, app_, request);
  ASSERT_TRUE(notification.has_value()) << notification.error().message;
  EXPECT_EQ(base::ListValue().Append(executable.AsUTF8Unsafe()),
            notification->argv);
  EXPECT_EQ(app_, notification->working_directory);
  auto invalid = XenonAppSingleInstanceBroker::NotificationFromRequest(
      {}, executable, app_,
      base::DictValue().Set("additionalData", "not an object"));
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ("ERR_INVALID_ARG_VALUE", invalid.error().code);
}

TEST_F(XenonAppSingleInstanceTest,
       NativeDirectorySnapshotIsRequiredAndForwarded) {
  ASSERT_TRUE(Request("primary").value());
  auto request =
      base::DictValue()
          .Set("additionalData", base::DictValue().Set("cwd", "user data"))
          .Set("__xenonRuntime",
               base::DictValue().Set("workingDirectory",
                                     profile_.AsUTF8Unsafe()));
  auto notification =
      XenonAppSingleInstanceBroker::NotificationFromNativeRequest(
          {"native argv"}, app_.AppendASCII("launcher"), request);
  ASSERT_TRUE(notification.has_value());
  EXPECT_FALSE(
      broker_.Request("secondary", app_, profile_, std::move(*notification))
          .value());
  ASSERT_EQ(1u, notifications_.size());
  EXPECT_EQ(profile_, notifications_[0].working_directory);
  EXPECT_EQ("user data",
            *notifications_[0].additional_data.GetDict().FindString("cwd"));
  for (const auto& value :
       {std::string(), std::string("relative"),
        profile_.AsUTF8Unsafe() + std::string("\0suffix", 7)}) {
    request.Set("__xenonRuntime",
                base::DictValue().Set("workingDirectory", value));
    EXPECT_FALSE(XenonAppSingleInstanceBroker::NotificationFromNativeRequest(
                     {}, app_, request)
                     .has_value());
  }
  request.Remove("__xenonRuntime");
  EXPECT_FALSE(XenonAppSingleInstanceBroker::NotificationFromNativeRequest(
                   {}, app_, request)
                   .has_value());
}

TEST_F(XenonAppSingleInstanceTest,
       DistinctApplicationsAndProfilesDoNotConflict) {
  ASSERT_TRUE(Request("app-one:1").value());
  const auto second_app = root_.GetPath().AppendASCII("other-application");
  const auto second_profile = root_.GetPath().AppendASCII("other-profile");
  ASSERT_TRUE(base::CreateDirectory(second_app));
  ASSERT_TRUE(base::CreateDirectory(second_profile));
  EXPECT_TRUE(
      broker_
          .Request("app-two:1", second_app, profile_, ExampleNotification(app_))
          .value());
  EXPECT_TRUE(broker_
                  .Request("profile-two:1", app_, second_profile,
                           ExampleNotification(app_))
                  .value());
  EXPECT_TRUE(notifications_.empty());
}

TEST_F(XenonAppSingleInstanceTest, OnlyOwnerCanReleaseAndRestartAcquiresAgain) {
  ASSERT_TRUE(Request("container:old").value());
  broker_.Release("container:stale-generation");
  EXPECT_TRUE(broker_.Has("container:old"));
  broker_.Release("container:old");
  EXPECT_FALSE(broker_.Has("container:old"));
  ASSERT_TRUE(Request("container:new").value());
  broker_.Release("container:old");
  EXPECT_TRUE(broker_.Has("container:new"));
  broker_.ReleaseAll();
  EXPECT_FALSE(broker_.Has("container:new"));
  EXPECT_TRUE(Request("container:next").value());
}

TEST_F(XenonAppSingleInstanceTest, ValidatesPayloadBeforeAcquiringLock) {
  auto invalid = ExampleNotification(app_);
  invalid.additional_data = base::Value("not an object");
  auto result = broker_.Request("invalid", app_, profile_, std::move(invalid));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ("ERR_INVALID_ARG_VALUE", result.error().code);
  invalid = ExampleNotification(app_);
  invalid.argv.Append(42);
  result = broker_.Request("invalid", app_, profile_, std::move(invalid));
  EXPECT_FALSE(result.has_value());
  invalid = ExampleNotification(app_);
  invalid.additional_data.GetDict().Set(
      "large",
      std::string(XenonAppSingleInstanceTransport::kMaxMessageBytes, 'x'));
  result = broker_.Request("invalid", app_, profile_, std::move(invalid));
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ("ERR_OUT_OF_RANGE", result.error().code);
  EXPECT_FALSE(broker_.Has("invalid"));
  EXPECT_TRUE(Request("valid").value());
}

TEST_F(XenonAppSingleInstanceTest,
       RealSecondProcessCannotAcquireAndDeliversData) {
  ASSERT_TRUE(Request("primary:1").value());
  auto child = Spawn("XenonSingleInstanceNotifyChild");
  ASSERT_TRUE(child.IsValid());
  int exit_code = -1;
  ASSERT_TRUE(base::WaitForMultiprocessTestChildExit(child, base::Seconds(10),
                                                     &exit_code));
  EXPECT_EQ(0, exit_code);
  environment_.RunUntilIdle();
  ASSERT_EQ(1u, notifications_.size());
  EXPECT_EQ("primary:1", owners_[0]);
  EXPECT_EQ(ExampleNotification(app_).argv, notifications_[0].argv);
  EXPECT_EQ(ExampleNotification(app_).additional_data,
            notifications_[0].additional_data);
  // A third process exercises the listener after the first connection closes.
  child = Spawn("XenonSingleInstanceNotifyChild");
  ASSERT_TRUE(child.IsValid());
  ASSERT_TRUE(base::WaitForMultiprocessTestChildExit(child, base::Seconds(10),
                                                     &exit_code));
  EXPECT_EQ(0, exit_code);
  environment_.RunUntilIdle();
  EXPECT_EQ(2u, notifications_.size());
}

TEST_F(XenonAppSingleInstanceTest, ReleasedOwnerDropsAlreadyQueuedRemoteEvent) {
  ASSERT_TRUE(Request("primary:old").value());
  auto child = Spawn("XenonSingleInstanceNotifyChild");
  ASSERT_TRUE(child.IsValid());
  int exit_code = -1;
  ASSERT_TRUE(base::WaitForMultiprocessTestChildExit(child, base::Seconds(10),
                                                     &exit_code));
  ASSERT_EQ(0, exit_code);
  broker_.Release("primary:old");
  ASSERT_TRUE(Request("primary:new").value());
  environment_.RunUntilIdle();
  EXPECT_TRUE(notifications_.empty());
}

TEST_F(XenonAppSingleInstanceTest,
       ProcessTerminationReleasesActualOperatingSystemLock) {
  auto child = Spawn("XenonSingleInstanceHoldChild");
  ASSERT_TRUE(child.IsValid());
  const auto ready = root_.GetPath().AppendASCII("child-ready");
  const auto deadline = base::TimeTicks::Now() + base::Seconds(10);
  while (!base::PathExists(ready) && base::TimeTicks::Now() < deadline) {
    base::PlatformThread::Sleep(base::Milliseconds(10));
  }
  const bool became_ready = base::PathExists(ready);
  // Always reap the test child, including a failed setup.
  EXPECT_TRUE(base::TerminateMultiProcessTestChild(child, 0, true));
  ASSERT_TRUE(became_ready);
  auto acquired = Request("new-primary");
  ASSERT_TRUE(acquired.has_value()) << acquired.error().message;
  EXPECT_TRUE(*acquired);
  EXPECT_TRUE(broker_.Has("new-primary"));
}

TEST_F(XenonAppSingleInstanceTest,
       TransportRejectsWrongLeaseAndStillAcceptsNextRequest) {
  std::vector<std::string> delivered;
  const std::string lease = base::UnguessableToken::Create().ToString();
  auto listener = XenonAppSingleInstanceTransport::Listen(
      lease, base::BindPostTaskToCurrentDefault(base::BindRepeating(
                 [](std::vector<std::string>* received, std::string message) {
                   received->push_back(std::move(message));
                 },
                 &delivered)));
  ASSERT_TRUE(listener.has_value()) << listener.error();
  auto envelope = base::DictValue()
                      .Set("version", 1)
                      .Set("lease", "wrong")
                      .Set("requestId", "one-request")
                      .Set("argv", base::ListValue().Append("host"))
                      .Set("cwd", app_.AsUTF8Unsafe())
                      .Set("additionalData", base::DictValue());
  EXPECT_FALSE(XenonAppSingleInstanceTransport::Notify(
                   (*listener)->endpoint(), *base::WriteJson(envelope))
                   .has_value());
  envelope.Set("lease", lease);
  ASSERT_TRUE(XenonAppSingleInstanceTransport::Notify(
                  (*listener)->endpoint(), *base::WriteJson(envelope))
                  .has_value());
  environment_.RunUntilIdle();
  EXPECT_EQ(1u, delivered.size());
}

TEST_F(XenonAppSingleInstanceTest, RetriedNotificationIsDeliveredOnlyOnce) {
  ASSERT_TRUE(Request("primary").value());
  base::FileEnumerator entries(profile_.AppendASCII("XenonAppLocks"), true,
                               base::FileEnumerator::FILES,
                               FILE_PATH_LITERAL("endpoint.json"),
                               base::FileEnumerator::FolderSearchPolicy::ALL);
  const auto metadata_path = entries.Next();
  ASSERT_FALSE(metadata_path.empty());
  std::string contents;
  ASSERT_TRUE(base::ReadFileToString(metadata_path, &contents));
  auto metadata = base::JSONReader::ReadDict(contents, base::JSON_PARSE_RFC);
  ASSERT_TRUE(metadata);
  ASSERT_TRUE(metadata->FindString("endpoint"));
  ASSERT_TRUE(metadata->FindString("lease"));
  auto envelope = base::DictValue()
                      .Set("version", 1)
                      .Set("lease", *metadata->FindString("lease"))
                      .Set("requestId", "same-logical-request")
                      .Set("argv", base::ListValue().Append("host"))
                      .Set("cwd", app_.AsUTF8Unsafe())
                      .Set("additionalData", base::DictValue());
  const auto message = base::WriteJson(envelope);
  ASSERT_TRUE(message);
  for (int attempt = 0; attempt < 2; ++attempt) {
    auto notified = XenonAppSingleInstanceTransport::Notify(
        *metadata->FindString("endpoint"), *message);
    ASSERT_TRUE(notified.has_value()) << notified.error();
  }
  environment_.RunUntilIdle();
  EXPECT_EQ(1u, notifications_.size());
}

}  // namespace

MULTIPROCESS_TEST_MAIN(XenonSingleInstanceNotifyChild) {
  base::test::TaskEnvironment environment;
  const auto* command = base::CommandLine::ForCurrentProcess();
  const auto app = command->GetSwitchValuePath("singleton-app");
  const auto profile = command->GetSwitchValuePath("singleton-profile");
  XenonAppSingleInstanceBroker broker(base::DoNothing());
  auto acquired =
      broker.Request("secondary", app, profile, ExampleNotification(app));
  if (!acquired.has_value()) {
    return 2;
  }
  return *acquired ? 3 : 0;
}

MULTIPROCESS_TEST_MAIN(XenonSingleInstanceHoldChild) {
  base::test::TaskEnvironment environment;
  const auto* command = base::CommandLine::ForCurrentProcess();
  const auto app = command->GetSwitchValuePath("singleton-app");
  const auto profile = command->GetSwitchValuePath("singleton-profile");
  XenonAppSingleInstanceBroker broker(base::DoNothing());
  auto acquired =
      broker.Request("holder", app, profile, ExampleNotification(app));
  if (!acquired.has_value() || !*acquired) {
    return 2;
  }
  const auto ready =
      command->GetSwitchValuePath("singleton-root").AppendASCII("child-ready");
  if (!base::WriteFile(ready, "ready")) {
    return 3;
  }
  // The parent explicitly terminates this test child to verify OS lock release.
  base::PlatformThread::Sleep(base::Seconds(20));
  return 4;
}

}  // namespace xenon::ipc
