// Copyright 2017 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifdef UNSAFE_BUFFERS_BUILD
// TODO(crbug.com/40285824): Remove this and convert code to safer constructs.
#pragma allow_unsafe_buffers
#endif

#include <stdint.h>

#include "base/clang_profiling_buildflags.h"
#include "build/build_config.h"
#include "chrome/installer/mini_installer/mini_installer.h"

// http://blogs.msdn.com/oldnewthing/archive/2004/10/25/247180.aspx
extern "C" IMAGE_DOS_HEADER __ImageBase;

extern "C" int __stdcall MainEntryPoint() {
  mini_installer::ProcessExitResult result =
      mini_installer::WMain(reinterpret_cast<HMODULE>(&__ImageBase));
  ::ExitProcess(result.exit_code);
}

#if defined(ADDRESS_SANITIZER) || BUILDFLAG(CLANG_PROFILING) || \
    (defined(MINI_INSTALLER_HAS_DUILIB_UI) && MINI_INSTALLER_HAS_DUILIB_UI)
// CRT-linked builds need a normal Windows entry point.
extern "C" int WINAPI wWinMain(HINSTANCE /* instance */,
                               HINSTANCE /* previous_instance */,
                               LPWSTR /* command_line */,
                               int /* command_show */) {
  return MainEntryPoint();
}
#endif

#if !defined(MINI_INSTALLER_HAS_DUILIB_UI) || !MINI_INSTALLER_HAS_DUILIB_UI
// We don't link with the CRT (this is enforced through use of the /ENTRY linker
// flag) so we have to implement CRT functions that the compiler generates calls
// to.
extern "C" {
__attribute__((used)) void* memset(void* dest, int c, size_t count) {
  uint8_t* scan = reinterpret_cast<uint8_t*>(dest);
  while (count--)
    *scan++ = static_cast<uint8_t>(c);
  return dest;
}

#if defined(_DEBUG) && defined(ARCH_CPU_ARM64)
__attribute__((used)) void* memcpy(void* destination,
                                   const void* source,
                                   size_t count) {
  auto* dst = reinterpret_cast<uint8_t*>(destination);
  auto* src = reinterpret_cast<const uint8_t*>(source);
  while (count--)
    *dst++ = *src++;
  return destination;
}
#endif
}  // extern "C"
#endif  // !MINI_INSTALLER_HAS_DUILIB_UI
