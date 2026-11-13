// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_x509_certificate.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "gin/function_template.h"
#include "third_party/boringssl/src/include/openssl/asn1.h"
#include "third_party/boringssl/src/include/openssl/bio.h"
#include "third_party/boringssl/src/include/openssl/bn.h"
#include "third_party/boringssl/src/include/openssl/digest.h"
#include "third_party/boringssl/src/include/openssl/err.h"
#include "third_party/boringssl/src/include/openssl/mem.h"
#include "third_party/boringssl/src/include/openssl/pem.h"
#include "third_party/boringssl/src/include/openssl/x509.h"
#include "v8/include/v8-array-buffer.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-typed-array.h"

namespace xenon::ipc {
namespace {

constexpr size_t kMaximumCertificateBytes = 16 * 1024 * 1024;
constexpr unsigned kAlwaysSubject = 1;
constexpr unsigned kNoWildcards = 2;
constexpr unsigned kNoPartialWildcards = 4;
constexpr unsigned kMultiLabelWildcards = 8;
constexpr unsigned kSingleLabelSubdomains = 16;
constexpr unsigned kNeverSubject = 32;

struct ClearErrors {
  ClearErrors() { ERR_clear_error(); }
  ~ClearErrors() { ERR_clear_error(); }
};

int NoPassword(char*, int, int, void*) {
  return 0;
}

void Throw(v8::Isolate* isolate,
           const char* code,
           const char* message,
           bool type_error = false) {
  auto text = gin::StringToV8(isolate, message);
  auto error =
      type_error ? v8::Exception::TypeError(text) : v8::Exception::Error(text);
  if (error.As<v8::Object>()
          ->CreateDataProperty(isolate->GetCurrentContext(),
                               gin::StringToV8(isolate, "code"),
                               gin::StringToV8(isolate, code))
          .IsNothing()) {
    return;
  }
  isolate->ThrowException(error);
}

bssl::UniquePtr<X509> ReadCertificate(gin::Arguments* args) {
  v8::Local<v8::Value> value;
  if (!args->GetNext(&value) || !value->IsArrayBufferView()) {
    Throw(args->isolate(), "ERR_INVALID_ARG_TYPE",
          "Certificate must be a Buffer, TypedArray or DataView", true);
    return {};
  }
  auto view = value.As<v8::ArrayBufferView>();
  if (view->ByteLength() > kMaximumCertificateBytes) {
    Throw(args->isolate(), "ERR_OUT_OF_RANGE", "Certificate exceeds 16 MiB");
    return {};
  }
  std::vector<uint8_t> bytes(view->ByteLength());
  if (view->CopyContents(bytes.data(), bytes.size()) != bytes.size()) {
    Throw(args->isolate(), "ERR_CRYPTO_OPERATION_FAILED",
          "Cannot read certificate bytes");
    return {};
  }
  bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(bytes.data(), bytes.size()));
  bssl::UniquePtr<X509> certificate;
  if (bio) {
    certificate.reset(
        PEM_read_bio_X509_AUX(bio.get(), nullptr, NoPassword, nullptr));
    if (!certificate) {
      ERR_clear_error();
      BIO_reset(bio.get());
      certificate.reset(d2i_X509_bio(bio.get(), nullptr));
    }
  }
  if (!certificate) {
    Throw(args->isolate(), "ERR_CRYPTO_OPERATION_FAILED",
          "Cannot parse X.509 certificate as PEM or DER");
  }
  return certificate;
}

std::string BioString(BIO* bio) {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
  if (!BIO_mem_contents(bio, &bytes, &size)) {
    return {};
  }
  return UNSAFE_BUFFERS(
      std::string(reinterpret_cast<const char*>(bytes), size));
}

std::string NameString(const X509_NAME* name) {
  bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
  constexpr unsigned long kFlags =
      ASN1_STRFLGS_ESC_2253 | ASN1_STRFLGS_ESC_CTRL |
      ASN1_STRFLGS_UTF8_CONVERT | XN_FLAG_SEP_MULTILINE | XN_FLAG_FN_SN;
  if (!bio || X509_NAME_print_ex(bio.get(), name, 0, kFlags) < 0) {
    return {};
  }
  return BioString(bio.get());
}

std::string TimeString(const ASN1_TIME* time) {
  bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
  if (!bio) {
    return {};
  }
  ASN1_TIME_print(bio.get(), time);
  return BioString(bio.get());
}

double TimeMilliseconds(const ASN1_TIME* time) {
  int64_t seconds = 0;
  return ASN1_TIME_to_posix(time, &seconds)
             ? static_cast<double>(seconds) * 1000
             : std::numeric_limits<double>::quiet_NaN();
}

std::string Fingerprint(const X509* certificate, const EVP_MD* algorithm) {
  uint8_t digest[EVP_MAX_MD_SIZE];
  unsigned size = 0;
  if (!X509_digest(certificate, algorithm, digest, &size)) {
    return {};
  }
  constexpr std::string_view kHex = "0123456789ABCDEF";
  std::string text;
  for (const auto byte : base::span(digest).first(size)) {
    if (!text.empty()) {
      text.push_back(':');
    }
    text.push_back(kHex[byte >> 4]);
    text.push_back(kHex[byte & 15]);
  }
  return text;
}

void Parse(gin::Arguments* args, X509* certificate) {
  auto* isolate = args->isolate();
  auto context = isolate->GetCurrentContext();
  auto result = v8::Object::New(isolate);
  auto put = [&](const char* key, const auto& value) {
    return result
        ->CreateDataProperty(context, gin::StringToV8(isolate, key),
                             gin::ConvertToV8(isolate, value))
        .FromMaybe(false);
  };
  bssl::UniquePtr<BIO> pem(BIO_new(BIO_s_mem()));
  bssl::UniquePtr<BIO> der(BIO_new(BIO_s_mem()));
  bssl::UniquePtr<BIGNUM> serial(
      ASN1_INTEGER_to_BN(X509_get0_serialNumber(certificate), nullptr));
  std::unique_ptr<char, decltype(&OPENSSL_free)> serial_hex(
      serial ? BN_bn2hex(serial.get()) : nullptr, OPENSSL_free);
  if (!pem || !der || !serial_hex ||
      !PEM_write_bio_X509(pem.get(), certificate) ||
      !i2d_X509_bio(der.get(), certificate)) {
    Throw(isolate, "ERR_CRYPTO_OPERATION_FAILED", "Cannot encode certificate");
    return;
  }
  auto raw = BioString(der.get());
  auto buffer = v8::ArrayBuffer::New(isolate, raw.size());
  UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(buffer->Data()), raw.size()))
      .copy_from(base::as_byte_span(raw));
  if (!put("raw", v8::Uint8Array::New(buffer, 0, raw.size()).As<v8::Value>()) ||
      !put("pem", BioString(pem.get())) ||
      !put("subject", NameString(X509_get_subject_name(certificate))) ||
      !put("issuer", NameString(X509_get_issuer_name(certificate))) ||
      !put("serialNumber", std::string(serial_hex.get())) ||
      !put("fingerprint", Fingerprint(certificate, EVP_sha1())) ||
      !put("fingerprint256", Fingerprint(certificate, EVP_sha256())) ||
      !put("fingerprint512", Fingerprint(certificate, EVP_sha512())) ||
      !put("validFrom", TimeString(X509_get0_notBefore(certificate))) ||
      !put("validTo", TimeString(X509_get0_notAfter(certificate))) ||
      !put("validFromMilliseconds",
           TimeMilliseconds(X509_get0_notBefore(certificate))) ||
      !put("validToMilliseconds",
           TimeMilliseconds(X509_get0_notAfter(certificate))) ||
      !put("ca", X509_check_ca(certificate) == 1)) {
    return;
  }
  args->Return(result);
}

bool IsPartialWildcard(const ASN1_STRING* value) {
  const int length = ASN1_STRING_length(value);
  if (length <= 0) {
    return false;
  }
  const auto name = UNSAFE_BUFFERS(std::string_view(
      reinterpret_cast<const char*>(ASN1_STRING_get0_data(value)), length));
  const auto star = name.find('*');
  return star != std::string_view::npos &&
         (star != 0 || !name.starts_with("*.") ||
          name.find('*', 1) != std::string_view::npos);
}

bool HasPartialWildcard(const X509* certificate, unsigned flags) {
  bssl::UniquePtr<GENERAL_NAMES> names(static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr)));
  bool has_dns_name = false;
  if (names) {
    for (const auto* name : names.get()) {
      if (name->type == GEN_DNS) {
        has_dns_name = true;
        if (IsPartialWildcard(name->d.dNSName)) {
          return true;
        }
      }
    }
  }
  if (has_dns_name || (flags & kNeverSubject)) {
    return false;
  }
  const auto* subject = X509_get_subject_name(certificate);
  int index = -1;
  while ((index = X509_NAME_get_index_by_NID(subject, NID_commonName, index)) >=
         0) {
    if (IsPartialWildcard(
            X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, index)))) {
      return true;
    }
  }
  return false;
}

bool HasNonDnsCommonName(const X509* certificate, unsigned flags) {
  if (flags & kNeverSubject) {
    return false;
  }
  bssl::UniquePtr<GENERAL_NAMES> names(static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr)));
  if (names) {
    for (const auto* name : names.get()) {
      if (name->type == GEN_DNS) {
        return false;
      }
    }
  }
  const auto* subject = X509_get_subject_name(certificate);
  int index = -1;
  while ((index = X509_NAME_get_index_by_NID(subject, NID_commonName, index)) >=
         0) {
    uint8_t* bytes = nullptr;
    const int length = ASN1_STRING_to_UTF8(
        &bytes, X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, index)));
    std::unique_ptr<uint8_t, decltype(&OPENSSL_free)> decoded(bytes,
                                                              OPENSSL_free);
    if (length <= 0) {
      continue;
    }
    auto name = UNSAFE_BUFFERS(std::string_view(
        reinterpret_cast<const char*>(bytes), static_cast<size_t>(length)));
    if (name.ends_with('.')) {
      name.remove_suffix(1);
    }
    if (name.starts_with("*.")) {
      name.remove_prefix(2);
    }
    if (name.empty()) {
      return true;
    }
    size_t label_start = 0;
    for (size_t position = 0; position < name.size(); ++position) {
      const char character = name[position];
      if ((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z') ||
          (character >= '0' && character <= '9') ||
          (character == '-' && position > label_start) || character == '_' ||
          character == ':') {
        continue;
      }
      if (character == '.' && position > label_start &&
          position + 1 < name.size()) {
        label_start = position + 1;
        continue;
      }
      return true;
    }
  }
  return false;
}

// OpenSSL permits subject fallback if a SAN extension has no name of the
// requested type. BoringSSL stops at any SAN extension, including IP-only SANs
// for a DNS query. Remove it only from a private copy used for this match.
bssl::UniquePtr<X509> SubjectFallbackCertificate(const X509* certificate,
                                                 int type,
                                                 unsigned flags) {
  if (flags & kNeverSubject) {
    return {};
  }
  bssl::UniquePtr<GENERAL_NAMES> names(static_cast<GENERAL_NAMES*>(
      X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr)));
  if (!names) {
    return {};
  }
  for (const auto* name : names.get()) {
    if (name->type == type) {
      return {};
    }
  }
  bssl::UniquePtr<X509> copy(X509_dup(certificate));
  if (copy) {
    int index;
    while ((index = X509_get_ext_by_NID(copy.get(), NID_subject_alt_name,
                                        -1)) >= 0) {
      X509_EXTENSION_free(X509_delete_ext(copy.get(), index));
    }
  }
  return copy;
}

void Call(gin::Arguments* args) {
  ClearErrors clear_errors;
  std::string operation;
  if (!args->GetNext(&operation)) {
    Throw(args->isolate(), "ERR_INVALID_ARG_TYPE", "Expected X.509 operation",
          true);
    return;
  }
  auto certificate = ReadCertificate(args);
  if (!certificate) {
    return;
  }
  if (operation == "parse") {
    Parse(args, certificate.get());
    return;
  }
  if (operation == "issued") {
    auto issuer = ReadCertificate(args);
    if (issuer) {
      args->Return(X509_check_issued(issuer.get(), certificate.get()) ==
                   X509_V_OK);
    }
    return;
  }
  std::string name;
  uint32_t flags = 0;
  if (!args->GetNext(&name) || !args->GetNext(&flags) || (flags & ~63u)) {
    Throw(args->isolate(), "ERR_INVALID_ARG_TYPE",
          "Invalid X.509 match arguments", true);
    return;
  }
  if (name.find('\0') != std::string::npos) {
    Throw(args->isolate(), "ERR_INVALID_ARG_VALUE",
          "Invalid certificate match name", true);
    return;
  }
  if (flags &
      (kAlwaysSubject | kMultiLabelWildcards | kSingleLabelSubdomains)) {
    Throw(args->isolate(), "ERR_NOT_SUPPORTED",
          "Requested X.509 matching options are unavailable");
    return;
  }
  if (operation == "host" &&
      (name.starts_with('.') ||
       (!(flags & (kNoWildcards | kNoPartialWildcards)) &&
        HasPartialWildcard(certificate.get(), flags)))) {
    Throw(args->isolate(), "ERR_NOT_SUPPORTED",
          "Partial wildcard and subdomain-only X.509 matching are unavailable");
    return;
  }
  // BoringSSL applies a DNS-format heuristic to common names that OpenSSL
  // does not. A decorative/Unicode CN cannot silently become a mismatch.
  if (operation == "host" && HasNonDnsCommonName(certificate.get(), flags)) {
    Throw(args->isolate(), "ERR_NOT_SUPPORTED",
          "Non-DNS certificate common-name matching is unavailable");
    return;
  }
  const unsigned native_flags =
      ((flags & kNoWildcards) ? X509_CHECK_FLAG_NO_WILDCARDS : 0) |
      ((flags & kNeverSubject) ? X509_CHECK_FLAG_NEVER_CHECK_SUBJECT : 0);
  char* peer_name = nullptr;
  int matched;
  auto fallback = operation == "host" || operation == "email"
                      ? SubjectFallbackCertificate(
                            certificate.get(),
                            operation == "host" ? GEN_DNS : GEN_EMAIL, flags)
                      : bssl::UniquePtr<X509>();
  X509* match_certificate = fallback ? fallback.get() : certificate.get();
  if (operation == "host") {
    matched = X509_check_host(match_certificate, name.data(), name.size(),
                              native_flags, &peer_name);
  } else if (operation == "email") {
    matched = X509_check_email(match_certificate, name.data(), name.size(),
                               native_flags);
  } else if (operation == "ip") {
    matched = X509_check_ip_asc(certificate.get(), name.c_str(), native_flags);
  } else {
    Throw(args->isolate(), "ERR_INVALID_ARG_VALUE", "Unknown X.509 operation",
          true);
    return;
  }
  std::unique_ptr<char, decltype(&OPENSSL_free)> peer(peer_name, OPENSSL_free);
  if (matched == 1) {
    args->Return(peer ? std::string(peer.get()) : name);
  } else if (matched < 0) {
    Throw(
        args->isolate(),
        matched == -2 ? "ERR_INVALID_ARG_VALUE" : "ERR_CRYPTO_OPERATION_FAILED",
        "Cannot match certificate name", matched == -2);
  }
  // A well-formed mismatch returns undefined, not false or a fabricated name.
}

}  // namespace

void InstallX509CertificateBridge(v8::Isolate* isolate,
                                  v8::Local<v8::Context> context,
                                  v8::Local<v8::Object> target,
                                  const char* name) {
  target
      ->CreateDataProperty(
          context, gin::StringToV8(isolate, name),
          gin::CreateFunctionTemplate(isolate, base::BindRepeating(&Call))
              ->GetFunction(context)
              .ToLocalChecked())
      .Check();
}

}  // namespace xenon::ipc
