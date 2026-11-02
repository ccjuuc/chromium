// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/path_service.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/run_loop.h"
#include "base/scoped_observation.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/execution_fence.h"
#include "base/task/single_thread_task_runner.h"
#include "base/test/bind.h"
#include "base/test/run_until.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/threading/platform_thread.h"
#include "net/base/address_list.h"
#include "net/base/features.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/isolation_info.h"
#include "net/base/load_timing_info.h"
#include "net/base/network_anonymization_key.h"
#include "net/base/network_handle.h"
#include "net/base/test_completion_callback.h"
#include "net/cert/mock_cert_verifier.h"
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"
#include "net/dns/mock_host_resolver.h"
#include "net/gm/gm_client_context.h"
#include "net/gm/gm_tls_policy.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_request_info.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_stream_factory.h"
#include "net/http/http_transaction_factory.h"
#include "net/http/transport_security_state.h"
#include "net/log/net_log.h"
#include "net/proxy_resolution/proxy_config_service_fixed.h"
#include "net/socket/gm_ssl_client_socket.h"
#include "net/socket/socket_test_util.h"
#include "net/socket/ssl_client_socket.h"
#include "net/socket/tcp_client_socket.h"
#include "net/ssl/ssl_client_session_cache.h"
#include "net/ssl/ssl_config.h"
#include "net/ssl/ssl_connection_status_flags.h"
#include "net/ssl/ssl_info.h"
#include "net/ssl/ssl_private_key.h"
#include "net/ssl/ssl_server_config.h"
#include "net/ssl/test_ssl_config_service.h"
#include "net/storage_access_api/status.h"
#include "net/test/cert_test_util.h"
#include "net/test/embedded_test_server/embedded_test_server.h"
#include "net/test/embedded_test_server/http_response.h"
#include "net/test/embedded_test_server/install_default_websocket_handlers.h"
#include "net/test/key_util.h"
#include "net/test/test_data_directory.h"
#include "net/test/test_with_task_environment.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "net/url_request/url_request.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_builder.h"
#include "net/url_request/url_request_test_util.h"
#include "net/websockets/websocket_frame.h"
#include "net/websockets/websocket_handshake_request_info.h"
#include "net/websockets/websocket_handshake_response_info.h"
#include "net/websockets/websocket_stream.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/include/openssl/bytestring.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/sm2.h"
#include "third_party/boringssl/src/include/openssl/ssl.h"

namespace net::gm {
namespace {

struct WebSocketResult {
  base::RunLoop completed;
  int error = ERR_IO_PENDING;
  SSLInfo ssl_info;
  std::unique_ptr<WebSocketStream> stream;
};

class WebSocketDelegate : public WebSocketStream::ConnectDelegate {
 public:
  explicit WebSocketDelegate(WebSocketResult* result) : result_(result) {}
  void OnCreateRequest(URLRequest* request) override { request_ = request; }
  int OnURLRequestConnected(URLRequest*,
                            const TransportInfo&,
                            CompletionOnceCallback) override {
    return OK;
  }
  void OnSuccess(std::unique_ptr<WebSocketStream> stream,
                 std::unique_ptr<WebSocketHandshakeResponseInfo>) override {
    result_->error = OK;
    result_->ssl_info = request_->ssl_info();
    result_->stream = std::move(stream);
    result_->completed.Quit();
  }
  void OnFailure(const std::string&, int error, std::optional<int>) override {
    result_->error = error;
    result_->completed.Quit();
  }
  void OnStartOpeningHandshake(
      std::unique_ptr<WebSocketHandshakeRequestInfo>) override {}
  void OnSSLCertificateError(
      std::unique_ptr<WebSocketEventInterface::SSLErrorCallbacks> callbacks,
      int error,
      const SSLInfo& info,
      bool) override {
    callbacks->CancelSSLRequest(error, &info);
  }
  int OnAuthRequired(const AuthChallengeInfo&,
                     scoped_refptr<HttpResponseHeaders>,
                     const IPEndPoint&,
                     base::OnceCallback<void(const AuthCredentials*)>,
                     std::optional<AuthCredentials>*) override {
    return ERR_ACCESS_DENIED;
  }

 private:
  raw_ptr<WebSocketResult> result_;
  raw_ptr<URLRequest> request_ = nullptr;
};

class PendingIdentityProvider : public ClientIdentityProvider {
 public:
  base::OnceClosure AcquireIdentity(const HostPortPair&,
                                    const ConnectionProfile&,
                                    const ClientCertificateRequest& request,
                                    AcquireCallback callback) override {
    last_request = request;
    if (last_request.gm_client_auth_context_resolver) {
      last_request.gm_client_auth_context =
          last_request.gm_client_auth_context_resolver.Run();
    }
    pending = std::move(callback);
    return base::BindOnce(
        [](PendingIdentityProvider* self) {
          self->cancelled = true;
          self->pending.Reset();
        },
        base::Unretained(this));
  }
  bool cancelled = false;
  ClientCertificateRequest last_request;
  AcquireCallback pending;
};

// A live authorization is deliberately available. Credentialless connections
// must not inspect it, request another authorization, or invoke its signer.
class ForbiddenIdentity final : public ClientIdentity {
 public:
  ForbiddenIdentity() : ClientIdentity("already-authorized", 1, {}, {}, {}) {}
  void SignDigest(uint64_t,
                  const std::array<uint8_t, 32>&,
                  SignCallback callback) override {
    ++sign_calls;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED,
                                  std::vector<uint8_t>()));
  }
  void Cancel(uint64_t) override {}
  int sign_calls = 0;

 private:
  ~ForbiddenIdentity() override = default;
};

class ForbiddenIdentityProvider : public ClientIdentityProvider {
 public:
  scoped_refptr<ClientIdentity> GetCachedIdentity(
      const HostPortPair&,
      const ConnectionProfile&) override {
    ++cache_lookups;
    return cached;
  }
  base::OnceClosure AcquireIdentity(const HostPortPair&,
                                    const ConnectionProfile&,
                                    const ClientCertificateRequest&,
                                    AcquireCallback callback) override {
    ++acquire_calls;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback),
                                  ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED,
                                  scoped_refptr<ClientIdentity>()));
    return {};
  }
  void ExpectUnused() const {
    EXPECT_EQ(cache_lookups, 0);
    EXPECT_EQ(acquire_calls, 0);
    EXPECT_EQ(cached->sign_calls, 0);
  }
  scoped_refptr<ForbiddenIdentity> cached =
      base::MakeRefCounted<ForbiddenIdentity>();
  int cache_lookups = 0;
  int acquire_calls = 0;
};

// Uses the independent peer's generated client credential. Only this test
// signer holds a software private key; the production provider stays external.
class SoftwareIdentity final : public ClientIdentity {
 public:
  SoftwareIdentity(std::string id,
                   uint64_t generation,
                   std::vector<uint8_t> certificate,
                   bssl::UniquePtr<EVP_PKEY> key)
      : ClientIdentity(std::move(id),
                       generation,
                       std::move(certificate),
                       {},
                       {}),
        key_(std::move(key)) {}

  void SignDigest(uint64_t request_id,
                  const std::array<uint8_t, 32>& digest,
                  SignCallback callback) override {
    ++sign_calls;
    if (hold_signatures) {
      pending_request = request_id;
      pending = std::move(callback);
      return;
    }
    std::vector<uint8_t> signature(EVP_PKEY_size(key_.get()));
    size_t size = 0;
    int result =
        SM2_sign_digest(digest.data(), signature.data(), &size,
                        signature.size(), EVP_PKEY_get0_EC_KEY(key_.get()))
            ? OK
            : ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED;
    signature.resize(size);
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(callback), result, std::move(signature)));
  }
  void Cancel(uint64_t request_id) override {
    EXPECT_EQ(request_id, pending_request);
    ++cancel_calls;
    pending.Reset();
  }

  int sign_calls = 0;
  int cancel_calls = 0;
  bool hold_signatures = false;
  uint64_t pending_request = 0;
  SignCallback pending;

 private:
  ~SoftwareIdentity() override = default;
  bssl::UniquePtr<EVP_PKEY> key_;
};

class SoftwareIdentityProvider final : public ClientIdentityProvider {
 public:
  explicit SoftwareIdentityProvider(scoped_refptr<SoftwareIdentity> identity)
      : identity(std::move(identity)) {}
  scoped_refptr<ClientIdentity> GetCachedIdentity(
      const HostPortPair&,
      const ConnectionProfile&) override {
    return authorized && identity->is_valid() ? identity : nullptr;
  }
  base::OnceClosure AcquireIdentity(const HostPortPair&,
                                    const ConnectionProfile&,
                                    const ClientCertificateRequest& request,
                                    AcquireCallback callback) override {
    ++acquire_calls;
    last_request = request;
    authorized = acquisition_result == OK;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(std::move(callback), acquisition_result,
                                  authorized ? identity : nullptr));
    return {};
  }

  scoped_refptr<SoftwareIdentity> identity;
  bool authorized = false;
  int acquisition_result = OK;
  int acquire_calls = 0;
  ClientCertificateRequest last_request;
};

class VerificationHookCertVerifier : public ParamRecordingMockCertVerifier {
 public:
  int Verify(const RequestParams& params,
             CertVerifyResult* result,
             CompletionOnceCallback callback,
             std::unique_ptr<Request>* request,
             const NetLogWithSource& net_log) override {
    const int error = ParamRecordingMockCertVerifier::Verify(
        params, result, std::move(callback), request, net_log);
    if (during_async_verify) {
      EXPECT_EQ(error, ERR_IO_PENDING);
      std::move(during_async_verify).Run();
    }
    return error;
  }
  base::OnceClosure during_async_verify;
};

class GmSocketTcpTest : public TestWithTaskEnvironment,
                        public testing::WithParamInterface<bool> {
 protected:
  virtual const char* PeerProtocol() const {
    return GetParam() ? "tlcp" : "rfc8998";
  }
  virtual bool RequireClientCertificate() const { return false; }
  void SetUp() override {
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    base::FilePath executable_dir;
    ASSERT_TRUE(base::PathService::Get(base::DIR_EXE, &executable_dir));
    peer_executable_ = executable_dir.AppendASCII("gm_interop_peer")
                           .AppendASCII("gm_backend_smoke.exe");
    ASSERT_TRUE(base::PathExists(peer_executable_));
    base::CommandLine command(peer_executable_);
    command.AppendArg("--serve");
    command.AppendArg("0");
    command.AppendArg(PeerProtocol());
    command.AppendArgPath(directory_.GetPath());
    if (RequireClientCertificate()) {
      command.AppendArgPath(
          directory_.GetPath().AppendASCII("server-root.der"));
    }
    base::LaunchOptions options;
    options.start_hidden = true;
    peer_ = base::LaunchProcess(command, options);
    ASSERT_TRUE(peer_.IsValid());
    const auto deadline = base::TimeTicks::Now() + base::Seconds(10);
    std::string ready;
    while (!base::ReadFileToString(directory_.GetPath().AppendASCII("ready"),
                                   &ready)) {
      ASSERT_LT(base::TimeTicks::Now(), deadline)
          << "Peer did not become ready";
      ASSERT_TRUE(peer_.IsRunning());
      base::PlatformThread::Sleep(base::Milliseconds(10));
    }
    unsigned int port = 0;
    ASSERT_TRUE(base::StringToUint(
        base::TrimWhitespaceASCII(ready, base::TRIM_ALL), &port));
    ASSERT_GT(port, 0u);
    ASSERT_LE(port, 65535u);
    port_ = static_cast<uint16_t>(port);
    rule_ = std::make_shared<TlsRule>();
    rule_->host = "gm.test";
    rule_->port = port_;
    rule_->profile = ConnectionProfile(
        GetParam() ? TlsProtocol::kTlcp : TlsProtocol::kRfc8998,
        "test-enterprise", 1, "", 0, 1);
    rule_->trust_anchors = {
        ReadDer(directory_.GetPath().AppendASCII("server-root.der"))};
    ASSERT_TRUE(rule_->IsValid());
  }
  void TearDown() override {
    if (peer_.IsValid()) {
      EXPECT_TRUE(
          base::WriteFile(directory_.GetPath().AppendASCII("stop"), "stop"));
      int exit_code = -1;
      if (!peer_.WaitForExitWithTimeout(base::Seconds(5), &exit_code)) {
        peer_.Terminate(1, true);
        ADD_FAILURE() << "Peer did not stop after sockets were released";
      } else {
        EXPECT_EQ(exit_code, 0);
      }
    }
  }
  std::vector<uint8_t> ReadDer(const base::FilePath& path) {
    std::string bytes;
    EXPECT_TRUE(base::ReadFileToString(path, &bytes));
    return {bytes.begin(), bytes.end()};
  }
  SSLConfig Config() const {
    SSLConfig config;
    config.alpn_protos = {NextProto::kProtoHTTP11};
    ApplyTlsRule(rule_, &config);
    return config;
  }
  std::unique_ptr<URLRequestContext> HttpContext(
      std::string proxy = {},
      std::vector<TlsRule> additional_rules = {}) {
    URLRequestContextBuilder builder;
    auto resolver = std::make_unique<MockHostResolver>();
    resolver->rules()->AddRule("*", "127.0.0.1");
    builder.set_host_resolver(std::move(resolver));
    ProxyConfig proxy_config;
    if (!proxy.empty()) {
      proxy_config.proxy_rules().ParseFromString(proxy);
    }
    builder.set_proxy_config_service(std::make_unique<ProxyConfigServiceFixed>(
        ProxyConfigWithAnnotation(proxy_config, TRAFFIC_ANNOTATION_FOR_TESTS)));
    // Ordinary local TLS fixtures use Chromium's verifier. National TLS uses
    // only the rule's real SM2 trust anchors and never this test verifier.
    auto verifier = std::make_unique<MockCertVerifier>();
    verifier->set_default_result(OK);
    builder.SetCertVerifier(std::move(verifier));
    additional_rules.push_back(*rule_);
    SSLContextConfig ssl;
    ssl.gm_tls_policy =
        std::make_shared<const TlsPolicy>(std::move(additional_rules));
    builder.set_ssl_config_service(std::make_unique<TestSSLConfigService>(ssl));
    return builder.Build();
  }
  GURL PeerUrl(std::string path = "/") const {
    return GURL("https://gm.test:" + base::NumberToString(port_) + path);
  }
  std::unique_ptr<WebSocketStreamRequest> WebSocket(URLRequestContext* context,
                                                    const GURL& url,
                                                    WebSocketResult* result) {
    return WebSocketStream::CreateAndConnectStream(
        url, {}, url::Origin::Create(PeerUrl()), StorageAccessApiStatus::kNone,
        IsolationInfo(), HttpRequestHeaders(), context, NetLogWithSource(),
        WebSocketPriorityHint::kDefault, TRAFFIC_ANNOTATION_FOR_TESTS,
        std::make_unique<WebSocketDelegate>(result));
  }
  void FetchThroughProxy(EmbeddedTestServer::Type type) {
    EmbeddedTestServer proxy(type);
    const std::array destinations{HostPortPair("gm.test", port_)};
    proxy.EnableConnectProxy(destinations);
    ASSERT_TRUE(proxy.Start());
    auto context = HttpContext(
        (type == EmbeddedTestServer::TYPE_HTTPS ? "https://" : "http://") +
        proxy.host_port_pair().ToString());
    TestDelegate delegate;
    auto request = context->CreateRequest(
        PeerUrl(), DEFAULT_PRIORITY, &delegate, TRAFFIC_ANNOTATION_FOR_TESTS,
        handles::kInvalidNetworkHandle);
    request->Start();
    delegate.RunUntilComplete();
    ASSERT_EQ(delegate.request_status(), OK);
    EXPECT_NE(delegate.data_received().find("GM TLS connection verified"),
              std::string::npos);
    EXPECT_EQ(request->ssl_info().cert_status, 0u);
    EXPECT_EQ(
        SSLConnectionStatusToCipherSuite(request->ssl_info().connection_status),
        GetParam() ? 0xe053 : 0x00c6);
  }
  std::unique_ptr<SSLClientSocket> CreateSocket(
      int* result,
      SSLConfig config,
      const char* hostname = "gm.test",
      ClientContext* gm_context = nullptr,
      ClientIdentityProvider* identity_provider = nullptr,
      uint16_t port = 0) {
    if (!port) {
      port = port_;
    }
    auto transport = std::make_unique<TCPClientSocket>(
        AddressList(IPEndPoint(IPAddress::IPv4Localhost(), port)), nullptr,
        nullptr, NetLog::Get(), NetLogSource(), handles::kInvalidNetworkHandle);
    TestCompletionCallback tcp;
    *result = tcp.GetResult(transport->Connect(tcp.callback()));
    if (*result != OK) {
      return nullptr;
    }
    std::unique_ptr<SSLClientSocket> socket;
    if (gm_context) {
      socket = std::make_unique<GmSSLClientSocket>(
          &context_, gm_context, identity_provider, std::move(transport),
          HostPortPair(hostname, port), config);
    } else {
      socket = context_.CreateSSLClientSocket(
          std::move(transport), HostPortPair(hostname, port), config);
    }
    return socket;
  }
  std::unique_ptr<SSLClientSocket> Connect(
      int* result,
      SSLConfig config,
      const char* hostname = "gm.test",
      ClientContext* gm_context = nullptr,
      ClientIdentityProvider* identity_provider = nullptr,
      uint16_t port = 0) {
    auto socket = CreateSocket(result, std::move(config), hostname, gm_context,
                               identity_provider, port);
    if (!socket) {
      return nullptr;
    }
    TestCompletionCallback handshake;
    *result = handshake.GetResult(socket->Connect(handshake.callback()));
    return socket;
  }
  std::string Exchange(SSLClientSocket* socket, bool close = false) {
    const std::string request =
        "GET / HTTP/1.1\r\nHost: gm.test\r\nConnection: " +
        std::string(close ? "close" : "keep-alive") + "\r\n\r\n";
    auto bytes = base::MakeRefCounted<StringIOBuffer>(request);
    auto outgoing =
        base::MakeRefCounted<DrainableIOBuffer>(bytes, bytes->size());
    while (outgoing->BytesRemaining()) {
      TestCompletionCallback write;
      const int result = write.GetResult(
          socket->Write(outgoing.get(), outgoing->BytesRemaining(),
                        write.callback(), TRAFFIC_ANNOTATION_FOR_TESTS));
      EXPECT_GT(result, 0);
      if (result <= 0) {
        return {};
      }
      outgoing->DidConsume(result);
    }
    std::string response;
    auto incoming = base::MakeRefCounted<IOBufferWithSize>(4096);
    while (response.size() < 64 * 1024) {
      TestCompletionCallback read;
      int result = read.GetResult(
          socket->Read(incoming.get(), incoming->size(), read.callback()));
      EXPECT_GT(result, 0);
      if (result <= 0) {
        return response;
      }
      response.append(incoming->data(), result);
      const auto headers_end = response.find("\r\n\r\n");
      const auto length_start = response.find("Content-Length:");
      if (headers_end != std::string::npos &&
          length_start != std::string::npos) {
        const auto value_start =
            length_start + std::string_view("Content-Length:").size();
        const auto value_end = response.find("\r\n", value_start);
        size_t length = 0;
        EXPECT_TRUE(base::StringToSizeT(
            base::TrimWhitespaceASCII(std::string_view(response).substr(
                                          value_start, value_end - value_start),
                                      base::TRIM_ALL),
            &length));
        if (response.size() >= headers_end + 4 + length) {
          return response;
        }
      }
    }
    ADD_FAILURE() << "Unbounded test response";
    return response;
  }

  base::test::ScopedRunLoopTimeout timeout_{FROM_HERE, base::Seconds(20)};
  VerificationHookCertVerifier verifier_;
  TransportSecurityState security_state_;
  SSLClientSessionCache session_cache_{SSLClientSessionCache::Config()};
  TestSSLConfigService config_service_{SSLContextConfig()};
  SSLClientContext context_{&config_service_, &verifier_, &security_state_,
                            &session_cache_, nullptr};
  base::ScopedTempDir directory_;
  base::FilePath peer_executable_;
  base::Process peer_;
  uint16_t port_ = 0;
  std::shared_ptr<TlsRule> rule_;
};

TEST_P(GmSocketTcpTest, AllDisabledCipherSuitesRejectBeforeIdentityLookup) {
  SSLContextConfig settings;
  settings.disabled_cipher_suites = {0xe053, 0xe013, 0x00c6, 0x00c7};
  config_service_.UpdateSSLConfigAndNotify(settings);
  ClientContext gm_context;
  ForbiddenIdentityProvider provider;
  int result = ERR_FAILED;
  auto socket = Connect(&result, Config(), "gm.test", &gm_context, &provider);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, ERR_SSL_VERSION_OR_CIPHER_MISMATCH);
  EXPECT_FALSE(socket->IsConnected());
  provider.ExpectUnused();
}

TEST_P(GmSocketTcpTest, DisabledAlternateCipherPreservesAllowedCipher) {
  SSLContextConfig settings;
  settings.disabled_cipher_suites = {0xe013, 0x00c7};
  config_service_.UpdateSSLConfigAndNotify(settings);
  int result = ERR_FAILED;
  auto socket = Connect(&result, Config());
  ASSERT_TRUE(socket);
  ASSERT_EQ(result, OK);
  SSLInfo info;
  ASSERT_TRUE(socket->GetSSLInfo(&info));
  EXPECT_EQ(SSLConnectionStatusToCipherSuite(info.connection_status),
            GetParam() ? 0xe053 : 0x00c6);
  EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
            std::string::npos);
}

TEST_P(GmSocketTcpTest, DisabledCipherRejectsPreviouslyCachedSession) {
  // Keep the explicit cache alive across SSLConfig notifications to exercise
  // ticket import validation independently of the usual cache invalidation.
  ClientContext gm_context;
  const auto config = Config();
  const auto key = ClientContext::MakeSessionKey(
      HostPortPair("gm.test", port_), config, nullptr,
      SSLClientSocket::SerializeNextProtos(config.alpn_protos));
  int result = ERR_FAILED;
  {
    auto socket = Connect(&result, config, "gm.test", &gm_context);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    ASSERT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return !gm_context.GetSession(key).blob.empty(); }));
  }
  SSLContextConfig settings;
  // The independent peer only enables GCM. CBC/CCM remain permitted locally,
  // but neither resumption nor a new handshake may use the disabled GCM suite.
  settings.disabled_cipher_suites = {0xe053, 0x00c6};
  config_service_.UpdateSSLConfigAndNotify(settings);
  auto socket = Connect(&result, config, "gm.test", &gm_context);
  ASSERT_TRUE(socket);
  EXPECT_LT(result, OK);
  EXPECT_FALSE(socket->IsConnected());
  EXPECT_TRUE(gm_context.GetSession(key).blob.empty());
}

TEST_P(GmSocketTcpTest, RequiredEcdheRejectsStaticTlcp) {
  auto config = Config();
  config.require_ecdhe = true;
  int result = ERR_FAILED;
  auto socket = Connect(&result, config);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, GetParam() ? ERR_SSL_VERSION_OR_CIPHER_MISMATCH : OK);
  EXPECT_EQ(socket->IsConnected(), !GetParam());
}

class GmRfc8998PolicyTcpTest : public GmSocketTcpTest {};

TEST_P(GmRfc8998PolicyTcpTest, VersionBoundsAndPerConnectionOverrides) {
  SSLContextConfig settings;
  settings.version_max = SSL_PROTOCOL_VERSION_TLS1_2;
  config_service_.UpdateSSLConfigAndNotify(settings);
  int result = ERR_FAILED;
  {
    auto socket = Connect(&result, Config());
    ASSERT_TRUE(socket);
    EXPECT_EQ(result, ERR_SSL_VERSION_OR_CIPHER_MISMATCH);
  }
  {
    auto config = Config();
    config.version_max_override = SSL_PROTOCOL_VERSION_TLS1_3;
    auto socket = Connect(&result, config);
    ASSERT_TRUE(socket);
    EXPECT_EQ(result, OK);
  }
  settings.version_max = SSL_PROTOCOL_VERSION_TLS1_3;
  config_service_.UpdateSSLConfigAndNotify(settings);
  {
    auto config = Config();
    config.version_max_override = SSL_PROTOCOL_VERSION_TLS1_2;
    auto socket = Connect(&result, config);
    ASSERT_TRUE(socket);
    EXPECT_EQ(result, ERR_SSL_VERSION_OR_CIPHER_MISMATCH);
  }
  {
    auto config = Config();
    config.version_min_override = SSL_PROTOCOL_VERSION_TLS1_3 + 1;
    auto socket = Connect(&result, config);
    ASSERT_TRUE(socket);
    EXPECT_EQ(result, ERR_SSL_VERSION_OR_CIPHER_MISMATCH);
  }
}

TEST_P(GmRfc8998PolicyTcpTest, VersionLimitRejectsPreviouslyCachedSession) {
  ClientContext gm_context;
  auto config = Config();
  const auto key = ClientContext::MakeSessionKey(
      HostPortPair("gm.test", port_), config, nullptr,
      SSLClientSocket::SerializeNextProtos(config.alpn_protos));
  int result = ERR_FAILED;
  {
    auto socket = Connect(&result, config, "gm.test", &gm_context);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    ASSERT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return !gm_context.GetSession(key).blob.empty(); }));
  }
  config.version_max_override = SSL_PROTOCOL_VERSION_TLS1_2;
  auto socket = Connect(&result, config, "gm.test", &gm_context);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, ERR_SSL_VERSION_OR_CIPHER_MISMATCH);
  EXPECT_FALSE(socket->IsConnected());
}

INSTANTIATE_TEST_SUITE_P(Rfc8998,
                         GmRfc8998PolicyTcpTest,
                         testing::Values(false));

TEST_P(GmSocketTcpTest, FactoryHandshakeKeepAliveAndAuthenticatedClose) {
  int result = ERR_FAILED;
  auto socket = Connect(&result, Config());
  ASSERT_TRUE(socket);
  ASSERT_EQ(result, OK);
  EXPECT_TRUE(socket->IsConnected());
  EXPECT_EQ(socket->GetNegotiatedProtocol(), NextProto::kProtoHTTP11);
  SSLInfo info;
  ASSERT_TRUE(socket->GetSSLInfo(&info));
  EXPECT_EQ(info.cert_status, 0u);
  EXPECT_EQ(info.handshake_type, SSLInfo::HANDSHAKE_FULL);
  EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
            std::string::npos);
  EXPECT_NE(Exchange(socket.get(), true).find("GM TLS connection verified"),
            std::string::npos);
  auto buffer = base::MakeRefCounted<IOBufferWithSize>(1);
  TestCompletionCallback read;
  EXPECT_EQ(read.GetResult(
                socket->Read(buffer.get(), buffer->size(), read.callback())),
            0);
  EXPECT_FALSE(socket->IsConnected());
}

TEST_P(GmSocketTcpTest, ResumptionRespectsNetworkPartitionAndPolicyRevision) {
  ClientContext gm_context;
  const auto config = Config();
  const auto key = ClientContext::MakeSessionKey(
      HostPortPair("gm.test", port_), config, nullptr,
      SSLClientSocket::SerializeNextProtos(config.alpn_protos));
  int result = ERR_FAILED;
  SSLInfo info;
  {
    auto socket = Connect(&result, config, "gm.test", &gm_context);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_EQ(info.handshake_type, SSLInfo::HANDSHAKE_FULL);
    // TLS 1.3 tickets may arrive after application data. The socket must pump
    // them while idle; wait for the real event rather than timing the network.
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return !gm_context.GetSession(key).blob.empty(); }));
  }
  {
    auto socket = Connect(&result, config, "gm.test", &gm_context);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_EQ(info.handshake_type, SSLInfo::HANDSHAKE_RESUME);
    EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
  }
  {
    auto partitioned = Config();
    partitioned.network_anonymization_key =
        NetworkAnonymizationKey::CreateTransient();
    auto socket = Connect(&result, partitioned, "gm.test", &gm_context);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_EQ(info.handshake_type, SSLInfo::HANDSHAKE_FULL);
  }
  {
    auto changed = std::make_shared<TlsRule>(*rule_);
    changed->profile = ConnectionProfile(rule_->profile.protocol(),
                                         "test-enterprise", 1, "", 0, 2);
    auto revised = Config();
    ApplyTlsRule(changed, &revised);
    auto socket = Connect(&result, revised, "gm.test", &gm_context);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_EQ(info.handshake_type, SSLInfo::HANDSHAKE_FULL);
  }
}

TEST_P(GmSocketTcpTest, RejectsWrongHostnameWithoutCertificateExceptions) {
  int result = ERR_FAILED;
  auto config = Config();
  config.ignore_certificate_errors = true;
  auto socket = Connect(&result, config, "other.example");
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, ERR_CERT_COMMON_NAME_INVALID);
  EXPECT_FALSE(socket->IsConnected());
  SSLInfo info;
  ASSERT_TRUE(socket->GetSSLInfo(&info));
  EXPECT_TRUE(info.cert);
  EXPECT_TRUE(info.is_fatal_cert_error);
}

TEST_P(GmSocketTcpTest, UntrustedSm2ChainRetainsFatalCertificateDiagnostics) {
  auto unrelated =
      ImportCertFromFile(GetTestCertsDirectory(), "root_ca_cert.pem");
  ASSERT_TRUE(unrelated);
  const auto der = x509_util::CryptoBufferAsSpan(unrelated->cert_buffer());
  auto wrong_trust = std::make_shared<TlsRule>(*rule_);
  wrong_trust->trust_anchors = {std::vector<uint8_t>(der.begin(), der.end())};
  auto config = Config();
  ApplyTlsRule(wrong_trust, &config);
  int result;
  auto socket = Connect(&result, config);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, ERR_CERT_AUTHORITY_INVALID);
  SSLInfo info;
  ASSERT_TRUE(socket->GetSSLInfo(&info));
  EXPECT_TRUE(info.cert);
  EXPECT_TRUE(info.is_fatal_cert_error);
  EXPECT_TRUE(info.cert_status & CERT_STATUS_AUTHORITY_INVALID);
}

TEST_P(GmSocketTcpTest, EffectiveAlpnOfferPartitionsSessions) {
  ClientContext gm_context;
  auto offered = Config();
  offered.alpn_protos = {NextProto::kProtoHTTP2, NextProto::kProtoHTTP11};
  const auto effective_alpn = SSLClientSocket::SerializeNextProtos(
      GetParam() ? Config().alpn_protos : offered.alpn_protos);
  const auto key = ClientContext::MakeSessionKey(
      HostPortPair("gm.test", port_), offered, nullptr, effective_alpn);
  int result;
  {
    auto socket = Connect(&result, offered, "gm.test", &gm_context);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return !gm_context.GetSession(key).blob.empty(); }));
  }
  auto changed = Connect(&result, Config(), "gm.test", &gm_context);
  ASSERT_TRUE(changed);
  ASSERT_EQ(result, OK);
  SSLInfo info;
  ASSERT_TRUE(changed->GetSSLInfo(&info));
  // TLCP filters both offers to HTTP/1.1, so its effective offer is unchanged.
  EXPECT_EQ(info.handshake_type,
            GetParam() ? SSLInfo::HANDSHAKE_RESUME : SSLInfo::HANDSHAKE_FULL);
  EXPECT_NE(Exchange(changed.get()).find("GM TLS connection verified"),
            std::string::npos);
}

TEST_P(GmSocketTcpTest,
       InvalidCachedSessionRetriesFullHandshakeInSameProtocol) {
  ClientContext gm_context;
  const auto config = Config();
  const auto alpn = SSLClientSocket::SerializeNextProtos(config.alpn_protos);
  const auto key = ClientContext::MakeSessionKey(HostPortPair("gm.test", port_),
                                                 config, nullptr, alpn);
  const auto initial = gm_context.GetSession(key);
  gm_context.StoreSession(key, initial.binding, {0x30, 0x01, 0xff});
  int result;
  {
    auto socket = Connect(&result, config, "gm.test", &gm_context);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    SSLInfo info;
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_EQ(info.handshake_type, SSLInfo::HANDSHAKE_FULL);
    EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return !gm_context.GetSession(key).blob.empty(); }));
  }
  auto resumed = Connect(&result, config, "gm.test", &gm_context);
  ASSERT_TRUE(resumed);
  ASSERT_EQ(result, OK);
  SSLInfo info;
  ASSERT_TRUE(resumed->GetSSLInfo(&info));
  EXPECT_EQ(info.handshake_type, SSLInfo::HANDSHAKE_RESUME);
}

TEST_P(GmSocketTcpTest, MissingRequiredKeyFailsWithoutAnonymousFallback) {
  auto identity_rule = std::make_shared<TlsRule>(*rule_);
  identity_rule->profile = ConnectionProfile(
      rule_->profile.protocol(), "test-enterprise", 1, "feitian", 1, 1);
  auto config = Config();
  ApplyTlsRule(identity_rule, &config);
  int result = ERR_FAILED;
  auto socket = Connect(&result, config);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, ERR_BAD_SSL_CLIENT_AUTH_CERT);
  EXPECT_FALSE(socket->IsConnected());
}

TEST_P(GmSocketTcpTest,
       CredentiallessRequiredIdentityFailsBeforeAccessingAuthorization) {
  ForbiddenIdentityProvider provider;
  ClientContext gm_context;
  auto identity_rule = std::make_shared<TlsRule>(*rule_);
  identity_rule->profile = ConnectionProfile(
      rule_->profile.protocol(), "test-enterprise", 1, "feitian", 1, 1);
  auto config = Config();
  ApplyTlsRule(identity_rule, &config);
  config.privacy_mode = PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS;
  int result = ERR_FAILED;
  auto socket = Connect(&result, config, "gm.test", &gm_context, &provider);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_FALSE(socket->IsConnected());
  provider.ExpectUnused();
}

TEST_P(GmSocketTcpTest,
       CredentiallessResumptionNeverConsultsExistingIdentityAuthorization) {
  ForbiddenIdentityProvider provider;
  ClientContext gm_context;
  auto config = Config();
  config.privacy_mode = PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS;
  const auto key = ClientContext::MakeSessionKey(
      HostPortPair("gm.test", port_), config, nullptr,
      SSLClientSocket::SerializeNextProtos(config.alpn_protos));
  for (int attempt = 0; attempt < 2; ++attempt) {
    int result = ERR_FAILED;
    auto socket = Connect(&result, config, "gm.test", &gm_context, &provider);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    SSLInfo info;
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_FALSE(info.client_cert_sent);
    EXPECT_EQ(info.handshake_type, attempt == 0 ? SSLInfo::HANDSHAKE_FULL
                                                : SSLInfo::HANDSHAKE_RESUME);
    EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
    ASSERT_TRUE(base::test::RunUntil(
        [&] { return !gm_context.GetSession(key).blob.empty(); }));
    provider.ExpectUnused();
  }
}

TEST_P(GmSocketTcpTest, ServerOnlyAuthenticationDoesNotRequestIdentity) {
  PendingIdentityProvider provider;
  context_.set_gm_identity_provider(&provider);
  int result = ERR_FAILED;
  auto socket = Connect(&result, Config());
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, OK);
  EXPECT_FALSE(provider.pending);
  EXPECT_EQ(provider.last_request.request_id, 0u);
  context_.set_gm_identity_provider(nullptr);
}

class GmSocketMutualTcpTest : public GmSocketTcpTest {
 protected:
  bool RequireClientCertificate() const override { return true; }

  void CheckRequestContextsWithPool(bool happy_eyeballs) {
    base::test::ScopedFeatureList features;
    features.InitWithFeatureState(net::features::kHappyEyeballsV3,
                                  happy_eyeballs);
    PendingIdentityProvider provider;
    auto context = HttpContext();
    auto* session = context->http_transaction_factory()->GetSession();
    session->ssl_client_context()->set_gm_identity_provider(&provider);
    const auto token = base::UnguessableToken::Create();

    // Even an accidentally supplied request token cannot make a speculative
    // connection eligible to obtain a fresh authorization or show a PIN UI.
    HttpRequestInfo preconnect;
    preconnect.url = PeerUrl();
    auto resolve_context = base::BindRepeating(
        [](base::UnguessableToken token) { return token; }, token);
    preconnect.gm_client_auth_context = resolve_context;
    preconnect.traffic_annotation =
        MutableNetworkTrafficAnnotationTag(TRAFFIC_ANNOTATION_FOR_TESTS);
    base::RunLoop preconnected;
    session->http_stream_factory()->PreconnectStreams(
        1, preconnect, preconnected.QuitClosure());
    ASSERT_TRUE(base::test::RunUntil([&] { return !!provider.pending; }));
    EXPECT_TRUE(provider.last_request.gm_client_auth_context.is_empty());
    std::move(provider.pending)
        .Run(ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED, nullptr);
    preconnected.Run();

    TestDelegate delegate;
    auto request = context->CreateRequest(
        PeerUrl(), DEFAULT_PRIORITY, &delegate, TRAFFIC_ANNOTATION_FOR_TESTS,
        handles::kInvalidNetworkHandle);
    request->set_gm_client_auth_context(resolve_context);
    request->Start();
    ASSERT_TRUE(base::test::RunUntil([&] { return !!provider.pending; }));
    EXPECT_EQ(provider.last_request.gm_client_auth_context, token);
    request->Cancel();
    request.reset();
    ASSERT_TRUE(base::test::RunUntil([&] { return provider.cancelled; }));
    EXPECT_FALSE(provider.pending);
    session->ssl_client_context()->set_gm_identity_provider(nullptr);
  }
};

TEST_P(GmSocketMutualTcpTest, LegacyPoolPreconnectNeverInheritsRequestContext) {
  CheckRequestContextsWithPool(false);
}

TEST_P(GmSocketMutualTcpTest, StreamPoolPreconnectNeverInheritsRequestContext) {
  CheckRequestContextsWithPool(true);
}

TEST_P(GmSocketMutualTcpTest,
       CredentiallessServerRequestCannotAcquireOrSendClientIdentity) {
  ForbiddenIdentityProvider provider;
  ClientContext gm_context;
  auto config = Config();
  config.privacy_mode = PRIVACY_MODE_ENABLED_WITHOUT_CLIENT_CERTS;
  int result = ERR_FAILED;
  auto socket = Connect(&result, config, "gm.test", &gm_context, &provider);
  ASSERT_TRUE(socket);
  // TLS 1.3 may finish the client flight before the peer's fatal alert arrives.
  // In either protocol the peer requiring mTLS cannot accept application data.
  if (result == OK) {
    SSLInfo info;
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_FALSE(info.client_cert_sent);
    auto buffer = base::MakeRefCounted<IOBufferWithSize>(1);
    TestCompletionCallback read;
    result = read.GetResult(
        socket->Read(buffer.get(), buffer->size(), read.callback()));
  }
  EXPECT_LT(result, 0);
  provider.ExpectUnused();
}

TEST_P(GmSocketMutualTcpTest, DisconnectWithdrawsPendingIdentityAuthorization) {
  PendingIdentityProvider provider;
  context_.set_gm_identity_provider(&provider);
  auto transport = std::make_unique<TCPClientSocket>(
      AddressList(IPEndPoint(IPAddress::IPv4Localhost(), port_)), nullptr,
      nullptr, NetLog::Get(), NetLogSource(), handles::kInvalidNetworkHandle);
  TestCompletionCallback tcp;
  ASSERT_EQ(tcp.GetResult(transport->Connect(tcp.callback())), OK);
  auto required = std::make_shared<TlsRule>(*rule_);
  required->profile = ConnectionProfile(rule_->profile.protocol(),
                                        "test-enterprise", 1, "feitian", 1, 1);
  auto config = Config();
  ApplyTlsRule(required, &config);
  auto socket = context_.CreateSSLClientSocket(
      std::move(transport), HostPortPair("gm.test", port_), config);
  TestCompletionCallback handshake;
  EXPECT_EQ(socket->Connect(handshake.callback()), ERR_IO_PENDING);
  EXPECT_FALSE(provider.pending);
  ASSERT_TRUE(base::test::RunUntil([&] { return !!provider.pending; }));
  EXPECT_NE(provider.last_request.request_id, 0u);
  EXPECT_EQ(provider.last_request.protocol, rule_->profile.protocol());
  EXPECT_EQ(provider.last_request.required_roles,
            kClientCertificateSigning |
                (GetParam() ? kClientCertificateEncryption : 0u));
  EXPECT_FALSE(socket->IsConnected());
  socket->Disconnect();
  EXPECT_TRUE(provider.cancelled);
  EXPECT_FALSE(provider.pending);
  RunUntilIdle();
  EXPECT_FALSE(handshake.have_result());
  context_.set_gm_identity_provider(nullptr);
}

INSTANTIATE_TEST_SUITE_P(TlcpAndRfc8998,
                         GmSocketMutualTcpTest,
                         testing::Bool());

TEST_P(GmSocketTcpTest, HttpConnectProxyTunnelsNationalTls) {
  FetchThroughProxy(EmbeddedTestServer::TYPE_HTTP);
}

TEST_P(GmSocketTcpTest, HttpsConnectProxyKeepsProxyTlsSeparateFromNationalTls) {
  FetchThroughProxy(EmbeddedTestServer::TYPE_HTTPS);
}

TEST_P(GmSocketTcpTest,
       RedirectFromOrdinaryTlsSelectsDestinationNationalPolicy) {
  EmbeddedTestServer ordinary(EmbeddedTestServer::TYPE_HTTPS);
  ordinary.RegisterRequestHandler(base::BindRepeating(
      [](const std::string& destination, const test_server::HttpRequest&)
          -> std::unique_ptr<test_server::HttpResponse> {
        auto response = std::make_unique<test_server::BasicHttpResponse>();
        response->set_code(HTTP_FOUND);
        response->AddCustomHeader("Location", destination);
        return response;
      },
      PeerUrl().spec()));
  ASSERT_TRUE(ordinary.Start());
  auto context = HttpContext();
  TestDelegate delegate;
  auto request = context->CreateRequest(
      ordinary.GetURL("gm.test", "/start"), DEFAULT_PRIORITY, &delegate,
      TRAFFIC_ANNOTATION_FOR_TESTS, handles::kInvalidNetworkHandle);
  request->Start();
  delegate.RunUntilComplete();
  ASSERT_EQ(delegate.request_status(), OK);
  EXPECT_EQ(delegate.received_redirect_count(), 1);
  EXPECT_EQ(request->url(), PeerUrl());
  EXPECT_NE(delegate.data_received().find("GM TLS connection verified"),
            std::string::npos);
  EXPECT_EQ(
      SSLConnectionStatusToCipherSuite(request->ssl_info().connection_status),
      GetParam() ? 0xe053 : 0x00c6);
}

TEST_P(GmSocketTcpTest, RedirectFromNationalTlsReevaluatesOriginAndPort) {
  EmbeddedTestServer ordinary(EmbeddedTestServer::TYPE_HTTPS);
  ordinary.RegisterRequestHandler(
      base::BindRepeating([](const test_server::HttpRequest&)
                              -> std::unique_ptr<test_server::HttpResponse> {
        auto response = std::make_unique<test_server::BasicHttpResponse>();
        response->set_content("ordinary TLS destination");
        return response;
      }));
  ASSERT_TRUE(ordinary.Start());
  const GURL destination = ordinary.GetURL("gm.test", "/final");
  auto context = HttpContext();
  TestDelegate delegate;
  auto request = context->CreateRequest(
      PeerUrl("/redirect?url=" +
              base::EscapeQueryParamValue(destination.spec(), false)),
      DEFAULT_PRIORITY, &delegate, TRAFFIC_ANNOTATION_FOR_TESTS,
      handles::kInvalidNetworkHandle);
  request->Start();
  delegate.RunUntilComplete();
  ASSERT_EQ(delegate.request_status(), OK);
  EXPECT_EQ(delegate.received_redirect_count(), 1);
  EXPECT_EQ(request->url(), destination);
  EXPECT_EQ(delegate.data_received(), "ordinary TLS destination");
  EXPECT_NE(
      SSLConnectionStatusToCipherSuite(request->ssl_info().connection_status),
      0x00c6);
}

TEST_P(GmSocketTcpTest, RedirectToManagedOrdinaryTlsNeverFallsBack) {
  EmbeddedTestServer ordinary(EmbeddedTestServer::TYPE_HTTPS);
  ordinary.RegisterRequestHandler(
      base::BindRepeating([](const test_server::HttpRequest&)
                              -> std::unique_ptr<test_server::HttpResponse> {
        auto response = std::make_unique<test_server::BasicHttpResponse>();
        response->set_content("must not be returned");
        return response;
      }));
  ASSERT_TRUE(ordinary.Start());
  const GURL destination = ordinary.GetURL("ordinary.test", "/final");
  TlsRule additional = *rule_;
  additional.host = "ordinary.test";
  additional.port = ordinary.port();
  auto context = HttpContext({}, {additional});
  TestDelegate delegate;
  auto request = context->CreateRequest(
      PeerUrl("/redirect?url=" +
              base::EscapeQueryParamValue(destination.spec(), false)),
      DEFAULT_PRIORITY, &delegate, TRAFFIC_ANNOTATION_FOR_TESTS,
      handles::kInvalidNetworkHandle);
  request->Start();
  delegate.RunUntilComplete();
  EXPECT_NE(delegate.request_status(), OK);
  EXPECT_EQ(delegate.received_redirect_count(), 1);
  EXPECT_TRUE(delegate.data_received().empty());
}

TEST_P(GmSocketTcpTest, SecureWebSocketUsesNationalTlsAndExchangesFrames) {
  auto context = HttpContext();
  WebSocketResult result;
  auto request =
      WebSocket(context.get(), test_server::ToWebSocketUrl(PeerUrl()), &result);
  result.completed.Run();
  ASSERT_EQ(result.error, OK);
  ASSERT_TRUE(result.stream);
  EXPECT_EQ(SSLConnectionStatusToCipherSuite(result.ssl_info.connection_status),
            GetParam() ? 0xe053 : 0x00c6);
  const std::string payload = "national websocket echo";
  auto frame =
      std::make_unique<WebSocketFrame>(WebSocketFrameHeader::kOpCodeText);
  frame->header.final = true;
  frame->header.masked = true;
  frame->header.payload_length = payload.size();
  frame->payload = base::as_byte_span(payload);
  std::vector<std::unique_ptr<WebSocketFrame>> outgoing;
  outgoing.push_back(std::move(frame));
  TestCompletionCallback write;
  ASSERT_EQ(
      write.GetResult(result.stream->WriteFrames(&outgoing, write.callback())),
      OK);
  std::vector<std::unique_ptr<WebSocketFrame>> incoming;
  TestCompletionCallback read;
  ASSERT_EQ(
      read.GetResult(result.stream->ReadFrames(&incoming, read.callback())),
      OK);
  ASSERT_EQ(incoming.size(), 1u);
  EXPECT_EQ(base::as_string_view(incoming.front()->payload), payload);
  result.stream->Close();
}

TEST_P(GmSocketTcpTest, ManagedWebSocketRejectsOrdinaryTlsWithoutFallback) {
  EmbeddedTestServer ordinary(EmbeddedTestServer::TYPE_HTTPS);
  test_server::InstallDefaultWebSocketHandlers(&ordinary);
  ASSERT_TRUE(ordinary.Start());
  const GURL url = test_server::GetWebSocketURL(ordinary, "ordinary.test",
                                                "/echo-with-no-extension");
  {
    auto context = HttpContext();
    WebSocketResult result;
    auto request = WebSocket(context.get(), url, &result);
    result.completed.Run();
    ASSERT_EQ(result.error, OK);
    ASSERT_TRUE(result.stream);
    result.stream->Close();
  }
  TlsRule additional = *rule_;
  additional.host = "ordinary.test";
  additional.port = ordinary.port();
  auto context = HttpContext({}, {additional});
  WebSocketResult result;
  auto request = WebSocket(context.get(), url, &result);
  result.completed.Run();
  EXPECT_NE(result.error, OK);
  EXPECT_FALSE(result.stream);
}

INSTANTIATE_TEST_SUITE_P(TlcpAndRfc8998, GmSocketTcpTest, testing::Bool());

// Reuse the independent RFC 8998 peer and its certificate fixtures, but route
// through SSLClientSocketImpl's single TLS 1.3 ClientHello.
class GmMixedSocketTcpTest : public GmSocketTcpTest {
 protected:
  void SetUp() override {
    GmSocketTcpTest::SetUp();
    if (HasFatalFailure()) {
      return;
    }
    rule_->profile = ConnectionProfile(TlsProtocol::kTls13Auto,
                                       "test-enterprise", 1, "", 0, 1);
    ASSERT_TRUE(rule_->IsValid());
  }

  SSLConfig IdentityConfig() const {
    auto rule = std::make_shared<TlsRule>(*rule_);
    rule->profile = ConnectionProfile(TlsProtocol::kTls13Auto,
                                      "test-enterprise", 1, "feitian", 1, 1);
    auto config = Config();
    ApplyTlsRule(std::move(rule), &config);
    return config;
  }

  scoped_refptr<SoftwareIdentity> CreateSoftwareIdentity(
      uint64_t generation = 1) {
    auto cert = ImportCertFromFile(directory_.GetPath(), "client-sign.pem");
    if (!cert) {
      return nullptr;
    }
    auto key = key_util::LoadEVP_PKEYFromPEM(
        directory_.GetPath().AppendASCII("client-sign-key.pem"));
    if (!key) {
      return nullptr;
    }
    const auto der = x509_util::CryptoBufferAsSpan(cert->cert_buffer());
    return base::MakeRefCounted<SoftwareIdentity>(
        "software-feitian", generation,
        std::vector<uint8_t>(der.begin(), der.end()), std::move(key));
  }

  void ExpectSingleHandshake(const std::string& response) {
    EXPECT_NE(response.find("data-connection-id=1 data-client-hellos=1>"),
              std::string::npos);
    EXPECT_TRUE(base::PathExists(
        directory_.GetPath().AppendASCII("accepted-connection-1")));
    EXPECT_FALSE(base::PathExists(
        directory_.GetPath().AppendASCII("accepted-connection-2")));
  }

  static bool CheckClientHello(const SSL_CLIENT_HELLO* hello, bool expect_gm) {
    bool sm4 = false;
    bool ordinary = false;
    CBS suites;
    CBS_init(&suites, hello->cipher_suites, hello->cipher_suites_len);
    while (CBS_len(&suites)) {
      uint16_t suite;
      if (!CBS_get_u16(&suites, &suite)) {
        return false;
      }
      sm4 |= suite == SSL_CIPHER_SM4_GCM_SM3 || suite == SSL_CIPHER_SM4_CCM_SM3;
      ordinary |= suite == SSL_CIPHER_AES_128_GCM_SHA256;
    }
    const uint8_t* data;
    size_t size;
    if (!SSL_early_callback_ctx_extension_get(hello, TLSEXT_TYPE_key_share,
                                              &data, &size)) {
      return false;
    }
    CBS contents, shares;
    CBS_init(&contents, data, size);
    if (!CBS_get_u16_length_prefixed(&contents, &shares) ||
        CBS_len(&contents)) {
      return false;
    }
    bool sm2 = false;
    while (CBS_len(&shares)) {
      uint16_t group;
      CBS key;
      if (!CBS_get_u16(&shares, &group) ||
          !CBS_get_u16_length_prefixed(&shares, &key)) {
        return false;
      }
      sm2 |= group == SSL_GROUP_SM2;
    }
    EXPECT_EQ(sm4, expect_gm);
    EXPECT_EQ(sm2, expect_gm);
    EXPECT_TRUE(ordinary);
    return sm4 == expect_gm && sm2 == expect_gm && ordinary;
  }

  void CheckOrdinaryTls(bool mixed,
                        bool mutual,
                        int verifier_result = OK,
                        bool invalidate_cached_gm_identity = false) {
    std::atomic<int> hellos{0};
    EmbeddedTestServer ordinary(EmbeddedTestServer::TYPE_HTTPS);
    SSLServerConfig server_config;
    server_config.version_min = SSL_PROTOCOL_VERSION_TLS1_3;
    server_config.client_cert_type = mutual
                                         ? SSLServerConfig::REQUIRE_CLIENT_CERT
                                         : SSLServerConfig::NO_CLIENT_CERT;
    server_config.client_hello_callback_for_testing =
        base::BindLambdaForTesting([&](const SSL_CLIENT_HELLO* hello) {
          ++hellos;
          return CheckClientHello(hello, mixed);
        });
    ordinary.SetSSLConfig(EmbeddedTestServer::CERT_OK, server_config);
    ASSERT_TRUE(ordinary.Start());
    verifier_.set_default_result(verifier_result);
    ForbiddenIdentityProvider provider;
    context_.set_gm_identity_provider(&provider);
    if (invalidate_cached_gm_identity) {
      verifier_.set_async(true);
      verifier_.during_async_verify = base::BindOnce(
          [](scoped_refptr<ClientIdentity> identity) {
            identity->Invalidate();
          },
          provider.cached);
    }
    if (mutual) {
      auto cert = ImportCertFromFile(GetTestCertsDirectory(), "client_1.pem");
      auto key = key_util::LoadPrivateKeyOpenSSL(
          GetTestCertsDirectory().AppendASCII("client_1.key"));
      ASSERT_TRUE(cert);
      ASSERT_TRUE(key);
      context_.SetClientCertificate(HostPortPair("gm.test", ordinary.port()),
                                    std::move(cert), std::move(key));
    }
    SSLConfig config;
    if (mixed) {
      auto rule = std::make_shared<TlsRule>(*rule_);
      rule->port = ordinary.port();
      ApplyTlsRule(rule, &config);
    }
    int result;
    auto socket =
        Connect(&result, config, "gm.test", nullptr, nullptr, ordinary.port());
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, verifier_result);
    SSLInfo info;
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_FALSE(info.gm_tls);
    EXPECT_EQ(info.client_cert_sent, mutual);
    EXPECT_NE(SSLConnectionStatusToCipherSuite(info.connection_status),
              SSL_CIPHER_SM4_GCM_SM3);
    EXPECT_NE(SSLConnectionStatusToCipherSuite(info.connection_status),
              SSL_CIPHER_SM4_CCM_SM3);
    ASSERT_EQ(verifier_.GetVerifyParams().size(), 1u);
    EXPECT_EQ(verifier_.GetVerifyParams()[0].hostname(), "gm.test");
    EXPECT_EQ(hellos.load(), 1);
    EXPECT_EQ(provider.acquire_calls, 0);
    EXPECT_EQ(provider.cached->sign_calls, 0);
    if (!mixed) {
      EXPECT_EQ(provider.cache_lookups, 0);
    }
    socket.reset();
    context_.set_gm_identity_provider(nullptr);
  }
};

TEST_P(GmMixedSocketTcpTest, Rfc8998UsesOneConnectionAndClientHello) {
  int result;
  auto socket = Connect(&result, Config());
  ASSERT_TRUE(socket);
  ASSERT_EQ(result, OK);
  SSLInfo info;
  ASSERT_TRUE(socket->GetSSLInfo(&info));
  EXPECT_TRUE(info.gm_tls);
  EXPECT_EQ(SSLConnectionStatusToCipherSuite(info.connection_status),
            SSL_CIPHER_SM4_GCM_SM3);
  EXPECT_TRUE(verifier_.GetVerifyParams().empty());
  ExpectSingleHandshake(Exchange(socket.get()));
}

TEST_P(GmMixedSocketTcpTest, SameMixedConfigurationNegotiatesOrdinaryTls) {
  CheckOrdinaryTls(/*mixed=*/true, /*mutual=*/false);
}

TEST_P(GmMixedSocketTcpTest, OrdinaryMutualTlsUsesOrdinaryIdentity) {
  CheckOrdinaryTls(/*mixed=*/true, /*mutual=*/true);
}

TEST_P(GmMixedSocketTcpTest, DefaultSocketDoesNotAdvertiseGm) {
  CheckOrdinaryTls(/*mixed=*/false, /*mutual=*/false);
}

TEST_P(GmMixedSocketTcpTest, GmTrustDoesNotOverrideOrdinaryCertificateErrors) {
  CheckOrdinaryTls(/*mixed=*/true, /*mutual=*/false,
                   ERR_CERT_AUTHORITY_INVALID);
}

TEST_P(GmMixedSocketTcpTest,
       CachedGmIdentityInvalidationDoesNotCancelOrdinaryVerification) {
  CheckOrdinaryTls(/*mixed=*/true, /*mutual=*/false, OK,
                   /*invalidate_cached_gm_identity=*/true);
}

TEST_P(GmMixedSocketTcpTest, ResumptionPartitionsNetworkAndPolicy) {
  base::test::ScopedFeatureList features;
  features.InitAndEnableFeature(
      features::kPartitionConnectionsByNetworkIsolationKey);
  int result;
  for (int attempt = 0; attempt < 4; ++attempt) {
    SCOPED_TRACE(attempt);
    auto config = Config();
    if (attempt == 2) {
      config.network_anonymization_key =
          NetworkAnonymizationKey::CreateTransient();
    } else if (attempt == 3) {
      auto revised = std::make_shared<TlsRule>(*rule_);
      revised->profile = ConnectionProfile(TlsProtocol::kTls13Auto,
                                           "test-enterprise", 1, "", 0, 2);
      ApplyTlsRule(revised, &config);
    }
    auto socket = Connect(&result, config);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    SSLInfo info;
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_TRUE(info.gm_tls);
    EXPECT_EQ(info.handshake_type, attempt == 1 ? SSLInfo::HANDSHAKE_RESUME
                                                : SSLInfo::HANDSHAKE_FULL);
    EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
    ASSERT_TRUE(
        base::test::RunUntil([&] { return session_cache_.size() != 0; }));
  }
  EXPECT_TRUE(verifier_.GetVerifyParams().empty());
}

// Hold transport writes after a real handshake so both TLS IO callbacks can be
// pending when the Key authorization is revoked. Disconnect cancels the write.
class GmBlockingWriteSocket : public WrappedStreamSocket {
 public:
  explicit GmBlockingWriteSocket(std::unique_ptr<StreamSocket> transport)
      : WrappedStreamSocket(std::move(transport)) {}

  void BlockWrites() { block_writes_ = true; }

  int Write(IOBuffer* buffer,
            int length,
            CompletionOnceCallback callback,
            const NetworkTrafficAnnotationTag& annotation) override {
    if (!block_writes_) {
      return WrappedStreamSocket::Write(buffer, length, std::move(callback),
                                        annotation);
    }
    EXPECT_FALSE(write_callback_);
    write_callback_ = std::move(callback);
    return ERR_IO_PENDING;
  }

  void Disconnect() override {
    write_callback_.Reset();
    WrappedStreamSocket::Disconnect();
  }

 private:
  bool block_writes_ = false;
  CompletionOnceCallback write_callback_;
};

class GmInvalidationObserver : public SSLClientContext::Observer {
 public:
  explicit GmInvalidationObserver(base::RepeatingClosure callback)
      : callback_(std::move(callback)) {}

  void OnGmConfigForServersChanged(
      const base::flat_set<HostPortPair>&) override {
    callback_.Run();
  }
  void OnSSLConfigChanged(SSLClientContext::SSLConfigChangeType) override {}
  void OnSSLConfigForServersChanged(
      const base::flat_set<HostPortPair>&) override {}

 private:
  base::RepeatingClosure callback_;
};

class GmMixedSocketMutualTcpTest : public GmMixedSocketTcpTest {
 protected:
  bool RequireClientCertificate() const override { return true; }

  enum class InvalidationAction {
    kKeepSocket,
    kDeleteInNotification,
    kDeleteInReadCallback,
    kDisconnectInReadCallback,
  };

  void CheckInvalidationCallbacks(InvalidationAction action) {
    auto identity = CreateSoftwareIdentity();
    ASSERT_TRUE(identity);
    SoftwareIdentityProvider provider(identity);
    context_.set_gm_identity_provider(&provider);
    base::ScopedClosureRunner reset_provider(base::BindLambdaForTesting(
        [&] { context_.set_gm_identity_provider(nullptr); }));

    auto transport = std::make_unique<GmBlockingWriteSocket>(
        std::make_unique<TCPClientSocket>(
            AddressList(IPEndPoint(IPAddress::IPv4Localhost(), port_)), nullptr,
            nullptr, NetLog::Get(), NetLogSource(),
            handles::kInvalidNetworkHandle));
    auto* blocking_transport = transport.get();
    TestCompletionCallback tcp;
    ASSERT_EQ(tcp.GetResult(transport->Connect(tcp.callback())), OK);
    auto socket = context_.CreateSSLClientSocket(
        std::move(transport), HostPortPair("gm.test", port_), IdentityConfig());
    TestCompletionCallback handshake;
    ASSERT_EQ(handshake.GetResult(socket->Connect(handshake.callback())), OK);
    SSLInfo info;
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    ASSERT_TRUE(info.gm_tls);
    ASSERT_TRUE(info.client_cert_sent);
    EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);

    blocking_transport->BlockWrites();
    int read_calls = 0;
    int write_calls = 0;
    auto input = base::MakeRefCounted<IOBufferWithSize>(1);
    ASSERT_EQ(socket->Read(
                  input.get(), input->size(),
                  base::BindLambdaForTesting([&](int result) {
                    EXPECT_EQ(result,
                              ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
                    ++read_calls;
                    if (action == InvalidationAction::kDeleteInReadCallback) {
                      socket.reset();
                    } else if (action ==
                               InvalidationAction::kDisconnectInReadCallback) {
                      socket->Disconnect();
                    }
                  })),
              ERR_IO_PENDING);
    auto output =
        base::MakeRefCounted<StringIOBuffer>(std::string(16 * 1024, 'x'));
    int result = OK;
    for (int attempt = 0; attempt < 16; ++attempt) {
      result = socket->Write(
          output.get(), output->size(),
          base::BindLambdaForTesting([&](int error) {
            EXPECT_EQ(error, ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
            ++write_calls;
          }),
          TRAFFIC_ANNOTATION_FOR_TESTS);
      if (result == ERR_IO_PENDING) {
        break;
      }
      ASSERT_GT(result, 0);
    }
    ASSERT_EQ(result, ERR_IO_PENDING);

    int notifications = 0;
    GmInvalidationObserver observer(base::BindLambdaForTesting([&] {
      ++notifications;
      if (action == InvalidationAction::kDeleteInNotification) {
        socket.reset();
      }
    }));
    base::ScopedObservation<SSLClientContext, SSLClientContext::Observer>
        observation(&observer);
    observation.Observe(&context_);
    identity->Invalidate();
    RunUntilIdle();
    EXPECT_EQ(notifications, 1);
    EXPECT_EQ(read_calls,
              action == InvalidationAction::kDeleteInNotification ? 0 : 1);
    EXPECT_EQ(write_calls, action == InvalidationAction::kKeepSocket ? 1 : 0);
    if (socket) {
      EXPECT_FALSE(socket->IsConnected());
    }
  }
};

TEST_P(GmMixedSocketMutualTcpTest, KeyInvalidationCompletesPendingIo) {
  CheckInvalidationCallbacks(InvalidationAction::kKeepSocket);
}

TEST_P(GmMixedSocketMutualTcpTest,
       KeyInvalidationPoolDeletionCancelsPendingIo) {
  CheckInvalidationCallbacks(InvalidationAction::kDeleteInNotification);
}

TEST_P(GmMixedSocketMutualTcpTest,
       KeyInvalidationReadDeletionCancelsPendingWrite) {
  CheckInvalidationCallbacks(InvalidationAction::kDeleteInReadCallback);
}

TEST_P(GmMixedSocketMutualTcpTest,
       KeyInvalidationReadDisconnectCancelsPendingWrite) {
  CheckInvalidationCallbacks(InvalidationAction::kDisconnectInReadCallback);
}

TEST_P(GmMixedSocketMutualTcpTest, SoftwareKeyAuthenticatesWithoutReconnect) {
  auto identity = CreateSoftwareIdentity();
  ASSERT_TRUE(identity);
  SoftwareIdentityProvider provider(identity);
  context_.set_gm_identity_provider(&provider);
  int result;
  auto socket = Connect(&result, IdentityConfig());
  ASSERT_TRUE(socket);
  ASSERT_EQ(result, OK);
  SSLInfo info;
  ASSERT_TRUE(socket->GetSSLInfo(&info));
  EXPECT_TRUE(info.gm_tls);
  EXPECT_TRUE(info.client_cert_sent);
  EXPECT_EQ(provider.acquire_calls, 1);
  EXPECT_EQ(provider.last_request.protocol, TlsProtocol::kRfc8998);
  EXPECT_EQ(provider.last_request.required_roles, kClientCertificateSigning);
  EXPECT_EQ(identity->sign_calls, 1);
  EXPECT_TRUE(verifier_.GetVerifyParams().empty());
  ExpectSingleHandshake(Exchange(socket.get()));
  socket.reset();
  context_.set_gm_identity_provider(nullptr);
}

TEST_P(GmMixedSocketMutualTcpTest,
       PendingVerificationDoesNotPromptOrBlockOrdinaryTls) {
  auto identity = CreateSoftwareIdentity();
  ASSERT_TRUE(identity);
  SoftwareIdentityProvider provider(identity);
  context_.set_gm_identity_provider(&provider);
  verifier_.set_default_result(OK);
  verifier_.set_async(true);
  EmbeddedTestServer ordinary(EmbeddedTestServer::TYPE_HTTPS);
  ASSERT_TRUE(ordinary.Start());
  int result;
  auto socket = CreateSocket(&result, IdentityConfig());
  ASSERT_TRUE(socket);
  ASSERT_EQ(result, OK);
  auto ordinary_socket = CreateSocket(&result, SSLConfig(), "ordinary.test",
                                      nullptr, nullptr, ordinary.port());
  ASSERT_TRUE(ordinary_socket);
  ASSERT_EQ(result, OK);
  TestCompletionCallback handshake;
  {
    // This also stops a cold trust/CRL cache from being parsed. Network IO and
    // the upstream ordinary certificate-verifier continuation can still run.
    base::ScopedThreadPoolExecutionFence fence;
    ASSERT_EQ(socket->Connect(handshake.callback()), ERR_IO_PENDING);
    ASSERT_TRUE(base::test::RunUntil([&] {
      SSLInfo info;
      return socket->GetSSLInfo(&info);
    }));
    EXPECT_FALSE(handshake.have_result());
    EXPECT_FALSE(socket->IsConnected());
    EXPECT_EQ(provider.acquire_calls, 0);
    EXPECT_EQ(identity->sign_calls, 0);

    TestCompletionCallback ordinary_handshake;
    ASSERT_EQ(ordinary_handshake.GetResult(
                  ordinary_socket->Connect(ordinary_handshake.callback())),
              OK);
    SSLInfo info;
    ASSERT_TRUE(ordinary_socket->GetSSLInfo(&info));
    EXPECT_FALSE(info.gm_tls);
    EXPECT_EQ(verifier_.GetVerifyParams().size(), 1u);
    EXPECT_FALSE(handshake.have_result());
    EXPECT_EQ(provider.acquire_calls, 0);
    EXPECT_EQ(identity->sign_calls, 0);
  }

  ASSERT_EQ(handshake.GetResult(ERR_IO_PENDING), OK);
  EXPECT_EQ(provider.acquire_calls, 1);
  EXPECT_EQ(identity->sign_calls, 1);
  ExpectSingleHandshake(Exchange(socket.get()));
  socket.reset();
  ordinary_socket.reset();
  context_.set_gm_identity_provider(nullptr);
}

TEST_P(GmMixedSocketMutualTcpTest,
       CancelledVerificationNeverResumesOrRequestsPin) {
  PendingIdentityProvider provider;
  context_.set_gm_identity_provider(&provider);
  for (bool disconnect : {true, false}) {
    SCOPED_TRACE(disconnect);
    int result;
    auto socket = CreateSocket(&result, IdentityConfig());
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    TestCompletionCallback handshake;
    {
      base::ScopedThreadPoolExecutionFence fence;
      ASSERT_EQ(socket->Connect(handshake.callback()), ERR_IO_PENDING);
      ASSERT_TRUE(base::test::RunUntil([&] {
        SSLInfo info;
        return socket->GetSSLInfo(&info);
      }));
      EXPECT_FALSE(handshake.have_result());
      EXPECT_FALSE(provider.pending);
      if (disconnect) {
        socket->Disconnect();
      }
      socket.reset();
      // A pending job must also survive destruction of the cache's reference.
      context_.GetGmClientContext()->Clear();
    }
    RunUntilIdle();
    EXPECT_FALSE(handshake.have_result());
    EXPECT_FALSE(provider.pending);
    EXPECT_EQ(provider.last_request.request_id, 0u);
  }
  context_.set_gm_identity_provider(nullptr);
}

TEST_P(GmMixedSocketMutualTcpTest,
       MalformedTrustAndCrlAreRejectedOffTheNetworkSequence) {
  PendingIdentityProvider provider;
  context_.set_gm_identity_provider(&provider);
  for (bool malformed_crl : {false, true}) {
    SCOPED_TRACE(malformed_crl);
    auto config = IdentityConfig();
    auto malformed = std::make_shared<TlsRule>(*config.gm_rule);
    if (malformed_crl) {
      malformed->crls = {{0x30, 0}};
      malformed->revocation_mode = GM_REVOCATION_REQUIRE_LEAF_CRL;
    } else {
      malformed->trust_anchors = {{0x30, 0}};
    }
    ASSERT_TRUE(malformed->IsValid());  // Structural bounds, not DER parsing.
    ApplyTlsRule(malformed, &config);
    int result;
    auto socket = CreateSocket(&result, config);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    TestCompletionCallback handshake;
    {
      base::ScopedThreadPoolExecutionFence fence;
      ASSERT_EQ(socket->Connect(handshake.callback()), ERR_IO_PENDING);
      ASSERT_TRUE(base::test::RunUntil([&] {
        SSLInfo info;
        return socket->GetSSLInfo(&info);
      }));
      EXPECT_FALSE(handshake.have_result());
      EXPECT_FALSE(provider.pending);
    }
    EXPECT_EQ(handshake.GetResult(ERR_IO_PENDING), ERR_CERT_INVALID);
    EXPECT_FALSE(provider.pending);
    EXPECT_EQ(provider.last_request.request_id, 0u);
    EXPECT_FALSE(socket->IsConnected());
    EXPECT_TRUE(verifier_.GetVerifyParams().empty());
  }
  context_.set_gm_identity_provider(nullptr);
}

TEST_P(GmMixedSocketMutualTcpTest, UntrustedServerNeverRequestsPin) {
  auto unrelated =
      ImportCertFromFile(GetTestCertsDirectory(), "root_ca_cert.pem");
  ASSERT_TRUE(unrelated);
  const auto der = x509_util::CryptoBufferAsSpan(unrelated->cert_buffer());
  auto rule = std::make_shared<TlsRule>(*rule_);
  rule->trust_anchors = {std::vector<uint8_t>(der.begin(), der.end())};
  auto config = Config();
  ApplyTlsRule(rule, &config);
  // Ordinary trust success cannot rescue an untrusted national certificate.
  verifier_.set_default_result(OK);
  PendingIdentityProvider provider;
  context_.set_gm_identity_provider(&provider);
  int result;
  auto socket = Connect(&result, config);
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, ERR_CERT_AUTHORITY_INVALID);
  EXPECT_FALSE(provider.pending);
  EXPECT_EQ(provider.last_request.request_id, 0u);
  EXPECT_TRUE(verifier_.GetVerifyParams().empty());
  SSLInfo info;
  ASSERT_TRUE(socket->GetSSLInfo(&info));
  EXPECT_TRUE(info.is_fatal_cert_error);
  EXPECT_TRUE(info.cert_status & CERT_STATUS_AUTHORITY_INVALID);
  EXPECT_FALSE(base::PathExists(
      directory_.GetPath().AppendASCII("accepted-connection-2")));
  socket.reset();
  context_.set_gm_identity_provider(nullptr);
}

TEST_P(GmMixedSocketMutualTcpTest, WrongPinDoesNotSignOrRetryProtocol) {
  auto identity = CreateSoftwareIdentity();
  ASSERT_TRUE(identity);
  SoftwareIdentityProvider provider(identity);
  provider.acquisition_result = ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED;
  context_.set_gm_identity_provider(&provider);
  int result;
  auto socket = Connect(&result, IdentityConfig());
  ASSERT_TRUE(socket);
  EXPECT_EQ(result, ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED);
  EXPECT_EQ(provider.acquire_calls, 1);
  EXPECT_EQ(identity->sign_calls, 0);
  EXPECT_FALSE(socket->IsConnected());
  EXPECT_FALSE(base::PathExists(
      directory_.GetPath().AppendASCII("accepted-connection-2")));
  EXPECT_EQ(session_cache_.size(), 0u);
  socket.reset();
  context_.set_gm_identity_provider(nullptr);
}

TEST_P(GmMixedSocketMutualTcpTest, DisconnectCancelsPendingPinAuthorization) {
  PendingIdentityProvider provider;
  context_.set_gm_identity_provider(&provider);
  int result;
  auto socket = CreateSocket(&result, IdentityConfig());
  ASSERT_TRUE(socket);
  ASSERT_EQ(result, OK);
  TestCompletionCallback handshake;
  EXPECT_EQ(socket->Connect(handshake.callback()), ERR_IO_PENDING);
  ASSERT_TRUE(base::test::RunUntil([&] { return !!provider.pending; }));
  EXPECT_FALSE(socket->IsConnected());
  socket->Disconnect();
  EXPECT_TRUE(provider.cancelled);
  EXPECT_FALSE(provider.pending);
  RunUntilIdle();
  EXPECT_FALSE(handshake.have_result());
  EXPECT_FALSE(base::PathExists(
      directory_.GetPath().AppendASCII("accepted-connection-2")));
  socket.reset();
  context_.set_gm_identity_provider(nullptr);
}

TEST_P(GmMixedSocketMutualTcpTest, DisconnectCancelsPendingKeySignature) {
  auto identity = CreateSoftwareIdentity();
  ASSERT_TRUE(identity);
  identity->hold_signatures = true;
  SoftwareIdentityProvider provider(identity);
  context_.set_gm_identity_provider(&provider);
  int result;
  auto socket = CreateSocket(&result, IdentityConfig());
  ASSERT_TRUE(socket);
  ASSERT_EQ(result, OK);
  TestCompletionCallback handshake;
  EXPECT_EQ(socket->Connect(handshake.callback()), ERR_IO_PENDING);
  ASSERT_TRUE(base::test::RunUntil([&] { return !!identity->pending; }));
  socket->Disconnect();
  EXPECT_EQ(identity->cancel_calls, 1);
  EXPECT_FALSE(identity->pending);
  RunUntilIdle();
  EXPECT_FALSE(handshake.have_result());
  EXPECT_FALSE(base::PathExists(
      directory_.GetPath().AppendASCII("accepted-connection-2")));
  socket.reset();
  context_.set_gm_identity_provider(nullptr);
}

TEST_P(GmMixedSocketMutualTcpTest,
       ResumptionRequiresSameLiveIdentityGeneration) {
  auto identity = CreateSoftwareIdentity();
  ASSERT_TRUE(identity);
  SoftwareIdentityProvider provider(identity);
  context_.set_gm_identity_provider(&provider);
  for (int attempt = 0; attempt < 3; ++attempt) {
    SCOPED_TRACE(attempt);
    if (attempt == 2) {
      identity = CreateSoftwareIdentity(/*generation=*/2);
      ASSERT_TRUE(identity);
      provider.identity = identity;
    }
    int result;
    auto socket = Connect(&result, IdentityConfig());
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    SSLInfo info;
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_TRUE(info.gm_tls);
    EXPECT_EQ(info.handshake_type, attempt == 1 ? SSLInfo::HANDSHAKE_RESUME
                                                : SSLInfo::HANDSHAKE_FULL);
    EXPECT_EQ(identity->sign_calls, 1);
    EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
    ASSERT_TRUE(
        base::test::RunUntil([&] { return session_cache_.size() != 0; }));
  }
  context_.set_gm_identity_provider(nullptr);
}

TEST_P(GmMixedSocketMutualTcpTest,
       OfferedGmIdentityTicketDoesNotBindAnOrdinaryFullHandshake) {
  auto identity = CreateSoftwareIdentity();
  ASSERT_TRUE(identity);
  SoftwareIdentityProvider provider(identity);
  context_.set_gm_identity_provider(&provider);
  // Client authentication is requested by the GM server, but the policy also
  // permits ordinary TLS. This keeps the same effective profile on both legs.
  const auto config = Config();
  {
    int result;
    auto socket = Connect(&result, config);
    ASSERT_TRUE(socket);
    ASSERT_EQ(result, OK);
    SSLInfo info;
    ASSERT_TRUE(socket->GetSSLInfo(&info));
    EXPECT_TRUE(info.gm_tls);
    EXPECT_TRUE(info.client_cert_sent);
    EXPECT_NE(Exchange(socket.get()).find("GM TLS connection verified"),
              std::string::npos);
    ASSERT_TRUE(
        base::test::RunUntil([&] { return session_cache_.size() != 0; }));
  }
  ASSERT_EQ(provider.acquire_calls, 1);
  ASSERT_EQ(identity->sign_calls, 1);
  ASSERT_TRUE(provider.authorized);
  EXPECT_TRUE(verifier_.GetVerifyParams().empty());

  std::atomic<int> offered_tickets{0};
  EmbeddedTestServer ordinary(EmbeddedTestServer::TYPE_HTTPS);
  SSLServerConfig server_config;
  server_config.version_min = SSL_PROTOCOL_VERSION_TLS1_3;
  server_config.client_hello_callback_for_testing =
      base::BindLambdaForTesting([&](const SSL_CLIENT_HELLO* hello) {
        const uint8_t* data;
        size_t size;
        const bool offered = SSL_early_callback_ctx_extension_get(
            hello, TLSEXT_TYPE_pre_shared_key, &data, &size);
        EXPECT_TRUE(offered);
        if (offered && size != 0) {
          ++offered_tickets;
        }
        return offered && size != 0 && CheckClientHello(hello, true);
      });
  ordinary.SetSSLConfig(EmbeddedTestServer::CERT_OK, server_config);
  ASSERT_TRUE(ordinary.Start());
  verifier_.set_default_result(OK);
  verifier_.set_async(true);
  verifier_.during_async_verify = base::BindOnce(
      [](scoped_refptr<ClientIdentity> cached) { cached->Invalidate(); },
      identity);

  // Model a backend change for the same origin: only the TCP destination is
  // replaced. The logical origin, policy, live identity and cache stay equal.
  auto transport = std::make_unique<TCPClientSocket>(
      AddressList(IPEndPoint(IPAddress::IPv4Localhost(), ordinary.port())),
      nullptr, nullptr, NetLog::Get(), NetLogSource(),
      handles::kInvalidNetworkHandle);
  TestCompletionCallback tcp;
  ASSERT_EQ(tcp.GetResult(transport->Connect(tcp.callback())), OK);
  auto socket = context_.CreateSSLClientSocket(
      std::move(transport), HostPortPair("gm.test", port_), config);
  TestCompletionCallback handshake;
  ASSERT_EQ(handshake.GetResult(socket->Connect(handshake.callback())), OK);
  SSLInfo info;
  ASSERT_TRUE(socket->GetSSLInfo(&info));
  EXPECT_FALSE(info.gm_tls);
  EXPECT_FALSE(info.client_cert_sent);
  EXPECT_EQ(info.handshake_type, SSLInfo::HANDSHAKE_FULL);
  EXPECT_EQ(offered_tickets.load(), 1);
  EXPECT_EQ(verifier_.GetVerifyParams().size(), 1u);
  EXPECT_FALSE(identity->is_valid());
  EXPECT_EQ(provider.acquire_calls, 1);
  EXPECT_EQ(identity->sign_calls, 1);
  socket.reset();
  context_.set_gm_identity_provider(nullptr);
}

INSTANTIATE_TEST_SUITE_P(Rfc8998Peer,
                         GmMixedSocketTcpTest,
                         testing::Values(false));
INSTANTIATE_TEST_SUITE_P(Rfc8998Peer,
                         GmMixedSocketMutualTcpTest,
                         testing::Values(false));

class GmHttpSessionTcpTest : public GmSocketTcpTest {
 protected:
  const char* PeerProtocol() const override {
    return GetParam() ? "tlcp" : "rfc8998-h2";
  }
  void FetchWithPool(bool happy_eyeballs) {
    base::test::ScopedFeatureList features;
    features.InitWithFeatureState(net::features::kHappyEyeballsV3,
                                  happy_eyeballs);
    URLRequestContextBuilder builder;
    auto resolver = std::make_unique<MockHostResolver>();
    resolver->rules()->AddRule("gm.test", "127.0.0.1");
    builder.set_host_resolver(std::move(resolver));
    builder.set_proxy_config_service(std::make_unique<ProxyConfigServiceFixed>(
        ProxyConfigWithAnnotation::CreateDirect()));
    SSLContextConfig ssl;
    ssl.gm_tls_policy =
        std::make_shared<const TlsPolicy>(std::vector<TlsRule>{*rule_});
    builder.set_ssl_config_service(std::make_unique<TestSSLConfigService>(ssl));
    auto context = builder.Build();
    const GURL url("https://gm.test:" + base::NumberToString(port_) + "/");
    for (int i = 0; i < 3; ++i) {
      TestDelegate delegate;
      auto request = context->CreateRequest(url, DEFAULT_PRIORITY, &delegate,
                                            TRAFFIC_ANNOTATION_FOR_TESTS,
                                            handles::kInvalidNetworkHandle);
      request->Start();
      delegate.RunUntilComplete();
      ASSERT_EQ(delegate.request_status(), OK);
      EXPECT_NE(delegate.data_received().find("GM TLS connection verified"),
                std::string::npos);
      EXPECT_EQ(request->response_info().was_fetched_via_spdy, !GetParam());
      EXPECT_EQ(request->ssl_info().cert_status, 0u);
      LoadTimingInfo timing;
      request->GetLoadTimingInfo(&timing);
      EXPECT_EQ(timing.socket_reused, i != 0);
    }
  }
};

TEST_P(GmHttpSessionTcpTest, LegacyPoolRoutesAndReusesAuthenticatedConnection) {
  FetchWithPool(false);
}

TEST_P(GmHttpSessionTcpTest, StreamPoolRoutesAndReusesAuthenticatedConnection) {
  FetchWithPool(true);
}

INSTANTIATE_TEST_SUITE_P(TlcpAndRfc8998, GmHttpSessionTcpTest, testing::Bool());
}  // namespace
}  // namespace net::gm
