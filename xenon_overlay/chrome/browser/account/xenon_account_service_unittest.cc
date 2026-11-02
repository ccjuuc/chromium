// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/account/xenon_account_service.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/xenon_login_controller.h"
#include "xenon_overlay/chrome/browser/xenon_prefs.h"

namespace {

constexpr char kAccountId[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr char kToken[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr char kUsername[] = "test-user";
constexpr char kPassword[] = "account-test-password!";

using Response = XenonAccountService::Response;

base::DictValue AccountBody() {
  return base::DictValue().Set(
      "account",
      base::DictValue().Set("id", kAccountId).Set("username", kUsername));
}

base::DictValue SessionBody() {
  return AccountBody()
      .Set("token", kToken)
      .Set("expiresAt",
           (base::Time::Now() + base::Days(1)).InSecondsFSinceUnixEpoch());
}

// Runs only through the service's worker task runner; tests inspect the atomic
// counters after request completion. No network or production credentials.
class FakeAccountServer {
 public:
  Response Handle(std::string method,
                  std::string path,
                  std::string token,
                  base::DictValue body) {
    if (path == "/auth/login" || path == "/auth/register") {
      ++authentication_requests;
      EXPECT_TRUE(token.empty());
      EXPECT_EQ(body.FindString("username") ? *body.FindString("username") : "",
                kUsername);
      EXPECT_EQ(body.FindString("password") ? *body.FindString("password") : "",
                kPassword);
      return {200, SessionBody(), {}};
    }
    EXPECT_EQ(token, kToken);
    if (reject_session) {
      return {401, {}, {}};
    }
    if (path == "/auth/me") {
      ++validation_requests;
      return {200, AccountBody(), {}};
    }
    if (path == "/auth/logout") {
      ++revocation_requests;
      EXPECT_EQ(method, "POST");
      revoked_device_id =
          body.FindString("deviceId") ? *body.FindString("deviceId") : "";
      return {200, base::DictValue().Set("ok", true), {}};
    }
    if (path == "/devices") {
      return {200, base::DictValue().Set("devices", base::ListValue()), {}};
    }
    if (path.starts_with("/devices/")) {
      ++device_requests;
      return {200,
              base::DictValue().Set(
                  "device", base::DictValue().Set("id", path.substr(9, 32))),
              {}};
    }
    ADD_FAILURE() << "Unexpected account request: " << method << " " << path;
    return {500, {}, {}};
  }

  std::atomic<bool> reject_session{false};
  std::atomic<int> authentication_requests{0};
  std::atomic<int> validation_requests{0};
  std::atomic<int> device_requests{0};
  std::atomic<int> revocation_requests{0};
  std::string revoked_device_id;
};

class XenonAccountServiceTest : public testing::Test {
 protected:
  void SetUp() override {
    profile_ = std::make_unique<TestingProfile>();
    XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
        &FakeAccountServer::Handle, base::Unretained(&server_)));
  }

  void TearDown() override {
    profile_.reset();
    task_environment_.RunUntilIdle();
    XenonAccountService::SetRequestHandlerForTesting({});
  }

  XenonAccountService* InitializedService() {
    auto* service = XenonAccountService::GetForProfile(profile_.get());
    task_environment_.RunUntilIdle();
    return service;
  }

  void SaveSession() {
    profile_->GetPrefs()->SetString(xenon::prefs::kAccountCredentials,
                                    *base::WriteJson(SessionBody()));
  }

  base::DictValue Login(XenonAccountService* service) {
    base::test::TestFuture<base::DictValue> result;
    service->Authenticate(false, kUsername, kPassword, result.GetCallback());
    return result.Take();
  }

  content::BrowserTaskEnvironment task_environment_;
  FakeAccountServer server_;
  std::unique_ptr<TestingProfile> profile_;
};

TEST_F(XenonAccountServiceTest, LegacyBooleanDoesNotAuthorizeLogin) {
  profile_->GetPrefs()->SetBoolean(xenon::prefs::kAppSessionLoggedIn, true);
  auto* service = InitializedService();
  EXPECT_FALSE(service->HasAuthenticatedSession());
  EXPECT_FALSE(xenon::XenonLoginController::IsSessionLoggedIn(profile_.get()));
  EXPECT_EQ(server_.authentication_requests, 0);
  EXPECT_EQ(server_.validation_requests, 0);
}

TEST_F(XenonAccountServiceTest, RestoredTokenRequiresServerValidation) {
  SaveSession();
  profile_->GetPrefs()->SetBoolean(xenon::prefs::kAppSessionLoggedIn, true);
  base::test::TestFuture<void> request_started;
  auto release = std::make_shared<base::WaitableEvent>();
  base::ScopedClosureRunner ensure_released(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      release));
  XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
      [](FakeAccountServer* server, base::RepeatingClosure started,
         std::shared_ptr<base::WaitableEvent> release, std::string method,
         std::string path, std::string token, base::DictValue body) {
        if (path == "/auth/me") {
          started.Run();
          base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
          release->Wait();
        }
        return server->Handle(std::move(method), std::move(path),
                              std::move(token), std::move(body));
      },
      base::Unretained(&server_),
      request_started.GetSequenceBoundRepeatingCallback(), release));

  auto* service = XenonAccountService::GetForProfile(profile_.get());
  ASSERT_TRUE(request_started.Wait());
  EXPECT_FALSE(service->HasAuthenticatedSession());
  EXPECT_FALSE(xenon::XenonLoginController::IsSessionLoggedIn(profile_.get()));
  EXPECT_TRUE(service->GetState().FindBool("restoring").value_or(false));

  release->Signal();
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(service->HasAuthenticatedSession());
  EXPECT_EQ(server_.validation_requests, 1);
  EXPECT_EQ(server_.authentication_requests, 0);
  EXPECT_EQ(server_.device_requests, 1);
}

TEST_F(XenonAccountServiceTest, LoginStoresPlainSessionTokenWithoutPassword) {
  auto* service = InitializedService();
  const auto state = Login(service);
  EXPECT_TRUE(state.FindBool("authenticated").value_or(false));
  EXPECT_TRUE(service->HasAuthenticatedSession());
  EXPECT_EQ(server_.device_requests, 1);

  const auto& stored =
      profile_->GetPrefs()->GetString(xenon::prefs::kAccountCredentials);
  const auto session = base::JSONReader::ReadDict(stored, base::JSON_PARSE_RFC);
  ASSERT_TRUE(session);
  EXPECT_EQ(session->FindString("token") ? *session->FindString("token") : "",
            kToken);
  EXPECT_EQ(stored.find(kPassword), std::string::npos);
  EXPECT_FALSE(session->contains("password"));
  const auto public_state = base::WriteJson(state);
  ASSERT_TRUE(public_state);
  EXPECT_EQ(public_state->find(kToken), std::string::npos);
  EXPECT_EQ(public_state->find(kPassword), std::string::npos);
  EXPECT_FALSE(state.contains("seed"));
}

TEST_F(XenonAccountServiceTest, UnauthorizedResponseClearsPersistedSession) {
  auto* service = InitializedService();
  ASSERT_TRUE(Login(service).FindBool("authenticated").value_or(false));
  server_.reject_session = true;
  base::test::TestFuture<base::DictValue> result;
  service->Refresh(result.GetCallback());
  const auto state = result.Take();
  EXPECT_FALSE(service->HasAuthenticatedSession());
  EXPECT_FALSE(state.FindBool("authenticated").value_or(true));
  EXPECT_TRUE(state.FindList("devices")->empty());
  EXPECT_TRUE(profile_->GetPrefs()
                  ->GetString(xenon::prefs::kAccountCredentials)
                  .empty());
}

TEST_F(XenonAccountServiceTest,
       LogoutClearsImmediatelyAndRevokesServerSession) {
  auto* service = InitializedService();
  ASSERT_TRUE(Login(service).FindBool("authenticated").value_or(false));
  const auto device_id = service->GetDeviceId();
  ASSERT_FALSE(device_id.empty());
  base::test::TestFuture<base::DictValue> result;
  service->Logout(result.GetCallback());
  EXPECT_TRUE(result.IsReady());
  EXPECT_FALSE(service->HasAuthenticatedSession());
  EXPECT_TRUE(profile_->GetPrefs()
                  ->GetString(xenon::prefs::kAccountCredentials)
                  .empty());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(server_.revocation_requests, 1);
  EXPECT_EQ(server_.revoked_device_id, device_id);
}

TEST_F(XenonAccountServiceTest, LogoutRejectsAnAlreadyRunningLoginResponse) {
  auto* service = InitializedService();
  base::test::TestFuture<void> request_started;
  auto release = std::make_shared<base::WaitableEvent>();
  base::ScopedClosureRunner ensure_released(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      release));
  XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
      [](FakeAccountServer* server, base::RepeatingClosure started,
         std::shared_ptr<base::WaitableEvent> release, std::string method,
         std::string path, std::string token, base::DictValue body) {
        if (path == "/auth/login") {
          started.Run();
          base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
          release->Wait();
        }
        return server->Handle(std::move(method), std::move(path),
                              std::move(token), std::move(body));
      },
      base::Unretained(&server_),
      request_started.GetSequenceBoundRepeatingCallback(), release));
  base::test::TestFuture<base::DictValue> login_result;
  service->Authenticate(false, kUsername, kPassword,
                        login_result.GetCallback());
  ASSERT_TRUE(request_started.Wait());
  service->Logout({});
  EXPECT_FALSE(service->HasAuthenticatedSession());
  release->Signal();

  EXPECT_FALSE(login_result.Get().FindBool("authenticated").value_or(true));
  EXPECT_FALSE(service->HasAuthenticatedSession());
  EXPECT_TRUE(profile_->GetPrefs()
                  ->GetString(xenon::prefs::kAccountCredentials)
                  .empty());
  EXPECT_EQ(server_.device_requests, 0);
}

TEST_F(XenonAccountServiceTest, ProfilesAreIsolatedAndIncognitoSharesOriginal) {
  auto* service = InitializedService();
  ASSERT_TRUE(Login(service).FindBool("authenticated").value_or(false));
  auto* incognito = profile_->GetPrimaryOTRProfile(/*create_if_needed=*/true);
  EXPECT_EQ(XenonAccountService::GetForProfile(incognito), service);
  EXPECT_TRUE(xenon::XenonLoginController::IsSessionLoggedIn(incognito));

  TestingProfile other_profile;
  auto* other_service = XenonAccountService::GetForProfile(&other_profile);
  task_environment_.RunUntilIdle();
  EXPECT_NE(other_service, service);
  EXPECT_FALSE(other_service->HasAuthenticatedSession());
  EXPECT_TRUE(other_profile.GetPrefs()
                  ->GetString(xenon::prefs::kAccountCredentials)
                  .empty());
  other_service->Logout({});
  EXPECT_TRUE(service->HasAuthenticatedSession());
}

TEST_F(XenonAccountServiceTest, ExpiredCachedTokenIsRemovedWithoutNetwork) {
  auto session = SessionBody();
  session.Set(
      "expiresAt",
      (base::Time::Now() - base::Seconds(1)).InSecondsFSinceUnixEpoch());
  profile_->GetPrefs()->SetString(xenon::prefs::kAccountCredentials,
                                  *base::WriteJson(session));
  auto* service = InitializedService();
  EXPECT_FALSE(service->HasAuthenticatedSession());
  EXPECT_TRUE(profile_->GetPrefs()
                  ->GetString(xenon::prefs::kAccountCredentials)
                  .empty());
  EXPECT_EQ(server_.validation_requests, 0);
  EXPECT_EQ(server_.authentication_requests, 0);
}

}  // namespace
