// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/containers/flat_set.h"
#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/test_future.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"
#include "crypto/hash.h"
#include "mojo/core/embedder/scoped_ipc_support.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "net/base/address_list.h"
#include "net/base/features.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/network_handle.h"
#include "net/base/test_completion_callback.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/dns/mock_host_resolver.h"
#include "net/gm/gm_tls_policy.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_info.h"
#include "net/http/http_stream_factory.h"
#include "net/http/http_stream_pool.h"
#include "net/http/http_transaction_factory.h"
#include "net/http/transport_security_state.h"
#include "net/log/net_log.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/tcp_client_socket.h"
#include "net/ssl/ssl_config.h"
#include "net/ssl/ssl_info.h"
#include "net/ssl/test_ssl_config_service.h"
#include "net/test/test_with_task_environment.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_test_util.h"
#include "services/gm_key/test/key_service_test_client.h"
#include "services/network/gm_identity_provider.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace network {
namespace {
using KeyError = gm_key::mojom::KeyError;
using KeyResult = gm_key::mojom::KeyResultPtr;
using KeyIdentity = gm_key::mojom::KeyIdentityPtr;

// Pauses only the first CertificateRequest at either side of the real identity
// provider. This deterministically exercises changes to the page owning an
// in-flight handshake, including adoption of a preconnect.
class GatedIdentityProvider final : public net::gm::ClientIdentityProvider {
 public:
  explicit GatedIdentityProvider(net::gm::ClientIdentityProvider* provider)
      : provider_(provider) {}
  scoped_refptr<net::gm::ClientIdentity> GetCachedIdentity(
      const net::HostPortPair& server,
      const net::gm::ConnectionProfile& profile) override {
    return provider_->GetCachedIdentity(server, profile);
  }
  base::OnceClosure AcquireIdentity(
      const net::HostPortPair& server,
      const net::gm::ConnectionProfile& profile,
      const net::gm::ClientCertificateRequest& request,
      AcquireCallback callback) override {
    if (++acquire_calls != 1) {
      return provider_->AcquireIdentity(server, profile, request,
                                        std::move(callback));
    }
    server_ = server;
    profile_ = profile;
    request_ = request;
    callback_ = std::move(callback);
    return base::BindOnce(&GatedIdentityProvider::CancelFirst,
                          weak_factory_.GetWeakPtr());
  }
  void Proceed(bool hold_reply) {
    hold_reply_ = hold_reply;
    cancel_.ReplaceClosure(provider_->AcquireIdentity(
        server_, profile_, request_,
        base::BindOnce(&GatedIdentityProvider::OnAcquired,
                       weak_factory_.GetWeakPtr())));
  }
  void Deliver() {
    ASSERT_TRUE(reply_ready);
    reply_ready = false;
    if (callback_) {
      std::move(callback_).Run(error_, std::move(identity_));
    }
  }
  int acquire_calls = 0;
  bool reply_ready = false;

 private:
  void CancelFirst() {
    callback_.Reset();
    cancel_.RunAndReset();
  }
  void OnAcquired(int error, scoped_refptr<net::gm::ClientIdentity> identity) {
    cancel_.Release().Reset();
    if (!callback_) {
      return;
    }
    error_ = error;
    identity_ = std::move(identity);
    reply_ready = true;
    if (!hold_reply_) {
      Deliver();
    }
  }
  raw_ptr<net::gm::ClientIdentityProvider> provider_;
  net::HostPortPair server_;
  net::gm::ConnectionProfile profile_;
  net::gm::ClientCertificateRequest request_;
  AcquireCallback callback_;
  base::ScopedClosureRunner cancel_;
  bool hold_reply_ = false;
  int error_ = net::ERR_IO_PENDING;
  scoped_refptr<net::gm::ClientIdentity> identity_;
  base::WeakPtrFactory<GatedIdentityProvider> weak_factory_{this};
};

// Only the browser authorization boundary is simulated. KeySigner is the real
// service implementation in another process, backed by the SM2 SKF test DLL.
class ServiceBroker : public mojom::GmIdentityBroker,
                      public gm_key::mojom::KeyObserver {
 public:
  explicit ServiceBroker(gm_key::test::ServiceProcess* process)
      : process_(process) {}
  mojo::PendingRemote<mojom::GmIdentityBroker> Bind() {
    return broker_receiver_.BindNewPipeAndPassRemote();
  }
  mojo::PendingRemote<gm_key::mojom::KeyObserver> BindObserver() {
    return observer_receiver_.BindNewPipeAndPassRemote();
  }
  void AcquireIdentity(uint64_t request_id,
                       const url::Origin& origin,
                       const std::string& policy_id,
                       uint64_t generation,
                       mojom::GmClientCertificateRequestPtr constraints,
                       AcquireIdentityCallback callback) override {
    ++authorizations;
    EXPECT_EQ(origin.host(), "gm.test");
    EXPECT_EQ(policy_id, "feitian");
    EXPECT_EQ(generation, 1u);
    mojo::PendingRemote<gm_key::mojom::KeySigner> signer;
    auto receiver = signer.InitWithNewPipeAndPassReceiver();
    process_->service()->Authorize(
        request_id, identity->identity_id, identity->generation,
        gm_key::SecurePin(
            valid_pin ? std::vector<uint8_t>{'1', '2', '3', '4', '5', '6'}
                      : std::vector<uint8_t>{'b', 'a', 'd'}),
        std::move(receiver),
        base::BindOnce(&ServiceBroker::OnAuthorizationPrepared,
                       weak_factory_.GetWeakPtr(), request_id,
                       std::move(callback), std::move(signer)));
  }
  void Cancel(uint64_t request_id) override {
    cancelled_requests_.insert(request_id);
    process_->service()->CancelAuthorization(request_id);
  }
  void OnIdentitiesChanged() override {}
  void OnServiceFailure(KeyError error) override {}
  KeyIdentity identity;
  bool valid_pin = true;
  int authorizations = 0;
  int commits = 0;
  bool hold_first_authorization = false;
  base::OnceClosure held_authorization;

 private:
  void OnAuthorizationPrepared(
      uint64_t request_id,
      AcquireIdentityCallback reply,
      mojo::PendingRemote<gm_key::mojom::KeySigner> signer,
      KeyResult result,
      KeyIdentity prepared_identity) {
    auto complete = base::BindOnce(
        &ServiceBroker::CompleteAuthorization, weak_factory_.GetWeakPtr(),
        request_id, std::move(reply), std::move(signer), std::move(result),
        std::move(prepared_identity));
    if (hold_first_authorization && request_id == 1) {
      held_authorization = std::move(complete);
    } else {
      std::move(complete).Run();
    }
  }
  void CompleteAuthorization(
      uint64_t request_id,
      AcquireIdentityCallback reply,
      mojo::PendingRemote<gm_key::mojom::KeySigner> signer,
      KeyResult result,
      KeyIdentity prepared_identity) {
    if (cancelled_requests_.contains(request_id)) {
      process_->service()->CancelAuthorization(request_id);
      std::move(reply).Run(net::ERR_ABORTED, nullptr);
      return;
    }
    if (result->error != KeyError::kOk || !prepared_identity) {
      process_->service()->CancelAuthorization(request_id);
      std::move(reply).Run(net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED,
                           nullptr);
      return;
    }
    auto grant = mojom::GmIdentityGrant::New();
    grant->identity = std::move(prepared_identity);
    grant->signer = std::move(signer);
    process_->service()->CommitAuthorization(request_id);
    ++commits;
    std::move(reply).Run(net::OK, std::move(grant));
  }
  raw_ptr<gm_key::test::ServiceProcess> process_;
  base::flat_set<uint64_t> cancelled_requests_;
  mojo::Receiver<mojom::GmIdentityBroker> broker_receiver_{this};
  mojo::Receiver<gm_key::mojom::KeyObserver> observer_receiver_{this};
  base::WeakPtrFactory<ServiceBroker> weak_factory_{this};
};

class GmIdentityTlsTest : public net::TestWithTaskEnvironment,
                          public testing::WithParamInterface<bool> {
 protected:
  virtual bool RequireClientCertificate() const { return true; }
  void SetUp() override {
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir_));
    base::Thread::Options options(base::MessagePumpType::IO, 0);
    ASSERT_TRUE(ipc_thread_.StartWithOptions(std::move(options)));
    ipc_ = std::make_unique<mojo::core::ScopedIPCSupport>(
        ipc_thread_.task_runner(),
        mojo::core::ScopedIPCSupport::ShutdownPolicy::CLEAN);
    process_ = std::make_unique<gm_key::test::ServiceProcess>();
    ASSERT_TRUE(process_->Launch(
        executable_dir_.AppendASCII("gm_key_service_test_helper.exe")));
    broker_ = std::make_unique<ServiceBroker>(process_.get());
    auto driver = gm_key::mojom::DriverConfig::New();
    driver->library_path = executable_dir_.AppendASCII("gm_sm2_simulator.dll");
    driver->sha256.resize(32);
    base::File file(driver->library_path,
                    base::File::FLAG_OPEN | base::File::FLAG_READ);
    ASSERT_TRUE(
        crypto::hash::HashFile(crypto::hash::kSha256, &file, driver->sha256));
    base::test::TestFuture<KeyResult> configured;
    process_->service()->Configure(std::move(driver), broker_->BindObserver(),
                                   configured.GetCallback());
    ASSERT_EQ(configured.Get()->error, KeyError::kOk);
    base::test::TestFuture<KeyResult, std::vector<KeyIdentity>> enumerated;
    process_->service()->Enumerate(enumerated.GetCallback());
    ASSERT_EQ(enumerated.Get<0>()->error, KeyError::kOk);
    ASSERT_EQ(enumerated.Get<1>().size(), 1u);
    broker_->identity = enumerated.Get<1>().front().Clone();
    const auto client_root =
        directory_.GetPath().AppendASCII("device-root.der");
    ASSERT_TRUE(base::WriteFile(client_root, Control(0)));
    provider_ = std::make_unique<GmIdentityProvider>(broker_->Bind());
    gm_client_auth_context_ = RegisterAuthContext();
    context_.set_gm_identity_provider(provider_.get());

    base::CommandLine command(executable_dir_.AppendASCII("gm_interop_peer")
                                  .AppendASCII("gm_backend_smoke.exe"));
    command.AppendArg("--serve");
    command.AppendArg("0");
    command.AppendArg(GetParam() ? "tlcp" : "rfc8998");
    command.AppendArgPath(directory_.GetPath());
    if (RequireClientCertificate()) {
      command.AppendArgPath(client_root);
    }
    base::LaunchOptions launch;
    launch.start_hidden = true;
    peer_ = base::LaunchProcess(command, launch);
    ASSERT_TRUE(peer_.IsValid());
    const auto deadline = base::TimeTicks::Now() + base::Seconds(10);
    std::string ready;
    while (!base::ReadFileToString(directory_.GetPath().AppendASCII("ready"),
                                   &ready)) {
      ASSERT_LT(base::TimeTicks::Now(), deadline);
      ASSERT_TRUE(peer_.IsRunning());
      base::PlatformThread::Sleep(base::Milliseconds(10));
    }
    unsigned int port;
    ASSERT_TRUE(base::StringToUint(
        base::TrimWhitespaceASCII(ready, base::TRIM_ALL), &port));
    ASSERT_GT(port, 0u);
    ASSERT_LE(port, 65535u);
    port_ = static_cast<uint16_t>(port);
    std::string root;
    ASSERT_TRUE(base::ReadFileToString(
        directory_.GetPath().AppendASCII("server-root.der"), &root));
    rule_ = std::make_shared<net::gm::TlsRule>();
    rule_->host = "gm.test";
    rule_->port = port_;
    rule_->profile =
        net::gm::ConnectionProfile(GetParam() ? net::gm::TlsProtocol::kTlcp
                                              : net::gm::TlsProtocol::kRfc8998,
                                   "enterprise", 1, "feitian", 1, 1);
    rule_->trust_anchors = {std::vector<uint8_t>(root.begin(), root.end())};
    ASSERT_TRUE(rule_->IsValid());
  }
  void TearDown() override {
    context_.set_gm_identity_provider(nullptr);
    provider_.reset();
    broker_.reset();
    process_.reset();
    ipc_.reset();
    ipc_thread_.Stop();
    if (peer_.IsValid()) {
      EXPECT_TRUE(
          base::WriteFile(directory_.GetPath().AppendASCII("stop"), "stop"));
      int code = -1;
      if (!peer_.WaitForExitWithTimeout(base::Seconds(5), &code)) {
        peer_.Terminate(1, true);
        ADD_FAILURE() << "TLS peer did not stop";
      } else {
        EXPECT_EQ(code, 0);
      }
    }
  }
  std::vector<uint8_t> Control(uint32_t operation, uint32_t value = 0) {
    base::test::TestFuture<KeyResult, std::vector<uint8_t>> future;
    process_->control()->Control(
        operation, value,
        future.GetCallback<KeyResult, const std::vector<uint8_t>&>());
    EXPECT_EQ(future.Get<0>()->error, KeyError::kOk);
    return std::move(std::get<1>(future.Take()));
  }
  base::UnguessableToken RegisterAuthContext() {
    return provider_->RegisterRequest(base::BindRepeating(
        [](ServiceBroker* broker, const url::Origin& origin,
           const std::string& policy_id, uint64_t generation,
           mojom::GmClientCertificateRequestPtr constraints,
           mojom::GmIdentityBroker::AcquireIdentityCallback callback) {
          const uint64_t id = broker->authorizations + 1;
          broker->AcquireIdentity(id, origin, policy_id, generation,
                                  std::move(constraints), std::move(callback));
          return base::BindOnce(&ServiceBroker::Cancel,
                                base::Unretained(broker), id);
        },
        base::Unretained(broker_.get())));
  }
  std::unique_ptr<net::SSLClientSocket> Connect(
      int* result,
      net::PrivacyMode privacy_mode = net::PRIVACY_MODE_DISABLED) {
    auto transport = std::make_unique<net::TCPClientSocket>(
        net::AddressList(
            net::IPEndPoint(net::IPAddress::IPv4Localhost(), port_)),
        nullptr, nullptr, net::NetLog::Get(), net::NetLogSource(),
        net::handles::kInvalidNetworkHandle);
    net::TestCompletionCallback tcp;
    *result = tcp.GetResult(transport->Connect(tcp.callback()));
    if (*result != net::OK) {
      return nullptr;
    }
    net::SSLConfig config;
    config.alpn_protos = {net::NextProto::kProtoHTTP11};
    config.privacy_mode = privacy_mode;
    config.gm_client_auth_context =
        base::BindRepeating([](base::UnguessableToken token) { return token; },
                            gm_client_auth_context_);
    net::gm::ApplyTlsRule(rule_, &config);
    auto socket = context_.CreateSSLClientSocket(
        std::move(transport), net::HostPortPair("gm.test", port_), config);
    net::TestCompletionCallback handshake;
    *result = handshake.GetResult(socket->Connect(handshake.callback()));
    return socket;
  }
  void Exchange(net::SSLClientSocket* socket, bool close = false) {
    auto request = base::MakeRefCounted<net::StringIOBuffer>(
        "GET / HTTP/1.1\r\nHost: gm.test\r\nConnection: " +
        std::string(close ? "close" : "keep-alive") + "\r\n\r\n");
    auto remaining =
        base::MakeRefCounted<net::DrainableIOBuffer>(request, request->size());
    while (remaining->BytesRemaining()) {
      net::TestCompletionCallback write;
      int result = write.GetResult(
          socket->Write(remaining.get(), remaining->BytesRemaining(),
                        write.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
      ASSERT_GT(result, 0);
      remaining->DidConsume(result);
    }
    auto buffer = base::MakeRefCounted<net::IOBufferWithSize>(4096);
    std::string response;
    while (response.find("</h1>") == std::string::npos &&
           response.size() < 65536) {
      net::TestCompletionCallback read;
      int result = read.GetResult(
          socket->Read(buffer.get(), buffer->size(), read.callback()));
      ASSERT_GT(result, 0);
      response.append(buffer->data(), result);
    }
    EXPECT_NE(response.find("HTTP/1.1 200 OK"), std::string::npos);
    EXPECT_NE(response.find("GM TLS connection verified"), std::string::npos);
    if (close) {
      net::TestCompletionCallback eof;
      EXPECT_EQ(eof.GetResult(
                    socket->Read(buffer.get(), buffer->size(), eof.callback())),
                0);
    }
  }
  scoped_refptr<net::gm::ClientIdentity> Lease() {
    auto identity = provider_->GetCachedIdentity(
        net::HostPortPair("gm.test", port_), rule_->profile);
    EXPECT_TRUE(identity);
    return identity;
  }

  std::unique_ptr<net::URLRequestContext> HttpContext(
      net::gm::ClientIdentityProvider* identity_provider) {
    net::URLRequestContextBuilder builder;
    auto resolver = std::make_unique<net::MockHostResolver>();
    resolver->rules()->AddRule("gm.test", "127.0.0.1");
    builder.set_host_resolver(std::move(resolver));
    builder.set_proxy_config_service(
        std::make_unique<net::ProxyConfigServiceFixed>(
            net::ProxyConfigWithAnnotation::CreateDirect()));
    net::SSLContextConfig ssl;
    ssl.gm_tls_policy = std::make_shared<const net::gm::TlsPolicy>(
        std::vector<net::gm::TlsRule>{*rule_});
    builder.set_ssl_config_service(
        std::make_unique<net::TestSSLConfigService>(ssl));
    auto context = builder.Build();
    context->http_transaction_factory()
        ->GetSession()
        ->ssl_client_context()
        ->set_gm_identity_provider(identity_provider);
    return context;
  }

  void CheckPreconnectAdoption(bool ownerless_reply_already_decided,
                               bool happy_eyeballs = false) {
    base::test::ScopedFeatureList features;
    features.InitWithFeatureState(net::features::kHappyEyeballsV3,
                                  happy_eyeballs);
    GatedIdentityProvider gate(provider_.get());
    auto context = HttpContext(&gate);
    auto* session = context->http_transaction_factory()->GetSession();
    if (happy_eyeballs) {
      session->http_stream_pool()->set_max_stream_sockets_per_group_for_testing(
          1);
      session->http_stream_pool()->set_max_stream_sockets_per_pool_for_testing(
          1);
    }
    const GURL url("https://gm.test:" + base::NumberToString(port_) + "/");
    net::HttpRequestInfo preconnect;
    preconnect.url = url;
    preconnect.traffic_annotation =
        net::MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS);
    base::RunLoop preconnected;
    session->http_stream_factory()->PreconnectStreams(
        1, preconnect, preconnected.QuitClosure());
    ASSERT_TRUE(base::test::RunUntil([&] { return gate.acquire_calls == 1; }));
    EXPECT_EQ(broker_->authorizations, 0);
    if (ownerless_reply_already_decided) {
      gate.Proceed(true);
      ASSERT_TRUE(base::test::RunUntil([&] { return gate.reply_ready; }));
      EXPECT_EQ(broker_->authorizations, 0);
    }

    net::TestDelegate delegate;
    auto request = context->CreateRequest(url, net::DEFAULT_PRIORITY, &delegate,
                                          TRAFFIC_ANNOTATION_FOR_TESTS,
                                          net::handles::kInvalidNetworkHandle);
    request->set_gm_client_auth_context(base::BindRepeating(
        [](base::UnguessableToken context) { return context; },
        gm_client_auth_context_));
    request->Start();
    base::RunLoop().RunUntilIdle();
    EXPECT_EQ(gate.acquire_calls, 1);
    if (ownerless_reply_already_decided) {
      gate.Deliver();
    } else {
      gate.Proceed(false);
    }
    delegate.RunUntilComplete();
    ASSERT_EQ(delegate.request_status(), net::OK);
    EXPECT_NE(delegate.data_received().find("GM TLS connection verified"),
              std::string::npos);
    EXPECT_EQ(broker_->authorizations, 1);
    EXPECT_EQ(gate.acquire_calls, ownerless_reply_already_decided ? 2 : 1);
    EXPECT_TRUE(request->ssl_info().client_cert_sent);
    preconnected.Run();
    session->ssl_client_context()->set_gm_identity_provider(nullptr);
  }

  void CheckStreamPoolOwnerCancellation(bool after_authorization) {
    base::test::ScopedFeatureList features;
    features.InitAndEnableFeature(net::features::kHappyEyeballsV3);
    GatedIdentityProvider gate(provider_.get());
    auto context = HttpContext(&gate);
    auto* session = context->http_transaction_factory()->GetSession();
    session->http_stream_pool()->set_max_stream_sockets_per_group_for_testing(
        1);
    session->http_stream_pool()->set_max_stream_sockets_per_pool_for_testing(1);
    const GURL url("https://gm.test:" + base::NumberToString(port_) + "/");
    net::TestDelegate first_delegate;
    net::TestDelegate second_delegate;
    auto first = context->CreateRequest(
        url, net::DEFAULT_PRIORITY, &first_delegate,
        TRAFFIC_ANNOTATION_FOR_TESTS, net::handles::kInvalidNetworkHandle);
    auto second = context->CreateRequest(
        url, net::DEFAULT_PRIORITY, &second_delegate,
        TRAFFIC_ANNOTATION_FOR_TESTS, net::handles::kInvalidNetworkHandle);
    bool first_alive = true;
    int first_resolutions = 0;
    int second_resolutions = 0;
    const auto second_context = RegisterAuthContext();
    first->set_gm_client_auth_context(base::BindLambdaForTesting([&] {
      ++first_resolutions;
      return first_alive ? gm_client_auth_context_ : base::UnguessableToken();
    }));
    second->set_gm_client_auth_context(base::BindLambdaForTesting([&] {
      ++second_resolutions;
      return second_context;
    }));
    broker_->hold_first_authorization = after_authorization;
    first->Start();
    ASSERT_TRUE(base::test::RunUntil([&] { return gate.acquire_calls == 1; }));
    if (after_authorization) {
      gate.Proceed(false);
      ASSERT_TRUE(
          base::test::RunUntil([&] { return !!broker_->held_authorization; }));
      ASSERT_EQ(first_resolutions, 1);
      ASSERT_EQ(broker_->commits, 0);
    }
    second->Start();
    base::RunLoop().RunUntilIdle();
    ASSERT_EQ(gate.acquire_calls, 1);
    first_alive = false;
    first.reset();
    provider_->UnregisterRequest(gm_client_auth_context_);
    if (after_authorization) {
      // The SDK has returned a valid SM2 identity, but the page was cancelled
      // before it accepted the reply. Its late success must never commit.
      std::move(broker_->held_authorization).Run();
      EXPECT_EQ(broker_->commits, 0);
    } else {
      gate.Proceed(false);
    }
    second_delegate.RunUntilComplete();
    ASSERT_EQ(second_delegate.request_status(), net::OK);
    EXPECT_NE(
        second_delegate.data_received().find("GM TLS connection verified"),
        std::string::npos);
    EXPECT_TRUE(second->ssl_info().client_cert_sent);
    EXPECT_EQ(first_resolutions, after_authorization ? 1 : 0);
    EXPECT_EQ(second_resolutions, 1);
    EXPECT_EQ(gate.acquire_calls, after_authorization ? 2 : 1);
    EXPECT_EQ(broker_->authorizations, after_authorization ? 2 : 1);
    EXPECT_EQ(broker_->commits, 1);
    second.reset();
    provider_->UnregisterRequest(second_context);
    session->ssl_client_context()->set_gm_identity_provider(nullptr);
  }

  base::UnguessableToken gm_client_auth_context_;
  base::test::ScopedRunLoopTimeout timeout_{FROM_HERE, base::Seconds(20)};
  base::Thread ipc_thread_{"GmTlsKeyIpc"};
  std::unique_ptr<mojo::core::ScopedIPCSupport> ipc_;
  std::unique_ptr<gm_key::test::ServiceProcess> process_;
  std::unique_ptr<ServiceBroker> broker_;
  std::unique_ptr<GmIdentityProvider> provider_;
  net::MockCertVerifier verifier_;
  net::TransportSecurityState security_state_;
  net::SSLClientContext context_{nullptr, &verifier_, &security_state_, nullptr,
                                 nullptr};
  base::ScopedTempDir directory_;
  base::FilePath executable_dir_;
  base::Process peer_;
  uint16_t port_ = 0;
  std::shared_ptr<net::gm::TlsRule> rule_;
};

TEST_P(GmIdentityTlsTest,
       LegacyPreconnectAdoptionContinuesSameMutualHandshake) {
  CheckPreconnectAdoption(false);
}

TEST_P(GmIdentityTlsTest, LegacyOwnerlessPreconnectDoesNotFailItsAdoptingPage) {
  CheckPreconnectAdoption(true);
}

TEST_P(GmIdentityTlsTest, StreamPoolOwnerCancelledBeforeCertificateRequest) {
  CheckStreamPoolOwnerCancellation(false);
}

TEST_P(GmIdentityTlsTest, StreamPoolOwnerCancelledDuringAuthorization) {
  CheckStreamPoolOwnerCancellation(true);
}

TEST_P(GmIdentityTlsTest,
       StreamPoolOwnerlessPreconnectDoesNotFailAdoptingPage) {
  CheckPreconnectAdoption(true, true);
}

TEST_P(GmIdentityTlsTest, ProcessIsolatedSkfMutualAuthenticationAndResumption) {
  int result;
  {
    auto socket = Connect(&result);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, net::OK);
    net::SSLInfo info;
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_TRUE(info.client_cert_sent);
    EXPECT_EQ(info.handshake_type, net::SSLInfo::HANDSHAKE_FULL);
    Exchange(socket.get());
    Exchange(socket.get(), true);
  }
  auto resumed = Connect(&result);
  ASSERT_TRUE(resumed);
  ASSERT_EQ(result, net::OK);
  net::SSLInfo info;
  ASSERT_TRUE(resumed->GetSSLInfo(&info));
  EXPECT_EQ(info.handshake_type, net::SSLInfo::HANDSHAKE_RESUME);
  Exchange(resumed.get());
  EXPECT_EQ(broker_->authorizations, 1);
}

TEST_P(GmIdentityTlsTest,
       CredentiallessCannotUseAuthorizedKeyOrResumeMutualTls) {
  int result;
  {
    auto socket = Connect(&result);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, net::OK);
    Exchange(socket.get(), true);
  }
  ASSERT_EQ(broker_->authorizations, 1);
  ASSERT_TRUE(Lease());
  auto forbidden =
      Connect(&result, net::PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS);
  ASSERT_TRUE(forbidden);
  EXPECT_EQ(result, net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(forbidden->IsConnected());
  EXPECT_EQ(broker_->authorizations, 1);
}

TEST_P(GmIdentityTlsTest, RemovingDeviceRevokesConnectedSocket) {
  int result;
  auto socket = Connect(&result);
  ASSERT_TRUE(socket);
  ASSERT_EQ(result, net::OK);
  Exchange(socket.get());
  auto lease = Lease();
  ASSERT_TRUE(lease);
  base::test::TestFuture<void> invalidated;
  auto subscription =
      lease->AddInvalidationCallback(invalidated.GetRepeatingCallback());
  Control(1, 0);
  ASSERT_TRUE(invalidated.Wait());
  EXPECT_FALSE(socket->IsConnected());
  auto buffer = base::MakeRefCounted<net::IOBufferWithSize>(1);
  net::TestCompletionCallback read;
  EXPECT_EQ(read.GetResult(
                socket->Read(buffer.get(), buffer->size(), read.callback())),
            net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
}

TEST_P(GmIdentityTlsTest, DriverCrashDuringSigningFailsHandshake) {
  Control(3, 1);
  int result;
  auto socket = Connect(&result);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(socket->IsConnected());
  int exit_code;
  ASSERT_TRUE(
      process_->process().WaitForExitWithTimeout(base::Seconds(5), &exit_code));
  EXPECT_NE(exit_code, 0);
}

TEST_P(GmIdentityTlsTest, IncorrectPinNeverFallsBackToAnonymousTls) {
  broker_->valid_pin = false;
  int result;
  auto socket = Connect(&result);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, net::ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(socket->IsConnected());
  EXPECT_EQ(broker_->authorizations, 1);
}

INSTANTIATE_TEST_SUITE_P(TlcpAndRfc8998, GmIdentityTlsTest, testing::Bool());

class GmIdentityOneWayPeerTest : public GmIdentityTlsTest {
 protected:
  bool RequireClientCertificate() const override { return false; }
};

TEST_P(GmIdentityOneWayPeerTest,
       RequiredKeyRejectsServerWithoutMutualAuthentication) {
  int result;
  auto socket = Connect(&result);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, net::ERR_BAD_SSL_CLIENT_AUTH_CERT);
  EXPECT_FALSE(socket->IsConnected());
  EXPECT_EQ(broker_->authorizations, 0);
}

INSTANTIATE_TEST_SUITE_P(TlcpAndRfc8998,
                         GmIdentityOneWayPeerTest,
                         testing::Bool());
}  // namespace
}  // namespace network
