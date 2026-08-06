// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Minimal C ABI test library for window.renderDll.loadDll / invokeAdd.
// Stem name: render_dll_add (see base::GetNativeLibraryName).
// Links against render_dll_dep (private shared_library) so loadDll of the
// main module also requires the dep to be next to the exe and on the Win
// AllowFileAccess / AllowExtraDll lists (see render_dll_names.h).

#include "tools/render_dll_add/render_dll_dep.h"

#if defined(_WIN32)
#define RENDER_DLL_EXPORT __declspec(dllexport)
#else
#define RENDER_DLL_EXPORT __attribute__((visibility("default")))
#endif

#if defined(__cplusplus)
extern "C" {
#endif

RENDER_DLL_EXPORT int Add(int a, int b) {
  // Route through the dependency so a missing/blocked dep fails load or call.
  return RenderDllDepCombine(a, b);
}

#if defined(__cplusplus)
}  // extern "C"
#endif
