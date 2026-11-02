// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_system_trust_win.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "base/containers/to_vector.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/numerics/safe_conversions.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/time/time.h"
#include "crypto/scoped_capi_types.h"
#include "net/test/cert_builder.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/boringssl/src/pki/extended_key_usage.h"
#include "third_party/boringssl/src/pki/input.h"

namespace gm_browser {
namespace {

class GmSystemTrustWinTest : public testing::Test {
 protected:
  GmSystemTrustWinTest()
      : roots_(CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr)),
        intermediates_(CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr)),
        disallowed_(CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr)),
        chain_(net::CertBuilder::CreateSimpleChain3()) {
    for (const auto& cert : chain_) {
      cert->SetValidity(base::Time::Now() - base::Days(1),
                        base::Time::Now() + base::Days(30));
    }
  }

  bool Add(HCERTSTORE store,
           const std::string& der,
           bool set_usage_property = false,
           const char* usage_oid = nullptr) {
    const auto bytes = base::as_byte_span(der);
    crypto::ScopedPCCERT_CONTEXT cert(
        CertCreateCertificateContext(X509_ASN_ENCODING, bytes.data(),
                                     base::checked_cast<DWORD>(bytes.size())));
    if (!cert) {
      return false;
    }
    if (set_usage_property) {
      CERT_ENHKEY_USAGE usage = {};
      if (!CertSetEnhancedKeyUsage(cert.get(), &usage)) {
        return false;
      }
      if (usage_oid &&
          !CertAddEnhancedKeyUsageIdentifier(cert.get(), usage_oid)) {
        return false;
      }
    }
    return CertAddCertificateContextToStore(store, cert.get(),
                                            CERT_STORE_ADD_ALWAYS, nullptr);
  }

  network::mojom::GmAutoConfigPtr Read() {
    return ReadGmSystemTrustFromStoresForTesting(
        roots_.get(), intermediates_.get(), disallowed_.get());
  }

  static std::vector<uint8_t> Der(net::CertBuilder* cert) {
    const std::string der = cert->GetDER();
    return base::ToVector(base::as_byte_span(der));
  }

  crypto::ScopedHCERTSTORE roots_;
  crypto::ScopedHCERTSTORE intermediates_;
  crypto::ScopedHCERTSTORE disallowed_;
  std::array<std::unique_ptr<net::CertBuilder>, 3> chain_;
};

TEST_F(GmSystemTrustWinTest, KeepsRootsIssuersAndDistrustSeparate) {
  ASSERT_TRUE(Add(roots_.get(), chain_[2]->GetDER()));
  ASSERT_TRUE(Add(intermediates_.get(), chain_[1]->GetDER()));
  ASSERT_TRUE(Add(disallowed_.get(), chain_[0]->GetDER()));
  const auto snapshot = Read();
  EXPECT_TRUE(snapshot->enabled);
  EXPECT_EQ(snapshot->trust_anchors,
            (std::vector<std::vector<uint8_t>>{Der(chain_[2].get())}));
  EXPECT_EQ(snapshot->untrusted_intermediates,
            (std::vector<std::vector<uint8_t>>{Der(chain_[1].get())}));
  EXPECT_EQ(snapshot->disallowed_certificates,
            (std::vector<std::vector<uint8_t>>{Der(chain_[0].get())}));
}

TEST_F(GmSystemTrustWinTest, CaStoreNeverCreatesAnAnchor) {
  ASSERT_TRUE(Add(intermediates_.get(), chain_[2]->GetDER()));
  const auto snapshot = Read();
  EXPECT_TRUE(snapshot->enabled);
  EXPECT_TRUE(snapshot->trust_anchors.empty());
  EXPECT_EQ(snapshot->untrusted_intermediates.size(), 1u);
}

TEST_F(GmSystemTrustWinTest, IntersectsEkuExtensionAndProperty) {
  chain_[2]->SetExtendedKeyUsages({bssl::der::Input(bssl::kServerAuth)});
  chain_[1]->SetExtendedKeyUsages({bssl::der::Input(bssl::kClientAuth)});
  ASSERT_TRUE(
      Add(roots_.get(), chain_[2]->GetDER(), true, szOID_PKIX_KP_CLIENT_AUTH));
  ASSERT_TRUE(Add(intermediates_.get(), chain_[1]->GetDER(), true,
                  szOID_PKIX_KP_SERVER_AUTH));
  const auto snapshot = Read();
  EXPECT_TRUE(snapshot->trust_anchors.empty());
  EXPECT_TRUE(snapshot->untrusted_intermediates.empty());
  // Keeping these DERs denied also blocks server-supplied issuer copies.
  EXPECT_EQ(snapshot->disallowed_certificates.size(), 2u);
}

TEST_F(GmSystemTrustWinTest, ExplicitlyEmptyEkuPropertyDeniesAllPurposes) {
  ASSERT_TRUE(Add(roots_.get(), chain_[2]->GetDER(), true));
  const auto snapshot = Read();
  EXPECT_TRUE(snapshot->trust_anchors.empty());
  EXPECT_EQ(snapshot->disallowed_certificates.size(), 1u);
}

TEST_F(GmSystemTrustWinTest, AnyUsableDuplicateAllowsServerAuth) {
  ASSERT_TRUE(
      Add(roots_.get(), chain_[2]->GetDER(), true, szOID_PKIX_KP_CLIENT_AUTH));
  ASSERT_TRUE(
      Add(roots_.get(), chain_[2]->GetDER(), true, szOID_PKIX_KP_SERVER_AUTH));
  const auto snapshot = Read();
  EXPECT_EQ(snapshot->trust_anchors.size(), 1u);
  EXPECT_TRUE(snapshot->disallowed_certificates.empty());
}

TEST_F(GmSystemTrustWinTest, DisallowedOverridesUsableRootAndIssuerCopies) {
  const auto der = chain_[2]->GetDER();
  ASSERT_TRUE(Add(roots_.get(), der));
  ASSERT_TRUE(Add(intermediates_.get(), der));
  ASSERT_TRUE(Add(disallowed_.get(), der, true, szOID_PKIX_KP_CLIENT_AUTH));
  const auto snapshot = Read();
  EXPECT_TRUE(snapshot->trust_anchors.empty());
  EXPECT_TRUE(snapshot->untrusted_intermediates.empty());
  EXPECT_EQ(snapshot->disallowed_certificates.size(), 1u);
}

TEST_F(GmSystemTrustWinTest, ExcludesExpiredRootsButKeepsExpiredDistrust) {
  chain_[2]->SetValidity(base::Time::Now() - base::Days(2),
                         base::Time::Now() - base::Days(1));
  ASSERT_TRUE(Add(roots_.get(), chain_[2]->GetDER()));
  ASSERT_TRUE(Add(disallowed_.get(), chain_[2]->GetDER()));
  const auto snapshot = Read();
  EXPECT_TRUE(snapshot->trust_anchors.empty());
  EXPECT_EQ(snapshot->disallowed_certificates.size(), 1u);
}

TEST_F(GmSystemTrustWinTest, IncompleteSnapshotCannotRetainPartialTrust) {
  ASSERT_TRUE(Add(roots_.get(), chain_[2]->GetDER()));
  const auto snapshot = ReadGmSystemTrustFromStoresForTesting(
      roots_.get(), intermediates_.get(), nullptr);
  EXPECT_TRUE(snapshot->enabled);
  EXPECT_TRUE(snapshot->trust_anchors.empty());
  EXPECT_TRUE(snapshot->untrusted_intermediates.empty());
}

TEST(GmSystemTrustMonitorTest, RearmsEveryChangeWithoutStandardVerifierAccess) {
  base::test::TaskEnvironment tasks;
  std::atomic<HANDLE> event = nullptr;
  std::atomic<int> resyncs = 0;
  base::test::TestFuture<bool> changed;
  base::ScopedClosureRunner stop(StartGmSystemTrustMonitoringForTesting(
      base::BindRepeating(
          [](std::atomic<HANDLE>* event, std::atomic<int>* resyncs, DWORD code,
             HANDLE* handle) {
            *event = *handle;
            if (code == CERT_STORE_CTRL_RESYNC) {
              ++*resyncs;
            }
            return true;
          },
          &event, &resyncs),
      changed.GetRepeatingCallback()));
  ASSERT_TRUE(changed.Take());
  ASSERT_NE(event.load(), nullptr);
  for (int change = 1; change <= 3; ++change) {
    ASSERT_TRUE(SetEvent(event.load()));
    ASSERT_TRUE(changed.Take());
    EXPECT_EQ(resyncs.load(), change);
    EXPECT_EQ(WaitForSingleObject(event.load(), 0), DWORD{WAIT_TIMEOUT});
  }
  stop.RunAndReset();
  tasks.RunUntilIdle();
  EXPECT_FALSE(changed.IsReady());
}

TEST(GmSystemTrustMonitorTest, RetainsAChangeDuringResyncAndStopsOnFailure) {
  base::test::TaskEnvironment tasks;
  std::atomic<HANDLE> event = nullptr;
  std::atomic<int> resyncs = 0;
  base::test::TestFuture<bool> changed(base::test::TestFutureMode::kQueue);
  base::ScopedClosureRunner stop(StartGmSystemTrustMonitoringForTesting(
      base::BindRepeating(
          [](std::atomic<HANDLE>* event, std::atomic<int>* resyncs, DWORD code,
             HANDLE* handle) {
            *event = *handle;
            if (code != CERT_STORE_CTRL_RESYNC) {
              return true;
            }
            const int count = ++*resyncs;
            if (count == 1) {
              // A second modification races the first synchronization.
              SetEvent(*handle);
            }
            return count != 3;
          },
          &event, &resyncs),
      changed.GetRepeatingCallback()));
  ASSERT_TRUE(changed.Take());
  ASSERT_TRUE(SetEvent(event.load()));
  EXPECT_TRUE(changed.Take());
  EXPECT_TRUE(changed.Take());
  EXPECT_EQ(resyncs.load(), 2);
  ASSERT_TRUE(SetEvent(event.load()));
  EXPECT_FALSE(changed.Take());
  ASSERT_TRUE(SetEvent(event.load()));
  stop.RunAndReset();
  tasks.RunUntilIdle();
  EXPECT_EQ(resyncs.load(), 3);
  EXPECT_FALSE(changed.IsReady());
}

TEST(GmSystemTrustMonitorTest, RegistrationFailureIsReported) {
  base::test::TaskEnvironment tasks;
  base::test::TestFuture<bool> changed;
  base::ScopedClosureRunner stop(StartGmSystemTrustMonitoringForTesting(
      base::BindRepeating([](DWORD, HANDLE*) { return false; }),
      changed.GetRepeatingCallback()));
  EXPECT_FALSE(changed.Take());
  stop.RunAndReset();
  tasks.RunUntilIdle();
}

}  // namespace
}  // namespace gm_browser
