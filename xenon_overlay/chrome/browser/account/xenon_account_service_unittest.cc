// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/account/xenon_account_service.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/browser/browser_process.h"
#include "chrome/test/base/testing_profile.h"
#include "components/os_crypt/async/browser/os_crypt_async.h"
#include "components/os_crypt/async/common/encryptor.h"
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
      auto session = SessionBody();
      session.FindDict("account")->Set("id", account_id);
      return {200, std::move(session), {}};
    }
    EXPECT_EQ(token, kToken);
    if (reject_session) {
      return {401, {}, {}};
    }
    if (path == "/auth/me") {
      ++validation_requests;
      auto account = AccountBody();
      account.FindDict("account")->Set("id", account_id);
      return {200, std::move(account), {}};
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
      if (method == "PUT") {
        registered_public_key =
            body.FindString("publicKey") ? *body.FindString("publicKey") : "";
      }
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
  std::string account_id = kAccountId;
  std::string registered_public_key;
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

  base::DictValue SwitchAccount(XenonAccountService* service,
                                std::string account_id) {
    service->Logout({});
    task_environment_.RunUntilIdle();
    server_.account_id = std::move(account_id);
    return Login(service);
  }

  scoped_refptr<os_crypt_async::Encryptor> Encryptor() {
    base::test::TestFuture<scoped_refptr<os_crypt_async::Encryptor>> ready;
    g_browser_process->os_crypt_async()->GetInstance(ready.GetCallback());
    return ready.Take();
  }

  base::DictValue ReadDeviceIdentities() {
    std::string encrypted;
    std::string json;
    EXPECT_TRUE(base::Base64Decode(
        profile_->GetPrefs()->GetString(xenon::prefs::kAccountDeviceIdentity),
        &encrypted));
    EXPECT_TRUE(Encryptor()->DecryptString(encrypted, &json));
    auto parsed = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
    EXPECT_TRUE(parsed);
    return parsed ? std::move(*parsed) : base::DictValue();
  }

  void WriteDeviceIdentities(const base::DictValue& value) {
    const auto json = base::WriteJson(value);
    ASSERT_TRUE(json);
    std::string encrypted;
    ASSERT_TRUE(Encryptor()->EncryptString(*json, &encrypted));
    profile_->GetPrefs()->SetString(xenon::prefs::kAccountDeviceIdentity,
                                    base::Base64Encode(encrypted));
  }

  XenonAccountService* RecreateProfileWithIdentities() {
    const auto encrypted =
        profile_->GetPrefs()->GetString(xenon::prefs::kAccountDeviceIdentity);
    profile_.reset();
    task_environment_.RunUntilIdle();
    profile_ = std::make_unique<TestingProfile>();
    profile_->GetPrefs()->SetString(xenon::prefs::kAccountDeviceIdentity,
                                    encrypted);
    return InitializedService();
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

TEST_F(XenonAccountServiceTest, AccountSwitchPreservesEachDeviceIdentity) {
  auto* service = InitializedService();
  ASSERT_TRUE(Login(service).FindBool("authenticated").value_or(false));
  const auto first_device = service->GetDeviceId();
  const auto first_key = server_.registered_public_key;
  ASSERT_FALSE(first_device.empty());
  ASSERT_FALSE(first_key.empty());

  constexpr char kSecondAccount[] = "cccccccccccccccccccccccccccccccc";
  ASSERT_TRUE(SwitchAccount(service, kSecondAccount)
                  .FindBool("authenticated")
                  .value_or(false));
  const auto second_device = service->GetDeviceId();
  const auto second_key = server_.registered_public_key;
  EXPECT_NE(second_device, first_device);
  EXPECT_NE(second_key, first_key);
  ASSERT_TRUE(SwitchAccount(service, kAccountId)
                  .FindBool("authenticated")
                  .value_or(false));
  EXPECT_EQ(service->GetDeviceId(), first_device);
  EXPECT_EQ(server_.registered_public_key, first_key);

  // A fresh service must load both accounts from the encrypted preference,
  // rather than succeeding only because the earlier keys remain in memory.
  service->Logout({});
  task_environment_.RunUntilIdle();
  service = RecreateProfileWithIdentities();
  server_.account_id = kSecondAccount;
  ASSERT_TRUE(Login(service).FindBool("authenticated").value_or(false));
  EXPECT_EQ(service->GetDeviceId(), second_device);
  EXPECT_EQ(server_.registered_public_key, second_key);
  ASSERT_TRUE(SwitchAccount(service, kAccountId)
                  .FindBool("authenticated")
                  .value_or(false));
  EXPECT_EQ(service->GetDeviceId(), first_device);
  EXPECT_EQ(server_.registered_public_key, first_key);
}

TEST_F(XenonAccountServiceTest, ExistingSingleIdentityMigratesWithoutRotation) {
  auto* service = InitializedService();
  ASSERT_TRUE(Login(service).FindBool("authenticated").value_or(false));
  const auto device = service->GetDeviceId();
  const auto public_key = server_.registered_public_key;
  const auto stored = ReadDeviceIdentities();
  ASSERT_TRUE(stored.FindDict("accounts"));
  const auto* original = stored.FindDict("accounts")->FindDict(kAccountId);
  ASSERT_TRUE(original);
  service->Logout({});
  task_environment_.RunUntilIdle();
  WriteDeviceIdentities(*original);

  service = RecreateProfileWithIdentities();
  ASSERT_TRUE(Login(service).FindBool("authenticated").value_or(false));
  EXPECT_EQ(service->GetDeviceId(), device);
  EXPECT_EQ(server_.registered_public_key, public_key);
  const auto migrated = ReadDeviceIdentities();
  ASSERT_TRUE(migrated.FindDict("accounts"));
  EXPECT_TRUE(migrated.FindDict("accounts")->FindDict(kAccountId));
  EXPECT_FALSE(migrated.contains("seed"));
}

TEST_F(XenonAccountServiceTest, IdentityCapacityNeverEvictsExistingAccounts) {
  auto* service = InitializedService();
  ASSERT_TRUE(Login(service).FindBool("authenticated").value_or(false));
  const auto device = service->GetDeviceId();
  const auto public_key = server_.registered_public_key;
  auto stored = ReadDeviceIdentities();
  ASSERT_TRUE(stored.FindDict("accounts"));
  auto* accounts = stored.FindDict("accounts");
  const auto original = accounts->FindDict(kAccountId)->Clone();
  for (int i = 0; i < 63; ++i) {
    const auto suffix = base::NumberToString(i);
    const auto account_id = std::string(32 - suffix.size(), '0') + suffix;
    auto identity = original.Clone();
    identity.Set("accountId", account_id);
    accounts->Set(account_id, std::move(identity));
  }
  service->Logout({});
  task_environment_.RunUntilIdle();
  WriteDeviceIdentities(stored);
  service = RecreateProfileWithIdentities();
  server_.account_id = "cccccccccccccccccccccccccccccccc";
  const auto limited = Login(service);
  EXPECT_TRUE(service->GetDeviceId().empty());
  EXPECT_NE(limited.FindString("error")->find("64"), std::string::npos);
  ASSERT_TRUE(SwitchAccount(service, kAccountId)
                  .FindBool("authenticated")
                  .value_or(false));
  EXPECT_EQ(service->GetDeviceId(), device);
  EXPECT_EQ(server_.registered_public_key, public_key);
  EXPECT_EQ(ReadDeviceIdentities().FindDict("accounts")->size(), 64u);
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

TEST_F(XenonAccountServiceTest, LogoutInvalidatesTransferWorkerRequests) {
  auto* service = InitializedService();
  ASSERT_TRUE(Login(service).FindBool("authenticated").value_or(false));
  auto request = service->GetSessionRequest();
  ASSERT_TRUE(request);
  const int before = server_.validation_requests;
  service->Logout({});
  const auto response = request.Run("GET", "/auth/me", {});
  EXPECT_EQ(response.status, 0);
  EXPECT_FALSE(response.error.empty());
  EXPECT_EQ(server_.validation_requests, before);
  EXPECT_FALSE(service->GetSessionRequest());
}

}  // namespace
