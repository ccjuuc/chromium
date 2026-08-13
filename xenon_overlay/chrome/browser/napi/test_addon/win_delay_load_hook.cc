// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <windows.h>
#include <delayimp.h>

namespace {

FARPROC WINAPI LoadNodeFromCurrentProcess(unsigned int event,
                                           DelayLoadInfo* info) {
  if (event != dliNotePreLoadLibrary ||
      ::lstrcmpiA(info->szDll, "node.exe") != 0) {
    return nullptr;
  }
  return reinterpret_cast<FARPROC>(::GetModuleHandle(nullptr));
}

}  // namespace

decltype(__pfnDliNotifyHook2) __pfnDliNotifyHook2 =
    LoadNodeFromCurrentProcess;
