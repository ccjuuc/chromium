// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_web_request_filter.h"

#include "base/memory/scoped_refptr.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_response_headers.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-shared.h"
#include "url/gurl.h"

namespace xenon::ipc {
namespace {
TEST(ElectronWebRequestFilterTest, DefaultsAndEmptyUrlsMatchActualSchemes) {
  ElectronWebRequestFilter filter;
  std::string error;
  ASSERT_TRUE(filter.Parse({}, &error));
  EXPECT_TRUE(filter.Matches(GURL("https://example.test/"), "script"));
  EXPECT_TRUE(filter.Matches(GURL("file:///app/index.html"), "mainFrame"));
  EXPECT_TRUE(filter.Matches(GURL("fixture://host/path"), "xhr"));
  EXPECT_FALSE(filter.Matches(GURL(), "script"));
  ASSERT_TRUE(
      filter.Parse(base::DictValue().Set("urls", base::ListValue()), &error));
  EXPECT_TRUE(filter.Matches(GURL("https://example.test/"), "xhr"));
}

TEST(ElectronWebRequestFilterTest, FilePatternAndExcludeUseChromiumSemantics) {
  ElectronWebRequestFilter filter;
  std::string error;
  ASSERT_TRUE(
      filter.Parse(base::DictValue()
                       .Set("urls", base::ListValue().Append("file://*"))
                       .Set("excludeUrls",
                            base::ListValue().Append("file:///app/private/*")),
                   &error));
  EXPECT_TRUE(filter.Matches(GURL("file:///app/index.html"), "mainFrame"));
  EXPECT_FALSE(filter.Matches(GURL("file:///app/private/key"), "xhr"));
  EXPECT_FALSE(
      filter.Matches(GURL("https://example.test/app/index.html"), "mainFrame"));
}

TEST(ElectronWebRequestFilterTest, HostPortPathAndTypesAllConstrainMatching) {
  ElectronWebRequestFilter filter;
  std::string error;
  ASSERT_TRUE(filter.Parse(
      base::DictValue()
          .Set("urls",
               base::ListValue().Append("https://*.example.test:8443/api/*"))
          .Set("types", base::ListValue().Append("xhr")),
      &error));
  EXPECT_TRUE(filter.Matches(
      GURL("https://sub.example.test:8443/api/items?q=1"), "xhr"));
  EXPECT_TRUE(
      filter.Matches(GURL("https://example.test:8443/api/items"), "xhr"));
  EXPECT_FALSE(
      filter.Matches(GURL("https://badexample.test:8443/api/items"), "xhr"));
  EXPECT_FALSE(filter.Matches(GURL("https://example.test/api/items"), "xhr"));
  EXPECT_FALSE(
      filter.Matches(GURL("https://example.test:8443/api/items"), "script"));
}

TEST(ElectronWebRequestFilterTest,
     InvalidRegistrationDoesNotReplaceGoodFilter) {
  ElectronWebRequestFilter filter;
  std::string error;
  ASSERT_TRUE(filter.Parse(
      base::DictValue().Set("urls",
                            base::ListValue().Append("https://example.test/*")),
      &error));
  for (auto value :
       {"http://*", "http://*example.test/*", "http://example.test:99999/*"}) {
    EXPECT_FALSE(filter.Parse(
        base::DictValue().Set("urls", base::ListValue().Append(value)),
        &error));
  }
  EXPECT_FALSE(filter.Parse(
      base::DictValue().Set("types", base::ListValue().Append("invented")),
      &error));
  EXPECT_TRUE(filter.Matches(GURL("https://example.test/path"), "xhr"));
  EXPECT_FALSE(filter.Matches(GURL("https://other.test/path"), "xhr"));
}

TEST(ElectronWebRequestFilterTest, ResourceTypesUseActualRequestMetadata) {
  network::ResourceRequest request;
  request.destination = network::mojom::RequestDestination::kDocument;
  EXPECT_EQ("mainFrame", ElectronWebRequestResourceType(request));
  request.destination = network::mojom::RequestDestination::kIframe;
  EXPECT_EQ("subFrame", ElectronWebRequestResourceType(request));
  request.destination = network::mojom::RequestDestination::kServiceWorker;
  EXPECT_EQ("script", ElectronWebRequestResourceType(request));
  request.destination = network::mojom::RequestDestination::kEmpty;
  request.resource_type = static_cast<int>(blink::mojom::ResourceType::kPing);
  EXPECT_EQ("ping", ElectronWebRequestResourceType(request));
}

TEST(ElectronWebRequestFilterTest,
     ExplicitWebSocketInterceptionIsNotPretended) {
  ElectronWebRequestFilter filter;
  std::string error;
  EXPECT_FALSE(filter.Parse(
      base::DictValue().Set("urls",
                            base::ListValue().Append("wss://example.test/*")),
      &error));
  EXPECT_TRUE(error.starts_with("ERR_NOT_SUPPORTED:"));
  EXPECT_FALSE(filter.Parse(
      base::DictValue().Set("types", base::ListValue().Append("webSocket")),
      &error));
  EXPECT_TRUE(error.starts_with("ERR_NOT_SUPPORTED:"));
}

TEST(ElectronWebRequestHeadersTest, RealHeadersPreserveMultivalueCookies) {
  auto original =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 200 OK");
  original->AddHeader("Set-Cookie", "a=1");
  original->AddHeader("set-cookie", "b=2");
  original->AddHeader("Content-Type", "text/html");
  auto details = ElectronWebRequestHeaders(*original);
  ASSERT_TRUE(details.FindList("set-cookie"));
  EXPECT_EQ(base::ListValue().Append("a=1").Append("b=2"),
            *details.FindList("set-cookie"));
  scoped_refptr<net::HttpResponseHeaders> replacement;
  std::string error;
  ASSERT_TRUE(BuildElectronWebRequestHeaders(
      *original, base::DictValue().Set("responseHeaders", std::move(details)),
      &replacement, &error));
  EXPECT_EQ(ElectronWebRequestHeaders(*original),
            ElectronWebRequestHeaders(*replacement));
}

TEST(ElectronWebRequestHeadersTest, OmittedRetainsAndEmptyDictionaryDeletes) {
  auto original =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 200 OK");
  original->AddHeader("Content-Security-Policy", "script-src 'none'");
  scoped_refptr<net::HttpResponseHeaders> replacement;
  std::string error;
  ASSERT_TRUE(
      BuildElectronWebRequestHeaders(*original, {}, &replacement, &error));
  EXPECT_FALSE(replacement);
  ASSERT_TRUE(BuildElectronWebRequestHeaders(
      *original, base::DictValue().Set("responseHeaders", base::DictValue()),
      &replacement, &error));
  EXPECT_FALSE(replacement->HasHeader("Content-Security-Policy"));
  EXPECT_TRUE(original->HasHeader("Content-Security-Policy"));
}

TEST(ElectronWebRequestHeadersTest, ReplacesCspAndStatusAtomically) {
  auto original =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 200 OK");
  original->AddHeader("Content-Security-Policy", "script-src 'none'");
  scoped_refptr<net::HttpResponseHeaders> replacement;
  std::string error;
  ASSERT_TRUE(BuildElectronWebRequestHeaders(
      *original,
      base::DictValue()
          .Set("statusLine", "HTTP/1.1 201 Created")
          .Set("responseHeaders",
               base::DictValue()
                   .Set("content-security-policy",
                        base::ListValue().Append("script-src 'self'"))
                   .Set("access-control-allow-origin", "https://source.test")),
      &replacement, &error));
  EXPECT_EQ(201, replacement->response_code());
  EXPECT_EQ("script-src 'self'",
            replacement->GetNormalizedHeader("Content-Security-Policy"));
  EXPECT_EQ("script-src 'none'",
            original->GetNormalizedHeader("Content-Security-Policy"));
}

TEST(ElectronWebRequestHeadersTest,
     RejectsInjectionDuplicatesAndExcessiveHeaders) {
  auto original =
      base::MakeRefCounted<net::HttpResponseHeaders>("HTTP/1.1 200 OK");
  scoped_refptr<net::HttpResponseHeaders> replacement;
  std::string error;
  for (auto status : {"invalid", "HTTP/1.1 000 Bad", "HTTP/z 200 OK",
                      "HTTP/1.1 200 OK\r\nInjected: true"}) {
    EXPECT_FALSE(BuildElectronWebRequestHeaders(
        *original, base::DictValue().Set("statusLine", status), &replacement,
        &error));
    EXPECT_FALSE(replacement);
  }
  auto invalid = [&](base::DictValue headers) {
    EXPECT_FALSE(BuildElectronWebRequestHeaders(
        *original, base::DictValue().Set("responseHeaders", std::move(headers)),
        &replacement, &error));
    EXPECT_FALSE(replacement);
  };
  invalid(base::DictValue().Set("bad name", "value"));
  invalid(base::DictValue().Set("x-value", "ok\r\nInjected: true"));
  invalid(base::DictValue().Set("x-value", 5));
  invalid(base::DictValue().Set("Content-Type", "a").Set("content-type", "b"));
  invalid(base::DictValue().Set("x-huge", std::string(256 * 1024, 'x')));
}

TEST(ElectronWebRequestRequestHeadersTest, ActualHeadersAreAStringDictionary) {
  net::HttpRequestHeaders original;
  original.SetHeader("Cookie", "session=a; preference=b");
  original.SetHeader("Authorization", "Bearer original-token");
  original.SetHeader("X-Empty", "");
  const auto details = ElectronWebRequestHeaders(original);
  EXPECT_EQ(base::DictValue()
                .Set("Cookie", "session=a; preference=b")
                .Set("Authorization", "Bearer original-token")
                .Set("X-Empty", ""),
            details);

  std::optional<net::HttpRequestHeaders> replacement;
  std::string error;
  ASSERT_TRUE(BuildElectronWebRequestRequestHeaders(
      base::DictValue().Set("requestHeaders", details.Clone()), &replacement,
      &error));
  ASSERT_TRUE(replacement);
  EXPECT_EQ(details, ElectronWebRequestHeaders(*replacement));
}

TEST(ElectronWebRequestRequestHeadersTest,
     OmittedRetainsAndEmptyDictionaryReplacesEveryHeader) {
  net::HttpRequestHeaders original;
  original.SetHeader("Authorization", "Bearer original-token");
  std::optional<net::HttpRequestHeaders> replacement(original);
  std::string error;
  ASSERT_TRUE(BuildElectronWebRequestRequestHeaders({}, &replacement, &error));
  EXPECT_FALSE(replacement);
  ASSERT_TRUE(BuildElectronWebRequestRequestHeaders(
      base::DictValue().Set("requestHeaders", base::DictValue()), &replacement,
      &error));
  ASSERT_TRUE(replacement);
  EXPECT_TRUE(replacement->IsEmpty());
  EXPECT_EQ("Bearer original-token", original.GetHeader("Authorization"));
}

TEST(ElectronWebRequestRequestHeadersTest,
     ReplacementCanDeleteModifyAndAddWithoutMutatingOriginal) {
  net::HttpRequestHeaders original;
  original.SetHeader("Authorization", "Bearer original-token");
  original.SetHeader("Cookie", "session=original");
  original.SetHeader("X-Retained", "unchanged");
  auto details = ElectronWebRequestHeaders(original);
  details.Remove("Authorization");
  details.Set("Cookie", "session=replacement");
  details.Set("X-New", "added");
  std::optional<net::HttpRequestHeaders> replacement;
  std::string error;
  ASSERT_TRUE(BuildElectronWebRequestRequestHeaders(
      base::DictValue().Set("requestHeaders", std::move(details)), &replacement,
      &error));
  ASSERT_TRUE(replacement);
  EXPECT_FALSE(replacement->HasHeader("Authorization"));
  EXPECT_EQ("session=replacement", replacement->GetHeader("Cookie"));
  EXPECT_EQ("unchanged", replacement->GetHeader("X-Retained"));
  EXPECT_EQ("added", replacement->GetHeader("x-new"));
  EXPECT_EQ("Bearer original-token", original.GetHeader("Authorization"));
  EXPECT_EQ("session=original", original.GetHeader("Cookie"));
  EXPECT_FALSE(original.HasHeader("X-New"));
}

TEST(ElectronWebRequestRequestHeadersTest,
     RejectsMalformedReplacementWithoutPartialHeaders) {
  const auto invalid = [](base::Value headers) {
    // A failed conversion must clear even a prior successful output. Valid
    // dictionary entries must not survive alongside an invalid later entry.
    std::optional<net::HttpRequestHeaders> replacement(std::in_place);
    replacement->SetHeader("Previous-Output", "must-not-survive");
    std::string error;
    EXPECT_FALSE(BuildElectronWebRequestRequestHeaders(
        base::DictValue().Set("requestHeaders", std::move(headers)),
        &replacement, &error));
    EXPECT_FALSE(replacement);
    EXPECT_TRUE(error.starts_with("ERR_INVALID_ARG_VALUE:"));
  };
  invalid(base::Value());
  invalid(base::Value("not a dictionary"));
  invalid(base::Value(5));
  invalid(base::Value(base::ListValue()));

  const auto invalid_entry = [&](std::string name, base::Value value) {
    invalid(base::Value(base::DictValue()
                            .Set("A-Valid", "must-not-partially-survive")
                            .Set(std::move(name), std::move(value))));
  };
  invalid_entry("bad name", base::Value("value"));
  invalid_entry("", base::Value("value"));
  invalid_entry(std::string("x-bad\0name", 10), base::Value("value"));
  invalid_entry("x-value", base::Value("ok\r\nInjected: true"));
  invalid_entry("x-value", base::Value(std::string("a\0b", 3)));
  invalid_entry("x-value", base::Value(5));
  invalid_entry("x-value", base::Value(true));
  invalid_entry("x-value", base::Value());
  invalid_entry("x-value", base::Value(base::DictValue()));
  // The callback array form is not implemented: fail explicitly instead of
  // silently dropping a Cookie/Authorization header or inventing a separator.
  invalid_entry("x-value", base::Value(base::ListValue()));
  invalid_entry("Cookie", base::Value(base::ListValue().Append("a=1")));
  invalid(base::Value(base::DictValue()
                          .Set("A-Valid", "must-not-partially-survive")
                          .Set("Authorization", "Bearer one")
                          .Set("authorization", "Bearer two")));
}

TEST(ElectronWebRequestRequestHeadersTest, EnforcesAggregateHeaderByteLimit) {
  constexpr size_t kLimit = 256 * 1024;
  // Every header contributes its name, value, colon/space and CRLF.
  auto at_limit = base::DictValue()
                      .Set("x-one", std::string(kLimit / 2 - 9, 'a'))
                      .Set("x-two", std::string(kLimit / 2 - 9, 'b'));
  std::optional<net::HttpRequestHeaders> replacement;
  std::string error;
  ASSERT_TRUE(BuildElectronWebRequestRequestHeaders(
      base::DictValue().Set("requestHeaders", at_limit.Clone()), &replacement,
      &error));
  ASSERT_TRUE(replacement);
  EXPECT_EQ(2u, replacement->GetHeaderVector().size());

  at_limit.Set("x-two", std::string(kLimit / 2 - 8, 'b'));
  EXPECT_FALSE(BuildElectronWebRequestRequestHeaders(
      base::DictValue().Set("requestHeaders", std::move(at_limit)), &replacement,
      &error));
  EXPECT_FALSE(replacement);
  EXPECT_TRUE(error.starts_with("ERR_INVALID_ARG_VALUE:"));
}
}  // namespace
}  // namespace xenon::ipc
