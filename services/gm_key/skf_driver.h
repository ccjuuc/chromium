// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_GM_KEY_SKF_DRIVER_H_
#define SERVICES_GM_KEY_SKF_DRIVER_H_

#include <windows.h>

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace gm_key {

enum class Error {
  kOk,
  kInvalidArgument,
  kWrongThread,
  kUnsupportedArchitecture,
  kUntrustedDriver,
  kLoadFailed,
  kMissingExport,
  kDriverError,
  kMalformedResponse,
  kAuthenticationRequired,
};

struct Status {
  Error error = Error::kOk;
  uint32_t skf_error = 0;
  uint32_t system_error = 0;
  // Only VerifyUserPin() reports a retry count. No automatic PIN retries occur.
  uint32_t pin_retries = UINT32_MAX;
  bool ok() const { return error == Error::kOk; }
};

// Fixed-width, big-endian r || s, 32 bytes each (not SKF's 64-byte fields).
using Sm2Signature = std::array<uint8_t, 64>;

namespace internal {
struct DriverState;
}  // namespace internal

// Synchronous device adapter, exclusively for a dedicated, single OS thread in
// an isolated device process. NEVER call from a browser/network IO thread.
// All calls and destruction must occur on the thread that loaded the driver.
// Blocking or hung SDK calls are contained by the process; they cannot be
// cancelled safely by terminating a thread. IPC cancellation must ignore late
// results and enforce identity generations before making them visible.
class SkfSession {
 public:
  ~SkfSession();
  SkfSession(const SkfSession&) = delete;
  SkfSession& operator=(const SkfSession&) = delete;

  Status ExportCertificate(bool signing, std::vector<uint8_t>* der);
  // Inventory refresh uses the existing handles so it cannot log out an
  // authorized application by repeatedly opening/closing SDK sessions.
  Status EnumerateApplications(std::vector<std::string>* names);
  Status EnumerateContainers(std::vector<std::string>* names);

  // Consumes a NUL-terminated PIN buffer (1..128 bytes excluding the NUL) and
  // securely clears it before returning. Move the caller's buffer into this
  // call; the caller must clear any other copies. Never log or persist a PIN.
  Status VerifyUserPin(std::vector<char> pin);
  Status ClearAuthentication();

  // Accepts exactly 32 bytes: e = SM3(ZA || message), already prepared by the
  // protocol backend using the correct identity and public key. This method
  // neither hashes again nor accepts raw TLS transcripts. It requires a
  // successful VerifyUserPin() on this session. Software private keys are never
  // accepted. Only the signing key is used; SM2 agreement is not implemented.
  Status SignSm2Digest(std::span<const uint8_t> digest,
                       Sm2Signature* signature);

 private:
  friend class SkfDriver;
  SkfSession(std::shared_ptr<internal::DriverState> driver,
             void* device,
             void* application,
             void* container);
  std::shared_ptr<internal::DriverState> driver_;
  HANDLE device_;
  HANDLE application_;
  HANDLE container_;
  bool authenticated_ = false;
  bool authentication_attempted_ = false;
};

class SkfDriver {
 public:
  // Requires an explicit local drive-absolute path (C:\\...); no search by
  // basename, environment, registry, relative path, UNC, or alternate streams.
  // The DLL must be AMD64 and have valid Windows Authenticode trust. Its folder
  // must be installed with administrator-controlled ACLs. The service must
  // additionally enforce its provisioned vendor/hash allowlist before calling
  // Load(): a trusted signature by itself is not a Feitian identity check.
  static Status Load(const std::wstring& path,
                     std::unique_ptr<SkfDriver>* driver);
  // Verifies the provisioned SHA-256 using the same locked file that is loaded.
  static Status LoadPinned(const std::wstring& path,
                           const std::array<uint8_t, 32>& sha256,
                           std::unique_ptr<SkfDriver>* driver);

#if defined(GM_KEY_TESTING)
  // Compiled only in the isolated test executable, never the production target.
  static Status LoadUnsignedForTesting(const std::wstring& path,
                                       std::unique_ptr<SkfDriver>* driver);
  static Status LoadPinnedUnsignedForTesting(
      const std::wstring& path,
      const std::array<uint8_t, 32>& sha256,
      std::unique_ptr<SkfDriver>* driver);
  Status ControlForTesting(uint32_t operation,
                           uint32_t value,
                           std::vector<uint8_t>* result);
#endif

  ~SkfDriver();
  SkfDriver(const SkfDriver&) = delete;
  SkfDriver& operator=(const SkfDriver&) = delete;

  // Names are opaque SDK bytes, not assumed to be UTF-8. Pass them unchanged.
  Status EnumerateDevices(std::vector<std::string>* names);
  Status EnumerateApplications(const std::string& device,
                               std::vector<std::string>* names);
  Status EnumerateContainers(const std::string& device,
                             const std::string& application,
                             std::vector<std::string>* names);
  Status OpenSession(const std::string& device,
                     const std::string& application,
                     const std::string& container,
                     std::unique_ptr<SkfSession>* session);

 private:
  explicit SkfDriver(std::shared_ptr<internal::DriverState> driver);
  static Status LoadInternal(const std::wstring& path,
                             bool require_signature,
                             const std::array<uint8_t, 32>* sha256,
                             std::unique_ptr<SkfDriver>* driver);
  std::shared_ptr<internal::DriverState> driver_;
};

// Converts validated, fixed-width SM2 scalars to canonical ASN.1 DER SEQUENCE.
// Rejects zero or out-of-range scalars; no OpenSSL/BoringSSL dependency.
Status EncodeSm2SignatureDer(const Sm2Signature& signature,
                             std::vector<uint8_t>* der);

}  // namespace gm_key

#endif  // SERVICES_GM_KEY_SKF_DRIVER_H_
