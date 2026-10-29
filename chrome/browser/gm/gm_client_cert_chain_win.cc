// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/gm/gm_client_cert_chain_win.h"

#include <map>
#include <set>
#include <string>
#include <utility>

#include "base/containers/to_vector.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/time/time.h"
#include "crypto/scoped_capi_types.h"
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"
#include "net/cert/x509_util_win.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"
#include "third_party/boringssl/src/pki/verify_certificate_chain.h"

namespace gm_browser {
namespace {
using Der = std::vector<uint8_t>;
using Parsed = std::shared_ptr<const bssl::ParsedCertificate>;
constexpr size_t kCertificateLimit = 256 * 1024;
constexpr size_t kStoreBytesLimit = 64 * 1024 * 1024;
constexpr size_t kCandidateLimit = 8192;
constexpr size_t kChainLimit = 16;

Parsed Parse(base::span<const uint8_t> der) {
  return der.empty() || der.size() > kCertificateLimit
             ? nullptr
             : bssl::ParsedCertificate::Create(
                   net::x509_util::CreateCryptoBuffer(der), {}, nullptr);
}

std::string Name(bssl::der::Input name) {
  return {reinterpret_cast<const char*>(name.data()), name.size()};
}

bool ReadStore(HCERTSTORE store,
               std::set<Der>* candidates,
               size_t* bytes,
               size_t* count) {
  if (!store) {
    return false;
  }
  crypto::ScopedPCCERT_CONTEXT certificate;
  while (true) {
    certificate.reset(
        CertEnumCertificatesInStore(store, certificate.release()));
    if (!certificate) {
      return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
    }
    const auto der = net::x509_util::CertContextAsSpan(certificate.get());
    if (der.empty() || der.size() > kCertificateLimit ||
        ++*count > kCandidateLimit || der.size() > kStoreBytesLimit - *bytes) {
      return false;
    }
    *bytes += der.size();
    candidates->insert(base::ToVector(der));
  }
}

bool OpenStores(HCERTSTORE collection, const wchar_t* name) {
  for (DWORD location : {CERT_SYSTEM_STORE_LOCAL_MACHINE,
                         CERT_SYSTEM_STORE_LOCAL_MACHINE_GROUP_POLICY,
                         CERT_SYSTEM_STORE_LOCAL_MACHINE_ENTERPRISE,
                         CERT_SYSTEM_STORE_CURRENT_USER,
                         CERT_SYSTEM_STORE_CURRENT_USER_GROUP_POLICY}) {
    crypto::ScopedHCERTSTORE store(CertOpenStore(
        CERT_STORE_PROV_SYSTEM_REGISTRY_W, 0, 0,
        location | CERT_STORE_OPEN_EXISTING_FLAG | CERT_STORE_READONLY_FLAG,
        name));
    if (!store.get()) {
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
          error == ERROR_NO_SUCH_DOMAIN ||
          error == static_cast<DWORD>(CRYPT_E_NOT_FOUND)) {
        continue;
      }
      return false;
    }
    if (!CertAddStoreToCollection(collection, store.get(), 0, 0)) {
      return false;
    }
  }
  return true;
}

std::optional<std::vector<Der>> FindIntermediates(
    base::span<const uint8_t> signing,
    base::span<const uint8_t> encryption,
    const std::vector<Der>& existing,
    HCERTSTORE roots,
    HCERTSTORE intermediates) {
  Parsed sign = Parse(signing);
  Parsed enc = encryption.empty() ? nullptr : Parse(encryption);
  if (!sign || (!encryption.empty() && !enc) || existing.size() > kChainLimit) {
    return std::nullopt;
  }
  std::set<Der> candidates;
  size_t bytes = 0;
  size_t count = 0;
  for (const auto& der : existing) {
    if (!Parse(der)) {
      return std::nullopt;
    }
    candidates.insert(der);
    bytes += der.size();
    ++count;
  }
  if (!ReadStore(intermediates, &candidates, &bytes, &count) ||
      !ReadStore(roots, &candidates, &bytes, &count)) {
    return std::nullopt;
  }
  const auto now = base::Time::Now();
  std::map<std::string, std::vector<Parsed>> by_subject;
  for (const auto& der : candidates) {
    auto parsed = Parse(der);
    if (!parsed || !parsed->has_basic_constraints() ||
        !parsed->basic_constraints().is_ca ||
        (parsed->has_key_usage() &&
         !parsed->key_usage().AssertsBit(bssl::KEY_USAGE_BIT_KEY_CERT_SIGN))) {
      continue;
    }
    auto cert = net::X509Certificate::CreateFromBytes(der);
    if (!cert || now < cert->valid_start() || now > cert->valid_expiry()) {
      continue;
    }
    by_subject[Name(parsed->normalized_subject())].push_back(std::move(parsed));
  }
  std::vector<Parsed> pending{std::move(sign)};
  if (enc) {
    pending.push_back(std::move(enc));
  }
  std::set<Der> visited;
  std::map<const bssl::ParsedCertificate*, bool> self_signed;
  std::vector<Der> output;
  for (size_t index = 0; index < pending.size(); ++index) {
    const auto current = pending[index];
    auto found = by_subject.find(Name(current->normalized_issuer()));
    if (found == by_subject.end()) {
      continue;
    }
    for (const auto& issuer : found->second) {
      const auto& authority = current->authority_key_identifier();
      const auto& subject_key = issuer->subject_key_identifier();
      if (authority && authority->key_identifier && subject_key &&
          *authority->key_identifier != *subject_key) {
        continue;
      }
      // A same-name CA rollover is self-issued but signed by the previous key.
      // Omit only roots whose self-signature can actually be verified. Unknown
      // algorithms remain public issuer candidates;
      // sending such a root does not make it trusted by the server. Cache the
      // result so same-name chains do not repeatedly verify the same candidate.
      if (issuer->normalized_subject() == issuer->normalized_issuer()) {
        auto [known, inserted] = self_signed.try_emplace(issuer.get(), false);
        if (inserted) {
          known->second = bssl::VerifyCertificateIsSelfSigned(
              *issuer, /*cache=*/nullptr, /*errors=*/nullptr);
        }
        if (known->second) {
          continue;
        }
      }
      Der der = base::ToVector(
          net::x509_util::CryptoBufferAsSpan(issuer->cert_buffer()));
      if (!visited.insert(der).second) {
        continue;
      }
      if (output.size() == kChainLimit) {
        return std::nullopt;
      }
      output.push_back(std::move(der));
      pending.push_back(issuer);
    }
  }
  return output;
}
}  // namespace

std::optional<std::vector<std::vector<uint8_t>>>
FindGmClientCertificateIntermediates(
    base::span<const uint8_t> signing_certificate,
    base::span<const uint8_t> encryption_certificate,
    const std::vector<std::vector<uint8_t>>& existing_intermediates) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  crypto::ScopedHCERTSTORE roots(
      CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, nullptr));
  crypto::ScopedHCERTSTORE intermediates(
      CertOpenStore(CERT_STORE_PROV_COLLECTION, 0, 0, 0, nullptr));
  if (!roots.get() || !intermediates.get() ||
      !OpenStores(roots.get(), L"ROOT") ||
      !OpenStores(intermediates.get(), L"CA")) {
    return std::nullopt;
  }
  return FindIntermediates(signing_certificate, encryption_certificate,
                           existing_intermediates, roots.get(),
                           intermediates.get());
}

std::optional<std::vector<std::vector<uint8_t>>>
FindGmClientCertificateIntermediatesFromStoresForTesting(
    base::span<const uint8_t> signing_certificate,
    base::span<const uint8_t> encryption_certificate,
    const std::vector<std::vector<uint8_t>>& existing_intermediates,
    HCERTSTORE roots,
    HCERTSTORE intermediates) {
  return FindIntermediates(signing_certificate, encryption_certificate,
                           existing_intermediates, roots, intermediates);
}
}  // namespace gm_browser
