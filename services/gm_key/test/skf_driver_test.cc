// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/gm_key/skf_driver.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "services/gm_key/compiler_compat.h"
#include "services/gm_key/skf_abi.h"

namespace {

int failures = 0;
int checks = 0;

#define EXPECT(condition)                                               \
  do {                                                                  \
    ++checks;                                                           \
    if (!(condition)) {                                                 \
      std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); \
      ++failures;                                                       \
    }                                                                   \
  } while (false)

std::vector<char> Pin() {
  return {'1', '2', '3', '4', '5', '6', 0};
}

void CheckWrongArchitecture(const std::wstring& source, WORD machine) {
  wchar_t directory[MAX_PATH];
  wchar_t path[MAX_PATH];
  if (!GetTempPathW(MAX_PATH, directory) ||
      !GetTempFileNameW(directory, L"skf", 0, path)) {
    EXPECT(false);
    return;
  }
  if (!CopyFileW(source.c_str(), path, FALSE)) {
    EXPECT(false);
    DeleteFileW(path);
    return;
  }
  HANDLE file = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  EXPECT(file != INVALID_HANDLE_VALUE);
  if (file != INVALID_HANDLE_VALUE) {
    IMAGE_DOS_HEADER dos;
    DWORD size = 0;
    EXPECT(ReadFile(file, &dos, sizeof(dos), &size, nullptr));
    EXPECT(size == sizeof(dos));
    SetFilePointer(file, dos.e_lfanew + sizeof(DWORD), nullptr, FILE_BEGIN);
    EXPECT(WriteFile(file, &machine, sizeof(machine), &size, nullptr));
    CloseHandle(file);
    std::unique_ptr<gm_key::SkfDriver> driver;
    EXPECT(gm_key::SkfDriver::LoadUnsignedForTesting(path, &driver).error ==
           gm_key::Error::kUnsupportedArchitecture);
    EXPECT(!driver);
  }
  EXPECT(DeleteFileW(path));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  using namespace gm_key;
  if (argc != 2) {
    std::fprintf(stderr,
                 "usage: gm_skf_test.exe <absolute fake-SKF DLL path>\n");
    return 2;
  }
  // SAFETY: The CRT provides argc NUL-terminated arguments; argc == 2 above.
  const std::wstring path = GM_KEY_UNSAFE_BUFFERS(argv[1]);
  std::unique_ptr<SkfDriver> driver;
  EXPECT(SkfDriver::Load(L"fake_skf.dll", &driver).error ==
         Error::kInvalidArgument);
  EXPECT(SkfDriver::Load(L"\\\\server\\share\\fake.dll", &driver).error ==
         Error::kInvalidArgument);
  EXPECT(SkfDriver::Load(path + L":alternate", &driver).error ==
         Error::kInvalidArgument);
  EXPECT(SkfDriver::LoadUnsignedForTesting(path, nullptr).error ==
         Error::kInvalidArgument);
  EXPECT(SkfDriver::Load(path, &driver).error == Error::kUntrustedDriver);
  EXPECT(!driver);
  CheckWrongArchitecture(path, IMAGE_FILE_MACHINE_I386);
  CheckWrongArchitecture(path, IMAGE_FILE_MACHINE_ARM64);
  const auto separator = path.find_last_of(L"\\/");
  const auto missing_export =
      path.substr(0, separator + 1) + L"gm_fake_skf_missing.dll";
  EXPECT(SkfDriver::LoadUnsignedForTesting(missing_export, &driver).error ==
         Error::kMissingExport);
  EXPECT(!driver);
  EXPECT(SkfDriver::LoadUnsignedForTesting(path, &driver).ok());
  if (!driver) {
    return 1;
  }
  HMODULE fake = LoadLibraryExW(
      path.c_str(), nullptr,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!fake) {
    return 1;
  }
  auto set_mode =
      reinterpret_cast<void(WINAPI*)(int)>(GetProcAddress(fake, "FakeSetMode"));
  auto counter = reinterpret_cast<int(WINAPI*)(int)>(
      GetProcAddress(fake, "FakeGetCounter"));
  if (!set_mode || !counter) {
    return 1;
  }

  std::vector<std::string> names;
  EXPECT(driver->EnumerateDevices(&names).ok());
  EXPECT(names == std::vector<std::string>{"test-device"});
  EXPECT(driver->EnumerateApplications("test-device", &names).ok());
  EXPECT(names == std::vector<std::string>{"test-app"});
  EXPECT(driver->EnumerateContainers("test-device", "test-app", &names).ok());
  EXPECT(names == std::vector<std::string>{"test-container"});
  EXPECT(counter(0) == 0 && counter(1) == 0 && counter(2) == 0);
  EXPECT(driver->EnumerateDevices(nullptr).error == Error::kInvalidArgument);
  EXPECT(driver->EnumerateApplications(std::string("bad\0name", 8), &names)
             .error == Error::kInvalidArgument);
  std::thread other_thread([&] {
    EXPECT(driver->EnumerateDevices(&names).error == Error::kWrongThread);
  });
  other_thread.join();
  for (int mode : {1, 2}) {
    set_mode(mode);
    EXPECT(driver->EnumerateDevices(&names).error == Error::kMalformedResponse);
    EXPECT(names.empty());
  }
  set_mode(3);
  EXPECT(driver->EnumerateDevices(&names).skf_error ==
         internal::kBufferTooSmall);
  set_mode(4);
  EXPECT(driver->EnumerateDevices(&names).ok() && names.empty());
  set_mode(0);

  std::unique_ptr<SkfSession> session;
  for (int mode : {5, 6, 7}) {
    set_mode(mode);
    EXPECT(!driver
                ->OpenSession("test-device", "test-app", "test-container",
                              &session)
                .ok());
    EXPECT(!session);
    EXPECT(counter(0) == 0 && counter(1) == 0 && counter(2) == 0);
  }
  set_mode(0);
  EXPECT(
      driver->OpenSession("test-device", "test-app", "test-container", &session)
          .ok());
  if (!session) {
    return 1;
  }
  EXPECT(counter(0) == 1 && counter(1) == 1 && counter(2) == 1);
  std::vector<uint8_t> cert;
  EXPECT(session->ExportCertificate(true, &cert).ok());
  EXPECT(!cert.empty() && cert.back() == 1);
  EXPECT(session->ExportCertificate(false, &cert).ok());
  EXPECT(!cert.empty() && cert.back() == 2);
  for (int mode : {8, 9}) {
    set_mode(mode);
    EXPECT(session->ExportCertificate(true, &cert).error ==
           Error::kMalformedResponse);
    EXPECT(cert.empty());
  }
  set_mode(0);

  std::array<uint8_t, 32> digest{};
  Sm2Signature signature{};
  EXPECT(session->SignSm2Digest(digest, &signature).error ==
         Error::kAuthenticationRequired);
  EXPECT(counter(4) == 0);
  EXPECT(session->VerifyUserPin({'a', 0, 'b', 0}).error ==
         Error::kInvalidArgument);
  EXPECT(counter(3) == 0);
  auto bad_pin = session->VerifyUserPin({'0', 0});
  EXPECT(bad_pin.skf_error == internal::kPinIncorrect &&
         bad_pin.pin_retries == 2);
  EXPECT(counter(3) == 1);
  set_mode(10);
  auto locked = session->VerifyUserPin(Pin());
  EXPECT(locked.skf_error == internal::kPinLocked && locked.pin_retries == 0);
  EXPECT(counter(3) == 2);
  set_mode(0);
  EXPECT(session->VerifyUserPin(Pin()).ok());
  EXPECT(
      session->SignSm2Digest(std::span(digest).first(31), &signature).error ==
      Error::kInvalidArgument);
  EXPECT(counter(4) == 0);
  EXPECT(session->SignSm2Digest(digest, &signature).ok());
  EXPECT(counter(4) == 1);
  EXPECT(signature[31] == 1 && signature[32] == 0x80);
  std::vector<uint8_t> der;
  EXPECT(EncodeSm2SignatureDer(signature, &der).ok());
  EXPECT(der.size() == 40 && der[0] == 0x30 && der[1] == 38 && der[2] == 2 &&
         der[3] == 1 && der[4] == 1 && der[5] == 2 && der[6] == 33 &&
         der[7] == 0 && der[8] == 0x80);
  signature.fill(0);
  EXPECT(EncodeSm2SignatureDer(signature, &der).error ==
         Error::kMalformedResponse);
  EXPECT(der.empty());
  constexpr uint8_t order[] = {0xff, 0xff, 0xff, 0xfe, 0xff, 0xff, 0xff, 0xff,
                               0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                               0x72, 0x03, 0xdf, 0x6b, 0x21, 0xc6, 0x05, 0x2b,
                               0x53, 0xbb, 0xf4, 0x09, 0x39, 0xd5, 0x41, 0x23};
  std::copy(std::begin(order), std::end(order), signature.begin());
  signature[63] = 1;
  EXPECT(EncodeSm2SignatureDer(signature, &der).error ==
         Error::kMalformedResponse);
  --signature[31];
  EXPECT(EncodeSm2SignatureDer(signature, &der).ok());

  for (int mode : {11, 12, 13, 14}) {
    EXPECT(session->VerifyUserPin(Pin()).ok());
    set_mode(mode);
    const Status result = session->SignSm2Digest(digest, &signature);
    EXPECT(mode == 11 ? result.skf_error == internal::kDeviceRemoved
                      : result.error == Error::kMalformedResponse);
    EXPECT(std::all_of(signature.begin(), signature.end(),
                       [](uint8_t value) { return value == 0; }));
    EXPECT(session->SignSm2Digest(digest, &signature).error ==
           Error::kAuthenticationRequired);
    set_mode(0);
  }

  EXPECT(session->VerifyUserPin(Pin()).ok());
  EXPECT(session->ClearAuthentication().ok());
  EXPECT(session->SignSm2Digest(digest, &signature).error ==
         Error::kAuthenticationRequired);
  EXPECT(session->VerifyUserPin(Pin()).ok());
  // The DLL remains alive until the last session closes its vendor handles.
  driver.reset();
  EXPECT(session->SignSm2Digest(digest, &signature).ok());
  session.reset();
  EXPECT(counter(0) == 0 && counter(1) == 0 && counter(2) == 0);
  FreeLibrary(fake);
  std::printf("SKF adapter: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
