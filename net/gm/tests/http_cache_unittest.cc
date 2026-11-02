// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_cache.h"

#include "base/files/scoped_temp_dir.h"
#include "base/memory/raw_ptr.h"
#include "base/run_loop.h"
#include "net/base/cache_type.h"
#include "net/base/io_buffer.h"
#include "net/base/load_flags.h"
#include "net/base/test_completion_callback.h"
#include "net/gm/gm_tls_policy.h"
#include "net/http/http_network_session.h"
#include "net/http/http_transaction_test_util.h"
#include "net/http/no_vary_search_cache_storage_file_operations.h"
#include "net/log/net_log_with_source.h"
#include "net/spdy/spdy_test_util_common.h"
#include "net/ssl/test_ssl_config_service.h"
#include "net/test/test_with_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace net::gm {
namespace {
class SessionNetworkLayer : public HttpTransactionFactory {
 public:
  explicit SessionNetworkLayer(HttpNetworkSession* session)
      : session_(session) {}
  std::unique_ptr<HttpTransaction> CreateTransaction(
      RequestPriority priority) override {
    auto transaction = mock_.CreateTransaction(priority);
    // Inject the handshake marker on the in-memory mock response. It is not a
    // persisted cache field and cannot be inferred from a cipher number.
    const_cast<HttpResponseInfo*>(transaction->GetResponseInfo())
        ->ssl_info.gm_tls = gm_response;
    return transaction;
  }
  HttpCache* GetCache() override { return nullptr; }
  HttpNetworkSession* GetSession() override { return session_; }
  int transaction_count() const { return mock_.transaction_count(); }
  bool gm_response = false;

 private:
  MockNetworkLayer mock_;
  raw_ptr<HttpNetworkSession> session_;
};

class GmHttpCacheTest : public TestWithTaskEnvironment,
                        public testing::WithParamInterface<bool> {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.CreateUniqueTempDir());
    auto service = std::make_unique<TestSSLConfigService>(SSLContextConfig());
    service_ = service.get();
    dependencies_.ssl_config_service = std::move(service);
    session_ = SpdySessionDependencies::SpdyCreateSession(&dependencies_);
    auto layer = std::make_unique<SessionNetworkLayer>(session_.get());
    layer_ = layer.get();
    std::unique_ptr<HttpCache::BackendFactory> factory;
    if (GetParam()) {
      factory = std::make_unique<HttpCache::DefaultBackend>(
          DISK_CACHE, CACHE_BACKEND_SIMPLE, nullptr, directory_.GetPath(),
          1024 * 1024, false, nullptr);
    } else {
      factory = HttpCache::DefaultBackend::InMemory(1024 * 1024);
    }
    cache_ = std::make_unique<HttpCache>(std::move(layer), std::move(factory),
                                         nullptr);
  }
  void SetPolicy(bool enabled) {
    SSLContextConfig config;
    if (enabled) {
      TlsRule rule;
      rule.host = "gm.example";
      rule.profile =
          ConnectionProfile(TlsProtocol::kTlcp, "enterprise", 1, "", 0, 1);
      rule.trust_anchors = {{0x30, 0}};
      config.gm_tls_policy =
          std::make_shared<const TlsPolicy>(std::vector<TlsRule>{rule});
    }
    service_->UpdateSSLConfigAndNotify(config);
  }
  std::string Fetch(const MockTransaction& transaction) {
    MockHttpRequest request(transaction);
    TestTransactionConsumer consumer(DEFAULT_PRIORITY, cache_.get());
    consumer.Start(&request, NetLogWithSource());
    EXPECT_EQ(consumer.error(), OK);
    return consumer.content();
  }
  base::ScopedTempDir directory_;
  SpdySessionDependencies dependencies_;
  raw_ptr<TestSSLConfigService> service_ = nullptr;
  std::unique_ptr<HttpNetworkSession> session_;
  raw_ptr<SessionNetworkLayer> layer_ = nullptr;
  std::unique_ptr<HttpCache> cache_;
};

TEST_P(GmHttpCacheTest,
       ManagedRequestsBypassOrdinaryCacheAndNeverWriteGmBodies) {
  ScopedMockTransaction transaction(kSimpleGET_Transaction,
                                    "https://gm.example/cache");
  transaction.data = "ordinary-before-policy";
  EXPECT_EQ(Fetch(transaction), "ordinary-before-policy");
  EXPECT_EQ(Fetch(transaction), "ordinary-before-policy");
  EXPECT_EQ(layer_->transaction_count(), 1);
  SetPolicy(true);
  transaction.data = "gm-policy-response";
  EXPECT_EQ(Fetch(transaction), "gm-policy-response");
  EXPECT_EQ(Fetch(transaction), "gm-policy-response");
  EXPECT_EQ(layer_->transaction_count(), 3);
  MockHttpRequest cache_only(transaction);
  cache_only.load_flags = LOAD_ONLY_FROM_CACHE;
  auto reader = cache_->CreateTransaction(DEFAULT_PRIORITY);
  TestCompletionCallback miss;
  EXPECT_EQ(miss.GetResult(reader->Start(&cache_only, miss.callback(),
                                         NetLogWithSource())),
            ERR_CACHE_MISS);
  EXPECT_EQ(layer_->transaction_count(), 3);
}

TEST_P(GmHttpCacheTest, RemovingPolicyCannotReuseGmResponseFromCache) {
  ScopedMockTransaction transaction(kSimpleGET_Transaction,
                                    "https://gm.example/fresh");
  SetPolicy(true);
  transaction.data = "gm-private-response";
  EXPECT_EQ(Fetch(transaction), "gm-private-response");
  EXPECT_EQ(layer_->transaction_count(), 1);
  SetPolicy(false);
  transaction.data = "ordinary-after-policy";
  EXPECT_EQ(Fetch(transaction), "ordinary-after-policy");
  EXPECT_EQ(layer_->transaction_count(), 2);
  EXPECT_EQ(Fetch(transaction), "ordinary-after-policy");
  EXPECT_EQ(layer_->transaction_count(), 2);
}

TEST_P(GmHttpCacheTest, FirstDiscoveredGmResponseIsNeverStored) {
  ScopedMockTransaction transaction(kSimpleGET_Transaction,
                                    "https://gm.example/discovered");
  layer_->gm_response = true;
  transaction.data = "discovered-gm-body";
  EXPECT_EQ(Fetch(transaction), "discovered-gm-body");
  EXPECT_EQ(Fetch(transaction), "discovered-gm-body");
  EXPECT_EQ(layer_->transaction_count(), 2);
  layer_->gm_response = false;
  transaction.data = "ordinary-body";
  EXPECT_EQ(Fetch(transaction), "ordinary-body");
  EXPECT_EQ(layer_->transaction_count(), 3);
}

TEST_P(GmHttpCacheTest, GmNotModifiedCannotMergeOrdinaryCachedBody) {
  ScopedMockTransaction transaction(kETagGET_Transaction,
                                    "https://gm.example/revalidate");
  transaction.data = "ordinary-cached-body";
  EXPECT_EQ(Fetch(transaction), "ordinary-cached-body");
  layer_->gm_response = true;
  transaction.status = "HTTP/1.1 304 Not Modified";
  transaction.data = "";
  transaction.load_flags = LOAD_VALIDATE_CACHE;
  EXPECT_EQ(Fetch(transaction), "");
  EXPECT_EQ(layer_->transaction_count(), 2);
  MockHttpRequest cache_only(transaction);
  cache_only.load_flags = LOAD_ONLY_FROM_CACHE;
  auto reader = cache_->CreateTransaction(DEFAULT_PRIORITY);
  TestCompletionCallback miss;
  EXPECT_EQ(miss.GetResult(reader->Start(&cache_only, miss.callback(),
                                         NetLogWithSource())),
            ERR_CACHE_MISS);
}

TEST_P(GmHttpCacheTest, PolicyChangeWhileReadingCachedBodyFailsClosed) {
  ScopedMockTransaction transaction(kSimpleGET_Transaction,
                                    "https://gm.example/race");
  transaction.data = "cached-ordinary-body";
  EXPECT_EQ(Fetch(transaction), "cached-ordinary-body");
  MockHttpRequest request(transaction);
  auto reader = cache_->CreateTransaction(DEFAULT_PRIORITY);
  TestCompletionCallback start;
  ASSERT_EQ(start.GetResult(
                reader->Start(&request, start.callback(), NetLogWithSource())),
            OK);
  SetPolicy(true);
  auto buffer = base::MakeRefCounted<IOBufferWithSize>(128);
  TestCompletionCallback read;
  EXPECT_EQ(read.GetResult(
                reader->Read(buffer.get(), buffer->size(), read.callback())),
            ERR_NETWORK_CHANGED);
}

INSTANTIATE_TEST_SUITE_P(MemoryAndDisk, GmHttpCacheTest, testing::Bool());
}  // namespace
}  // namespace net::gm
