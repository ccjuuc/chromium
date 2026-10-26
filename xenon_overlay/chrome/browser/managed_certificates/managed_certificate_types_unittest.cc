// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_types.h"

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace xenon::certificates {
namespace {

class ManagedCertificateTypesTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_.CreateUniqueTempDir());
    ASSERT_TRUE(base::PathService::Get(base::DIR_SRC_TEST_DATA_ROOT, &root_));
    ASSERT_TRUE(base::ReadFileToString(
        root_
            .AppendASCII("xenon_overlay/tools/updater/xenon_update_tool/"
                         "managed_certificates/certificates/")
            .AppendASCII("bjca-global-root-ca1.pem"),
        &pem_));
  }
  base::DictValue Document() {
    base::DictValue cert;
    cert.Set("name", "BJCA Global Root CA1");
    cert.Set("role", "trust_anchor");
    cert.Set("encoding", "pem");
    cert.Set("data", pem_);
    base::ListValue certs;
    certs.Append(std::move(cert));
    base::DictValue doc;
    doc.Set("schema_version", 1);
    doc.Set("source_id", "example");
    doc.Set("generation", "42");
    doc.Set("issued_at", "2026-10-25T06:00:00Z");
    doc.Set("certificates", std::move(certs));
    return doc;
  }
  std::string Json(const base::DictValue& dict) {
    return base::WriteJson(dict).value();
  }
  std::optional<Snapshot> Parse(const base::DictValue& dict) {
    return ParseSnapshot(Json(dict), "example", &error_);
  }
  base::DictValue& Cert(base::DictValue& doc) {
    return (*doc.FindList("certificates"))[0].GetDict();
  }
  PersistentState State() {
    PersistentState state;
    state.source_id = "example";
    state.snapshot = Parse(Document());
    state.highest_generation = state.snapshot->generation;
    state.body_hash = state.snapshot->body_hash;
    state.etag = "\"snapshot-42\"";
    state.request_context = "context";
    return state;
  }
  base::ScopedTempDir temp_;
  base::FilePath root_;
  std::string pem_;
  std::string error_;
};

TEST_F(ManagedCertificateTypesTest, RealCaPemAndDerHaveIdenticalIdentity) {
  auto doc = Document();
  auto pem = Parse(doc);
  ASSERT_TRUE(pem) << error_;
  ASSERT_EQ(pem->certificates.size(), 1u);
  EXPECT_EQ(Sha256Hex(std::string(pem->certificates[0].der.begin(),
                                  pem->certificates[0].der.end())),
            "f3896f88fe7c0a882766a7fa6ad2749fb57a7f3e98fb769c1fa7b09c2c44d5ae");
  Cert(doc).Set("encoding", "der_base64");
  Cert(doc).Set("data", base::Base64Encode(pem->certificates[0].der));
  auto der = Parse(doc);
  ASSERT_TRUE(der) << error_;
  EXPECT_EQ(pem->certificates[0].der, der->certificates[0].der);
  EXPECT_EQ(pem->certificates[0].spki, der->certificates[0].spki);
  EXPECT_NE(pem->body_hash, der->body_hash);
}

TEST_F(ManagedCertificateTypesTest, AcceptsAllSixServerAssets) {
  auto doc = Document();
  auto* certs = doc.FindList("certificates");
  certs->clear();
  for (const char* file :
       {"gdca-trustauth-e5-root.pem", "gdca-chinese-r5-root.pem",
        "bjca-global-root-ca1.pem", "bjca-global-root-ca2.pem",
        "trustasia-global-root-ca-g3.pem", "trustasia-global-root-ca-g4.pem"}) {
    std::string pem;
    ASSERT_TRUE(base::ReadFileToString(
        root_
            .AppendASCII("xenon_overlay/tools/updater/xenon_update_tool/"
                         "managed_certificates/certificates")
            .AppendASCII(file),
        &pem));
    base::DictValue cert;
    cert.Set("role", "trust_anchor");
    cert.Set("encoding", "pem");
    cert.Set("data", pem);
    certs->Append(std::move(cert));
  }
  auto result = Parse(doc);
  ASSERT_TRUE(result) << error_;
  EXPECT_EQ(result->certificates.size(), 6u);
}

TEST_F(ManagedCertificateTypesTest,
       RejectsDuplicateAndEscapedDuplicateJsonKeys) {
  EXPECT_FALSE(ParseSnapshot(R"({"schema_version":1,"schema_version":1})",
                             "example", &error_));
  auto body = Json(Document());
  body.insert(1, R"("\u0073ource_id":"example",)");
  EXPECT_FALSE(ParseSnapshot(body, "example", &error_));
  EXPECT_FALSE(ParseConfig(
      R"({"xenon_managed_ca":{"enabled":false,"enabled":true}})", &error_));
}

TEST_F(ManagedCertificateTypesTest,
       RejectsUnknownFieldsWrongSourceAndDuplicateDer) {
  auto doc = Document();
  doc.Set("new_feature", true);
  EXPECT_FALSE(Parse(doc));
  doc.Remove("new_feature");
  doc.Set("source_id", "other");
  EXPECT_FALSE(Parse(doc));
  doc.Set("source_id", "example");
  doc.FindList("certificates")->Append(Cert(doc).Clone());
  EXPECT_FALSE(Parse(doc));
}

TEST_F(ManagedCertificateTypesTest,
       RejectsChainPrivateKeyLeafAndDerTrailingData) {
  auto doc = Document();
  Cert(doc).Set("data", pem_ + pem_);
  EXPECT_FALSE(Parse(doc));
  Cert(doc).Set("data",
                "-----BEGIN PRIVATE KEY-----\nAA==\n-----END PRIVATE KEY-----");
  EXPECT_FALSE(Parse(doc));
  auto valid = Parse(Document());
  ASSERT_TRUE(valid);
  auto der = valid->certificates[0].der;
  der.push_back(0);
  Cert(doc).Set("encoding", "der_base64");
  Cert(doc).Set("data", base::Base64Encode(der));
  EXPECT_FALSE(Parse(doc));
  std::string leaf;
  ASSERT_TRUE(base::ReadFileToString(
      root_.AppendASCII("net/data/ssl/certificates/ok_cert.pem"), &leaf));
  const size_t begin = leaf.find("-----BEGIN CERTIFICATE-----");
  const size_t end = leaf.find("-----END CERTIFICATE-----", begin);
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  Cert(doc).Set("encoding", "pem");
  Cert(doc).Set("data", leaf.substr(begin, end + 25 - begin));
  EXPECT_FALSE(Parse(doc));
}

TEST_F(ManagedCertificateTypesTest, EnforcesGenerationAndTimestampFormats) {
  for (const char* bad :
       {"0", "01", "-1", "+1", "1.0", "18446744073709551616"}) {
    auto doc = Document();
    doc.Set("generation", bad);
    EXPECT_FALSE(Parse(doc)) << bad;
  }
  auto doc = Document();
  doc.Set("generation", "18446744073709551615");
  EXPECT_TRUE(Parse(doc)) << error_;
  for (const char* bad :
       {"2026-02-30T06:00:00Z", "2026-11-01T06:00:00+00:00"}) {
    doc.Set("issued_at", bad);
    EXPECT_FALSE(Parse(doc));
  }
  // Publication time is metadata, independent of the local wall clock.
  doc.Set("issued_at", "2100-01-01T00:00:00Z");
  EXPECT_TRUE(Parse(doc)) << error_;
}

TEST_F(ManagedCertificateTypesTest, LegacySnapshotDeadlineDoesNotExpireTrust) {
  auto doc = Document();
  ASSERT_TRUE(Parse(doc)) << error_;
  doc.Set("expires_at", "2000-01-01T00:00:00Z");
  EXPECT_TRUE(Parse(doc)) << error_;
  doc.Set("expires_at", "2100-01-01T00:00:00Z");
  EXPECT_TRUE(Parse(doc)) << error_;
  doc.Set("expires_at", "2026-11-01T06:00:00+00:00");
  EXPECT_FALSE(Parse(doc));
  doc.Set("expires_at", base::Value());
  EXPECT_FALSE(Parse(doc));
}

TEST_F(ManagedCertificateTypesTest,
       EmptyListIsValidWithdrawalButMissingListIsNot) {
  auto doc = Document();
  doc.FindList("certificates")->clear();
  ASSERT_TRUE(Parse(doc));
  EXPECT_TRUE(Parse(doc)->certificates.empty());
  doc.Remove("certificates");
  EXPECT_FALSE(Parse(doc));
}

TEST_F(ManagedCertificateTypesTest, ChecksNameConstraintsAndCanonicalNetworks) {
  auto doc = Document();
  base::DictValue constraints;
  base::ListValue dns;
  dns.Append(".example.com");
  constraints.Set("permitted_dns_names", std::move(dns));
  base::ListValue cidrs;
  cidrs.Append("10.20.0.0/16");
  cidrs.Append("fd00:20::/48");
  constraints.Set("permitted_cidrs", std::move(cidrs));
  Cert(doc).Set("constraints", constraints.Clone());
  EXPECT_TRUE(Parse(doc)) << error_;
  for (const char* invalid :
       {"*.example.com", "Example.com", "https://example.com", "example.com.",
        "-a.example"}) {
    base::ListValue bad;
    bad.Append(invalid);
    constraints.Set("permitted_dns_names", std::move(bad));
    Cert(doc).Set("constraints", constraints.Clone());
    EXPECT_FALSE(Parse(doc)) << invalid;
  }
  constraints.Remove("permitted_dns_names");
  for (const char* invalid : {"10.20.0.1/16", "fd00:20::1/48", "10.0.0.0/33"}) {
    base::ListValue bad;
    bad.Append(invalid);
    constraints.Set("permitted_cidrs", std::move(bad));
    Cert(doc).Set("constraints", constraints.Clone());
    EXPECT_FALSE(Parse(doc)) << invalid;
  }
  Cert(doc).Set("constraints", base::DictValue());
  EXPECT_FALSE(Parse(doc));
  Cert(doc).Set("role", "intermediate");
  EXPECT_FALSE(Parse(doc));
  Cert(doc).Remove("constraints");
  EXPECT_TRUE(Parse(doc));
}

TEST_F(ManagedCertificateTypesTest, RejectsOversizeBody) {
  EXPECT_FALSE(ParseSnapshot(std::string(kMaxSnapshotBytes + 1, ' '), "example",
                             &error_));
}

TEST_F(ManagedCertificateTypesTest, EnabledByDefaultRequiresSourceAndEndpoint) {
  auto config = ParseConfig(
      R"({"xenon_managed_ca":{"source_id":"example","endpoint":"https://example.com/ca"}})",
      &error_);
  ASSERT_TRUE(config) << error_;
  EXPECT_TRUE(config->enabled);
  EXPECT_EQ(config->source_id, "example");
  EXPECT_EQ(config->endpoint, GURL("https://example.com/ca"));

  for (const char* body : {
           R"({"xenon_managed_ca":{}})",
           R"({"xenon_managed_ca":{"source_id":"example"}})",
           R"({"xenon_managed_ca":{"endpoint":"https://example.com/ca"}})",
       }) {
    EXPECT_FALSE(ParseConfig(body, &error_));
    EXPECT_EQ(error_, "missing_source_or_endpoint");
  }
}

TEST_F(ManagedCertificateTypesTest, ExplicitDisableDoesNotRequireAnEndpoint) {
  auto config =
      ParseConfig(R"({"xenon_managed_ca":{"enabled":false}})", &error_);
  ASSERT_TRUE(config) << error_;
  EXPECT_FALSE(config->enabled);
  EXPECT_TRUE(config->source_id.empty());
  EXPECT_TRUE(config->endpoint.is_empty());

  config = ParseConfig(
      R"({"xenon_managed_ca":{"enabled":false,"source_id":"example","endpoint":"https://example.com/ca"}})",
      &error_);
  ASSERT_TRUE(config) << error_;
  EXPECT_FALSE(config->enabled);
  EXPECT_FALSE(
      ParseConfig(R"({"xenon_managed_ca":{"enabled":"false"}})", &error_));
  EXPECT_EQ(error_, "invalid_enabled");
}

TEST_F(ManagedCertificateTypesTest,
       RequiresHttpsAndExplicitSafeAuthentication) {
  for (const char* url :
       {"http://localhost/ca", "https://user:pass@example.com/ca",
        "https://example.com/ca#fragment"}) {
    base::DictValue config;
    config.Set("enabled", true);
    config.Set("source_id", "example");
    config.Set("endpoint", url);
    base::DictValue root;
    root.Set("xenon_managed_ca", std::move(config));
    EXPECT_FALSE(ParseConfig(Json(root), &error_));
  }
  EXPECT_TRUE(ParseConfig(
      R"({"xenon_managed_ca":{"enabled":true,"source_id":"example","endpoint":"https://example.com/ca","auth":{"mode":"bearer","credential_ref":"example-ca"}}})",
      &error_));
  EXPECT_FALSE(ParseConfig(
      R"({"xenon_managed_ca":{"enabled":true,"source_id":"example","endpoint":"https://example.com/ca","auth":{"mode":"bearer","token":"secret"}}})",
      &error_));
  EXPECT_FALSE(ParseConfig(
      R"({"xenon_managed_ca":{"enabled":false,"enabled_typo":true}})",
      &error_));
}

TEST_F(ManagedCertificateTypesTest,
       BootstrapBundleMustContainOnlyValidCaBlocks) {
  const auto bootstrap = temp_.GetPath().AppendASCII("bootstrap.pem");
  const auto config_path = temp_.GetPath().AppendASCII("config.json");
  base::DictValue config;
  config.Set("enabled", true);
  config.Set("source_id", "example");
  config.Set("endpoint", "https://example.com/ca");
  config.Set("bootstrap_ca_file", bootstrap.AsUTF8Unsafe());
  base::DictValue root;
  root.Set("xenon_managed_ca", std::move(config));
  ASSERT_TRUE(base::WriteFile(config_path, Json(root)));
  ASSERT_TRUE(base::WriteFile(bootstrap, pem_));
  auto result = LoadConfig(config_path, &error_);
  ASSERT_TRUE(result) << error_;
  EXPECT_EQ(result->bootstrap_certificates.size(), 1u);
  ASSERT_TRUE(
      base::WriteFile(bootstrap, pem_ + "-----BEGIN PRIVATE KEY-----\n"));
  EXPECT_FALSE(LoadConfig(config_path, &error_));
}

TEST_F(ManagedCertificateTypesTest, PreservesVersionAndMigratesLegacyCache) {
  const auto path = StatePath(temp_.GetPath(), "example");
  auto state = State();
  auto legacy_doc = Document();
  legacy_doc.Set("expires_at", "2000-01-01T00:00:00Z");
  state.snapshot = Parse(legacy_doc);
  ASSERT_TRUE(state.snapshot) << error_;
  state.body_hash = state.snapshot->body_hash;
  ASSERT_TRUE(SaveState(path, state, &error_)) << error_;
  std::string text;
  ASSERT_TRUE(base::ReadFileToString(path, &text));
  auto cache = base::JSONReader::ReadDict(text, base::JSON_PARSE_RFC);
  ASSERT_TRUE(cache);
  EXPECT_FALSE(cache->contains("expired"));
  EXPECT_FALSE(cache->contains("last_observed_time"));
  // Old expiry tombstones are obsolete; the version/body checks still apply.
  cache->Set("expired", true);
  cache->Set("last_observed_time", "2100-01-01T00:00:00Z");
  ASSERT_TRUE(base::WriteFile(path, Json(*cache)));
  auto restored = LoadState(path, "example", &error_);
  ASSERT_TRUE(restored) << error_;
  EXPECT_EQ(restored->highest_generation, 42u);
  ASSERT_TRUE(restored->snapshot);
  EXPECT_EQ(restored->snapshot->body, state.snapshot->body);
  EXPECT_EQ(restored->etag, state.etag);
  EXPECT_EQ(restored->request_context, state.request_context);
  EXPECT_FALSE(LoadState(path, "different-source", &error_));
  ASSERT_TRUE(SaveState(path, *restored, &error_));
  ASSERT_TRUE(base::ReadFileToString(path, &text));
  cache = base::JSONReader::ReadDict(text, base::JSON_PARSE_RFC);
  ASSERT_TRUE(cache);
  EXPECT_FALSE(cache->contains("expired"));
  EXPECT_FALSE(cache->contains("last_observed_time"));
  EXPECT_EQ(*cache->FindString("body"), state.snapshot->body);
}

TEST_F(ManagedCertificateTypesTest, CorruptStateCannotSilentlyResetVersion) {
  const auto path = StatePath(temp_.GetPath(), "example");
  auto missing = LoadState(path, "example", &error_);
  ASSERT_TRUE(missing);
  EXPECT_EQ(missing->highest_generation, 0u);
  ASSERT_TRUE(SaveState(path, State(), &error_));
  ASSERT_TRUE(base::WriteFile(path, "{broken"));
  EXPECT_FALSE(LoadState(path, "example", &error_));
  EXPECT_TRUE(error_.starts_with("state_corrupt"));
}

TEST_F(ManagedCertificateTypesTest, RejectsUnsafeCachedEtag) {
  const auto path = StatePath(temp_.GetPath(), "example");
  for (const char* etag : {"\"tag\t\"", "\"tag\x7f\"", "\"tag\r\n\""}) {
    auto state = State();
    state.etag = etag;
    ASSERT_TRUE(SaveState(path, state, &error_));
    EXPECT_FALSE(LoadState(path, "example", &error_));
    EXPECT_TRUE(error_.starts_with("state_corrupt"));
  }
}

}  // namespace
}  // namespace xenon::certificates
