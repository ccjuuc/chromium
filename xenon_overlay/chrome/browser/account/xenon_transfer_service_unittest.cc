// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/account/xenon_transfer_service.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

// Inject only the file worker's completed result. The account requests and
// their asynchronous callbacks still run through the normal service path.
class XenonTransferServiceTestPeer {
 public:
  static void CompleteReceivedFile(XenonTransferService* service,
                                   const std::string& id) {
    service->active_id_ = id;
    service->active_role_ = "receive";
    service->attempted_files_.insert(id);
    service->accepted_receives_.insert(id);
    service->QueueFinalization(id, true);
  }
};

namespace {

constexpr char kAccountId[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr char kOtherDevice[] = "cccccccccccccccccccccccccccccccc";
constexpr char kInvitationId[] = "dddddddddddddddddddddddddddddddd";
constexpr char kToken[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char kSecret[] = "never-send-server-credentials-to-webui";
using Response = XenonAccountService::Response;

class FakeTransferServer {
 public:
  Response Handle(std::string method,
                  std::string path,
                  std::string token,
                  base::DictValue body) {
    if (path == "/auth/login") {
      return {200,
              base::DictValue()
                  .Set("account", base::DictValue()
                                      .Set("id", kAccountId)
                                      .Set("username", "transfer-test"))
                  .Set("token", kToken)
                  .Set("expiresAt", (base::Time::Now() + base::Days(1))
                                        .InSecondsFSinceUnixEpoch()),
              {}};
    }
    EXPECT_EQ(token, kToken);
    if (path == "/auth/logout" || method == "DELETE") {
      return {200, base::DictValue().Set("ok", true), {}};
    }
    if (method == "GET" && path.ends_with("/transfers")) {
      ++list_requests;
      if (fail_lists) {
        return {503, {}, {}};
      }
      base::ListValue list;
      if (!invitation.empty()) {
        list.Append(invitation.Clone());
      }
      return {200, base::DictValue().Set("transfers", std::move(list)), {}};
    }
    if (path == "/devices") {
      return {200, base::DictValue().Set("devices", base::ListValue()), {}};
    }
    if (path.starts_with("/devices/")) {
      device_id = path.substr(9, 32);
      return {200,
              base::DictValue().Set("device",
                                    base::DictValue().Set("id", device_id)),
              {}};
    }
    if (path == "/transfers") {
      ++create_requests;
      last_creation = body.Clone();
      invitation = body.Clone();
      invitation.Set("id", kInvitationId);
      invitation.Set("senderName", "This device");
      invitation.Set("receiverName", "Other device");
      invitation.Set("status", "pending");
      invitation.Set("createdAt", base::Time::Now().InSecondsFSinceUnixEpoch());
      invitation.Set(
          "expiresAt",
          (base::Time::Now() + base::Minutes(5)).InSecondsFSinceUnixEpoch());
      invitation.Set("bearerToken", kSecret);
      return {201, base::DictValue().Set("transfer", invitation.Clone()), {}};
    }
    if (path.starts_with("/transfers/")) {
      ++action_requests;
      EXPECT_EQ(*body.FindString("deviceId"), device_id);
      if (path.ends_with("/complete")) {
        ++completion_requests;
        if (completion_failures > 0) {
          --completion_failures;
          return {503, {}, {}};
        }
        invitation.Set("status", "completed");
        if (lose_completion_response) {
          lose_completion_response = false;
          return {0, {}, "连接中断"};
        }
      } else if (path.ends_with("/accept")) {
        invitation.Set("status", *invitation.FindString("kind") == "text"
                                     ? "completed"
                                     : "accepted");
      } else if (path.ends_with("/cancel")) {
        invitation.Set("status", "cancelled");
      } else if (path.ends_with("/reject")) {
        invitation.Set("status", "rejected");
      } else {
        ADD_FAILURE() << "Unexpected transfer action: " << path;
        return {500, {}, {}};
      }
      return {200, base::DictValue().Set("transfer", invitation.Clone()), {}};
    }
    ADD_FAILURE() << "Unexpected request: " << method << " " << path;
    return {500, {}, {}};
  }

  void SetIncomingText() {
    invitation =
        base::DictValue()
            .Set("id", kInvitationId)
            .Set("senderDeviceId", kOtherDevice)
            .Set("receiverDeviceId", device_id)
            .Set("senderName", "Other device")
            .Set("receiverName", "This device")
            .Set("kind", "text")
            .Set("text", "Hello from my other device")
            .Set("fileName", "")
            .Set("fileSize", 0.0)
            .Set("status", "pending")
            .Set("createdAt", base::Time::Now().InSecondsFSinceUnixEpoch())
            .Set("expiresAt", (base::Time::Now() + base::Minutes(5))
                                  .InSecondsFSinceUnixEpoch())
            .Set("bearerToken", kSecret);
  }

  void SetAcceptedFile() {
    SetIncomingText();
    invitation.Set("kind", "file");
    invitation.Set("text", "");
    invitation.Set("fileName", "received.txt");
    invitation.Set("fileSize", 12.0);
    invitation.Set("status", "accepted");
  }

  std::string device_id;
  base::DictValue invitation;
  base::DictValue last_creation;
  std::atomic<int> list_requests{0};
  std::atomic<int> create_requests{0};
  std::atomic<int> action_requests{0};
  std::atomic<int> completion_requests{0};
  int completion_failures = 0;
  bool lose_completion_response = false;
  bool fail_lists = false;
};

class XenonTransferServiceTest : public testing::Test {
 protected:
  void SetUp() override {
    profile_ = std::make_unique<TestingProfile>();
    XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
        &FakeTransferServer::Handle, base::Unretained(&server_)));
  }

  void TearDown() override {
    profile_.reset();
    task_environment_.RunUntilIdle();
    XenonAccountService::SetRequestHandlerForTesting({});
  }

  XenonTransferService* LoggedInService() {
    auto* service = XenonTransferService::GetForProfile(profile_.get());
    task_environment_.RunUntilIdle();
    base::test::TestFuture<base::DictValue> login;
    XenonAccountService::GetForProfile(profile_.get())
        ->Authenticate(false, "transfer-test", "test-password-valid",
                       login.GetCallback());
    EXPECT_TRUE(login.Get().FindBool("authenticated").value_or(false));
    task_environment_.RunUntilIdle();
    return service;
  }

  base::DictValue Refresh(XenonTransferService* service) {
    base::test::TestFuture<base::DictValue> result;
    service->Refresh(result.GetCallback());
    return result.Take();
  }

  content::BrowserTaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  FakeTransferServer server_;
  std::unique_ptr<TestingProfile> profile_;
};

TEST_F(XenonTransferServiceTest, SignedOutCannotSend) {
  auto* service = XenonTransferService::GetForProfile(profile_.get());
  task_environment_.RunUntilIdle();
  base::test::TestFuture<base::DictValue> result;
  service->SendText(kOtherDevice, "hello", result.GetCallback());
  const auto state = result.Take();
  EXPECT_FALSE(state.FindString("error")->empty());
  EXPECT_TRUE(state.FindList("transfers")->empty());
  EXPECT_EQ(server_.create_requests, 0);
}

TEST_F(XenonTransferServiceTest,
       TextInvitationUsesOwnDeviceAndSanitizesResponse) {
  auto* service = LoggedInService();
  base::test::TestFuture<base::DictValue> result;
  service->SendText(kOtherDevice, "你好，同账号设备", result.GetCallback());
  const auto state = result.Take();
  ASSERT_EQ(state.FindList("transfers")->size(), 1u);
  EXPECT_EQ(*server_.last_creation.FindString("senderDeviceId"),
            server_.device_id);
  EXPECT_EQ(*server_.last_creation.FindString("receiverDeviceId"),
            kOtherDevice);
  EXPECT_EQ(*server_.last_creation.FindString("kind"), "text");
  EXPECT_EQ(*server_.last_creation.FindString("text"), "你好，同账号设备");
  EXPECT_FALSE(state.FindBool("busy").value_or(true));
  EXPECT_EQ(base::WriteJson(state)->find(kToken), std::string::npos);
  EXPECT_EQ(base::WriteJson(state)->find(kSecret), std::string::npos);
}

TEST_F(XenonTransferServiceTest, IncomingTextRequiresExplicitAcceptance) {
  auto* service = LoggedInService();
  server_.SetIncomingText();
  auto state = Refresh(service);
  ASSERT_EQ(state.FindList("transfers")->size(), 1u);
  EXPECT_EQ(
      *state.FindList("transfers")->front().GetDict().FindString("status"),
      "pending");
  EXPECT_EQ(server_.action_requests, 0);
  base::test::TestFuture<base::DictValue> accepted;
  service->Respond(kInvitationId, "accept", accepted.GetCallback());
  state = accepted.Take();
  EXPECT_EQ(
      *state.FindList("transfers")->front().GetDict().FindString("status"),
      "completed");
  EXPECT_EQ(server_.action_requests, 1);
}

TEST_F(XenonTransferServiceTest, MaximumEscapedTextCanBeSent) {
  auto* service = LoggedInService();
  const std::string text(16 * 1024, '<');
  ASSERT_GT(base::WriteJson(base::DictValue().Set("text", text))->size(),
            64u * 1024u);
  base::test::TestFuture<base::DictValue> result;
  service->SendText(kOtherDevice, text, result.GetCallback());
  EXPECT_TRUE(result.Get().FindString("error")->empty());
  EXPECT_EQ(server_.create_requests, 1);
  EXPECT_EQ(*server_.last_creation.FindString("text"), text);

  base::test::TestFuture<base::DictValue> too_large;
  service->SendText(kOtherDevice, text + '<', too_large.GetCallback());
  EXPECT_FALSE(too_large.Get().FindString("error")->empty());
  EXPECT_EQ(server_.create_requests, 1);
}

TEST_F(XenonTransferServiceTest, CompletedFileRetriesTemporaryFailure) {
  auto* service = LoggedInService();
  server_.SetAcceptedFile();
  Refresh(service);
  server_.completion_failures = 1;
  XenonTransferServiceTestPeer::CompleteReceivedFile(service, kInvitationId);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(server_.completion_requests, 1);
  EXPECT_TRUE(service->GetState().FindDict("job")->FindBool("active").value());
  EXPECT_FALSE(Refresh(service).FindString("error")->empty());

  // Neither repeated refreshes nor another acceptance may restart a receiver
  // or submit an unthrottled completion request.
  base::test::TestFuture<base::DictValue> accepted;
  service->Respond(kInvitationId, "accept", accepted.GetCallback());
  EXPECT_FALSE(accepted.Get().FindString("error")->empty());
  EXPECT_EQ(server_.action_requests, 1);
  task_environment_.FastForwardBy(base::Seconds(1));
  EXPECT_EQ(server_.completion_requests, 1);
  task_environment_.FastForwardBy(base::Seconds(1));
  EXPECT_EQ(server_.completion_requests, 2);
  const auto state = service->GetState();
  EXPECT_EQ(
      *state.FindList("transfers")->front().GetDict().FindString("status"),
      "completed");
  EXPECT_TRUE(state.FindString("error")->empty());
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
}

TEST_F(XenonTransferServiceTest, LostCompletionResponseRetriesIdempotently) {
  auto* service = LoggedInService();
  server_.SetAcceptedFile();
  Refresh(service);
  server_.lose_completion_response = true;
  server_.fail_lists = true;
  XenonTransferServiceTestPeer::CompleteReceivedFile(service, kInvitationId);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(server_.completion_requests, 1);
  EXPECT_EQ(*server_.invitation.FindString("status"), "completed");
  EXPECT_TRUE(service->GetState().FindDict("job")->FindBool("active").value());

  task_environment_.FastForwardBy(base::Seconds(2));
  EXPECT_EQ(server_.completion_requests, 2);
  server_.fail_lists = false;
  const auto state = Refresh(service);
  EXPECT_EQ(
      *state.FindList("transfers")->front().GetDict().FindString("status"),
      "completed");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  task_environment_.FastForwardBy(base::Seconds(4));
  EXPECT_EQ(server_.completion_requests, 2);
}

TEST_F(XenonTransferServiceTest,
       TerminalListAcknowledgesLostCompletionResponse) {
  auto* service = LoggedInService();
  server_.SetAcceptedFile();
  Refresh(service);
  server_.lose_completion_response = true;
  XenonTransferServiceTestPeer::CompleteReceivedFile(service, kInvitationId);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(server_.completion_requests, 1);
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
  EXPECT_TRUE(service->GetState().FindString("error")->empty());
  task_environment_.FastForwardBy(base::Seconds(4));
  EXPECT_EQ(server_.completion_requests, 1);
}

TEST_F(XenonTransferServiceTest, CompletedFileRetryStopsAtInvitationExpiry) {
  auto* service = LoggedInService();
  server_.SetAcceptedFile();
  server_.invitation.Set(
      "expiresAt",
      (base::Time::Now() + base::Seconds(3)).InSecondsFSinceUnixEpoch());
  Refresh(service);
  server_.completion_failures = 100;
  XenonTransferServiceTestPeer::CompleteReceivedFile(service, kInvitationId);
  task_environment_.RunUntilIdle();
  task_environment_.FastForwardBy(base::Seconds(4));
  EXPECT_EQ(server_.completion_requests, 2);
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
  task_environment_.FastForwardBy(base::Seconds(4));
  EXPECT_EQ(server_.completion_requests, 2);
}

TEST_F(XenonTransferServiceTest, LogoutStopsCompletedFileRetry) {
  auto* service = LoggedInService();
  server_.SetAcceptedFile();
  Refresh(service);
  server_.completion_failures = 1;
  XenonTransferServiceTestPeer::CompleteReceivedFile(service, kInvitationId);
  task_environment_.RunUntilIdle();
  XenonAccountService::GetForProfile(profile_.get())->Logout({});
  task_environment_.FastForwardBy(base::Seconds(4));
  EXPECT_EQ(server_.completion_requests, 1);
  EXPECT_TRUE(service->GetState().FindList("transfers")->empty());
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
}

TEST_F(XenonTransferServiceTest, LogoutDiscardsLateCompletionResponse) {
  auto* service = LoggedInService();
  server_.SetAcceptedFile();
  Refresh(service);
  base::test::TestFuture<void> request_started;
  auto release = std::make_shared<base::WaitableEvent>();
  base::ScopedClosureRunner ensure_released(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      release));
  XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
      [](FakeTransferServer* server, base::RepeatingClosure started,
         std::shared_ptr<base::WaitableEvent> release, std::string method,
         std::string path, std::string token, base::DictValue body) {
        if (path.ends_with("/complete")) {
          started.Run();
          base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
          release->Wait();
        }
        return server->Handle(std::move(method), std::move(path),
                              std::move(token), std::move(body));
      },
      base::Unretained(&server_),
      request_started.GetSequenceBoundRepeatingCallback(), release));
  XenonTransferServiceTestPeer::CompleteReceivedFile(service, kInvitationId);
  ASSERT_TRUE(request_started.Wait());
  XenonAccountService::GetForProfile(profile_.get())->Logout({});
  release->Signal();
  task_environment_.RunUntilIdle();
  task_environment_.FastForwardBy(base::Seconds(4));
  EXPECT_EQ(server_.completion_requests, 1);
  EXPECT_TRUE(service->GetState().FindList("transfers")->empty());
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
}

TEST_F(XenonTransferServiceTest, CannotAcceptAnOutgoingInvitation) {
  auto* service = LoggedInService();
  base::test::TestFuture<base::DictValue> sent;
  service->SendText(kOtherDevice, "hello", sent.GetCallback());
  ASSERT_EQ(sent.Get().FindList("transfers")->size(), 1u);
  base::test::TestFuture<base::DictValue> result;
  service->Respond(kInvitationId, "accept", result.GetCallback());
  EXPECT_FALSE(result.Get().FindString("error")->empty());
  EXPECT_EQ(server_.action_requests, 0);
}

TEST_F(XenonTransferServiceTest, FileWaitsForConsentAndKeepsSourcePathNative) {
  auto* service = LoggedInService();
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto source = directory.GetPath().AppendASCII("test-file.txt");
  ASSERT_TRUE(base::WriteFile(source, "test contents"));
  base::test::TestFuture<base::DictValue> sent;
  service->SendFile(kOtherDevice, source, sent.GetCallback());
  auto state = sent.Take();
  EXPECT_EQ(*state.FindDict("job")->FindString("state"), "waiting");
  EXPECT_TRUE(state.FindDict("job")->FindBool("active").value_or(false));
  EXPECT_EQ(*server_.last_creation.FindString("fileName"), "test-file.txt");
  EXPECT_EQ(server_.last_creation.FindDouble("fileSize"), 13.0);
  EXPECT_EQ(base::WriteJson(state)->find(source.AsUTF8Unsafe()),
            std::string::npos);
  EXPECT_EQ(server_.action_requests, 0);
  base::test::TestFuture<base::DictValue> cancelled;
  service->Cancel(cancelled.GetCallback());
  state = cancelled.Take();
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value_or(true));
  EXPECT_EQ(server_.action_requests, 1);
}

TEST_F(XenonTransferServiceTest, InvalidMetadataIsNotExposed) {
  auto* service = LoggedInService();
  server_.SetIncomingText();
  server_.invitation.Set("kind", "file");
  server_.invitation.Set("text", "");
  server_.invitation.Set("fileName", "../outside.txt");
  const auto state = Refresh(service);
  EXPECT_FALSE(state.FindString("error")->empty());
  EXPECT_TRUE(state.FindList("transfers")->empty());
}

TEST_F(XenonTransferServiceTest, LogoutImmediatelyClearsInvitations) {
  auto* service = LoggedInService();
  server_.SetIncomingText();
  ASSERT_EQ(Refresh(service).FindList("transfers")->size(), 1u);
  XenonAccountService::GetForProfile(profile_.get())->Logout({});
  EXPECT_TRUE(service->GetState().FindList("transfers")->empty());
  EXPECT_FALSE(
      service->GetState().FindDict("job")->FindBool("active").value_or(true));
}

TEST_F(XenonTransferServiceTest, LogoutDiscardsLateInvitationCreation) {
  auto* service = LoggedInService();
  base::test::TestFuture<void> request_started;
  auto release = std::make_shared<base::WaitableEvent>();
  base::ScopedClosureRunner ensure_released(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      release));
  XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
      [](FakeTransferServer* server, base::RepeatingClosure started,
         std::shared_ptr<base::WaitableEvent> release, std::string method,
         std::string path, std::string token, base::DictValue body) {
        if (path == "/transfers") {
          started.Run();
          base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
          release->Wait();
        }
        return server->Handle(std::move(method), std::move(path),
                              std::move(token), std::move(body));
      },
      base::Unretained(&server_),
      request_started.GetSequenceBoundRepeatingCallback(), release));
  base::test::TestFuture<base::DictValue> result;
  service->SendText(kOtherDevice, "hello", result.GetCallback());
  ASSERT_TRUE(request_started.Wait());
  XenonAccountService::GetForProfile(profile_.get())->Logout({});
  release->Signal();
  EXPECT_TRUE(result.Get().FindList("transfers")->empty());
  EXPECT_TRUE(service->GetState().FindList("transfers")->empty());
}

}  // namespace
