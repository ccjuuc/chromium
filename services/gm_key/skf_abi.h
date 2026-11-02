// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_GM_KEY_SKF_ABI_H_
#define SERVICES_GM_KEY_SKF_ABI_H_

#include <windows.h>

#include <cstdint>

// Minimal declarations of the GM/T 0016 Windows ABI. These are interface
// declarations, not a vendor SDK implementation. No vendor DLL is distributed.
namespace gm_key::internal {

static_assert(sizeof(void*) == 8, "SKF integration requires 64-bit Windows");
static_assert(sizeof(ULONG) == 4 && sizeof(BOOL) == 4);

inline constexpr ULONG kOk = 0;
inline constexpr ULONG kBufferTooSmall = 0x0a000020;
inline constexpr ULONG kDeviceRemoved = 0x0a000023;
inline constexpr ULONG kPinIncorrect = 0x0a000024;
inline constexpr ULONG kPinLocked = 0x0a000025;
inline constexpr ULONG kNotLoggedIn = 0x0a00002d;
inline constexpr ULONG kUserPin = 1;
inline constexpr ULONG kEccContainer = 2;

struct EccSignatureBlob {
  BYTE r[64];
  BYTE s[64];
};
static_assert(sizeof(EccSignatureBlob) == 128);

struct SkfApi {
  ULONG(WINAPI* enum_dev)(BOOL, LPSTR, ULONG*) = nullptr;
  ULONG(WINAPI* connect_dev)(LPSTR, HANDLE*) = nullptr;
  ULONG(WINAPI* disconnect_dev)(HANDLE) = nullptr;
  ULONG(WINAPI* enum_application)(HANDLE, LPSTR, ULONG*) = nullptr;
  ULONG(WINAPI* open_application)(HANDLE, LPSTR, HANDLE*) = nullptr;
  ULONG(WINAPI* close_application)(HANDLE) = nullptr;
  ULONG(WINAPI* enum_container)(HANDLE, LPSTR, ULONG*) = nullptr;
  ULONG(WINAPI* open_container)(HANDLE, LPSTR, HANDLE*) = nullptr;
  ULONG(WINAPI* close_container)(HANDLE) = nullptr;
  ULONG(WINAPI* get_container_type)(HANDLE, ULONG*) = nullptr;
  ULONG(WINAPI* export_certificate)(HANDLE, BOOL, BYTE*, ULONG*) = nullptr;
  ULONG(WINAPI* verify_pin)(HANDLE, ULONG, LPSTR, ULONG*) = nullptr;
  ULONG(WINAPI* clear_secure_state)(HANDLE) = nullptr;
  ULONG(WINAPI* ecc_sign_data)
  (HANDLE, BYTE*, ULONG, EccSignatureBlob*) = nullptr;
};

}  // namespace gm_key::internal

#endif  // SERVICES_GM_KEY_SKF_ABI_H_
