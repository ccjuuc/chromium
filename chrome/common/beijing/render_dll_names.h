// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_BEIJING_RENDER_DLL_NAMES_H_
#define CHROME_COMMON_BEIJING_RENDER_DLL_NAMES_H_

#include <stddef.h>

#include <array>

#include "base/files/file_path.h"
#include "base/native_library.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "build/build_config.h"

namespace beijing {

inline constexpr char kRenderDllTestHost[] = "render-dll-test";

// Internal renderer-process switch added only for chrome://render-dll-test/.
// It is consumed before the renderer sandbox applies delayed CIG.
inline constexpr char kPreloadRenderDllSwitch[] = "preload-render-dll";

// Compile-time module list for window.renderDll (non-official desktop).
// Stems are GN shared_library names (no lib/ prefix, no extension).
// Platform file names via base::GetNativeLibraryName():
//   Win:   <stem>.dll
//   Mac:   lib<stem>.dylib
//   Linux: lib<stem>.so
//
// Order: private dependencies first, JS-loadable main last.
// When adding a shared_library used by the test stack, append its stem here
// and build/install it next to the browser executable. Prefer static_library.
inline constexpr std::array<const char*, 2> kRenderDllModuleStems = {
    "render_dll_dep",  // private dep of render_dll_add (must load with main)
    "render_dll_add",
};

inline constexpr size_t kRenderDllModuleStemCount = kRenderDllModuleStems.size();

static_assert(kRenderDllModuleStemCount > 0,
              "kRenderDllModuleStems must list at least the main module");

// Basename JS loadDll() may request — always the last entry above.
inline constexpr const char* kRenderDllMainStem =
    kRenderDllModuleStems[kRenderDllModuleStemCount - 1];

inline base::FilePath RenderDllFileNameForStem(const char* stem) {
  return base::FilePath::FromUTF8Unsafe(base::GetNativeLibraryName(stem));
}

inline base::FilePath RenderDllFileName() {
  return RenderDllFileNameForStem(kRenderDllMainStem);
}

// Absolute path next to the browser/renderer executable, or empty on failure.
inline base::FilePath RenderDllPathNextToExeForStem(const char* stem) {
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return base::FilePath();
  }
  return exe_dir.Append(RenderDllFileNameForStem(stem));
}

inline base::FilePath RenderDllPathNextToExe() {
  return RenderDllPathNextToExeForStem(kRenderDllMainStem);
}

// True if |path| is exactly the allowlisted main module next to the exe.
inline bool IsAllowedRenderDllLoadPath(const base::FilePath& path) {
  const base::FilePath expected = RenderDllPathNextToExe();
  if (expected.empty() || path.empty()) {
    return false;
  }
  // JS often passes forward slashes (H:/out/...); DIR_EXE uses native
  // separators (H:\out\...). Normalize before comparing.
  const base::FilePath normalized = path.NormalizePathSeparators();
  const base::FilePath expected_normalized = expected.NormalizePathSeparators();
#if BUILDFLAG(IS_WIN)
  return base::EqualsCaseInsensitiveASCII(normalized.value(),
                                          expected_normalized.value());
#else
  return normalized == expected_normalized;
#endif
}

// Invokes |fn(absolute_path)| for each compile-time module next to the exe.
// Returns false only if |fn| returns false. If DIR_EXE is unavailable, no-ops
// and returns true (same as having no modules to register).
template <typename Fn>
bool ForEachRenderDllPathNextToExe(Fn fn) {
  base::FilePath exe_dir;
  if (!base::PathService::Get(base::DIR_EXE, &exe_dir)) {
    return true;
  }
  for (size_t i = 0; i < kRenderDllModuleStemCount; ++i) {
    const base::FilePath path =
        exe_dir.Append(RenderDllFileNameForStem(kRenderDllModuleStems[i]));
    if (!fn(path)) {
      return false;
    }
  }
  return true;
}

}  // namespace beijing

#endif  // CHROME_COMMON_BEIJING_RENDER_DLL_NAMES_H_
