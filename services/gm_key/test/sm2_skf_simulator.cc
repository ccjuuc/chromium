// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// TEST ONLY. Real SM2 operations with process-local software keys, exposed via
// the same SKF ABI as the hardware adapter. Never deploy as a vendor driver.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

#include "services/gm_key/compiler_compat.h"
#include "services/gm_key/skf_abi.h"
#include "third_party/boringssl/src/include/openssl/bn.h"
#include "third_party/boringssl/src/include/openssl/ec.h"
#include "third_party/boringssl/src/include/openssl/ec_key.h"
#include "third_party/boringssl/src/include/openssl/ecdsa.h"
#include "third_party/boringssl/src/include/openssl/evp.h"
#include "third_party/boringssl/src/include/openssl/nid.h"
#include "third_party/boringssl/src/include/openssl/sm2.h"
#include "third_party/boringssl/src/include/openssl/x509v3.h"

namespace {
using namespace gm_key::internal;
constexpr ULONG kFailure = 0x0a000001;
constexpr ULONG kInvalid = 0x0a000006;

EVP_PKEY* NewKey() {
  bssl::UniquePtr<EC_KEY> ec(EC_KEY_new_by_curve_name(NID_sm2));
  bssl::UniquePtr<EVP_PKEY> key(EVP_PKEY_new());
  if (!ec || !key || !EC_KEY_generate_key(ec.get()) ||
      !EVP_PKEY_set1_EC_KEY(key.get(), ec.get())) {
    return nullptr;
  }
  return key.release();
}

bool AddExtension(X509* certificate, X509* issuer, int nid, const char* value) {
  X509V3_CTX context;
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509_EXTENSION* extension =
      X509V3_EXT_nconf_nid(nullptr, &context, nid, value);
  bool ok = extension && X509_add_ext(certificate, extension, -1);
  X509_EXTENSION_free(extension);
  return ok;
}

X509* Certificate(EVP_PKEY* key,
                  X509* issuer,
                  EVP_PKEY* issuer_key,
                  bool encryption,
                  long serial) {
  if (!key) {
    return nullptr;
  }
  X509* cert = X509_new();
  const bool ca = !issuer;
  bool ok = cert && X509_set_version(cert, 2) &&
            ASN1_INTEGER_set(X509_get_serialNumber(cert), serial) &&
            X509_gmtime_adj(X509_getm_notBefore(cert), -3600) &&
            X509_gmtime_adj(X509_getm_notAfter(cert), 86400) &&
            X509_set_pubkey(cert, key);
  X509_NAME* subject = cert ? X509_get_subject_name(cert) : nullptr;
  const char* name = ca ? "GM simulator test CA" : "GM simulator client";
  ok = ok &&
       X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                  reinterpret_cast<const unsigned char*>(name),
                                  -1, -1, 0) &&
       X509_set_issuer_name(cert,
                            issuer ? X509_get_subject_name(issuer) : subject) &&
       AddExtension(cert, issuer ? issuer : cert, NID_basic_constraints,
                    ca ? "critical,CA:TRUE" : "critical,CA:FALSE") &&
       AddExtension(
           cert, issuer ? issuer : cert, NID_key_usage,
           ca ? "critical,keyCertSign,cRLSign"
           : encryption
               ? "critical,keyEncipherment,dataEncipherment,keyAgreement"
               : "critical,digitalSignature");
  if (!ca) {
    ok = ok && AddExtension(cert, issuer, NID_ext_key_usage, "clientAuth");
  }
  ok = ok && X509_sign(cert, issuer_key ? issuer_key : key, EVP_sm3()) > 0;
  if (!ok) {
    X509_free(cert);
    return nullptr;
  }
  return cert;
}

struct Token {
  EVP_PKEY* ca_key = NewKey();
  X509* ca = Certificate(ca_key, nullptr, nullptr, false, 1);
  EVP_PKEY* sign_key = NewKey();
  EVP_PKEY* enc_key = NewKey();
  X509* sign_cert = Certificate(sign_key, ca, ca_key, false, 2);
  X509* enc_cert = Certificate(enc_key, ca, ca_key, true, 3);
  bool present = true;
  bool authenticated = false;
  bool crash = false;
  DWORD delay = 0;
  DWORD pin_delay = 0;
  ULONG retries = 3;
  long certificate_serial = 4;
  bool extra_container = false;
  ~Token() {
    X509_free(sign_cert);
    X509_free(enc_cert);
    X509_free(ca);
    EVP_PKEY_free(sign_key);
    EVP_PKEY_free(enc_key);
    EVP_PKEY_free(ca_key);
  }
};

Token& token() {
  static Token value;
  return value;
}

struct Handle {
  int kind;
};
std::set<Handle*> handles;
bool Valid(HANDLE handle, int kind) {
  auto* value = static_cast<Handle*>(handle);
  return handles.contains(value) && value->kind == kind;
}
ULONG Open(int kind, HANDLE* out) {
  if (!out || !token().present) {
    return kDeviceRemoved;
  }
  auto handle = std::make_unique<Handle>();
  handle->kind = kind;
  handles.insert(handle.get());
  *out = handle.release();
  return kOk;
}
ULONG Close(HANDLE handle, int kind) {
  if (!Valid(handle, kind)) {
    return kInvalid;
  }
  auto* value = static_cast<Handle*>(handle);
  handles.erase(value);
  delete value;
  return kOk;
}
ULONG Copy(const void* source, size_t length, void* output, ULONG* size) {
  if (!size) {
    return kInvalid;
  }
  const ULONG capacity = *size;
  *size = static_cast<ULONG>(length);
  if (!output) {
    return kOk;
  }
  if (capacity < length) {
    return kBufferTooSmall;
  }
  // SAFETY: SKF's caller reports the output capacity, checked above; all
  // simulator callers supply a source buffer of exactly length bytes.
  GM_KEY_UNSAFE_BUFFERS(std::memcpy(output, source, length));
  return kOk;
}
ULONG CopyCertificate(X509* cert, BYTE* output, ULONG* size) {
  if (!cert) {
    return kFailure;
  }
  int length = i2d_X509(cert, nullptr);
  if (length <= 0) {
    return kFailure;
  }
  std::vector<uint8_t> der(length);
  auto* cursor = der.data();
  if (i2d_X509(cert, &cursor) != length) {
    return kFailure;
  }
  return Copy(der.data(), der.size(), output, size);
}
}  // namespace

#define EXPORT extern "C" __declspec(dllexport)
EXPORT ULONG WINAPI SKF_EnumDev(BOOL present, LPSTR output, ULONG* size) {
  if (!present) {
    return kInvalid;
  }
  static constexpr char names[] = "gm-sm2-simulator\0";
  static constexpr char empty[] = "";
  return token().present ? Copy(names, sizeof(names), output, size)
                         : Copy(empty, sizeof(empty), output, size);
}
EXPORT ULONG WINAPI SKF_ConnectDev(LPSTR name, HANDLE* output) {
  if (!name || std::string_view(name) != "gm-sm2-simulator") {
    return kInvalid;
  }
  if (!token().ca || !token().sign_cert || !token().enc_cert) {
    return kFailure;
  }
  return Open(1, output);
}
EXPORT ULONG WINAPI SKF_DisConnectDev(HANDLE handle) {
  return Close(handle, 1);
}
EXPORT ULONG WINAPI SKF_EnumApplication(HANDLE dev, LPSTR output, ULONG* size) {
  if (!Valid(dev, 1)) {
    return kInvalid;
  }
  static constexpr char names[] = "test-app\0";
  return Copy(names, sizeof(names), output, size);
}
EXPORT ULONG WINAPI SKF_OpenApplication(HANDLE dev,
                                        LPSTR name,
                                        HANDLE* output) {
  if (!Valid(dev, 1) || !name || std::string_view(name) != "test-app") {
    return kInvalid;
  }
  return Open(2, output);
}
EXPORT ULONG WINAPI SKF_CloseApplication(HANDLE handle) {
  return Close(handle, 2);
}
EXPORT ULONG WINAPI SKF_EnumContainer(HANDLE app, LPSTR output, ULONG* size) {
  if (!Valid(app, 2)) {
    return kInvalid;
  }
  static constexpr char names[] = "sm2-dual-cert\0";
  static constexpr char extended[] = "sm2-dual-cert\0extra-container\0";
  return token().extra_container
             ? Copy(extended, sizeof(extended), output, size)
             : Copy(names, sizeof(names), output, size);
}
EXPORT ULONG WINAPI SKF_OpenContainer(HANDLE app, LPSTR name, HANDLE* output) {
  if (!Valid(app, 2) || !name || std::string_view(name) != "sm2-dual-cert") {
    return kInvalid;
  }
  return Open(3, output);
}
EXPORT ULONG WINAPI SKF_CloseContainer(HANDLE handle) {
  return Close(handle, 3);
}
EXPORT ULONG WINAPI SKF_GetContainerType(HANDLE handle, ULONG* type) {
  if (!Valid(handle, 3) || !type) {
    return kInvalid;
  }
  *type = kEccContainer;
  return kOk;
}
EXPORT ULONG WINAPI SKF_ExportCertificate(HANDLE handle,
                                          BOOL signing,
                                          BYTE* output,
                                          ULONG* size) {
  if (!Valid(handle, 3) || !token().present) {
    return kDeviceRemoved;
  }
  return CopyCertificate(signing ? token().sign_cert : token().enc_cert, output,
                         size);
}
EXPORT ULONG WINAPI SKF_VerifyPIN(HANDLE app,
                                  ULONG kind,
                                  LPSTR pin,
                                  ULONG* retries) {
  if (!Valid(app, 2) || !pin || !retries || kind != kUserPin) {
    return kInvalid;
  }
  auto& state = token();
  state.authenticated = false;
  if (state.pin_delay) {
    Sleep(state.pin_delay);
  }
  if (!state.present) {
    return kDeviceRemoved;
  }
  if (!state.retries) {
    *retries = 0;
    return kPinLocked;
  }
  if (std::string_view(pin) != "123456") {
    *retries = --state.retries;
    return state.retries ? kPinIncorrect : kPinLocked;
  }
  state.authenticated = true;
  *retries = state.retries = 3;
  return kOk;
}
EXPORT ULONG WINAPI SKF_ClearSecureState(HANDLE app) {
  if (!Valid(app, 2)) {
    return kInvalid;
  }
  token().authenticated = false;
  return kOk;
}
EXPORT ULONG WINAPI SKF_ECCSignData(HANDLE handle,
                                    BYTE* digest,
                                    ULONG length,
                                    EccSignatureBlob* output) {
  if (!Valid(handle, 3) || !digest || length != 32 || !output) {
    return kInvalid;
  }
  auto& state = token();
  if (!state.present) {
    return kDeviceRemoved;
  }
  if (!state.authenticated) {
    return kNotLoggedIn;
  }
  if (state.crash) {
    TerminateProcess(GetCurrentProcess(), 73);
  }
  if (state.delay) {
    Sleep(state.delay);
  }
  std::array<uint8_t, 72> der;
  size_t size = der.size();
  // SKF signs the already identity-bound 32-byte digest supplied by the
  // network layer. Do not hash it again or apply the default certificate ID.
  bool ok = SM2_sign_digest(digest, der.data(), &size, der.size(),
                            EVP_PKEY_get0_EC_KEY(state.sign_key));
  if (!ok) {
    return kFailure;
  }
  bssl::UniquePtr<ECDSA_SIG> signature(ECDSA_SIG_from_bytes(der.data(), size));
  if (!signature) {
    return kFailure;
  }
  const BIGNUM* r;
  const BIGNUM* s;
  ECDSA_SIG_get0(signature.get(), &r, &s);
  *output = {};
  ok = BN_bn2bin_padded(output->r, sizeof(output->r), r) &&
       BN_bn2bin_padded(output->s, sizeof(output->s), s);
  return ok ? kOk : kFailure;
}
EXPORT ULONG WINAPI GM_SimulatorControl(ULONG operation,
                                        ULONG value,
                                        BYTE* output,
                                        ULONG* size) {
  auto& state = token();
  if (operation == 0) {
    return CopyCertificate(state.ca, output, size);
  }
  // Two-pass SKF buffer sizing must not execute control operations twice.
  if (output) {
    switch (operation) {
      case 1:
        state.present = value != 0;
        state.authenticated = false;
        break;
      case 2:
        state.delay = std::min<ULONG>(value, 60000);
        break;
      case 3:
        state.crash = value != 0;
        break;
      case 4:
        state.retries = 3;
        state.authenticated = false;
        break;
      case 5:
        state.pin_delay = std::min<ULONG>(value, 60000);
        break;
      case 6:
        state.present = true;
        state.authenticated = false;
        break;
      case 7: {
        X509* signing = Certificate(state.sign_key, state.ca, state.ca_key,
                                    false, state.certificate_serial++);
        X509* encryption = Certificate(state.enc_key, state.ca, state.ca_key,
                                       true, state.certificate_serial++);
        if (!signing || !encryption) {
          X509_free(signing);
          X509_free(encryption);
          return kFailure;
        }
        X509_free(state.sign_cert);
        X509_free(state.enc_cert);
        state.sign_cert = signing;
        state.enc_cert = encryption;
        break;
      }
      case 8:
        state.extra_container = value != 0;
        break;
      default:
        return kInvalid;
    }
  }
  const BYTE acknowledgement = 1;
  return Copy(&acknowledgement, 1, output, size);
}
