// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_NAPI_NAPI_LOADER_H_
#define XENON_OVERLAY_CHROME_BROWSER_NAPI_NAPI_LOADER_H_

#include <memory>

#include "base/files/file_path.h"
#include "base/scoped_native_library.h"
#include "v8/include/v8.h"

struct napi_env__;

namespace xenon {

struct LoadedNodeAddon {
  LoadedNodeAddon();
  ~LoadedNodeAddon();
  LoadedNodeAddon(LoadedNodeAddon&&);
  LoadedNodeAddon& operator=(LoadedNodeAddon&&);

  LoadedNodeAddon(const LoadedNodeAddon&) = delete;
  LoadedNodeAddon& operator=(const LoadedNodeAddon&) = delete;

  base::ScopedNativeLibrary library;
  std::unique_ptr<napi_env__> env;
};

// Loads a native addon. On macOS a loaded image remains mapped until process
// exit, including when addon initialization subsequently fails. Environments
// are still cleaned up normally; a queued native close callback must not use
// the destroyed environment.
base::ScopedNativeLibrary LoadNodeAddonLibrary(
    const base::FilePath& addon_path,
    base::NativeLibraryLoadError* error);

// Loads a Node-API native addon (.node) into the given V8 context.
// Returns the exports object registered by the addon, or an empty handle on
// failure. |loaded_addon| owns a library reference and the N-API environment on
// success, with the macOS image lifetime described above.
v8::Local<v8::Value> LoadAndInitializeNodeAddon(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    const base::FilePath& addon_path,
    LoadedNodeAddon* loaded_addon);

// Initializes an addon loaded with LoadNodeAddonLibrary on a blocking-capable
// sequence.
v8::Local<v8::Value> InitializeLoadedNodeAddon(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    base::ScopedNativeLibrary library,
    LoadedNodeAddon* loaded_addon);

#if BUILDFLAG(IS_WIN)
void EnsureNodeHostLibraryLoaded();
#endif

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_NAPI_NAPI_LOADER_H_
