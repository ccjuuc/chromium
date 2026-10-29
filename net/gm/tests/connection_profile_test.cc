// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>
#include <iostream>
#include <set>
#include <string_view>

#include "net/gm/gm_connection_profile.h"

namespace {

using net::gm::ConnectionProfile;
using net::gm::ProfileValidationError;
using net::gm::TlsProtocol;

class TestResults {
 public:
  void Expect(bool condition, std::string_view description) {
    ++checks_;
    if (!condition) {
      ++failures_;
      std::cerr << "FAIL: " << description << '\n';
    }
  }

  int Finish() const {
    std::cout << checks_ << " profile checks, " << failures_ << " failures\n";
    return failures_ == 0 ? 0 : 1;
  }

 private:
  int checks_ = 0;
  int failures_ = 0;
};

void CheckCapabilities(TestResults& results,
                       const ConnectionProfile& profile,
                       bool http11,
                       bool http2,
                       bool quic,
                       bool cross_origin_http2,
                       std::string_view description) {
  results.Expect(profile.AllowsHttp11() == http11 &&
                     profile.AllowsHttp2() == http2 &&
                     profile.AllowsQuic() == quic &&
                     profile.AllowsCrossOriginHttp2() == cross_origin_http2,
                 description);
}

}  // namespace

int main() {
  TestResults results;
  const ConnectionProfile standard;
  results.Expect(standard.IsValid(), "default standard profile is valid");
  results.Expect(standard == ConnectionProfile{},
                 "default profiles compare equally");
  CheckCapabilities(results, standard, true, true, true, true,
                    "ordinary HTTPS retains its protocol permissions");

  const ConnectionProfile tlcp(TlsProtocol::kTlcp, "corp-ca", 1, "", 0, 1);
  const ConnectionProfile rfc8998(TlsProtocol::kRfc8998, "corp-ca", 1, "", 0,
                                  1);
  const ConnectionProfile tls13_auto(TlsProtocol::kTls13Auto, "corp-ca", 1, "",
                                     0, 1);
  const ConnectionProfile authenticated(TlsProtocol::kRfc8998, "corp-ca", 1,
                                        "finance-key", 1, 1);
  const ConnectionProfile standard_authenticated(TlsProtocol::kStandardTls, "",
                                                 0, "finance-key", 1, 1);
  CheckCapabilities(results, tlcp, true, false, false, false,
                    "TLCP permits only HTTP/1.1");
  CheckCapabilities(results, rfc8998, true, true, false, true,
                    "RFC 8998 permits HTTP/2 but forbids QUIC");
  CheckCapabilities(results, tls13_auto, true, true, false, false,
                    "joint TLS 1.3 forbids QUIC and cross-origin HTTP/2");
  results.Expect(
      tls13_auto != rfc8998 && tls13_auto != standard,
      "joint negotiation remains distinct from strict and ordinary TLS");
  CheckCapabilities(results, authenticated, true, true, false, false,
                    "Key policy forbids cross-origin HTTP/2 and QUIC");
  CheckCapabilities(results, standard_authenticated, true, true, false, false,
                    "standard TLS cannot bypass an explicit identity policy");

  struct InvalidCase {
    ConnectionProfile profile;
    ProfileValidationError error;
    std::string_view description;
  };
  const std::array invalid_profiles = {
      InvalidCase{{static_cast<TlsProtocol>(99), "", 0, "", 0, 0},
                  ProfileValidationError::kUnknownProtocol,
                  "unknown protocol"},
      InvalidCase{{TlsProtocol::kTlcp, "corp-ca", 0, "", 0, 1},
                  ProfileValidationError::kInvalidTrustGeneration,
                  "trust ID requires a generation"},
      InvalidCase{{TlsProtocol::kTlcp, "", 1, "", 0, 1},
                  ProfileValidationError::kInvalidTrustGeneration,
                  "trust generation requires an ID"},
      InvalidCase{{TlsProtocol::kTlcp, "corp-ca", 1, "key", 0, 1},
                  ProfileValidationError::kInvalidIdentityGeneration,
                  "identity ID requires a generation"},
      InvalidCase{{TlsProtocol::kTlcp, "corp-ca", 1, "", 1, 1},
                  ProfileValidationError::kInvalidIdentityGeneration,
                  "identity generation requires an ID"},
      InvalidCase{{TlsProtocol::kTlcp, "", 0, "", 0, 1},
                  ProfileValidationError::kMissingGmTrust,
                  "GM must name its trust policy"},
      InvalidCase{{TlsProtocol::kTls13Auto, "", 0, "", 0, 1},
                  ProfileValidationError::kMissingGmTrust,
                  "joint TLS must preserve explicit GM trust"},
      InvalidCase{{TlsProtocol::kTls13Auto, "corp-ca", 1, "", 0, 0},
                  ProfileValidationError::kMissingPolicyRevision,
                  "joint TLS requires an effective policy revision"},
      InvalidCase{{TlsProtocol::kTlcp, "corp-ca", 1, "", 0, 0},
                  ProfileValidationError::kMissingPolicyRevision,
                  "GM requires an effective policy revision"},
      InvalidCase{{TlsProtocol::kStandardTls, "corp-ca", 1, "", 0, 0},
                  ProfileValidationError::kMissingPolicyRevision,
                  "managed standard trust requires a policy revision"},
  };
  for (const auto& test : invalid_profiles) {
    results.Expect(test.profile.Validate() == test.error, test.description);
    CheckCapabilities(results, test.profile, false, false, false, false,
                      test.description);
  }

  // Each row is a distinct reuse boundary. A certificate rotation, Key removal,
  // trust change or policy update must not find a previous connection/ticket.
  const std::array boundaries = {
      authenticated,
      ConnectionProfile(TlsProtocol::kStandardTls, "corp-ca", 1, "finance-key",
                        1, 1),
      ConnectionProfile(TlsProtocol::kTlcp, "corp-ca", 1, "finance-key", 1, 1),
      ConnectionProfile(TlsProtocol::kTls13Auto, "corp-ca", 1, "finance-key", 1,
                        1),
      ConnectionProfile(TlsProtocol::kRfc8998, "other-ca", 1, "finance-key", 1,
                        1),
      ConnectionProfile(TlsProtocol::kRfc8998, "corp-ca", 2, "finance-key", 1,
                        1),
      ConnectionProfile(TlsProtocol::kRfc8998, "corp-ca", 1, "other-key", 1, 1),
      ConnectionProfile(TlsProtocol::kRfc8998, "corp-ca", 1, "finance-key", 2,
                        1),
      ConnectionProfile(TlsProtocol::kRfc8998, "corp-ca", 1, "finance-key", 1,
                        2),
  };
  std::set<ConnectionProfile> cache_keys;
  for (const auto& profile : boundaries) {
    results.Expect(profile.IsValid(), "reuse-boundary profile is valid");
    results.Expect(cache_keys.insert(profile).second,
                   "distinct security policy gets a distinct ordered key");
  }
  results.Expect(!cache_keys.insert(authenticated).second,
                 "identical security policy finds its existing key");
  for (size_t i = 0; i < boundaries.size(); ++i) {
    for (size_t j = 0; j < boundaries.size(); ++j) {
      results.Expect((boundaries[i] == boundaries[j]) == (i == j),
                     "equality preserves every reuse boundary");
    }
  }

  // Revisions belong to each effective policy. Updating an unrelated GM policy
  // must not change the ordinary TLS default or force a global cache flush.
  results.Expect(standard == ConnectionProfile{},
                 "unrelated GM revisions leave ordinary TLS identity stable");
  return results.Finish();
}
