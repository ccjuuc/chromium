// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Fault-injection test DLL, deliberately unsigned. NOT a Feitian implementation
// and NOT suitable for cryptographic use. It returns synthetic signatures.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>

#include "services/gm_key/compiler_compat.h"
#include "services/gm_key/skf_abi.h"

namespace {
using namespace gm_key::internal;

int mode = 0;
int live_devices = 0;
int live_apps = 0;
int live_containers = 0;
int pin_calls = 0;
int sign_calls = 0;
bool authenticated = false;

ULONG Copy(const void* data, ULONG length, void* out, ULONG* size) {
  ULONG capacity = *size;
  *size = length;
  if (!out) {
    return kOk;
  }
  if (capacity < length) {
    return kBufferTooSmall;
  }
  // SAFETY: Every caller supplies an array and its exact sizeof; the receiving
  // adapter supplies a buffer of *size bytes, checked above against length.
  GM_KEY_UNSAFE_BUFFERS(memcpy(out, data, length));
  return kOk;
}

bool EqualInput(const char* value, std::string_view expected) {
  // SAFETY: The adapter supplies owned NUL-terminated strings at this SKF ABI
  // boundary; the test driver never retains or changes them.
  return GM_KEY_UNSAFE_BUFFERS(std::string_view(value)) == expected;
}

}  // namespace

#define EXPORT extern "C" __declspec(dllexport)

EXPORT void WINAPI FakeSetMode(int next) {
  mode = next;
}
EXPORT int WINAPI FakeGetCounter(int which) {
  switch (which) {
    case 0:
      return live_devices;
    case 1:
      return live_apps;
    case 2:
      return live_containers;
    case 3:
      return pin_calls;
    case 4:
      return sign_calls;
  }
  return -1;
}

EXPORT ULONG WINAPI SKF_EnumDev(BOOL present, LPSTR names, ULONG* size) {
  if (!present) {
    return 0x0a000006;
  }
  if (mode == 1) {
    static constexpr char bad[] = {'b', 'a', 'd', 0};
    return Copy(bad, sizeof(bad), names, size);
  }
  if (mode == 2) {
    *size = 1000000;
    return kOk;
  }
  if (mode == 3) {
    *size = names ? *size + 1 : 4;
    return kBufferTooSmall;
  }
  if (mode == 4) {
    static constexpr char empty[] = {0};
    return Copy(empty, sizeof(empty), names, size);
  }
  static constexpr char devices[] = "test-device\0";
  return Copy(devices, sizeof(devices), names, size);
}

EXPORT ULONG WINAPI SKF_ConnectDev(LPSTR name, HANDLE* out) {
  if (!EqualInput(name, "test-device")) {
    return kDeviceRemoved;
  }
  *out = new int(1);
  ++live_devices;
  return kOk;
}

EXPORT ULONG WINAPI SKF_DisConnectDev(HANDLE handle) {
  delete static_cast<int*>(handle);
  --live_devices;
  return kOk;
}

EXPORT ULONG WINAPI SKF_EnumApplication(HANDLE, LPSTR names, ULONG* size) {
  static constexpr char apps[] = "test-app\0";
  return Copy(apps, sizeof(apps), names, size);
}

EXPORT ULONG WINAPI SKF_OpenApplication(HANDLE, LPSTR name, HANDLE* out) {
  if (!EqualInput(name, "test-app") || mode == 5) {
    return 0x0a00002e;
  }
  *out = new int(2);
  ++live_apps;
  return kOk;
}

EXPORT ULONG WINAPI SKF_CloseApplication(HANDLE handle) {
  delete static_cast<int*>(handle);
  --live_apps;
  return kOk;
}

EXPORT ULONG WINAPI SKF_EnumContainer(HANDLE, LPSTR names, ULONG* size) {
  static constexpr char containers[] = "test-container\0";
  return Copy(containers, sizeof(containers), names, size);
}

EXPORT ULONG WINAPI SKF_OpenContainer(HANDLE, LPSTR name, HANDLE* out) {
  if (!EqualInput(name, "test-container") || mode == 6) {
    return 0x0a00001b;
  }
  *out = new int(3);
  ++live_containers;
  return kOk;
}

EXPORT ULONG WINAPI SKF_CloseContainer(HANDLE handle) {
  delete static_cast<int*>(handle);
  --live_containers;
  return kOk;
}

EXPORT ULONG WINAPI SKF_GetContainerType(HANDLE, ULONG* type) {
  *type = mode == 7 ? 1 : kEccContainer;
  return kOk;
}

EXPORT ULONG WINAPI SKF_ExportCertificate(HANDLE,
                                          BOOL signing,
                                          BYTE* cert,
                                          ULONG* size) {
  // Merely distinct DER bytes for plumbing, not a real X.509 certificate.
  const BYTE data[] = {0x30, 0x03, 0x02, 0x01,
                       static_cast<BYTE>(signing ? 1 : 2)};
  if (mode == 8) {
    *size = 1000000;
    return kOk;
  }
  if (mode == 9 && cert) {
    *size += 1;
    return kOk;
  }
  return Copy(data, sizeof(data), cert, size);
}

EXPORT ULONG WINAPI SKF_VerifyPIN(HANDLE,
                                  ULONG kind,
                                  LPSTR pin,
                                  ULONG* retries) {
  ++pin_calls;
  authenticated = false;
  if (kind != kUserPin) {
    return 0x0a00002a;
  }
  if (mode == 10) {
    *retries = 0;
    return kPinLocked;
  }
  if (!EqualInput(pin, "123456")) {
    *retries = 2;
    return kPinIncorrect;
  }
  authenticated = true;
  *retries = 6;
  return kOk;
}

EXPORT ULONG WINAPI SKF_ClearSecureState(HANDLE) {
  authenticated = false;
  return kOk;
}

#if !defined(GM_FAKE_MISSING_EXPORT)
EXPORT ULONG WINAPI SKF_ECCSignData(HANDLE,
                                    BYTE*,
                                    ULONG length,
                                    EccSignatureBlob* signature) {
  ++sign_calls;
  if (!authenticated) {
    return kNotLoggedIn;
  }
  if (length != 32) {
    return 0x0a000010;
  }
  if (mode == 11) {
    return kDeviceRemoved;
  }
  *signature = {};
  signature->r[63] = 1;
  signature->s[32] = 0x80;  // DER must insert a sign padding byte.
  if (mode == 12) {
    signature->r[0] = 1;
  }
  if (mode == 13) {
    std::ranges::fill(std::span(signature->r).last<32>(), BYTE{0xff});
  }
  if (mode == 14) {
    std::ranges::fill(signature->s, BYTE{0});
  }
  return kOk;
}
#endif
