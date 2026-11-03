// Copyright 2026 The Xenon Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/account/xenon_transfer_service.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_writer.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "chrome/browser/download/chrome_download_manager_delegate.h"
#include "chrome/browser/download/download_core_service_factory.h"
#include "chrome/browser/download/download_core_service_impl.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/test/browser_task_environment.h"
#include "cross_transfer/test_job.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/chrome/browser/xenon_prefs.h"

// Inject busy state, core outcomes and process restarts. Messages and
// acknowledgements exercise the public service and account runner.
class XenonTransferServiceTestPeer {
 public:
  static void SetOperationBusy(XenonTransferService* service, bool busy) {
    service->operation_busy_ = busy;
  }
  static void TrackRunningTask(XenonTransferService* service,
                               const std::string& id) {
    const auto* task = service->FindInvitation(id);
    ASSERT_TRUE(task);
    service->SetActiveFile(*task);
    service->SetTaskState(id, "accepted");
    service->file_attempts_.insert(id);
    if (service->active_role_ == "receive") {
      service->accepted_receives_.insert(id);
    }
    service->PersistLocalTasks();
  }
  static void Complete(XenonTransferService* service, const std::string& id) {
    service->RecordCompletedTransfer(id);
  }
  static void PrepareFailedCallback(XenonTransferService* service,
                                    const std::string& id) {
    TrackRunningTask(service, id);
    cross_transfer::TestJobOptions options;
    options.task_id = id;
    service->job_ =
        std::make_shared<cross_transfer::TestTransferJob>(std::move(options));
    EXPECT_FALSE(service->job_->Run());
    ASSERT_EQ(service->job_->snapshot().state, "failed");
    service->job_running_ = true;
  }
  static void DeliverFailedCallback(XenonTransferService* service,
                                    const std::string& id) {
    service->OnJobFinished(service->generation_, id, {});
  }
  static void ReportUnconfirmedSend(XenonTransferService* service,
                                    const std::string& id) {
    service->HandleJobFailure(id, /*completion_requested=*/true);
  }
  static void ReloadSession(XenonTransferService* service) {
    service->account_id_.clear();
    service->device_id_.clear();
    service->OnSessionChanged();
  }
  static void SetDeadline(XenonTransferService* service,
                          const std::string& id,
                          base::Time deadline) {
    auto* task = service->local_tasks_.FindDict(id);
    ASSERT_TRUE(task);
    task->Set("expiresAt", deadline.InSecondsFSinceUnixEpoch());
  }
  static void TrackActiveTransport(XenonTransferService* service,
                                   const std::string& id,
                                   bool preparing) {
    TrackRunningTask(service, id);
    service->relay_pending_ = preparing;
    service->job_running_ = !preparing;
  }
  static void FinishTrackedTransport(XenonTransferService* service,
                                     const std::string& id) {
    service->relay_pending_ = false;
    service->job_running_ = false;
    Complete(service, id);
  }
  static void RememberDirectSuccess(XenonTransferService* service,
                                    const std::string& peer) {
    service->RememberDirectSuccess(peer);
  }
  static bool WasDirectSuccessful(XenonTransferService* service,
                                  const std::string& peer) {
    return service->WasDirectSuccessful(peer);
  }
  static void ClearCachedConnections(XenonTransferService* service) {
    service->ResetDirectConnections();
  }
};

namespace {
constexpr char kAccountId[] = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr char kOtherDevice[] = "cccccccccccccccccccccccccccccccc";
constexpr char kTaskId[] = "dddddddddddddddddddddddddddddddd";
constexpr char kSecondTaskId[] = "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
constexpr char kMessageId[] = "11111111111111111111111111111111";
constexpr char kSecondMessageId[] = "22222222222222222222222222222222";
constexpr char kThirdMessageId[] = "33333333333333333333333333333333";
constexpr char kToken[] =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
using Response = XenonAccountService::Response;

class TestDownloadCoreService : public DownloadCoreServiceImpl {
 public:
  explicit TestDownloadCoreService(Profile* profile)
      : DownloadCoreServiceImpl(profile), delegate_(profile) {}
  ChromeDownloadManagerDelegate* GetDownloadManagerDelegate() override {
    return &delegate_;
  }

 private:
  ChromeDownloadManagerDelegate delegate_;
};

// The fake never interprets payload types or task states. It only delivers
// opaque envelopes, acknowledges message identities and supplies transport.
class FakeMailbox {
 public:
  Response Handle(std::string method,
                  std::string path,
                  std::string token,
                  base::DictValue body) {
    std::lock_guard lock(mutex_);
    if (path == "/auth/login") {
      return {200,
              base::DictValue()
                  .Set("account", base::DictValue()
                                      .Set("id", account_id)
                                      .Set("username", "transfer-test"))
                  .Set("token", kToken)
                  .Set("expiresAt", (base::Time::Now() + base::Days(1))
                                        .InSecondsFSinceUnixEpoch()),
              {}};
    }
    EXPECT_EQ(token, kToken);
    if (path == "/auth/me") {
      return {200,
              base::DictValue().Set("account",
                                    base::DictValue()
                                        .Set("id", account_id)
                                        .Set("username", "transfer-test")),
              {}};
    }
    if (path == "/auth/logout" ||
        (method == "DELETE" && path.starts_with("/devices/"))) {
      return {200, base::DictValue().Set("ok", true), {}};
    }
    if (path == "/devices") {
      return {200, base::DictValue().Set("devices", base::ListValue()), {}};
    }
    if (path.starts_with("/devices/") && path.ends_with("/signals/ack")) {
      ++ack_requests;
      EXPECT_EQ(method, "POST");
      EXPECT_EQ(body.size(), 1u);
      const auto* ids = body.FindList("messageIds");
      if (!ids) {
        return {400, {}, {}};
      }
      if (ack_failures > 0) {
        --ack_failures;
        return {503, {}, {}};
      }
      for (const auto& id : *ids) {
        mailbox.erase(id.GetString());
      }
      if (lose_ack_response) {
        lose_ack_response = false;
        return {0, {}, "Connection closed"};
      }
      return {200, base::DictValue().Set("ok", true), {}};
    }
    if (path.starts_with("/devices/") && path.ends_with("/signals")) {
      ++poll_requests;
      EXPECT_EQ(method, "GET");
      if (fail_polls) {
        return {503, {}, {}};
      }
      const std::string recipient = path.substr(9, 32);
      base::ListValue signals;
      for (const auto& [id, envelope] : mailbox) {
        if (*envelope.FindString("toDeviceId") == recipient) {
          signals.Append(envelope.Clone());
        }
      }
      return {200, base::DictValue().Set("signals", std::move(signals)), {}};
    }
    if (path.starts_with("/devices/") &&
        ((method == "PUT" && path.size() == 41) ||
         (method == "POST" && path.ends_with("/heartbeat")))) {
      device_id = path.substr(9, 32);
      return {200,
              base::DictValue().Set("device",
                                    base::DictValue().Set("id", device_id)),
              {}};
    }
    if (path == "/signals") {
      ++publish_requests;
      EXPECT_EQ(method, "POST");
      EXPECT_EQ(body.size(), 4u);
      EXPECT_TRUE(body.FindString("id"));
      EXPECT_TRUE(body.FindString("fromDeviceId"));
      EXPECT_TRUE(body.FindString("toDeviceId"));
      EXPECT_TRUE(body.FindDict("payload"));
      publications.push_back(body.Clone());
      if (malformed_publish_responses > 0) {
        const int status = malformed_publish_responses-- == 2 ? 201 : 200;
        return {status, base::DictValue().Set("signal", base::DictValue()), {}};
      }
      if (publish_failures > 0) {
        --publish_failures;
        return {503, {}, {}};
      }
      const std::string id = *body.FindString("id");
      auto previous = receipts.find(id);
      if (previous != receipts.end()) {
        EXPECT_EQ(base::WriteJson(previous->second), base::WriteJson(body));
        return {
            200, base::DictValue().Set("signal", envelopes.at(id).Clone()), {}};
      }
      receipts.emplace(id, body.Clone());
      auto envelope = body.Clone();
      envelope.Set("fromName", "Sender");
      envelope.Set("toName", "Receiver");
      envelope.Set("createdAt", base::Time::Now().InSecondsFSinceUnixEpoch());
      envelope.Set(
          "expiresAt",
          (base::Time::Now() + base::Minutes(5)).InSecondsFSinceUnixEpoch());
      mailbox.insert_or_assign(id, envelope.Clone());
      envelopes.emplace(id, envelope.Clone());
      if (lose_publish_response) {
        lose_publish_response = false;
        return {0, {}, "Connection closed"};
      }
      return {201, base::DictValue().Set("signal", std::move(envelope)), {}};
    }
    if (path == "/transport-config") {
      ++config_requests;
      EXPECT_EQ(method, "GET");
      return {200,
              base::DictValue()
                  .Set("directMinBytes", 1024 * 1024)
                  .Set("iceServers", base::ListValue()),
              {}};
    }
    if (path.starts_with("/channels/")) {
      if (method == "DELETE") {
        ++channel_deletes;
        return {200, base::DictValue().Set("ok", true), {}};
      }
      EXPECT_TRUE(body.FindString("deviceId"));
      EXPECT_TRUE(body.FindString("peerDeviceId"));
      EXPECT_TRUE(body.FindString("role"));
      if (const auto* role = body.FindString("role")) {
        EXPECT_TRUE(*role == "sender" || *role == "receiver");
      }
      if (path.ends_with("/exchange")) {
        ++exchange_requests;
        EXPECT_TRUE(body.FindDict("payload"));
        channel_exchanges.push_back(body.Clone());
        if (relay_after_offer &&
            body.FindDict("payload")->FindDict("description")) {
          exchange_peer =
              base::DictValue()
                  .Set("sequence", 1)
                  .Set("payload", base::DictValue()
                                      .Set("route", "relay")
                                      .Set("description", base::Value())
                                      .Set("candidates", base::ListValue()));
        }
        return {200,
                base::DictValue()
                    .Set("peer", exchange_peer.empty()
                                     ? base::Value()
                                     : base::Value(exchange_peer.Clone()))
                    .Set("expiresAt", (base::Time::Now() + base::Minutes(5))
                                          .InSecondsFSinceUnixEpoch()),
                {}};
      }
      if (path.ends_with("/release")) {
        ++channel_releases;
        EXPECT_EQ(method, "POST");
        EXPECT_EQ(body.size(), 3u);
        releases.push_back(body.Clone());
        return {200, base::DictValue().Set("ok", true), {}};
      }
      if (path.ends_with("/relay")) {
        ++relay_requests;
        return {503, {}, {}};
      }
    }
    ++unexpected_requests;
    ADD_FAILURE() << "Unexpected server API: " << method << " " << path;
    return {404, {}, {}};
  }
  void Queue(base::DictValue envelope) {
    std::lock_guard lock(mutex_);
    const auto id = *envelope.FindString("id");
    mailbox.insert_or_assign(id, std::move(envelope));
  }
  std::string account_id = kAccountId;
  std::string device_id;
  std::map<std::string, base::DictValue> mailbox;
  std::map<std::string, base::DictValue> receipts;
  std::map<std::string, base::DictValue> envelopes;
  std::vector<base::DictValue> publications;
  std::vector<base::DictValue> channel_exchanges;
  std::vector<base::DictValue> releases;
  base::DictValue exchange_peer;
  std::atomic<int> publish_requests{0}, poll_requests{0}, ack_requests{0};
  std::atomic<int> config_requests{0}, exchange_requests{0}, relay_requests{0};
  std::atomic<int> channel_deletes{0}, channel_releases{0},
      unexpected_requests{0};
  int publish_failures = 0;
  int malformed_publish_responses = 0;
  int ack_failures = 0;
  bool lose_publish_response = false;
  bool lose_ack_response = false;
  bool fail_polls = false;
  bool relay_after_offer = false;

 private:
  std::mutex mutex_;
};

class XenonTransferServiceTest : public testing::Test {
 protected:
  std::unique_ptr<TestingProfile> CreateProfile() {
    TestingProfile::Builder builder;
    builder.AddTestingFactory(
        DownloadCoreServiceFactory::GetInstance(),
        base::BindRepeating([](content::BrowserContext* context)
                                -> std::unique_ptr<KeyedService> {
          return std::make_unique<TestDownloadCoreService>(
              Profile::FromBrowserContext(context));
        }));
    return builder.Build();
  }
  void SetUp() override {
    profile_ = CreateProfile();
    XenonAccountService::SetRequestHandlerForTesting(
        base::BindRepeating(&FakeMailbox::Handle, base::Unretained(&server_)));
  }
  void TearDown() override {
    profile_.reset();
    task_environment_.RunUntilIdle();
    EXPECT_EQ(server_.unexpected_requests, 0);
    XenonAccountService::SetRequestHandlerForTesting({});
  }
  XenonTransferService* LoggedInService(TestingProfile* profile = nullptr) {
    if (!profile) {
      profile = profile_.get();
    }
    auto* service = XenonTransferService::GetForProfile(profile);
    task_environment_.RunUntilIdle();
    base::test::TestFuture<base::DictValue> login;
    XenonAccountService::GetForProfile(profile)->Authenticate(
        false, "transfer-test", "test-password-valid", login.GetCallback());
    EXPECT_TRUE(login.Get().FindBool("authenticated").value_or(false));
    task_environment_.RunUntilIdle();
    return service;
  }
  base::DictValue Refresh(XenonTransferService* service) {
    base::test::TestFuture<base::DictValue> result;
    service->Refresh(result.GetCallback());
    return result.Take();
  }
  base::DictValue SendText(XenonTransferService* service,
                           std::string text = "local payload") {
    base::test::TestFuture<base::DictValue> sent;
    service->SendText(kOtherDevice, std::move(text), sent.GetCallback());
    return sent.Take();
  }
  std::string FirstTaskId(const base::DictValue& state) {
    return *state.FindList("transfers")->front().GetDict().FindString("id");
  }
  std::string Status(const base::DictValue& state, const std::string& id) {
    for (const auto& value : *state.FindList("transfers")) {
      if (*value.GetDict().FindString("id") == id) {
        return *value.GetDict().FindString("status");
      }
    }
    return {};
  }
  const base::DictValue* Scope(TestingProfile* profile = nullptr) {
    if (!profile) {
      profile = profile_.get();
    }
    auto* account = XenonAccountService::GetForProfile(profile);
    return profile->GetPrefs()
        ->GetDict(xenon::prefs::kTransferHistory)
        .FindDict(account->GetAccountId() + ":" + account->GetDeviceId());
  }
  base::DictValue Signal(const std::string& message_id,
                         std::string type,
                         std::string task_id = kTaskId,
                         std::string sender = kOtherDevice) {
    auto payload =
        base::DictValue().Set("taskId", std::move(task_id)).Set("type", type);
    if (type == "offer") {
      payload.Set(
          "offer",
          base::DictValue()
              .Set("kind", "file")
              .Set("fileName", "received.txt")
              .Set("fileSize", 12.0)
              .Set("preferDirect", false)
              .Set("createdAt", base::Time::Now().InSecondsFSinceUnixEpoch())
              .Set("expiresAt", (base::Time::Now() + base::Minutes(5))
                                    .InSecondsFSinceUnixEpoch()));
    } else if (type == "accept") {
      payload.Set("preferDirect", false);
    }
    return base::DictValue()
        .Set("id", message_id)
        .Set("fromDeviceId", std::move(sender))
        .Set("toDeviceId", server_.device_id)
        .Set("fromName", "Other device")
        .Set("toName", "This device")
        .Set("createdAt", base::Time::Now().InSecondsFSinceUnixEpoch())
        .Set("expiresAt",
             (base::Time::Now() + base::Minutes(5)).InSecondsFSinceUnixEpoch())
        .Set("payload", std::move(payload));
  }
  void ReceiveOffer(XenonTransferService* service) {
    // Keep the offer pending while testing explicit state transitions below.
    XenonTransferServiceTestPeer::SetOperationBusy(service, true);
    server_.Queue(Signal(kMessageId, "offer"));
    EXPECT_EQ(Status(Refresh(service), kTaskId), "pending");
    task_environment_.RunUntilIdle();
    XenonTransferServiceTestPeer::SetOperationBusy(service, false);
  }
  void PeerChoosesRelay() {
    server_.exchange_peer =
        base::DictValue()
            .Set("sequence", 1)
            .Set("payload", base::DictValue()
                                .Set("route", "relay")
                                .Set("description", base::Value())
                                .Set("candidates", base::ListValue()));
  }
  int PublishedType(const std::string& type) {
    int count = 0;
    for (const auto& publication : server_.publications) {
      if (*publication.FindDict("payload")->FindString("type") == type) {
        ++count;
      }
    }
    return count;
  }
  content::BrowserTaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  FakeMailbox server_;
  std::unique_ptr<TestingProfile> profile_;
};

TEST_F(XenonTransferServiceTest, SignedOutCannotCreateTask) {
  auto* service = XenonTransferService::GetForProfile(profile_.get());
  task_environment_.RunUntilIdle();
  auto state = SendText(service);
  EXPECT_FALSE(state.FindString("error")->empty());
  EXPECT_TRUE(state.FindList("transfers")->empty());
  EXPECT_EQ(server_.publish_requests, 0);
}

TEST_F(XenonTransferServiceTest,
       FileOfferKeepsSourcePathOutOfMessagesAndWebUi) {
  auto* service = LoggedInService();
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto source = directory.GetPath().AppendASCII("sample.txt");
  ASSERT_TRUE(base::WriteFile(source, "test contents"));
  base::test::TestFuture<base::DictValue> sent;
  service->SendFile(kOtherDevice, source, sent.GetCallback());
  const auto state = sent.Take();
  ASSERT_TRUE(state.FindString("error")->empty());
  ASSERT_EQ(state.FindList("transfers")->size(), 1u);
  const auto& task = state.FindList("transfers")->front().GetDict();
  EXPECT_EQ(*task.FindString("fileName"), "sample.txt");
  EXPECT_EQ(task.FindDouble("fileSize"), 13.0);
  const auto private_directory = directory.GetPath().BaseName().AsUTF8Unsafe();
  EXPECT_EQ(base::WriteJson(state)->find(private_directory), std::string::npos);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(server_.publications.size(), 1u);
  const auto* offer =
      server_.publications.front().FindDict("payload")->FindDict("offer");
  ASSERT_TRUE(offer);
  EXPECT_EQ(offer->FindDouble("createdAt"), task.FindDouble("createdAt"));
  EXPECT_EQ(offer->FindDouble("expiresAt"), task.FindDouble("expiresAt"));
  EXPECT_EQ(
      base::WriteJson(server_.publications.front())->find(private_directory),
      std::string::npos);
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest, FileSendInitializesWebRtcOnWorker) {
  auto* service = LoggedInService();
  base::ScopedTempDir directory;
  ASSERT_TRUE(directory.CreateUniqueTempDir());
  const auto source = directory.GetPath().AppendASCII("direct.bin");
  ASSERT_TRUE(base::WriteFile(source, std::string(1024 * 1024, 'x')));

  // Switch to the failing test relay only after a real SDP offer is generated.
  // This exercises WebRTC initialization and teardown on the service's worker
  // without an external peer or a negotiation timeout.
  server_.relay_after_offer = true;
  base::test::TestFuture<base::DictValue> sent;
  service->SendFile(kOtherDevice, source, sent.GetCallback());
  const auto state = sent.Take();
  ASSERT_TRUE(state.FindString("error")->empty());
  const auto id = FirstTaskId(state);
  task_environment_.RunUntilIdle();
  server_.Queue(Signal(kMessageId, "accept", id));
  Refresh(service);
  task_environment_.RunUntilIdle();

  bool offered = false;
  for (const auto& exchange : server_.channel_exchanges) {
    const auto* description =
        exchange.FindDict("payload")->FindDict("description");
    if (description) {
      EXPECT_EQ(*description->FindString("type"), "offer");
      EXPECT_FALSE(description->FindString("sdp")->empty());
      offered = true;
    }
  }
  EXPECT_TRUE(offered);
  EXPECT_EQ(server_.relay_requests, 1);
  EXPECT_EQ(server_.channel_releases, 1);
  EXPECT_EQ(Status(service->GetState(), id), "failed");
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
}

TEST_F(XenonTransferServiceTest, TaskAndOutboxPersistBeforePublishResponse) {
  auto* service = LoggedInService();
  base::test::TestFuture<void> started;
  auto release = std::make_shared<base::WaitableEvent>();
  base::ScopedClosureRunner cleanup(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      release));
  XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
      [](FakeMailbox* server, base::RepeatingClosure request_started,
         std::shared_ptr<base::WaitableEvent> gate, std::string method,
         std::string path, std::string token, base::DictValue body) {
        if (path == "/signals") {
          request_started.Run();
          base::ScopedAllowBaseSyncPrimitivesForTesting allow;
          gate->Wait();
        }
        return server->Handle(std::move(method), std::move(path),
                              std::move(token), std::move(body));
      },
      base::Unretained(&server_), started.GetSequenceBoundRepeatingCallback(),
      release));
  base::test::TestFuture<base::DictValue> sent;
  service->SendText(kOtherDevice, "private local text", sent.GetCallback());
  ASSERT_TRUE(started.Wait());
  ASSERT_TRUE(sent.IsReady());
  const auto& state = sent.Get();
  ASSERT_EQ(state.FindList("transfers")->size(), 1u);
  const std::string id = FirstTaskId(state);
  EXPECT_EQ(Status(state, id), "pending");
  ASSERT_TRUE(Scope());
  ASSERT_TRUE(Scope()->FindDict("tasks")->FindDict(id));
  EXPECT_EQ(Scope()->FindDict("outbox")->size(), 1u);
  release->Signal();
  task_environment_.RunUntilIdle();
  ASSERT_EQ(server_.publications.size(), 1u);
  EXPECT_EQ(
      base::WriteJson(server_.publications.front())->find("private local text"),
      std::string::npos);
  EXPECT_EQ(base::WriteJson(state)->find(kToken), std::string::npos);
  EXPECT_EQ(*state.FindList("transfers")->front().GetDict().FindString("text"),
            "private local text");
}

TEST_F(XenonTransferServiceTest,
       HealthyActiveTransferSurvivesInvitationDeadline) {
  auto* service = LoggedInService();
  ReceiveOffer(service);
  for (const bool preparing : {true, false}) {
    XenonTransferServiceTestPeer::TrackActiveTransport(service, kTaskId,
                                                       preparing);
    XenonTransferServiceTestPeer::SetDeadline(
        service, kTaskId, base::Time::Now() - base::Minutes(35));
    const auto state = Refresh(service);
    EXPECT_EQ(Status(state, kTaskId), "accepted");
    EXPECT_TRUE(state.FindDict("job")->FindBool("active").value());
    task_environment_.RunUntilIdle();
    EXPECT_EQ(PublishedType("cancel"), 0);
  }
  XenonTransferServiceTestPeer::FinishTrackedTransport(service, kTaskId);
  EXPECT_EQ(Status(service->GetState(), kTaskId), "completed");
  EXPECT_TRUE(SendText(service).FindString("error")->empty());
}

TEST_F(XenonTransferServiceTest, ExpiredPendingSendCancelsAndReleasesSlot) {
  auto* service = LoggedInService();
  const auto id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  XenonTransferServiceTestPeer::SetDeadline(
      service, id, base::Time::Now() - base::Seconds(1));
  const auto state = Refresh(service);
  EXPECT_EQ(Status(state, id), "expired");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(PublishedType("cancel"), 1);
  EXPECT_EQ(server_.exchange_requests, 0);
  EXPECT_TRUE(SendText(service, "new task").FindString("error")->empty());
}

TEST_F(XenonTransferServiceTest, CannotAcceptAnAlreadyExpiredIncomingOffer) {
  auto* service = LoggedInService();
  ReceiveOffer(service);
  XenonTransferServiceTestPeer::SetDeadline(
      service, kTaskId, base::Time::Now() - base::Seconds(1));
  // Exercise Respond directly, without the periodic Refresh expiring it first.
  base::test::TestFuture<base::DictValue> accepted;
  service->Respond(kTaskId, "accept", accepted.GetCallback());
  EXPECT_EQ(Status(accepted.Get(), kTaskId), "expired");
  task_environment_.RunUntilIdle();
  EXPECT_EQ(PublishedType("accept"), 0);
  EXPECT_EQ(PublishedType("cancel"), 1);
  EXPECT_EQ(server_.exchange_requests, 0);
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest, DelayedOfferKeepsSendersOriginalDeadline) {
  auto* service = LoggedInService();
  XenonTransferServiceTestPeer::SetOperationBusy(service, true);
  auto signal = Signal(kMessageId, "offer");
  const auto now = base::Time::Now();
  const double created = (now - base::Minutes(4)).InSecondsFSinceUnixEpoch();
  const double expires = (now + base::Minutes(1)).InSecondsFSinceUnixEpoch();
  auto* offer = signal.FindDict("payload")->FindDict("offer");
  offer->Set("createdAt", created);
  offer->Set("expiresAt", expires);
  server_.Queue(std::move(signal));
  EXPECT_EQ(Status(Refresh(service), kTaskId), "pending");
  const auto* task = Scope()->FindDict("tasks")->FindDict(kTaskId);
  ASSERT_TRUE(task);
  EXPECT_EQ(task->FindDouble("createdAt"), created);
  EXPECT_EQ(task->FindDouble("expiresAt"), expires);
  task_environment_.FastForwardBy(base::Seconds(61));
  EXPECT_EQ(Status(service->GetState(), kTaskId), "expired");
  EXPECT_EQ(PublishedType("cancel"), 1);
}

TEST_F(XenonTransferServiceTest, AlreadyExpiredOfferNeverBecomesPending) {
  auto* service = LoggedInService();
  auto signal = Signal(kMessageId, "offer");
  const auto now = base::Time::Now();
  auto* offer = signal.FindDict("payload")->FindDict("offer");
  offer->Set("createdAt", (now - base::Minutes(6)).InSecondsFSinceUnixEpoch());
  offer->Set("expiresAt", (now - base::Minutes(1)).InSecondsFSinceUnixEpoch());
  // Its mailbox envelope is fresh; only the sender's task deadline has elapsed.
  server_.Queue(std::move(signal));
  EXPECT_EQ(Status(Refresh(service), kTaskId), "expired");
  task_environment_.RunUntilIdle();
  EXPECT_EQ(PublishedType("cancel"), 1);
  EXPECT_EQ(server_.exchange_requests, 0);
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest, LateAcceptanceCannotReviveExpiredSend) {
  auto* service = LoggedInService();
  const auto id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  XenonTransferServiceTestPeer::SetDeadline(
      service, id, base::Time::Now() - base::Seconds(1));
  server_.Queue(Signal(kMessageId, "accept", id));
  EXPECT_EQ(Status(Refresh(service), id), "expired");
  task_environment_.RunUntilIdle();
  EXPECT_EQ(PublishedType("cancel"), 1);
  EXPECT_EQ(server_.relay_requests, 0);
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
}

TEST_F(XenonTransferServiceTest, RejectsInvalidOfferLifetime) {
  auto* service = LoggedInService();
  int index = 0;
  for (const int seconds : {-1, 0, 301}) {
    auto message_id = std::string(32, '4');
    message_id.back() = static_cast<char>('1' + index++);
    auto signal = Signal(message_id, "offer");
    const auto now = base::Time::Now();
    auto* offer = signal.FindDict("payload")->FindDict("offer");
    offer->Set("createdAt", now.InSecondsFSinceUnixEpoch());
    offer->Set("expiresAt",
               (now + base::Seconds(seconds)).InSecondsFSinceUnixEpoch());
    server_.Queue(std::move(signal));
    EXPECT_TRUE(Refresh(service).FindList("transfers")->empty());
  }
  EXPECT_EQ(server_.exchange_requests, 0);
}

TEST_F(XenonTransferServiceTest,
       DirectSuccessSurvivesConnectionCacheResetAndSessionReload) {
  auto* service = LoggedInService();
  XenonTransferServiceTestPeer::RememberDirectSuccess(service, kOtherDevice);
  ASSERT_TRUE(Scope()->FindDict("directPeers"));
  EXPECT_TRUE(Scope()->FindDict("directPeers")->FindDouble(kOtherDevice));
  XenonTransferServiceTestPeer::ClearCachedConnections(service);
  XenonTransferServiceTestPeer::ReloadSession(service);
  EXPECT_TRUE(
      XenonTransferServiceTestPeer::WasDirectSuccessful(service, kOtherDevice));
  PeerChoosesRelay();
  const auto sent = SendText(service, "small payload");
  const auto id = FirstTaskId(sent);
  EXPECT_TRUE(sent.FindList("transfers")
                  ->front()
                  .GetDict()
                  .FindBool("preferDirect")
                  .value_or(false));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(server_.publications.size(), 1u);
  EXPECT_TRUE(server_.publications.front()
                  .FindDict("payload")
                  ->FindDict("offer")
                  ->FindBool("preferDirect")
                  .value_or(false));
  server_.Queue(Signal(kMessageId, "accept", id));
  Refresh(service);
  task_environment_.RunUntilIdle();
  ASSERT_FALSE(server_.channel_exchanges.empty());
  // A cached success must try ICE even below the one-MiB relay threshold.
  EXPECT_EQ(*server_.channel_exchanges.front().FindDict("payload")->FindString(
                "route"),
            "pending");
  EXPECT_TRUE(Scope()
                  ->FindDict("tasks")
                  ->FindDict(id)
                  ->FindBool("preferDirect")
                  .value_or(false));
}

TEST_F(XenonTransferServiceTest, ReceiverDirectPreferenceIsSentWithAcceptance) {
  auto* service = LoggedInService();
  XenonTransferServiceTestPeer::RememberDirectSuccess(service, kOtherDevice);
  PeerChoosesRelay();
  ReceiveOffer(service);
  EXPECT_TRUE(Scope()
                  ->FindDict("tasks")
                  ->FindDict(kTaskId)
                  ->FindBool("preferDirect")
                  .value_or(false));
  base::test::TestFuture<base::DictValue> accepted;
  service->Respond(kTaskId, "accept", accepted.GetCallback());
  ASSERT_TRUE(accepted.Wait());
  bool preferred = false;
  for (const auto [message_id, entry] : *Scope()->FindDict("outbox")) {
    const auto* payload =
        entry.GetDict().FindDict("message")->FindDict("payload");
    if (*payload->FindString("type") == "accept") {
      preferred = payload->FindBool("preferDirect").value_or(false);
    }
  }
  EXPECT_TRUE(preferred);
  task_environment_.RunUntilIdle();
  ASSERT_FALSE(server_.channel_exchanges.empty());
  EXPECT_EQ(*server_.channel_exchanges.front().FindDict("payload")->FindString(
                "route"),
            "pending");
}

TEST_F(XenonTransferServiceTest, PeerAcceptanceCanEnableDirectForSmallPayload) {
  auto* service = LoggedInService();
  PeerChoosesRelay();
  const auto sent = SendText(service, "small payload");
  const auto id = FirstTaskId(sent);
  EXPECT_FALSE(sent.FindList("transfers")
                   ->front()
                   .GetDict()
                   .FindBool("preferDirect")
                   .value_or(true));
  task_environment_.RunUntilIdle();
  auto accept = Signal(kMessageId, "accept", id);
  accept.FindDict("payload")->Set("preferDirect", true);
  server_.Queue(std::move(accept));
  Refresh(service);
  task_environment_.RunUntilIdle();
  ASSERT_FALSE(server_.channel_exchanges.empty());
  EXPECT_EQ(*server_.channel_exchanges.front().FindDict("payload")->FindString(
                "route"),
            "pending");
  EXPECT_TRUE(Scope()
                  ->FindDict("tasks")
                  ->FindDict(id)
                  ->FindBool("preferDirect")
                  .value_or(false));
  // A peer preference alone cannot claim a successful local direct transfer.
  EXPECT_FALSE(
      XenonTransferServiceTestPeer::WasDirectSuccessful(service, kOtherDevice));
}

TEST_F(XenonTransferServiceTest, IncomingPeerPreferenceSurvivesTaskReload) {
  auto* service = LoggedInService();
  XenonTransferServiceTestPeer::SetOperationBusy(service, true);
  auto signal = Signal(kMessageId, "offer");
  signal.FindDict("payload")->FindDict("offer")->Set("preferDirect", true);
  server_.Queue(std::move(signal));
  EXPECT_EQ(Status(Refresh(service), kTaskId), "pending");
  XenonTransferServiceTestPeer::ReloadSession(service);
  EXPECT_TRUE(Scope()
                  ->FindDict("tasks")
                  ->FindDict(kTaskId)
                  ->FindBool("preferDirect")
                  .value_or(false));
  EXPECT_FALSE(
      XenonTransferServiceTestPeer::WasDirectSuccessful(service, kOtherDevice));
}

TEST_F(XenonTransferServiceTest, DirectSuccessHistoryIsBounded) {
  auto* service = LoggedInService();
  std::string newest;
  const std::string digits = "0123456789abcdef";
  for (int index = 0; index < 33; ++index) {
    newest = std::string(32, 'a');
    newest[30] = digits[index / 16];
    newest[31] = digits[index % 16];
    XenonTransferServiceTestPeer::RememberDirectSuccess(service, newest);
  }
  ASSERT_TRUE(Scope()->FindDict("directPeers"));
  EXPECT_EQ(Scope()->FindDict("directPeers")->size(), 32u);
  EXPECT_TRUE(
      XenonTransferServiceTestPeer::WasDirectSuccessful(service, newest));
  XenonTransferServiceTestPeer::ReloadSession(service);
  EXPECT_TRUE(
      XenonTransferServiceTestPeer::WasDirectSuccessful(service, newest));
}

TEST_F(XenonTransferServiceTest, FailedTransportReleasesOnlyItsChannelLease) {
  auto* service = LoggedInService();
  const auto id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  server_.Queue(Signal(kMessageId, "accept", id));
  Refresh(service);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(Status(service->GetState(), id), "failed");
  EXPECT_EQ(server_.channel_releases, 1);
  EXPECT_EQ(server_.channel_deletes, 0);
  ASSERT_EQ(server_.releases.size(), 1u);
  EXPECT_EQ(*server_.releases.front().FindString("deviceId"),
            server_.device_id);
  EXPECT_EQ(*server_.releases.front().FindString("peerDeviceId"), kOtherDevice);
  EXPECT_EQ(*server_.releases.front().FindString("role"), "sender");
}

TEST_F(XenonTransferServiceTest, LostPublishResponseRetriesSameOpaqueMessage) {
  auto* service = LoggedInService();
  server_.lose_publish_response = true;
  auto state = SendText(service);
  const std::string id = FirstTaskId(state);
  task_environment_.RunUntilIdle();
  ASSERT_EQ(server_.publications.size(), 1u);
  const auto original = server_.publications.front().Clone();
  task_environment_.FastForwardBy(base::Seconds(3));
  ASSERT_EQ(server_.publications.size(), 2u);
  EXPECT_EQ(base::WriteJson(original),
            base::WriteJson(server_.publications.back()));
  EXPECT_EQ(server_.receipts.size(), 1u);
  EXPECT_EQ(Status(service->GetState(), id), "pending");
  EXPECT_TRUE(Scope()->FindDict("outbox")->empty());
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest,
       PublishFailuresBackOffWithoutDuplicatingTasks) {
  auto* service = LoggedInService();
  server_.publish_failures = 2;
  auto state = SendText(service);
  const std::string id = FirstTaskId(state);
  task_environment_.RunUntilIdle();
  task_environment_.FastForwardBy(base::Seconds(1));
  EXPECT_EQ(server_.publish_requests, 1);
  Refresh(service);
  Refresh(service);
  EXPECT_EQ(server_.publish_requests, 1);
  task_environment_.FastForwardBy(base::Seconds(2));
  EXPECT_EQ(server_.publish_requests, 2);
  task_environment_.FastForwardBy(base::Seconds(2));
  EXPECT_EQ(server_.publish_requests, 2);
  task_environment_.FastForwardBy(base::Seconds(2));
  EXPECT_EQ(server_.publish_requests, 3);
  EXPECT_EQ(service->GetState().FindList("transfers")->size(), 1u);
  EXPECT_EQ(Status(service->GetState(), id), "pending");
}

TEST_F(XenonTransferServiceTest, MalformedSuccessResponsesKeepOutboxForRetry) {
  auto* service = LoggedInService();
  server_.malformed_publish_responses = 2;
  const std::string id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(server_.publications.size(), 1u);
  const auto message = server_.publications.front().Clone();
  EXPECT_EQ(Scope()->FindDict("outbox")->size(), 1u);
  task_environment_.FastForwardBy(base::Seconds(3));
  ASSERT_EQ(server_.publications.size(), 2u);
  EXPECT_EQ(Scope()->FindDict("outbox")->size(), 1u);
  EXPECT_EQ(base::WriteJson(message),
            base::WriteJson(server_.publications.back()));
  task_environment_.FastForwardBy(base::Seconds(4));
  ASSERT_EQ(server_.publications.size(), 3u);
  EXPECT_EQ(base::WriteJson(message),
            base::WriteJson(server_.publications.back()));
  EXPECT_TRUE(Scope()->FindDict("outbox")->empty());
  EXPECT_EQ(Status(service->GetState(), id), "pending");
}

TEST_F(XenonTransferServiceTest, LateOfferRejectionCannotOverwritePeerConsent) {
  auto* service = LoggedInService();
  base::test::TestFuture<void> started;
  auto release = std::make_shared<base::WaitableEvent>();
  base::ScopedClosureRunner cleanup(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      release));
  XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
      [](FakeMailbox* server, base::RepeatingClosure request_started,
         std::shared_ptr<base::WaitableEvent> gate, std::string method,
         std::string path, std::string token,
         base::DictValue body) -> Response {
        if (path == "/signals") {
          request_started.Run();
          base::ScopedAllowBaseSyncPrimitivesForTesting allow;
          gate->Wait();
          return {403, {}, {}};
        }
        return server->Handle(std::move(method), std::move(path),
                              std::move(token), std::move(body));
      },
      base::Unretained(&server_), started.GetSequenceBoundRepeatingCallback(),
      release));
  base::test::TestFuture<base::DictValue> sent;
  service->SendText(kOtherDevice, "local payload", sent.GetCallback());
  ASSERT_TRUE(started.Wait());
  ASSERT_TRUE(sent.IsReady());
  const auto id = FirstTaskId(sent.Get());
  // Consent and the connection attempt predate this publication callback.
  XenonTransferServiceTestPeer::TrackRunningTask(service, id);
  release->Signal();
  task_environment_.RunUntilIdle();
  const auto state = service->GetState();
  EXPECT_EQ(Status(state, id), "accepted");
  EXPECT_TRUE(state.FindDict("job")->FindBool("active").value());
  EXPECT_TRUE(Scope()->FindDict("outbox")->empty());
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest,
       ReceivedTaskPersistsBeforeAckAndRedeliveryIsIdempotent) {
  auto* service = LoggedInService();
  XenonTransferServiceTestPeer::SetOperationBusy(service, true);
  server_.Queue(Signal(kMessageId, "offer"));
  server_.ack_failures = 1;
  base::test::TestFuture<void> started;
  auto release = std::make_shared<base::WaitableEvent>();
  auto held = std::make_shared<std::atomic<bool>>(false);
  base::ScopedClosureRunner cleanup(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      release));
  XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
      [](FakeMailbox* server, base::RepeatingClosure request_started,
         std::shared_ptr<base::WaitableEvent> gate,
         std::shared_ptr<std::atomic<bool>> was_held, std::string method,
         std::string path, std::string token, base::DictValue body) {
        if (path.ends_with("/signals/ack") && !was_held->exchange(true)) {
          request_started.Run();
          base::ScopedAllowBaseSyncPrimitivesForTesting allow;
          gate->Wait();
        }
        return server->Handle(std::move(method), std::move(path),
                              std::move(token), std::move(body));
      },
      base::Unretained(&server_), started.GetSequenceBoundRepeatingCallback(),
      release, held));
  base::test::TestFuture<base::DictValue> refreshed;
  service->Refresh(refreshed.GetCallback());
  ASSERT_TRUE(started.Wait());
  ASSERT_TRUE(Scope());
  ASSERT_TRUE(Scope()->FindDict("tasks")->FindDict(kTaskId));
  ASSERT_TRUE(Scope()->FindDict("seenSignals")->Find(kMessageId));
  EXPECT_EQ(Status(service->GetState(), kTaskId), "pending");
  release->Signal();
  ASSERT_TRUE(refreshed.Wait());
  EXPECT_EQ(server_.ack_requests, 1);
  const auto state = Refresh(service);
  EXPECT_EQ(server_.ack_requests, 2);
  EXPECT_EQ(state.FindList("transfers")->size(), 1u);
  EXPECT_EQ(Status(state, kTaskId), "pending");
  EXPECT_EQ(server_.publish_requests, 0);
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest, LostAckResponseDoesNotLoseLocalHistory) {
  auto* service = LoggedInService();
  server_.lose_ack_response = true;
  ReceiveOffer(service);
  EXPECT_EQ(server_.ack_requests, 1);
  XenonTransferServiceTestPeer::ReloadSession(service);
  Refresh(service);
  task_environment_.RunUntilIdle();
  // The persisted pending offer is automatically received after restart,
  // even though the mailbox already removed the acknowledged message.
  EXPECT_EQ(Status(service->GetState(), kTaskId), "failed");
  EXPECT_EQ(server_.ack_requests, 1);
  EXPECT_EQ(server_.relay_requests, 1);
}

TEST_F(XenonTransferServiceTest, AutomaticallyReceivesFilesAndTextOnce) {
  auto* service = LoggedInService();
  int attempts = 0;
  for (const bool text : {false, true}) {
    SCOPED_TRACE(text);
    const std::string id = text ? kSecondTaskId : kTaskId;
    auto signal = Signal(text ? kSecondMessageId : kMessageId, "offer", id);
    if (text) {
      auto* offer = signal.FindDict("payload")->FindDict("offer");
      offer->Set("kind", "text");
      offer->Set("fileName", "message.txt");
    }
    server_.Queue(std::move(signal));
    Refresh(service);
    task_environment_.RunUntilIdle();
    // A real receive job reaches the test relay without a UI accept action.
    // The unavailable relay fails it once; polling must not restart it.
    EXPECT_EQ(server_.relay_requests, ++attempts);
    EXPECT_EQ(Status(service->GetState(), id), "failed");
    ASSERT_FALSE(server_.releases.empty());
    EXPECT_EQ(*server_.releases.back().FindString("role"), "receiver");
    Refresh(service);
    task_environment_.FastForwardBy(base::Seconds(4));
    EXPECT_EQ(server_.relay_requests, attempts);
  }
}

TEST_F(XenonTransferServiceTest,
       AutomaticReceiveWaitsForSlotAndUsesArrivalOrder) {
  auto* service = LoggedInService();
  const auto sending = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  auto older = Signal(kSecondMessageId, "offer", kSecondTaskId);
  older.FindDict("payload")->FindDict("offer")->Set(
      "createdAt", (base::Time::Now() - base::Seconds(1))
                       .InSecondsFSinceUnixEpoch());
  // Keep the offer lifetime valid while making its creation precede the
  // first task in map order and the first message in the mailbox batch.
  older.FindDict("payload")->FindDict("offer")->Set(
      "expiresAt", (base::Time::Now() + base::Minutes(4))
                       .InSecondsFSinceUnixEpoch());
  server_.Queue(Signal(kMessageId, "offer"));
  server_.Queue(std::move(older));
  auto state = Refresh(service);
  EXPECT_EQ(Status(state, kTaskId), "pending");
  EXPECT_EQ(Status(state, kSecondTaskId), "pending");
  EXPECT_EQ(server_.relay_requests, 0);

  XenonTransferServiceTestPeer::Complete(service, sending);
  Refresh(service);
  task_environment_.RunUntilIdle();
  state = service->GetState();
  EXPECT_EQ(Status(state, kSecondTaskId), "failed");
  EXPECT_EQ(Status(state, kTaskId), "pending");
  EXPECT_EQ(server_.relay_requests, 1);
  Refresh(service);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(Status(service->GetState(), kTaskId), "failed");
  EXPECT_EQ(server_.relay_requests, 2);
}

TEST_F(XenonTransferServiceTest, CancelInSameBatchPreventsAutomaticReceive) {
  auto* service = LoggedInService();
  server_.Queue(Signal(kMessageId, "offer"));
  server_.Queue(Signal(kSecondMessageId, "cancel"));
  const auto state = Refresh(service);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(Status(state, kTaskId), "cancelled");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  EXPECT_EQ(PublishedType("accept"), 0);
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest, CancelBeforeOfferCannotResurrectTask) {
  auto* service = LoggedInService();
  server_.Queue(Signal(kMessageId, "cancel"));
  Refresh(service);
  server_.Queue(Signal(kSecondMessageId, "offer"));
  const auto state = Refresh(service);
  EXPECT_NE(Status(state, kTaskId), "pending");
  EXPECT_NE(Status(state, kTaskId), "accepted");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  EXPECT_EQ(server_.relay_requests, 0);
  EXPECT_EQ(server_.publish_requests, 0);
}

TEST_F(XenonTransferServiceTest,
       DuplicateOfferDoesNotRegressConsentOrRestartJob) {
  auto* service = LoggedInService();
  ReceiveOffer(service);
  XenonTransferServiceTestPeer::TrackRunningTask(service, kTaskId);
  server_.Queue(Signal(kSecondMessageId, "offer"));
  auto state = Refresh(service);
  EXPECT_EQ(Status(state, kTaskId), "accepted");
  EXPECT_EQ(state.FindList("transfers")->size(), 1u);
  EXPECT_EQ(server_.relay_requests, 0);
  server_.Queue(Signal(kThirdMessageId, "cancel"));
  state = Refresh(service);
  EXPECT_EQ(Status(state, kTaskId), "cancelled");
  server_.Queue(Signal(kMessageId, "offer"));
  EXPECT_EQ(Status(Refresh(service), kTaskId), "cancelled");
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest,
       RepeatedAcceptanceCannotRestartFailedTransfer) {
  auto* service = LoggedInService();
  const std::string id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  server_.Queue(Signal(kMessageId, "accept", id));
  Refresh(service);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(Status(service->GetState(), id), "failed");
  EXPECT_EQ(server_.relay_requests, 1);
  server_.Queue(Signal(kSecondMessageId, "accept", id));
  Refresh(service);
  task_environment_.FastForwardBy(base::Seconds(8));
  EXPECT_EQ(server_.relay_requests, 1);
  EXPECT_EQ(Status(service->GetState(), id), "failed");
}

TEST_F(XenonTransferServiceTest, WrongPeerCannotChangeLocalTask) {
  auto* service = LoggedInService();
  const std::string id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  server_.Queue(
      Signal(kMessageId, "cancel", id, "ffffffffffffffffffffffffffffffff"));
  EXPECT_EQ(Status(Refresh(service), id), "pending");
  EXPECT_TRUE(service->GetState().FindDict("job")->FindBool("active").value());
}

TEST_F(XenonTransferServiceTest, ReceiveStartsWithoutWaitingForAcceptDelivery) {
  auto* service = LoggedInService();
  ReceiveOffer(service);
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
  EXPECT_EQ(server_.relay_requests, 0);
  server_.publish_failures = 100;
  base::test::TestFuture<base::DictValue> accepted;
  service->Respond(kTaskId, "accept", accepted.GetCallback());
  ASSERT_TRUE(accepted.Wait());
  task_environment_.RunUntilIdle();
  // The unavailable relay fails the payload attempt; an unavailable mailbox
  // must not prevent the consented attempt from starting in the first place.
  EXPECT_EQ(server_.relay_requests, 1);
  EXPECT_EQ(Status(service->GetState(), kTaskId), "failed");
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
}

TEST_F(XenonTransferServiceTest,
       FailedRejectDeliveryKeepsTerminalLocalDecision) {
  auto* service = LoggedInService();
  ReceiveOffer(service);
  server_.publish_failures = 100;
  base::test::TestFuture<base::DictValue> rejected;
  service->Respond(kTaskId, "reject", rejected.GetCallback());
  ASSERT_TRUE(rejected.Wait());
  EXPECT_EQ(Status(rejected.Get(), kTaskId), "rejected");
  EXPECT_FALSE(rejected.Get().FindDict("job")->FindBool("active").value());
  server_.Queue(Signal(kSecondMessageId, "offer"));
  EXPECT_EQ(Status(Refresh(service), kTaskId), "rejected");
  EXPECT_EQ(server_.relay_requests, 0);
  EXPECT_TRUE(SendText(service).FindString("error")->empty());
}

TEST_F(XenonTransferServiceTest, PeerMessageCannotClaimPayloadCompletion) {
  auto* service = LoggedInService();
  const std::string id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  server_.Queue(Signal(kMessageId, "completed", id));
  server_.Queue(Signal(kSecondMessageId, "complete", id));
  EXPECT_EQ(Status(Refresh(service), id), "pending");
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest,
       CoreSuccessIsLocalAndDoesNotNeedServerConfirmation) {
  auto* service = LoggedInService();
  ReceiveOffer(service);
  XenonTransferServiceTestPeer::TrackRunningTask(service, kTaskId);
  const int requests = server_.publish_requests;
  server_.fail_polls = true;
  XenonTransferServiceTestPeer::Complete(service, kTaskId);
  auto state = service->GetState();
  EXPECT_EQ(Status(state, kTaskId), "completed");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  EXPECT_EQ(
      *Scope()->FindDict("tasks")->FindDict(kTaskId)->FindString("status"),
      "completed");
  task_environment_.FastForwardBy(base::Seconds(35));
  EXPECT_EQ(server_.publish_requests, requests);
  EXPECT_EQ(Status(service->GetState(), kTaskId), "completed");
  EXPECT_TRUE(SendText(service, "next payload").FindString("error")->empty());
}

TEST_F(XenonTransferServiceTest, CompletedTaskIgnoresLateCancelAndOffer) {
  auto* service = LoggedInService();
  ReceiveOffer(service);
  XenonTransferServiceTestPeer::TrackRunningTask(service, kTaskId);
  XenonTransferServiceTestPeer::Complete(service, kTaskId);
  server_.Queue(Signal(kSecondMessageId, "cancel"));
  server_.Queue(Signal(kThirdMessageId, "offer"));
  const auto state = Refresh(service);
  EXPECT_EQ(Status(state, kTaskId), "completed");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest, LateWorkerFailureCannotReopenCompletedTask) {
  auto* service = LoggedInService();
  const std::string id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  XenonTransferServiceTestPeer::PrepareFailedCallback(service, id);
  XenonTransferServiceTestPeer::Complete(service, id);
  server_.fail_polls = true;
  XenonTransferServiceTestPeer::DeliverFailedCallback(service, id);
  const auto state = service->GetState();
  EXPECT_EQ(Status(state, id), "completed");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  EXPECT_TRUE(SendText(service, "next payload").FindString("error")->empty());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(PublishedType("cancel"), 0);
}

TEST_F(XenonTransferServiceTest,
       UnconfirmedSendIsLocalFailureWithoutPeerCancellation) {
  auto* service = LoggedInService();
  const std::string id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  XenonTransferServiceTestPeer::TrackRunningTask(service, id);
  XenonTransferServiceTestPeer::ReportUnconfirmedSend(service, id);
  const auto state = service->GetState();
  EXPECT_EQ(Status(state, id), "failed");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  EXPECT_NE(state.FindDict("job")->FindString("error")->find("未确认"),
            std::string::npos);
  task_environment_.FastForwardBy(base::Seconds(35));
  EXPECT_EQ(PublishedType("cancel"), 0);
  EXPECT_TRUE(SendText(service, "next payload").FindString("error")->empty());
  XenonTransferServiceTestPeer::ReloadSession(service);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(Status(service->GetState(), id), "failed");
  // Reload may cancel the new pending task; it must never cancel the unknown
  // result, since the receiver may have durably committed that payload.
  for (const auto& publication : server_.publications) {
    const auto& payload = *publication.FindDict("payload");
    EXPECT_FALSE(*payload.FindString("taskId") == id &&
                 *payload.FindString("type") == "cancel");
  }
}

TEST_F(XenonTransferServiceTest,
       FailedCancelDeliveryImmediatelyReleasesTaskSlot) {
  auto* service = LoggedInService();
  const std::string id = FirstTaskId(SendText(service));
  task_environment_.RunUntilIdle();
  server_.publish_failures = 100;
  base::test::TestFuture<base::DictValue> cancelled;
  service->Cancel(cancelled.GetCallback());
  ASSERT_TRUE(cancelled.Wait());
  EXPECT_EQ(Status(cancelled.Get(), id), "cancelled");
  EXPECT_FALSE(cancelled.Get().FindDict("job")->FindBool("active").value());
  task_environment_.RunUntilIdle();
  EXPECT_EQ(PublishedType("cancel"), 1);
  const auto next = SendText(service, "next payload");
  EXPECT_TRUE(next.FindString("error")->empty());
  EXPECT_EQ(next.FindList("transfers")->size(), 2u);
  EXPECT_EQ(Status(next, id), "cancelled");
}

TEST_F(XenonTransferServiceTest,
       RestartPreservesIncomingPendingAndCompletedHistory) {
  auto* service = LoggedInService();
  ReceiveOffer(service);
  XenonTransferServiceTestPeer::SetOperationBusy(service, true);
  server_.Queue(Signal(kSecondMessageId, "offer", kSecondTaskId));
  Refresh(service);
  XenonTransferServiceTestPeer::TrackRunningTask(service, kSecondTaskId);
  XenonTransferServiceTestPeer::Complete(service, kSecondTaskId);
  XenonTransferServiceTestPeer::ReloadSession(service);
  const auto state = service->GetState();
  EXPECT_EQ(Status(state, kTaskId), "pending");
  EXPECT_EQ(Status(state, kSecondTaskId), "completed");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  EXPECT_EQ(server_.relay_requests, 0);
  Refresh(service);
  task_environment_.RunUntilIdle();
  EXPECT_EQ(Status(service->GetState(), kTaskId), "failed");
  EXPECT_EQ(Status(service->GetState(), kSecondTaskId), "completed");
  EXPECT_EQ(server_.relay_requests, 1);
}

TEST_F(XenonTransferServiceTest, RestartMarksOutgoingWithoutSourceInterrupted) {
  auto* service = LoggedInService();
  const std::string id = FirstTaskId(SendText(service, "local history"));
  task_environment_.RunUntilIdle();
  XenonTransferServiceTestPeer::ReloadSession(service);
  auto state = Refresh(service);
  EXPECT_EQ(Status(state, id), "failed");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  EXPECT_EQ(*state.FindList("transfers")->front().GetDict().FindString("text"),
            "local history");
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest, RestartMarksAcceptedReceiveInterrupted) {
  auto* service = LoggedInService();
  ReceiveOffer(service);
  XenonTransferServiceTestPeer::TrackRunningTask(service, kTaskId);
  XenonTransferServiceTestPeer::ReloadSession(service);
  const auto state = Refresh(service);
  EXPECT_EQ(Status(state, kTaskId), "failed");
  EXPECT_FALSE(state.FindDict("job")->FindBool("active").value());
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest, LocalHistoryIsScopedToAccount) {
  auto* service = LoggedInService();
  XenonTransferServiceTestPeer::RememberDirectSuccess(service, kOtherDevice);
  ReceiveOffer(service);
  XenonAccountService::GetForProfile(profile_.get())->Logout({});
  task_environment_.RunUntilIdle();
  server_.account_id = "ffffffffffffffffffffffffffffffff";
  service = LoggedInService();
  EXPECT_TRUE(service->GetState().FindList("transfers")->empty());
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
  EXPECT_FALSE(
      XenonTransferServiceTestPeer::WasDirectSuccessful(service, kOtherDevice));
}

TEST_F(XenonTransferServiceTest, LocalHistoryIsScopedToDevice) {
  auto* service = LoggedInService();
  XenonTransferServiceTestPeer::RememberDirectSuccess(service, kOtherDevice);
  const std::string original_device = server_.device_id;
  ReceiveOffer(service);
  auto history =
      profile_->GetPrefs()->GetDict(xenon::prefs::kTransferHistory).Clone();
  XenonAccountService::GetForProfile(profile_.get())->Logout({});
  task_environment_.RunUntilIdle();
  auto other_profile = CreateProfile();
  other_profile->GetPrefs()->SetDict(xenon::prefs::kTransferHistory,
                                     std::move(history));
  auto* other = LoggedInService(other_profile.get());
  EXPECT_NE(server_.device_id, original_device);
  EXPECT_TRUE(other->GetState().FindList("transfers")->empty());
  EXPECT_FALSE(other->GetState().FindDict("job")->FindBool("active").value());
  EXPECT_FALSE(
      XenonTransferServiceTestPeer::WasDirectSuccessful(other, kOtherDevice));
}

TEST_F(XenonTransferServiceTest, LogoutDiscardsLateMailboxResponseWithoutAck) {
  auto* service = LoggedInService();
  auto reply_signal = Signal(kMessageId, "offer");
  base::test::TestFuture<void> started;
  auto release = std::make_shared<base::WaitableEvent>();
  base::ScopedClosureRunner cleanup(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      release));
  XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
      [](FakeMailbox* server, base::RepeatingClosure request_started,
         std::shared_ptr<base::WaitableEvent> gate,
         const base::DictValue& signal, std::string method, std::string path,
         std::string token, base::DictValue body) -> Response {
        if (path.ends_with("/signals") && method == "GET") {
          request_started.Run();
          base::ScopedAllowBaseSyncPrimitivesForTesting allow;
          gate->Wait();
          base::ListValue messages;
          messages.Append(signal.Clone());
          return {
              200, base::DictValue().Set("signals", std::move(messages)), {}};
        }
        return server->Handle(std::move(method), std::move(path),
                              std::move(token), std::move(body));
      },
      base::Unretained(&server_), started.GetSequenceBoundRepeatingCallback(),
      release, std::move(reply_signal)));
  base::test::TestFuture<base::DictValue> refreshed;
  service->Refresh(refreshed.GetCallback());
  ASSERT_TRUE(started.Wait());
  XenonAccountService::GetForProfile(profile_.get())->Logout({});
  release->Signal();
  ASSERT_TRUE(refreshed.Wait());
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(service->GetState().FindList("transfers")->empty());
  EXPECT_EQ(server_.ack_requests, 0);
}

TEST_F(XenonTransferServiceTest, InvalidOfferMetadataIsNotMadeIntoLocalTask) {
  auto* service = LoggedInService();
  auto signal = Signal(kMessageId, "offer");
  signal.FindDict("payload")->FindDict("offer")->Set("fileName",
                                                     "../outside.txt");
  server_.Queue(std::move(signal));
  EXPECT_TRUE(Refresh(service).FindList("transfers")->empty());
  EXPECT_EQ(server_.relay_requests, 0);
}

TEST_F(XenonTransferServiceTest, LogoutDiscardsLatePublicationCallback) {
  auto* service = LoggedInService();
  base::test::TestFuture<void> started;
  auto release = std::make_shared<base::WaitableEvent>();
  base::ScopedClosureRunner cleanup(base::BindOnce(
      [](std::shared_ptr<base::WaitableEvent> event) { event->Signal(); },
      release));
  XenonAccountService::SetRequestHandlerForTesting(base::BindRepeating(
      [](FakeMailbox* server, base::RepeatingClosure request_started,
         std::shared_ptr<base::WaitableEvent> gate, std::string method,
         std::string path, std::string token, base::DictValue body) {
        if (path == "/signals") {
          request_started.Run();
          base::ScopedAllowBaseSyncPrimitivesForTesting allow;
          gate->Wait();
        }
        return server->Handle(std::move(method), std::move(path),
                              std::move(token), std::move(body));
      },
      base::Unretained(&server_), started.GetSequenceBoundRepeatingCallback(),
      release));
  base::test::TestFuture<base::DictValue> sent;
  service->SendText(kOtherDevice, "private local text", sent.GetCallback());
  ASSERT_TRUE(started.Wait());
  ASSERT_TRUE(sent.IsReady());
  XenonAccountService::GetForProfile(profile_.get())->Logout({});
  release->Signal();
  task_environment_.RunUntilIdle();
  task_environment_.FastForwardBy(base::Seconds(10));
  EXPECT_TRUE(service->GetState().FindList("transfers")->empty());
  EXPECT_FALSE(service->GetState().FindDict("job")->FindBool("active").value());
  EXPECT_EQ(server_.publish_requests, 1);
}

TEST_F(XenonTransferServiceTest,
       TextLimitUsesUtf8BytesAndStunPreferenceStaysLocal) {
  auto* service = LoggedInService();
  const std::string text(16 * 1024, '<');
  auto state = SendText(service, text);
  EXPECT_TRUE(state.FindString("error")->empty());
  const std::string id = FirstTaskId(state);
  EXPECT_EQ(*state.FindList("transfers")->front().GetDict().FindString("text"),
            text);
  base::test::TestFuture<base::DictValue> oversized;
  service->SendText(kOtherDevice, text + '<', oversized.GetCallback());
  EXPECT_FALSE(oversized.Get().FindString("error")->empty());
  EXPECT_EQ(service->GetState().FindList("transfers")->size(), 1u);
  base::test::TestFuture<base::DictValue> configured;
  service->SetUseStun(true, configured.GetCallback());
  EXPECT_TRUE(configured.Get().FindBool("useStun").value());
  EXPECT_EQ(Status(service->GetState(), id), "pending");
}

}  // namespace
