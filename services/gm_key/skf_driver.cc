// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/gm_key/skf_driver.h"

#include <windows.h>

#include <bcrypt.h>
#include <softpub.h>
#include <wintrust.h>

#include <algorithm>
#include <array>
#include <exception>
#include <limits>
#include <utility>

#include "services/gm_key/skf_abi.h"

namespace gm_key {
namespace {

constexpr ULONG kMaximumBlob = 64 * 1024;
constexpr size_t kMaximumName = 1024;
// Order of the SM2 curve, big endian.
constexpr std::array<uint8_t, 32> kSm2Order = {
    0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0x72, 0x03, 0xdf, 0x6b, 0x21, 0xc6,
    0x05, 0x2b, 0x53, 0xbb, 0xf4, 0x09, 0x39, 0xd5, 0x41, 0x23};

Status Fail(Error error) {
  return {.error = error};
}

Status DriverResult(ULONG result) {
  return {.error = result == internal::kOk ? Error::kOk : Error::kDriverError,
          .skf_error = result};
}

bool ValidName(const std::string& name) {
  return !name.empty() && name.size() <= kMaximumName &&
         name.find('\0') == std::string::npos;
}

class FileHandle {
 public:
  explicit FileHandle(HANDLE handle) : handle_(handle) {}
  ~FileHandle() {
    if (handle_ != INVALID_HANDLE_VALUE) {
      CloseHandle(handle_);
    }
  }
  HANDLE get() const { return handle_; }
  HANDLE release() { return std::exchange(handle_, INVALID_HANDLE_VALUE); }

 private:
  HANDLE handle_;
};

bool ReadAt(HANDLE file, LONGLONG offset, void* data, DWORD length) {
  LARGE_INTEGER position;
  position.QuadPart = offset;
  DWORD read = 0;
  return SetFilePointerEx(file, position, nullptr, FILE_BEGIN) &&
         ReadFile(file, data, length, &read, nullptr) && read == length;
}

bool IsAmd64Dll(HANDLE file) {
  IMAGE_DOS_HEADER dos = {};
  IMAGE_NT_HEADERS64 nt = {};
  LARGE_INTEGER size;
  if (!GetFileSizeEx(file, &size) || !ReadAt(file, 0, &dos, sizeof(dos)) ||
      dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < static_cast<LONG>(sizeof(dos)) ||
      static_cast<LONGLONG>(dos.e_lfanew) >
          size.QuadPart - static_cast<LONGLONG>(sizeof(nt)) ||
      !ReadAt(file, dos.e_lfanew, &nt, sizeof(nt))) {
    return false;
  }
  return nt.Signature == IMAGE_NT_SIGNATURE &&
         nt.FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
         (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) &&
         nt.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}

LONG CheckSignature(const std::wstring& path, HANDLE file) {
  WINTRUST_FILE_INFO info = {};
  info.cbStruct = sizeof(info);
  info.pcwszFilePath = path.c_str();
  info.hFile = file;
  WINTRUST_DATA data = {};
  data.cbStruct = sizeof(data);
  data.dwUIChoice = WTD_UI_NONE;
  data.fdwRevocationChecks = WTD_REVOKE_NONE;
  data.dwUnionChoice = WTD_CHOICE_FILE;
  data.pFile = &info;
  data.dwStateAction = WTD_STATEACTION_VERIFY;
  // Driver loading does not initiate network retrieval. Provisioning must
  // enforce the vendor/hash allowlist and its update/revocation policy.
  data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
  GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  const LONG result = WinVerifyTrust(nullptr, &action, &data);
  data.dwStateAction = WTD_STATEACTION_CLOSE;
  WinVerifyTrust(nullptr, &action, &data);
  return result;
}

bool CheckHash(HANDLE file, const std::array<uint8_t, 32>& expected) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr,
                                  0) < 0) {
    return false;
  }
  bool ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
  LARGE_INTEGER zero = {};
  ok = ok && SetFilePointerEx(file, zero, nullptr, FILE_BEGIN);
  std::array<uint8_t, 16384> buffer;
  while (ok) {
    DWORD length = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                  &length, nullptr)) {
      ok = false;
      break;
    }
    if (!length) {
      break;
    }
    ok = BCryptHashData(hash, buffer.data(), length, 0) >= 0;
  }
  std::array<uint8_t, 32> actual = {};
  ok = ok && BCryptFinishHash(hash, actual.data(),
                              static_cast<ULONG>(actual.size()), 0) >= 0;
  if (hash) {
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return ok && actual == expected;
}

bool IsScalarValid(std::span<const uint8_t, 32> scalar) {
  return std::any_of(scalar.begin(), scalar.end(),
                     [](uint8_t byte) { return byte != 0; }) &&
         std::lexicographical_compare(scalar.begin(), scalar.end(),
                                      kSm2Order.begin(), kSm2Order.end());
}

// A vendor may report an increased required size after enumeration changes.
// Bound both allocation and attempts; never spin while hardware is changing.
template <typename Function>
Status ReadBlob(Function read, std::vector<uint8_t>* bytes) {
  bytes->clear();
  ULONG size = 0;
  ULONG result = read(nullptr, &size);
  if (result != internal::kOk && result != internal::kBufferTooSmall) {
    return DriverResult(result);
  }
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (size == 0 || size > kMaximumBlob) {
      return Fail(Error::kMalformedResponse);
    }
    const ULONG capacity = size;
    std::vector<uint8_t> buffer(capacity, 0xa5);
    result = read(buffer.data(), &size);
    if (result == internal::kBufferTooSmall) {
      if (size <= capacity) {
        return Fail(Error::kMalformedResponse);
      }
      continue;
    }
    if (result != internal::kOk) {
      return DriverResult(result);
    }
    if (size == 0 || size > capacity) {
      return Fail(Error::kMalformedResponse);
    }
    buffer.resize(size);
    *bytes = std::move(buffer);
    return {};
  }
  return DriverResult(internal::kBufferTooSmall);
}

template <typename Function>
Status ReadNames(Function read, std::vector<std::string>* names) {
  if (!names) {
    return Fail(Error::kInvalidArgument);
  }
  names->clear();
  std::vector<uint8_t> bytes;
  Status status = ReadBlob(read, &bytes);
  if (!status.ok()) {
    return status;
  }
  // Empty MULTI_SZ may be one NUL; a nonempty list must be double terminated.
  if (bytes.back() != 0 || (bytes.size() > 1 && bytes[bytes.size() - 2] != 0)) {
    return Fail(Error::kMalformedResponse);
  }
  std::vector<std::string> parsed;
  size_t offset = 0;
  while (offset < bytes.size() && bytes[offset] != 0) {
    const auto begin = bytes.begin() + offset;
    const auto end = std::find(begin, bytes.end(), 0);
    std::string name(begin, end);
    if (!ValidName(name) || parsed.size() >= 256) {
      return Fail(Error::kMalformedResponse);
    }
    parsed.push_back(std::move(name));
    offset += static_cast<size_t>(end - begin) + 1;
  }
  if (!std::all_of(bytes.begin() + offset, bytes.end(),
                   [](uint8_t byte) { return byte == 0; })) {
    return Fail(Error::kMalformedResponse);
  }
  *names = std::move(parsed);
  return {};
}

class ScopedSkfHandle {
 public:
  explicit ScopedSkfHandle(ULONG(WINAPI* close)(HANDLE)) : close_(close) {}
  ~ScopedSkfHandle() {
    if (handle_) {
      close_(handle_);
    }
  }
  HANDLE* receive() { return &handle_; }
  HANDLE get() const { return handle_; }
  HANDLE release() { return std::exchange(handle_, nullptr); }

 private:
  ULONG(WINAPI* close_)(HANDLE);
  HANDLE handle_ = nullptr;
};

// SKF declares names mutable. Supply a private mutable buffer instead of
// allowing a driver to modify the caller's string through a const_cast.
template <typename Function>
Status OpenNamed(Function open, const std::string& name, HANDLE* handle) {
  std::vector<char> mutable_name(name.begin(), name.end());
  mutable_name.push_back(0);
  Status status = DriverResult(open(mutable_name.data(), handle));
  if (status.ok() && !*handle) {
    return Fail(Error::kMalformedResponse);
  }
  return status;
}

}  // namespace

namespace internal {

struct DriverState {
  ~DriverState() {
    // A wrong-thread destructor is a programming error, not a recoverable SDK
    // error. Never invoke a vendor close/unload on a different OS thread.
    if (owner != GetCurrentThreadId()) {
      std::terminate();
    }
    if (module) {
      FreeLibrary(module);
    }
    if (file != INVALID_HANDLE_VALUE) {
      CloseHandle(file);
    }
  }
  Status CheckThread() const {
    return owner == GetCurrentThreadId() ? Status{} : Fail(Error::kWrongThread);
  }
  DWORD owner = GetCurrentThreadId();
  HMODULE module = nullptr;
  HANDLE file = INVALID_HANDLE_VALUE;
  SkfApi api;
};

}  // namespace internal

Status SkfDriver::Load(const std::wstring& path,
                       std::unique_ptr<SkfDriver>* driver) {
  return LoadInternal(path, true, nullptr, driver);
}

Status SkfDriver::LoadPinned(const std::wstring& path,
                             const std::array<uint8_t, 32>& sha256,
                             std::unique_ptr<SkfDriver>* driver) {
  return LoadInternal(path, true, &sha256, driver);
}

#if defined(GM_KEY_TESTING)
Status SkfDriver::LoadUnsignedForTesting(const std::wstring& path,
                                         std::unique_ptr<SkfDriver>* driver) {
  return LoadInternal(path, false, nullptr, driver);
}

Status SkfDriver::LoadPinnedUnsignedForTesting(
    const std::wstring& path,
    const std::array<uint8_t, 32>& sha256,
    std::unique_ptr<SkfDriver>* driver) {
  return LoadInternal(path, false, &sha256, driver);
}
#endif

Status SkfDriver::LoadInternal(const std::wstring& path,
                               bool require_signature,
                               const std::array<uint8_t, 32>* sha256,
                               std::unique_ptr<SkfDriver>* driver) {
  if (!driver) {
    return Fail(Error::kInvalidArgument);
  }
  driver->reset();
  if (path.size() < 4 || path.size() > 32760 ||
      !((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) ||
      path[1] != L':' || path[2] != L'\\' ||
      path.find(L'\0') != std::wstring::npos ||
      path.find(L':', 2) != std::wstring::npos) {
    return Fail(Error::kInvalidArgument);
  }
  const std::wstring root = path.substr(0, 3);
  if (GetDriveTypeW(root.c_str()) == DRIVE_REMOTE) {
    return Fail(Error::kInvalidArgument);
  }
  // Keep a no-write/no-delete handle for the full module lifetime, so
  // validation and loading cannot race an in-place replacement of the selected
  // file.
  FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr));
  if (file.get() == INVALID_HANDLE_VALUE) {
    return {.error = Error::kLoadFailed, .system_error = GetLastError()};
  }
  if (!IsAmd64Dll(file.get())) {
    return Fail(Error::kUnsupportedArchitecture);
  }
  if (sha256 && !CheckHash(file.get(), *sha256)) {
    return Fail(Error::kUntrustedDriver);
  }
  std::wstring resolved(32768, L'\0');
  DWORD length = GetFinalPathNameByHandleW(
      file.get(), resolved.data(), static_cast<DWORD>(resolved.size()),
      FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (!length || length >= resolved.size()) {
    return {.error = Error::kLoadFailed, .system_error = GetLastError()};
  }
  resolved.resize(length);
  // Reject reparse-point redirection to a network path as well.
  if (resolved.size() < 7 || resolved.compare(0, 4, L"\\\\?\\") != 0 ||
      resolved[5] != L':' || resolved[6] != L'\\' ||
      GetDriveTypeW(resolved.substr(4, 3).c_str()) == DRIVE_REMOTE) {
    return Fail(Error::kInvalidArgument);
  }
  if (require_signature) {
    LONG result = CheckSignature(resolved, file.get());
    if (result != ERROR_SUCCESS) {
      return {.error = Error::kUntrustedDriver,
              .system_error = static_cast<uint32_t>(result)};
    }
  }
  auto state = std::make_shared<internal::DriverState>();
  state->file = file.release();
  state->module = LoadLibraryExW(
      resolved.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!state->module) {
    return {.error = Error::kLoadFailed, .system_error = GetLastError()};
  }
#define LOAD_SKF(member, symbol)                                     \
  state->api.member = reinterpret_cast<decltype(state->api.member)>( \
      GetProcAddress(state->module, symbol));                        \
  if (!state->api.member) {                                          \
    return Fail(Error::kMissingExport);                              \
  }
  LOAD_SKF(enum_dev, "SKF_EnumDev");
  LOAD_SKF(connect_dev, "SKF_ConnectDev");
  LOAD_SKF(disconnect_dev, "SKF_DisConnectDev");
  LOAD_SKF(enum_application, "SKF_EnumApplication");
  LOAD_SKF(open_application, "SKF_OpenApplication");
  LOAD_SKF(close_application, "SKF_CloseApplication");
  LOAD_SKF(enum_container, "SKF_EnumContainer");
  LOAD_SKF(open_container, "SKF_OpenContainer");
  LOAD_SKF(close_container, "SKF_CloseContainer");
  LOAD_SKF(get_container_type, "SKF_GetContainerType");
  LOAD_SKF(export_certificate, "SKF_ExportCertificate");
  LOAD_SKF(verify_pin, "SKF_VerifyPIN");
  LOAD_SKF(clear_secure_state, "SKF_ClearSecureState");
  LOAD_SKF(ecc_sign_data, "SKF_ECCSignData");
#undef LOAD_SKF
  driver->reset(new SkfDriver(std::move(state)));
  return {};
}

SkfDriver::SkfDriver(std::shared_ptr<internal::DriverState> driver)
    : driver_(std::move(driver)) {}
SkfDriver::~SkfDriver() = default;

#if defined(GM_KEY_TESTING)
Status SkfDriver::ControlForTesting(uint32_t operation,
                                    uint32_t value,
                                    std::vector<uint8_t>* result) {
  Status status = driver_->CheckThread();
  if (!status.ok() || !result) {
    return status.ok() ? Fail(Error::kInvalidArgument) : status;
  }
  using Control = ULONG(WINAPI*)(ULONG, ULONG, BYTE*, ULONG*);
  auto control = reinterpret_cast<Control>(
      GetProcAddress(driver_->module, "GM_SimulatorControl"));
  if (!control) {
    return Fail(Error::kMissingExport);
  }
  return ReadBlob(
      [&](BYTE* data, ULONG* size) {
        return control(operation, value, data, size);
      },
      result);
}
#endif

Status SkfDriver::EnumerateDevices(std::vector<std::string>* names) {
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  return ReadNames(
      [&](BYTE* data, ULONG* size) {
        return driver_->api.enum_dev(TRUE, reinterpret_cast<LPSTR>(data), size);
      },
      names);
}

Status SkfDriver::EnumerateApplications(const std::string& device,
                                        std::vector<std::string>* names) {
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  if (!ValidName(device) || !names) {
    return Fail(Error::kInvalidArgument);
  }
  names->clear();
  ScopedSkfHandle handle(driver_->api.disconnect_dev);
  status = OpenNamed(driver_->api.connect_dev, device, handle.receive());
  if (!status.ok()) {
    return status;
  }
  return ReadNames(
      [&](BYTE* data, ULONG* size) {
        return driver_->api.enum_application(
            handle.get(), reinterpret_cast<LPSTR>(data), size);
      },
      names);
}

Status SkfDriver::EnumerateContainers(const std::string& device,
                                      const std::string& application,
                                      std::vector<std::string>* names) {
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  if (!ValidName(device) || !ValidName(application) || !names) {
    return Fail(Error::kInvalidArgument);
  }
  names->clear();
  ScopedSkfHandle dev(driver_->api.disconnect_dev);
  status = OpenNamed(driver_->api.connect_dev, device, dev.receive());
  if (!status.ok()) {
    return status;
  }
  ScopedSkfHandle app(driver_->api.close_application);
  status = OpenNamed(
      [&](LPSTR name, HANDLE* out) {
        return driver_->api.open_application(dev.get(), name, out);
      },
      application, app.receive());
  if (!status.ok()) {
    return status;
  }
  return ReadNames(
      [&](BYTE* data, ULONG* size) {
        return driver_->api.enum_container(app.get(),
                                           reinterpret_cast<LPSTR>(data), size);
      },
      names);
}

Status SkfDriver::OpenSession(const std::string& device,
                              const std::string& application,
                              const std::string& container,
                              std::unique_ptr<SkfSession>* session) {
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  if (!session) {
    return Fail(Error::kInvalidArgument);
  }
  session->reset();
  if (!ValidName(device) || !ValidName(application) || !ValidName(container)) {
    return Fail(Error::kInvalidArgument);
  }
  ScopedSkfHandle dev(driver_->api.disconnect_dev);
  status = OpenNamed(driver_->api.connect_dev, device, dev.receive());
  if (!status.ok()) {
    return status;
  }
  ScopedSkfHandle app(driver_->api.close_application);
  status = OpenNamed(
      [&](LPSTR name, HANDLE* out) {
        return driver_->api.open_application(dev.get(), name, out);
      },
      application, app.receive());
  if (!status.ok()) {
    return status;
  }
  ScopedSkfHandle key(driver_->api.close_container);
  status = OpenNamed(
      [&](LPSTR name, HANDLE* out) {
        return driver_->api.open_container(app.get(), name, out);
      },
      container, key.receive());
  if (!status.ok()) {
    return status;
  }
  ULONG type = 0;
  status = DriverResult(driver_->api.get_container_type(key.get(), &type));
  if (!status.ok()) {
    return status;
  }
  if (type != internal::kEccContainer) {
    return Fail(Error::kMalformedResponse);
  }
  session->reset(
      new SkfSession(driver_, dev.release(), app.release(), key.release()));
  return {};
}

SkfSession::SkfSession(std::shared_ptr<internal::DriverState> driver,
                       void* device,
                       void* application,
                       void* container)
    : driver_(std::move(driver)),
      device_(device),
      application_(application),
      container_(container) {}

SkfSession::~SkfSession() {
  if (!driver_->CheckThread().ok()) {
    std::terminate();
  }
  if (authentication_attempted_) {
    driver_->api.clear_secure_state(application_);
  }
  driver_->api.close_container(container_);
  driver_->api.close_application(application_);
  driver_->api.disconnect_dev(device_);
}

Status SkfSession::ExportCertificate(bool signing, std::vector<uint8_t>* der) {
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  if (!der) {
    return Fail(Error::kInvalidArgument);
  }
  return ReadBlob(
      [&](BYTE* data, ULONG* size) {
        return driver_->api.export_certificate(
            container_, signing ? TRUE : FALSE, data, size);
      },
      der);
}

Status SkfSession::EnumerateApplications(std::vector<std::string>* names) {
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  return ReadNames(
      [&](BYTE* data, ULONG* size) {
        return driver_->api.enum_application(
            device_, reinterpret_cast<LPSTR>(data), size);
      },
      names);
}

Status SkfSession::EnumerateContainers(std::vector<std::string>* names) {
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  return ReadNames(
      [&](BYTE* data, ULONG* size) {
        return driver_->api.enum_container(application_,
                                           reinterpret_cast<LPSTR>(data), size);
      },
      names);
}

Status SkfSession::VerifyUserPin(std::vector<char> pin) {
  // RAII also clears invalid PIN buffers and buffers passed on the wrong
  // thread.
  struct Wipe {
    std::vector<char> bytes;
    ~Wipe() {
      if (!bytes.empty()) {
        SecureZeroMemory(bytes.data(), bytes.size());
      }
    }
  } wipe{std::move(pin)};
  auto& input = wipe.bytes;
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  if (input.size() < 2 || input.size() > 129 || input.back() != 0 ||
      std::find(input.begin(), input.end() - 1, '\0') != input.end() - 1) {
    return Fail(Error::kInvalidArgument);
  }
  authenticated_ = false;
  authentication_attempted_ = true;
  ULONG retries = std::numeric_limits<ULONG>::max();
  status = DriverResult(driver_->api.verify_pin(
      application_, internal::kUserPin, input.data(), &retries));
  status.pin_retries = retries;
  authenticated_ = status.ok();
  return status;
}

Status SkfSession::ClearAuthentication() {
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  authenticated_ = false;
  return DriverResult(driver_->api.clear_secure_state(application_));
}

Status SkfSession::SignSm2Digest(std::span<const uint8_t> digest,
                                 Sm2Signature* signature) {
  if (signature) {
    signature->fill(0);
  }
  Status status = driver_->CheckThread();
  if (!status.ok()) {
    return status;
  }
  if (digest.size() != 32 || !signature) {
    return Fail(Error::kInvalidArgument);
  }
  if (!authenticated_) {
    return Fail(Error::kAuthenticationRequired);
  }
  std::array<uint8_t, 32> mutable_digest;
  std::copy(digest.begin(), digest.end(), mutable_digest.begin());
  internal::EccSignatureBlob blob = {};
  status = DriverResult(driver_->api.ecc_sign_data(
      container_, mutable_digest.data(),
      static_cast<ULONG>(mutable_digest.size()), &blob));
  if (!status.ok()) {
    // Removal, logout and all other failures require a fresh authentication.
    authenticated_ = false;
    return status;
  }
  const auto high_zero = [](std::span<const uint8_t, 64> value) {
    const auto high = value.first<32>();
    return std::all_of(high.begin(), high.end(),
                       [](uint8_t byte) { return byte == 0; });
  };
  if (!high_zero(blob.r) || !high_zero(blob.s) ||
      !IsScalarValid(std::span(blob.r).last<32>()) ||
      !IsScalarValid(std::span(blob.s).last<32>())) {
    authenticated_ = false;
    return Fail(Error::kMalformedResponse);
  }
  std::ranges::copy(std::span(blob.r).last<32>(), signature->begin());
  std::ranges::copy(std::span(blob.s).last<32>(),
                    std::span(*signature).last<32>().begin());
  return {};
}

Status EncodeSm2SignatureDer(const Sm2Signature& signature,
                             std::vector<uint8_t>* der) {
  if (!der) {
    return Fail(Error::kInvalidArgument);
  }
  der->clear();
  auto r = std::span(signature).first<32>();
  auto s = std::span(signature).last<32>();
  if (!IsScalarValid(r) || !IsScalarValid(s)) {
    return Fail(Error::kMalformedResponse);
  }
  std::vector<uint8_t> result{0x30, 0};
  for (auto scalar : {r, s}) {
    auto begin = std::find_if(scalar.begin(), scalar.end(),
                              [](uint8_t byte) { return byte != 0; });
    const bool pad = (*begin & 0x80) != 0;
    result.push_back(0x02);
    result.push_back(
        static_cast<uint8_t>(scalar.end() - begin + (pad ? 1 : 0)));
    if (pad) {
      result.push_back(0);
    }
    result.insert(result.end(), begin, scalar.end());
  }
  result[1] = static_cast<uint8_t>(result.size() - 2);
  *der = std::move(result);
  return {};
}

}  // namespace gm_key
