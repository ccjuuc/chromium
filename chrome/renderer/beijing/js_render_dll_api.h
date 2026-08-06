// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_BEIJING_JS_RENDER_DLL_API_H_
#define CHROME_RENDERER_BEIJING_JS_RENDER_DLL_API_H_

#include <vector>

#include "base/files/file_path.h"
#include "base/scoped_native_library.h"
#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "gin/arguments.h"
#include "gin/wrappable.h"
#include "v8/include/v8-forward.h"

namespace beijing {

// Renderer-process C++ binding exposed as `window.renderDll` to test loading
// a native library (platform DLL/so/dylib) inside the Renderer process.
// Loads on demand via loadDll(); Win CIG relies on browser AllowExtraDll +
// AllowFileAccess for every stem in render_dll_names.h. Private deps are
// LoadNativeLibrary'd by absolute path before the main module.
class JSRenderDllApi final : public gin::Wrappable<JSRenderDllApi>,
                             public content::RenderFrameObserver {
 public:
  static constexpr gin::WrapperInfo kWrapperInfo = {
      {gin::kEmbedderNativeGin},
      gin::kBeijingRenderDllApi,
  };

  // No-op in OFFICIAL_BUILD. Non-official: injects window.renderDll when the
  // frame observer allows (file:// / localhost / beijing hosts).
  static void Install(content::RenderFrame* render_frame);

  explicit JSRenderDllApi(content::RenderFrame* render_frame);
  ~JSRenderDllApi() override;

  JSRenderDllApi(const JSRenderDllApi&) = delete;
  JSRenderDllApi& operator=(const JSRenderDllApi&) = delete;

  const gin::WrapperInfo* wrapper_info() const override;

  gin::ObjectTemplateBuilder GetObjectTemplateBuilder(
      v8::Isolate* isolate) override;

  void Trace(cppgc::Visitor* visitor) const final;

 private:
  void LoadDll(gin::Arguments* args);
  void UnloadDll(gin::Arguments* args);
  void InvokeAdd(gin::Arguments* args);
  void GetStatus(gin::Arguments* args);

  void WillReleaseScriptContext(v8::Local<v8::Context> context,
                                int32_t world_id) override;
  void OnDestruct() override;

  void ClearLoadedLibraries();

  // Private deps (all stems except main), kept alive while |library_| is held.
  std::vector<base::ScopedNativeLibrary> dependency_libraries_;
  base::ScopedNativeLibrary library_;
  base::FilePath loaded_path_;
};

}  // namespace beijing

#endif  // CHROME_RENDERER_BEIJING_JS_RENDER_DLL_API_H_
