// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_types.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

#include "base/base64.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/values.h"
#include "build/build_config.h"
#include "crypto/hash.h"
#include "net/cert/x509_util.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"

namespace xenon::certificates {
namespace {

constexpr size_t kMaxCertificates = 256;
constexpr size_t kMaxDerBytes = 16 * 1024;
constexpr std::string_view kPemBegin = "-----BEGIN CERTIFICATE-----";
constexpr std::string_view kPemEnd = "-----END CERTIFICATE-----";

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    *error = message;
  }
  return false;
}

// JSONReader validates the grammar but deliberately discards duplicate keys.
// Run this bounded second pass on validated input, decoding escaped keys before
// comparing them, so {"name":..., "\u006eame":...} is rejected as well.
class UniqueKeys {
 public:
  explicit UniqueKeys(std::string_view input) : input_(input) {}
  bool Check() { return Value(); }

 private:
  void Space() {
    while (pos_ < input_.size() && base::IsAsciiWhitespace(input_[pos_])) {
      ++pos_;
    }
  }
  std::optional<std::string> String() {
    Space();
    const size_t start = pos_++;
    while (pos_ < input_.size()) {
      const char c = input_[pos_++];
      if (c == '\\') {
        ++pos_;
      } else if (c == '"') {
        auto value = base::JSONReader::Read(input_.substr(start, pos_ - start),
                                            base::JSON_PARSE_RFC);
        if (!value || !value->is_string()) {
          return std::nullopt;
        }
        return value->GetString();
      }
    }
    return std::nullopt;
  }
  bool Value() {
    Space();
    if (pos_ >= input_.size()) {
      return false;
    }
    if (input_[pos_] == '"') {
      return String().has_value();
    }
    if (input_[pos_] == '{') {
      ++pos_;
      Space();
      std::set<std::string> keys;
      while (pos_ < input_.size() && input_[pos_] != '}') {
        auto key = String();
        if (!key || !keys.insert(*key).second) {
          return false;
        }
        Space();
        ++pos_;  // ':' (grammar was checked by JSONReader).
        if (!Value()) {
          return false;
        }
        Space();
        if (pos_ < input_.size() && input_[pos_] == ',') {
          ++pos_;
        }
        Space();
      }
      ++pos_;
      return true;
    }
    if (input_[pos_] == '[') {
      ++pos_;
      Space();
      while (pos_ < input_.size() && input_[pos_] != ']') {
        if (!Value()) {
          return false;
        }
        Space();
        if (pos_ < input_.size() && input_[pos_] == ',') {
          ++pos_;
        }
        Space();
      }
      ++pos_;
      return true;
    }
    while (pos_ < input_.size() && input_[pos_] != ',' && input_[pos_] != ']' &&
           input_[pos_] != '}' && !base::IsAsciiWhitespace(input_[pos_])) {
      ++pos_;
    }
    return true;
  }
  std::string_view input_;
  size_t pos_ = 0;
};

std::optional<base::DictValue> ReadObject(std::string_view body,
                                          size_t limit,
                                          std::string* error) {
  if (body.size() > limit || body.starts_with("\xef\xbb\xbf")) {
    Fail(error, "json_size_or_encoding");
    return std::nullopt;
  }
  auto value = base::JSONReader::ReadDict(body, base::JSON_PARSE_RFC, 16);
  if (!value || !UniqueKeys(body).Check()) {
    Fail(error, "invalid_json_or_duplicate_key");
    return std::nullopt;
  }
  return value;
}

bool Fields(const base::DictValue& dict,
            std::initializer_list<std::string_view> allowed) {
  for (const auto [key, value] : dict) {
    if (!std::ranges::contains(allowed, key)) {
      return false;
    }
  }
  return true;
}

bool SourceId(std::string_view value) {
  return !value.empty() && value.size() <= 64 &&
         std::ranges::all_of(value, [](char c) {
           return base::IsAsciiAlpha(c) || base::IsAsciiDigit(c) || c == '.' ||
                  c == '_' || c == '-';
         });
}

bool Generation(const std::string* value, uint64_t* result) {
  return value && !value->empty() && value->front() != '0' &&
         std::ranges::all_of(*value,
                             [](char c) { return base::IsAsciiDigit(c); }) &&
         base::StringToUint64(*value, result) && *result != 0;
}

std::string FormatTime(base::Time time) {
  base::Time::Exploded parts;
  time.UTCExplode(&parts);
  return base::StringPrintf("%04d-%02d-%02dT%02d:%02d:%02dZ", parts.year,
                            parts.month, parts.day_of_month, parts.hour,
                            parts.minute, parts.second);
}

bool ParseTime(const std::string* text, base::Time* result) {
  if (!text || text->size() != 20 || (*text)[4] != '-' || (*text)[7] != '-' ||
      (*text)[10] != 'T' || (*text)[13] != ':' || (*text)[16] != ':' ||
      (*text)[19] != 'Z') {
    return false;
  }
  constexpr size_t positions[] = {0, 5, 8, 11, 14, 17};
  for (size_t position : positions) {
    const auto part =
        std::string_view(*text).substr(position, position == 0 ? 4 : 2);
    if (!std::ranges::all_of(part,
                             [](char c) { return base::IsAsciiDigit(c); })) {
      return false;
    }
  }
  return base::Time::FromUTCString(text->c_str(), result) &&
         FormatTime(*result) == *text;
}

bool DecodeCertificate(std::string_view data,
                       std::string_view encoding,
                       Certificate* out,
                       std::string* error) {
  std::string encoded;
  if (encoding == "pem") {
    data = base::TrimWhitespaceASCII(data, base::TRIM_ALL);
    if (!data.starts_with(kPemBegin) || !data.ends_with(kPemEnd)) {
      return Fail(error, "invalid_pem");
    }
    data.remove_prefix(kPemBegin.size());
    data.remove_suffix(kPemEnd.size());
    base::RemoveChars(data, " \t\r\n", &encoded);
    data = encoded;
  } else if (encoding != "der_base64") {
    return Fail(error, "unsupported_encoding");
  }
  std::string der;
  if (!base::Base64Decode(data, &der) || der.empty() ||
      der.size() > kMaxDerBytes || base::Base64Encode(der) != data) {
    return Fail(error, "invalid_base64_or_certificate_size");
  }
  auto parsed = bssl::ParsedCertificate::Create(
      net::x509_util::CreateCryptoBuffer(der), {}, nullptr);
  if (!parsed || parsed->der_cert().size() != der.size() ||
      !parsed->signature_algorithm() || !parsed->has_basic_constraints() ||
      !parsed->basic_constraints().is_ca ||
      (parsed->has_key_usage() &&
       !parsed->key_usage().AssertsBit(bssl::KEY_USAGE_BIT_KEY_CERT_SIGN))) {
    return Fail(error, "invalid_ca_certificate");
  }
  out->der.assign(der.begin(), der.end());
  auto spki = parsed->tbs().spki_tlv;
  out->spki.assign(spki.begin(), spki.end());
  return true;
}

bool DnsName(std::string_view value) {
  if (value.starts_with('.')) {
    value.remove_prefix(1);
  }
  if (value.empty() || value.size() > 253 || value.ends_with('.')) {
    return false;
  }
  for (auto label : base::SplitStringPiece(value, ".", base::KEEP_WHITESPACE,
                                           base::SPLIT_WANT_ALL)) {
    if (label.empty() || label.size() > 63 || label.front() == '-' ||
        label.back() == '-' || !std::ranges::all_of(label, [](char c) {
          return (c >= 'a' && c <= 'z') || base::IsAsciiDigit(c) || c == '-';
        })) {
      return false;
    }
  }
  return true;
}

bool Constraints(const base::Value* value,
                 Certificate* cert,
                 std::string* error) {
  if (!value) {
    return true;
  }
  if (!cert->trust_anchor || !value->is_dict() ||
      !Fields(value->GetDict(), {"permitted_dns_names", "permitted_cidrs"})) {
    return Fail(error, "invalid_constraints");
  }
  const auto& dict = value->GetDict();
  for (const char* key : {"permitted_dns_names", "permitted_cidrs"}) {
    const auto* item = dict.Find(key);
    if (!item) {
      continue;
    }
    if (!item->is_list() || item->GetList().size() > 64) {
      return Fail(error, "invalid_constraint_list");
    }
    std::set<std::string> unique;
    for (const auto& entry : item->GetList()) {
      if (!entry.is_string() || !unique.insert(entry.GetString()).second) {
        return Fail(error, "invalid_constraint_entry");
      }
      const auto& text = entry.GetString();
      if (std::string_view(key) == "permitted_dns_names") {
        if (!DnsName(text)) {
          return Fail(error, "invalid_dns_constraint");
        }
        cert->permitted_dns_names.push_back(text);
      } else {
        Cidr cidr;
        if (!net::ParseCIDRBlock(text, &cidr.address, &cidr.prefix_length) ||
            text != cidr.address.ToString() + "/" +
                        base::NumberToString(cidr.prefix_length)) {
          return Fail(error, "invalid_cidr_constraint");
        }
        const auto& bytes = cidr.address.bytes();
        for (size_t bit = cidr.prefix_length; bit < bytes.size() * 8; ++bit) {
          if ((bytes[bit / 8] & (0x80 >> (bit % 8))) != 0) {
            return Fail(error, "cidr_has_host_bits");
          }
        }
        cert->permitted_cidrs.push_back(std::move(cidr));
      }
    }
  }
  return (!cert->permitted_dns_names.empty() ||
          !cert->permitted_cidrs.empty()) ||
         Fail(error, "empty_constraints");
}

}  // namespace

std::string Sha256Hex(std::string_view bytes) {
  return base::HexEncodeLower(crypto::hash::Sha256(bytes));
}

std::optional<Config> ParseConfig(std::string_view body, std::string* error) {
  auto root = ReadObject(body, 64 * 1024, error);
  const auto* dict = root ? root->FindDict("xenon_managed_ca") : nullptr;
  if (!dict || !Fields(*root, {"xenon_managed_ca"}) ||
      !Fields(*dict,
              {"enabled", "source_id", "endpoint", "auth", "bootstrap_ca_file",
               "refresh_interval_seconds", "request_timeout_seconds"})) {
    Fail(error, "invalid_config_fields");
    return std::nullopt;
  }
  Config config;
  config.revision = Sha256Hex(body);
  if (dict->contains("enabled") && !dict->FindBool("enabled")) {
    Fail(error, "invalid_enabled");
    return std::nullopt;
  }
  config.enabled = dict->FindBool("enabled").value_or(config.enabled);
  const auto* source = dict->FindString("source_id");
  const auto* endpoint = dict->FindString("endpoint");
  if ((!source || !endpoint) && config.enabled) {
    Fail(error, "missing_source_or_endpoint");
    return std::nullopt;
  }
  if (source) {
    config.source_id = *source;
  }
  if (endpoint) {
    config.endpoint = GURL(*endpoint);
  }
  if ((dict->contains("source_id") && (!source || !SourceId(*source))) ||
      (dict->contains("endpoint") &&
       (!endpoint || !config.endpoint.is_valid() ||
        !config.endpoint.SchemeIs("https") || config.endpoint.has_username() ||
        config.endpoint.has_password() || config.endpoint.has_ref()))) {
    Fail(error, "invalid_source_or_https_endpoint");
    return std::nullopt;
  }
  if (const auto* value = dict->Find("auth")) {
    if (!value->is_dict() ||
        !Fields(value->GetDict(), {"mode", "credential_ref"})) {
      Fail(error, "invalid_auth");
      return std::nullopt;
    }
    const auto& auth = value->GetDict();
    const auto* mode = auth.FindString("mode");
    if ((auth.contains("mode") && !mode) ||
        (mode && *mode != "none" && *mode != "bearer")) {
      Fail(error, "invalid_auth_mode");
      return std::nullopt;
    }
    config.auth_bearer = mode && *mode == "bearer";
    const auto* reference = auth.FindString("credential_ref");
    if ((config.auth_bearer &&
         (!reference || reference->empty() || reference->size() > 512 ||
          reference->find('\0') != std::string::npos)) ||
        (!config.auth_bearer && auth.contains("credential_ref"))) {
      Fail(error, "invalid_credential_reference");
      return std::nullopt;
    }
    if (reference) {
      config.credential_ref = *reference;
    }
  }
  if (dict->contains("bootstrap_ca_file")) {
    const auto* file = dict->FindString("bootstrap_ca_file");
    if (!file || file->find('\0') != std::string::npos) {
      Fail(error, "invalid_bootstrap_path");
      return std::nullopt;
    }
    config.bootstrap_ca_file = base::FilePath::FromUTF8Unsafe(*file);
    if (!config.bootstrap_ca_file.IsAbsolute()) {
      Fail(error, "bootstrap_path_must_be_absolute");
      return std::nullopt;
    }
  }
  for (const auto [key, minimum, maximum] :
       {std::tuple("refresh_interval_seconds", 300, 86400),
        std::tuple("request_timeout_seconds", 5, 120)}) {
    if (!dict->contains(key)) {
      continue;
    }
    const auto seconds = dict->FindInt(key);
    if (!seconds || *seconds < minimum || *seconds > maximum) {
      Fail(error, "invalid_interval");
      return std::nullopt;
    }
    if (std::string_view(key) == "refresh_interval_seconds") {
      config.refresh_interval = base::Seconds(*seconds);
    } else {
      config.request_timeout = base::Seconds(*seconds);
    }
  }
  return config;
}

std::optional<Config> LoadConfig(const base::FilePath& path,
                                 std::string* error) {
  std::string body;
  if (!path.IsAbsolute() ||
      !base::ReadFileToStringWithMaxSize(path, &body, 64 * 1024)) {
    Fail(error, "config_read_failed");
    return std::nullopt;
  }
  auto config = ParseConfig(body, error);
  if (!config || !config->enabled || config->bootstrap_ca_file.empty()) {
    return config;
  }
  std::string pem;
  if (!base::ReadFileToStringWithMaxSize(config->bootstrap_ca_file, &pem,
                                         kMaxSnapshotBytes)) {
    Fail(error, "bootstrap_read_failed");
    return std::nullopt;
  }
  std::string_view remaining(pem);
  std::set<std::string> unique;
  while (!(remaining = base::TrimWhitespaceASCII(remaining, base::TRIM_ALL))
              .empty()) {
    const size_t end = remaining.find(kPemEnd);
    Certificate cert;
    if (!remaining.starts_with(kPemBegin) || end == std::string_view::npos ||
        config->bootstrap_certificates.size() >= kMaxCertificates ||
        !DecodeCertificate(remaining.substr(0, end + kPemEnd.size()), "pem",
                           &cert, error)) {
      Fail(error, "invalid_bootstrap_bundle");
      return std::nullopt;
    }
    const std::string key(cert.der.begin(), cert.der.end());
    if (!unique.insert(key).second) {
      Fail(error, "duplicate_bootstrap_certificate");
      return std::nullopt;
    }
    config->bootstrap_certificates.push_back(std::move(cert.der));
    remaining.remove_prefix(end + kPemEnd.size());
  }
  if (config->bootstrap_certificates.empty()) {
    Fail(error, "empty_bootstrap_bundle");
    return std::nullopt;
  }
  config->revision = Sha256Hex(config->revision + pem);
  return config;
}

std::optional<Snapshot> ParseSnapshot(std::string body,
                                      std::string_view expected_source,
                                      std::string* error) {
  auto dict = ReadObject(body, kMaxSnapshotBytes, error);
  if (!dict ||
      !Fields(*dict, {"schema_version", "source_id", "generation", "issued_at",
                      "expires_at", "certificates"}) ||
      dict->FindInt("schema_version") != 1) {
    Fail(error, "invalid_snapshot_schema");
    return std::nullopt;
  }
  Snapshot result;
  const auto* source = dict->FindString("source_id");
  const auto* certs = dict->FindList("certificates");
  // Older publishers included a snapshot deadline. Accept its wire format for
  // migration, but certificate validity is enforced by the TLS verifier.
  base::Time legacy_expiry;
  if (!source || !SourceId(*source) || *source != expected_source || !certs ||
      certs->size() > kMaxCertificates ||
      !Generation(dict->FindString("generation"), &result.generation) ||
      !ParseTime(dict->FindString("issued_at"), &result.issued_at) ||
      (dict->contains("expires_at") &&
       !ParseTime(dict->FindString("expires_at"), &legacy_expiry))) {
    Fail(error, "invalid_source_version_or_timestamp");
    return std::nullopt;
  }
  std::set<std::vector<uint8_t>> unique;
  for (const auto& entry : *certs) {
    if (!entry.is_dict() ||
        !Fields(entry.GetDict(),
                {"name", "role", "encoding", "data", "constraints"})) {
      Fail(error, "invalid_certificate_fields");
      return std::nullopt;
    }
    const auto& item = entry.GetDict();
    const auto* role = item.FindString("role");
    const auto* encoding = item.FindString("encoding");
    const auto* data = item.FindString("data");
    Certificate cert;
    if (!role || (*role != "trust_anchor" && *role != "intermediate") ||
        !encoding || !data) {
      Fail(error, "invalid_certificate_role_or_data");
      return std::nullopt;
    }
    cert.trust_anchor = *role == "trust_anchor";
    if (item.contains("name")) {
      const auto* name = item.FindString("name");
      if (!name || std::ranges::count_if(*name, [](unsigned char c) {
                     return (c & 0xc0) != 0x80;
                   }) > 128) {
        Fail(error, "invalid_certificate_name");
        return std::nullopt;
      }
      cert.name = *name;
    }
    if (!DecodeCertificate(*data, *encoding, &cert, error) ||
        !Constraints(item.Find("constraints"), &cert, error)) {
      return std::nullopt;
    }
    if (!unique.insert(cert.der).second) {
      Fail(error, "duplicate_certificate");
      return std::nullopt;
    }
    result.certificates.push_back(std::move(cert));
  }
  result.source_id = *source;
  result.body_hash = Sha256Hex(body);
  result.body = std::move(body);
  return result;
}

base::FilePath StatePath(const base::FilePath& user_data_dir,
                         std::string_view source_id) {
  return user_data_dir.AppendASCII("ManagedCertificates")
      .AppendASCII(Sha256Hex(source_id))
      .AppendASCII("state.json");
}

std::optional<PersistentState> LoadState(const base::FilePath& path,
                                         std::string_view source_id,
                                         std::string* error) {
  PersistentState state;
  state.source_id = source_id;
  if (!base::PathExists(path)) {
    return state;
  }
  std::string text;
  if (!base::ReadFileToStringWithMaxSize(path, &text,
                                         2 * kMaxSnapshotBytes + 65536)) {
    Fail(error, "state_corrupt: read_failed");
    return std::nullopt;
  }
  auto dict = ReadObject(text, 2 * kMaxSnapshotBytes + 65536, error);
  const auto* source = dict ? dict->FindString("source_id") : nullptr;
  const auto* body = dict ? dict->FindString("body") : nullptr;
  const auto* hash = dict ? dict->FindString("body_hash") : nullptr;
  const auto* context = dict ? dict->FindString("request_context") : nullptr;
  const auto* etag = dict ? dict->FindString("etag") : nullptr;
  // Preserve the version and body of old caches, including snapshots formerly
  // marked expired. These legacy fields no longer determine trust lifetime.
  base::Time legacy_observed_time;
  if (!dict ||
      !Fields(*dict, {"schema_version", "source_id", "highest_generation",
                      "body_hash", "body", "etag", "request_context",
                      "last_observed_time", "expired"}) ||
      dict->FindInt("schema_version") != 1 || !source || *source != source_id ||
      !body || !hash || !context || !etag || etag->size() > 1024 ||
      !std::ranges::all_of(
          *etag, [](unsigned char c) { return c >= 0x20 && c < 0x7f; }) ||
      (dict->contains("expired") && !dict->FindBool("expired").has_value()) ||
      !Generation(dict->FindString("highest_generation"),
                  &state.highest_generation) ||
      (dict->contains("last_observed_time") &&
       !ParseTime(dict->FindString("last_observed_time"),
                  &legacy_observed_time))) {
    Fail(error, "state_corrupt: invalid_metadata");
    return std::nullopt;
  }
  state.snapshot = ParseSnapshot(*body, source_id, error);
  if (!state.snapshot ||
      state.snapshot->generation != state.highest_generation ||
      state.snapshot->body_hash != *hash) {
    Fail(error, "state_corrupt: body_mismatch");
    return std::nullopt;
  }
  state.body_hash = *hash;
  state.etag = *etag;
  state.request_context = *context;
  return state;
}

bool SaveState(const base::FilePath& path,
               const PersistentState& state,
               std::string* error) {
  if (!state.snapshot ||
      state.highest_generation != state.snapshot->generation ||
      state.source_id != state.snapshot->source_id ||
      state.body_hash != state.snapshot->body_hash ||
      state.body_hash != Sha256Hex(state.snapshot->body)) {
    return Fail(error, "state_write_invalid");
  }
  base::DictValue dict;
  dict.Set("schema_version", 1);
  dict.Set("source_id", state.source_id);
  dict.Set("highest_generation",
           base::NumberToString(state.highest_generation));
  dict.Set("body_hash", state.body_hash);
  dict.Set("body", state.snapshot->body);
  dict.Set("etag", state.etag);
  dict.Set("request_context", state.request_context);
  std::string output;
  if (!base::JSONWriter::Write(dict, &output) ||
      !base::CreateDirectory(path.DirName())) {
    return Fail(error, "state_write_failed");
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(path.DirName(), 0700)) {
    return Fail(error, "state_permissions_failed");
  }
#endif
  if (!base::ImportantFileWriter::WriteFileAtomically(path, output)) {
    return Fail(error, "state_write_failed");
  }
#if BUILDFLAG(IS_POSIX)
  if (!base::SetPosixFilePermissions(path, 0600)) {
    return Fail(error, "state_permissions_failed");
  }
#endif
  return true;
}

}  // namespace xenon::certificates
