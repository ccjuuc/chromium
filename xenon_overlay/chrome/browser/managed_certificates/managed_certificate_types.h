// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_MANAGED_CERTIFICATE_TYPES_H_
#define XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_MANAGED_CERTIFICATE_TYPES_H_

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "net/base/ip_address.h"
#include "url/gurl.h"

namespace xenon::certificates {

inline constexpr size_t kMaxSnapshotBytes = 4 * 1024 * 1024;

struct Config {
  bool enabled = true;
  std::string source_id;
  GURL endpoint;
  bool auth_bearer = false;
  std::string credential_ref;
  base::FilePath bootstrap_ca_file;
  std::vector<std::vector<uint8_t>> bootstrap_certificates;
  base::TimeDelta refresh_interval = base::Hours(6);
  base::TimeDelta request_timeout = base::Seconds(30);
  // Digest of the configuration and bootstrap bytes, never a credential.
  std::string revision;
};

struct Cidr {
  net::IPAddress address;
  size_t prefix_length = 0;
};

struct Certificate {
  std::string name;
  std::vector<uint8_t> der;
  std::vector<uint8_t> spki;
  bool trust_anchor = true;
  std::vector<std::string> permitted_dns_names;
  std::vector<Cidr> permitted_cidrs;
};

struct Snapshot {
  std::string source_id;
  uint64_t generation = 0;
  base::Time issued_at;
  std::string body;
  std::string body_hash;
  std::vector<Certificate> certificates;
};

struct PersistentState {
  std::string source_id;
  uint64_t highest_generation = 0;
  std::string body_hash;
  std::string etag;
  // Opaque digest associating the ETag with endpoint and authentication.
  std::string request_context;
  std::optional<Snapshot> snapshot;
};

// All disk helpers must run on a sequenced blocking task runner.
std::optional<Config> ParseConfig(std::string_view body, std::string* error);
std::optional<Config> LoadConfig(const base::FilePath& path,
                                 std::string* error);
std::optional<Snapshot> ParseSnapshot(std::string body,
                                      std::string_view expected_source,
                                      std::string* error);
base::FilePath StatePath(const base::FilePath& user_data_dir,
                         std::string_view source_id);
// Missing state is an empty state; malformed existing state is an error.
std::optional<PersistentState> LoadState(const base::FilePath& path,
                                         std::string_view source_id,
                                         std::string* error);
bool SaveState(const base::FilePath& path,
               const PersistentState& state,
               std::string* error);
std::string Sha256Hex(std::string_view bytes);

}  // namespace xenon::certificates

#endif  // XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_MANAGED_CERTIFICATE_TYPES_H_
