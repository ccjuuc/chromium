// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Private dependency of render_dll_add — used to verify sandbox/CIG allows
// dependent modules listed in chrome/common/beijing/render_dll_names.h.

#ifndef TOOLS_RENDER_DLL_ADD_RENDER_DLL_DEP_H_
#define TOOLS_RENDER_DLL_ADD_RENDER_DLL_DEP_H_

#if defined(_WIN32)
#if defined(RENDER_DLL_DEP_IMPLEMENTATION)
#define RENDER_DLL_DEP_EXPORT __declspec(dllexport)
#else
#define RENDER_DLL_DEP_EXPORT __declspec(dllimport)
#endif
#else
#define RENDER_DLL_DEP_EXPORT __attribute__((visibility("default")))
#endif

#if defined(__cplusplus)
extern "C" {
#endif

// Simple export so render_dll_add.dll has a hard link dependency on
// render_dll_dep.dll / librender_dll_dep.*.
RENDER_DLL_DEP_EXPORT int RenderDllDepCombine(int a, int b);

#if defined(__cplusplus)
}  // extern "C"
#endif

#endif  // TOOLS_RENDER_DLL_ADD_RENDER_DLL_DEP_H_
