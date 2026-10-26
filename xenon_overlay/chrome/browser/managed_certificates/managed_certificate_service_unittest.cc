// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_service.h"

#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/stringprintf.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "mojo/core/embedder/embedder.h"
#include "net/base/net_errors.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/url_loader_completion_status.h"
#include "services/network/public/mojom/fetch_api.mojom.h"
#include "services/network/public/mojom/url_response_head.mojom.h"
#include "services/network/test/test_url_loader_factory.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::certificates {
namespace {

constexpr char kEndpoint[] = "https://certificates.example.test/v1/snapshot";
constexpr char kConfig[] = R"({"xenon_managed_ca":{"enabled":true,
  "source_id":"test","endpoint":"https://certificates.example.test/v1/snapshot"}})";

std::string Timestamp(base::Time time) {
  base::Time::Exploded exploded;
  time.UTCExplode(&exploded);
  return base::StringPrintf("%04d-%02d-%02dT%02d:%02d:%02dZ", exploded.year,
                            exploded.month, exploded.day_of_month,
                            exploded.hour, exploded.minute, exploded.second);
}

}  // namespace

class ManagedCertificateServiceTest : public testing::Test {
 public:
  ManagedCertificateServiceTest() {
    // This standalone suite uses base's test main, which does not initialize
    // Mojo before SimpleURLLoader creates its message pipes.
    static const bool initialized = [] {
      mojo::core::Init();
      return true;
    }();
    (void)initialized;
  }

 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    config_path_ = directory_.GetPath().AppendASCII("managed.json");
    ASSERT_TRUE(base::WriteFile(config_path_, kConfig));
    base::FilePath root;
    ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root));
    ASSERT_TRUE(base::ReadFileToString(
        root.AppendASCII("xenon_overlay/tools/updater/xenon_update_tool/"
                         "managed_certificates/certificates/"
                         "bjca-global-root-ca1.pem"),
        &ca_pem_));
    service_.test_factory_ = &factory_;
  }

  void TearDown() override {
    service_.Shutdown();
    task_environment_.RunUntilIdle();
  }

  void Start() {
    service_.Start(config_path_, directory_.GetPath());
    task_environment_.RunUntilIdle();
  }

  void Refresh() {
    service_.Fetch();
    task_environment_.RunUntilIdle();
  }

  void NetworkRecovered() {
    service_.OnNetworkChanged(net::NetworkChangeNotifier::CONNECTION_ETHERNET);
    task_environment_.RunUntilIdle();
  }

  std::string Body(uint64_t generation,
                   std::optional<base::Time> legacy_expiry = std::nullopt,
                   bool include_ca = true) {
    base::ListValue certs;
    if (include_ca) {
      base::DictValue cert;
      cert.Set("role", "trust_anchor");
      cert.Set("encoding", "pem");
      cert.Set("data", ca_pem_);
      certs.Append(std::move(cert));
    }
    base::DictValue body;
    body.Set("schema_version", 1);
    body.Set("source_id", "test");
    body.Set("generation", base::NumberToString(generation));
    body.Set("issued_at", Timestamp(base::Time::Now() - base::Minutes(1)));
    if (legacy_expiry) {
      body.Set("expires_at", Timestamp(*legacy_expiry));
    }
    body.Set("certificates", std::move(certs));
    return base::WriteJson(body).value();
  }

  void FailNetwork() {
    ASSERT_TRUE(factory_.SimulateResponseForPendingRequest(
        GURL(kEndpoint),
        network::URLLoaderCompletionStatus(net::ERR_INTERNET_DISCONNECTED),
        network::mojom::URLResponseHead::New(), ""));
    task_environment_.RunUntilIdle();
  }

  void Respond(std::string body,
               int code = 200,
               std::string etag = "\"snapshot-1\"",
               std::string content_type = "application/json",
               std::string retry_after = "") {
    auto head = network::mojom::URLResponseHead::New();
    head->headers =
        net::HttpResponseHeaders::Builder({1, 1}, base::NumberToString(code))
            .AddHeader("Content-Type", content_type)
            .AddHeader("ETag", etag)
            .AddHeader("Retry-After", retry_after)
            .Build();
    ASSERT_TRUE(factory_.SimulateResponseForPendingRequest(
        GURL(kEndpoint), network::URLLoaderCompletionStatus(net::OK),
        std::move(head), body));
    task_environment_.RunUntilIdle();
  }

  std::unique_ptr<ServiceWriteResult> Apply(PersistentState state,
                                            std::string body) {
    return service_.ValidateAndSave(StatePath(directory_.GetPath(), "test"),
                                    std::move(state), std::move(body), "etag",
                                    "context");
  }

  PersistentState State() {
    std::string error;
    auto state =
        LoadState(StatePath(directory_.GetPath(), "test"), "test", &error);
    EXPECT_TRUE(state) << error;
    return state.value_or(PersistentState());
  }

  base::TimeDelta RefreshDelay() {
    return service_.refresh_timer_.GetCurrentDelay();
  }

  void SetCachedETag(std::string etag) {
    service_.state_.etag = std::move(etag);
  }

  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO,
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
  base::ScopedTempDir directory_;
  base::FilePath config_path_;
  std::string ca_pem_;
  network::TestURLLoaderFactory factory_;
  XenonManagedCertificateService service_;
};

TEST_F(ManagedCertificateServiceTest, StartupDoesNotWaitForServer) {
  EXPECT_TRUE(service_.ready());
  service_.Start(config_path_, directory_.GetPath());
  EXPECT_FALSE(service_.ready());
  task_environment_.RunUntilIdle();
  EXPECT_TRUE(service_.ready());
  EXPECT_FALSE(service_.snapshot());
  ASSERT_TRUE(factory_.IsPending(kEndpoint));
  const auto& request = factory_.GetPendingRequest(0)->request;
  EXPECT_EQ(request.credentials_mode, network::mojom::CredentialsMode::kOmit);
  EXPECT_EQ(request.redirect_mode, network::mojom::RedirectMode::kError);
  EXPECT_TRUE(request.do_not_prompt_for_login);
  EXPECT_EQ(request.headers.GetHeader("Accept-Encoding"), "identity");
  EXPECT_FALSE(request.headers.HasHeader("Authorization"));
}

TEST_F(ManagedCertificateServiceTest,
       StatusDistinguishesConfigurationAndRefreshFailures) {
  EXPECT_FALSE(service_.GetStatus().started);
  EXPECT_TRUE(service_.GetStatus().ready);
  ASSERT_TRUE(base::WriteFile(config_path_, "{invalid"));
  Start();
  auto status = service_.GetStatus();
  EXPECT_TRUE(status.started);
  EXPECT_FALSE(status.enabled);
  EXPECT_FALSE(status.configuration_error.empty());
  EXPECT_TRUE(status.refresh_error.empty());
  EXPECT_TRUE(status.last_attempt.is_null());
  ASSERT_TRUE(base::WriteFile(config_path_, kConfig));
  Start();
  status = service_.GetStatus();
  EXPECT_TRUE(status.enabled);
  EXPECT_TRUE(status.downloading);
  EXPECT_EQ(status.source_id, "test");
  EXPECT_TRUE(status.configuration_error.empty());
  EXPECT_FALSE(status.last_attempt.is_null());
  Respond("", 503);
  EXPECT_EQ(service_.GetStatus().refresh_error, "http_status:503");
  EXPECT_FALSE(service_.GetStatus().downloading);
  EXPECT_TRUE(service_.GetStatus().last_success.is_null());
  EXPECT_EQ(service_.GetStatus().refresh_error, "http_status:503");
  Refresh();
  Respond(Body(1, base::Time::Now() + base::Hours(1)));
  EXPECT_TRUE(service_.GetStatus().refresh_error.empty());
  EXPECT_FALSE(service_.GetStatus().last_success.is_null());
  service_.Shutdown();
  EXPECT_FALSE(service_.GetStatus().started);
}

TEST_F(ManagedCertificateServiceTest, PublishesOnlyAfterPersistence) {
  Start();
  int publications = 0;
  auto subscription = service_.AddObserver(base::BindLambdaForTesting([&] {
    ASSERT_TRUE(service_.snapshot());
    EXPECT_EQ(State().body_hash, service_.snapshot()->body_hash);
    ++publications;
  }));
  Respond(Body(1));
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(service_.snapshot()->generation, 1u);
  EXPECT_EQ(service_.snapshot()->certificates.size(), 1u);
  EXPECT_EQ(publications, 1);
}

TEST_F(ManagedCertificateServiceTest, NotModifiedDoesNotRewriteCache) {
  Start();
  Respond(Body(1));
  ASSERT_TRUE(service_.snapshot());
  const auto state_path = StatePath(directory_.GetPath(), "test");
  const auto untouched = base::Time::UnixEpoch() + base::Days(1);
  ASSERT_TRUE(base::TouchFile(state_path, untouched, untouched));
  Refresh();
  EXPECT_TRUE(factory_.GetPendingRequest(0)->request.headers.HasHeader(
      "If-None-Match"));
  Respond("", 304);
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(State().body_hash, service_.snapshot()->body_hash);
  base::File::Info info;
  ASSERT_TRUE(base::GetFileInfo(state_path, &info));
  EXPECT_EQ(info.last_modified, untouched);
  EXPECT_TRUE(service_.GetStatus().refresh_error.empty());
}

TEST_F(ManagedCertificateServiceTest, RejectsNotModifiedWithoutCachedBody) {
  Start();
  Respond("", 304);
  EXPECT_FALSE(service_.snapshot());
  EXPECT_FALSE(base::PathExists(StatePath(directory_.GetPath(), "test")));
}

TEST_F(ManagedCertificateServiceTest,
       RejectsNotModifiedForUnconditionalRequest) {
  Start();
  Respond(Body(1, base::Time::Now() + base::Hours(1)));
  SetCachedETag("\"invalid\tvalue\"");
  Refresh();
  EXPECT_FALSE(factory_.GetPendingRequest(0)->request.headers.HasHeader(
      "If-None-Match"));
  // Even if the current cache becomes usable, a 304 must correspond to the
  // condition actually sent on this request, not just the cache at completion.
  SetCachedETag("\"snapshot-1\"");
  Respond("", 304);
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(RefreshDelay(), base::Minutes(1));
}

TEST_F(ManagedCertificateServiceTest, DisabledConfigurationRequiresRestart) {
  Start();
  Respond(Body(9));
  ASSERT_TRUE(base::WriteFile(config_path_,
                              R"({"xenon_managed_ca":{"enabled":false}})"));
  task_environment_.FastForwardBy(base::Minutes(2));
  ASSERT_TRUE(service_.snapshot());
  EXPECT_TRUE(service_.GetStatus().enabled);
  NetworkRecovered();
  ASSERT_TRUE(factory_.IsPending(kEndpoint));
  Respond("", 304);
  ASSERT_TRUE(service_.snapshot());
  Start();
  EXPECT_TRUE(service_.ready());
  EXPECT_FALSE(service_.GetStatus().enabled);
  EXPECT_FALSE(service_.snapshot());
  EXPECT_FALSE(factory_.IsPending(kEndpoint));
  EXPECT_EQ(State().highest_generation, 9u);
  ASSERT_TRUE(base::WriteFile(config_path_, kConfig));
  Start();
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(service_.snapshot()->generation, 9u);
}

TEST_F(ManagedCertificateServiceTest, OfflineRestartRetainsLegacyExpiredCache) {
  Start();
  Respond(Body(8, base::Time::Now() + base::Seconds(10)));
  service_.Shutdown();
  task_environment_.FastForwardBy(base::Days(60));
  Start();
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(service_.snapshot()->generation, 8u);
  EXPECT_EQ(service_.snapshot()->certificates.size(), 1u);
  FailNetwork();
  EXPECT_TRUE(service_.snapshot());
  EXPECT_FALSE(service_.GetStatus().refresh_error.empty());
}

TEST_F(ManagedCertificateServiceTest, NoMinutePollingOfConfigurationOrCache) {
  Start();
  Respond(Body(1));
  ASSERT_TRUE(base::WriteFile(config_path_, "{invalid"));
  const auto state_path = StatePath(directory_.GetPath(), "test");
  ASSERT_TRUE(base::WriteFile(state_path, "{changed_on_disk"));
  task_environment_.FastForwardBy(base::Minutes(2));
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(service_.snapshot()->generation, 1u);
  EXPECT_TRUE(service_.GetStatus().configuration_error.empty());
  EXPECT_FALSE(factory_.IsPending(kEndpoint));
  std::string disk_body;
  ASSERT_TRUE(base::ReadFileToString(state_path, &disk_body));
  EXPECT_EQ(disk_body, "{changed_on_disk");
}

TEST_F(ManagedCertificateServiceTest,
       RejectsRollbackAndSameVersionEquivocation) {
  PersistentState state;
  state.source_id = "test";
  auto accepted = Apply(state, Body(10));
  ASSERT_TRUE(accepted->success) << accepted->error;
  auto rollback = Apply(accepted->state, Body(9));
  EXPECT_FALSE(rollback->success);
  auto equivocation = Apply(accepted->state, Body(10, std::nullopt, false));
  EXPECT_FALSE(equivocation->success);
  auto same_body = Apply(accepted->state, accepted->state.snapshot->body);
  EXPECT_TRUE(same_body->success);
  EXPECT_EQ(State().highest_generation, 10u);
}

TEST_F(ManagedCertificateServiceTest,
       EmptySnapshotRevokesCertificatesAndCannotBeRolledBack) {
  Start();
  const auto original = Body(1);
  Respond(original);
  ASSERT_TRUE(service_.snapshot());
  ASSERT_EQ(service_.snapshot()->certificates.size(), 1u);
  int publications = 0;
  auto subscription =
      service_.AddObserver(base::BindLambdaForTesting([&] { ++publications; }));
  Refresh();
  Respond(Body(2, std::nullopt, false));
  ASSERT_TRUE(service_.snapshot());
  EXPECT_TRUE(service_.snapshot()->certificates.empty());
  EXPECT_EQ(publications, 1);
  EXPECT_EQ(State().highest_generation, 2u);
  Refresh();
  Respond(original);
  EXPECT_EQ(publications, 1);
  ASSERT_TRUE(service_.snapshot());
  EXPECT_TRUE(service_.snapshot()->certificates.empty());
  Start();
  ASSERT_TRUE(service_.snapshot());
  EXPECT_TRUE(service_.snapshot()->certificates.empty());
  Respond(original);
  EXPECT_EQ(State().highest_generation, 2u);
  EXPECT_TRUE(service_.snapshot()->certificates.empty());
}

TEST_F(ManagedCertificateServiceTest,
       RetainsCertificatesPastLegacySnapshotExpiryWhileOffline) {
  Start();
  Respond(Body(1, base::Time::Now() + base::Seconds(10)));
  ASSERT_TRUE(service_.snapshot());
  const auto original = service_.snapshot()->body_hash;
  int publications = 0;
  auto subscription =
      service_.AddObserver(base::BindLambdaForTesting([&] { ++publications; }));
  Refresh();
  FailNetwork();
  task_environment_.FastForwardBy(base::Seconds(11));
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(service_.snapshot()->body_hash, original);
  EXPECT_EQ(service_.snapshot()->certificates.size(), 1u);
  EXPECT_EQ(publications, 0);
  EXPECT_EQ(State().body_hash, original);
}

TEST_F(ManagedCertificateServiceTest, ExistingCorruptStateFailsClosed) {
  const auto state_path = StatePath(directory_.GetPath(), "test");
  ASSERT_TRUE(base::CreateDirectory(state_path.DirName()));
  ASSERT_TRUE(base::WriteFile(state_path, "{broken"));
  Start();
  EXPECT_TRUE(service_.ready());
  EXPECT_FALSE(service_.snapshot());
  EXPECT_FALSE(factory_.IsPending(kEndpoint));
  std::string body;
  ASSERT_TRUE(base::ReadFileToString(state_path, &body));
  EXPECT_EQ(body, "{broken");
}

TEST_F(ManagedCertificateServiceTest,
       RuntimeStateDeletionDoesNotResetInMemoryGeneration) {
  Start();
  Respond(Body(9));
  ASSERT_TRUE(base::DeleteFile(StatePath(directory_.GetPath(), "test")));
  Refresh();
  Respond(Body(8));
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(service_.snapshot()->generation, 9u);
  EXPECT_FALSE(base::PathExists(StatePath(directory_.GetPath(), "test")));
  Refresh();
  Respond(Body(10));
  EXPECT_EQ(State().highest_generation, 10u);
}

TEST_F(ManagedCertificateServiceTest, FailedPersistenceDoesNotPublish) {
  Start();
  const auto state_path = StatePath(directory_.GetPath(), "test");
  ASSERT_TRUE(base::CreateDirectory(state_path));
  Respond(Body(1, base::Time::Now() + base::Hours(1)));
  EXPECT_FALSE(service_.snapshot());
}

TEST_F(ManagedCertificateServiceTest, RejectsUnexpectedMimeTypeAndCharset) {
  Start();
  const auto body = Body(1, base::Time::Now() + base::Hours(1));
  Respond(body, 200, "etag", "text/html");
  EXPECT_FALSE(service_.snapshot());
  Refresh();
  Respond(body, 200, "etag", "application/json; charset=iso-8859-1");
  EXPECT_FALSE(service_.snapshot());
  Refresh();
  Respond(body, 200, "etag", "application/json; charset=utf-8");
  EXPECT_TRUE(service_.snapshot());
}

TEST_F(ManagedCertificateServiceTest, RefreshAndRetryRemainScheduled) {
  Start();
  Respond(Body(1));
  EXPECT_GE(RefreshDelay(), base::Hours(6) * 0.9);
  EXPECT_LE(RefreshDelay(), base::Hours(6) * 1.1);
  task_environment_.FastForwardBy(RefreshDelay());
  ASSERT_TRUE(factory_.IsPending(kEndpoint));
  Respond("", 503, "", "text/plain", "86400");
  EXPECT_EQ(RefreshDelay(), base::Hours(1));
  task_environment_.FastForwardBy(base::Hours(1));
  ASSERT_TRUE(factory_.IsPending(kEndpoint));
  FailNetwork();
  EXPECT_EQ(RefreshDelay(), base::Minutes(5));
  EXPECT_TRUE(service_.snapshot());
}

TEST_F(ManagedCertificateServiceTest, ConfigurationEditDoesNotCancelRequest) {
  ASSERT_TRUE(base::WriteFile(
      config_path_,
      R"({"xenon_managed_ca":{"enabled":true,"source_id":"test",)"
      R"("endpoint":"https://certificates.example.test/v1/snapshot",)"
      R"("request_timeout_seconds":120}})"));
  Start();
  ASSERT_TRUE(base::WriteFile(config_path_, "{invalid"));
  task_environment_.FastForwardBy(base::Seconds(61));
  EXPECT_TRUE(factory_.IsPending(kEndpoint));
  Respond(Body(1, base::Time::Now() + base::Hours(1)));
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(service_.snapshot()->generation, 1u);
}

TEST_F(ManagedCertificateServiceTest, RestartLoadsCacheBeforeServerReply) {
  Start();
  Respond(Body(8));
  ASSERT_TRUE(service_.snapshot());
  const auto state_path = StatePath(directory_.GetPath(), "test");
  const auto untouched = base::Time::UnixEpoch() + base::Days(1);
  ASSERT_TRUE(base::TouchFile(state_path, untouched, untouched));
  Start();
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(service_.snapshot()->generation, 8u);
  EXPECT_TRUE(factory_.IsPending(kEndpoint));
  base::File::Info info;
  ASSERT_TRUE(base::GetFileInfo(state_path, &info));
  EXPECT_EQ(info.last_modified, untouched);
  Respond("", 304);
  ASSERT_TRUE(service_.snapshot());
  EXPECT_EQ(service_.snapshot()->generation, 8u);
}

TEST_F(ManagedCertificateServiceTest, EndpointChangeOnRestartSeparatesTrust) {
  Start();
  Respond(Body(8, base::Time::Now() + base::Hours(1)));
  ASSERT_TRUE(service_.snapshot());
  constexpr char replacement[] = "https://new.example.test/v1/snapshot";
  ASSERT_TRUE(base::WriteFile(
      config_path_, R"({"xenon_managed_ca":{"enabled":true,"source_id":"test",)"
                    R"("endpoint":"https://new.example.test/v1/snapshot"}})"));
  NetworkRecovered();
  ASSERT_TRUE(factory_.IsPending(kEndpoint));
  Respond("", 304);
  ASSERT_TRUE(service_.snapshot());
  Start();
  EXPECT_FALSE(service_.snapshot());
  const network::ResourceRequest* request = nullptr;
  ASSERT_TRUE(factory_.IsPending(replacement, &request));
  EXPECT_FALSE(request->headers.HasHeader("If-None-Match"));
  EXPECT_EQ(State().highest_generation, 8u);
}

TEST_F(ManagedCertificateServiceTest, BootstrapChangesRequireRestart) {
  const auto bootstrap = directory_.GetPath().AppendASCII("bootstrap.pem");
  ASSERT_TRUE(base::WriteFile(bootstrap, ca_pem_));
  base::DictValue config;
  config.Set("enabled", true);
  config.Set("source_id", "test");
  config.Set("endpoint", kEndpoint);
  config.Set("bootstrap_ca_file", bootstrap.AsUTF8Unsafe());
  base::DictValue root;
  root.Set("xenon_managed_ca", std::move(config));
  ASSERT_TRUE(base::WriteFile(config_path_, base::WriteJson(root).value()));
  Start();
  Respond(Body(1));
  ASSERT_TRUE(base::WriteFile(bootstrap, "invalid replacement"));
  NetworkRecovered();
  ASSERT_TRUE(factory_.IsPending(kEndpoint));
  Respond("", 304);
  ASSERT_TRUE(service_.snapshot());
  EXPECT_TRUE(service_.GetStatus().configuration_error.empty());
  Start();
  EXPECT_FALSE(service_.snapshot());
  EXPECT_FALSE(service_.GetStatus().configuration_error.empty());
  EXPECT_FALSE(factory_.IsPending(kEndpoint));
}

TEST_F(ManagedCertificateServiceTest, ShutdownClearsSnapshotAndNotifies) {
  Start();
  Respond(Body(1, base::Time::Now() + base::Hours(1)));
  int calls = 0;
  auto subscription =
      service_.AddObserver(base::BindLambdaForTesting([&calls] { ++calls; }));
  service_.Shutdown();
  EXPECT_EQ(calls, 1);
  EXPECT_TRUE(service_.ready());
  EXPECT_FALSE(service_.snapshot());
}

}  // namespace xenon::certificates
