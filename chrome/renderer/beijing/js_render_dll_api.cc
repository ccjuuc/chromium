// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/beijing/js_render_dll_api.h"

#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/native_library.h"
#include "base/strings/stringprintf.h"
#include "build/build_config.h"
#include "chrome/common/beijing/render_dll_names.h"
#include "content/public/common/isolated_world_ids.h"
#include "gin/converter.h"
#include "gin/object_template_builder.h"
#include "third_party/blink/public/platform/scheduler/web_agent_group_scheduler.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "v8/include/cppgc/allocation.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-microtask-queue.h"
#include "v8/include/v8-object.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#endif

namespace beijing {

namespace {

#if !defined(OFFICIAL_BUILD)
constexpr char kGlobalApiName[] = "renderDll";

void DefineGlobalReadOnly(v8::Isolate* isolate,
                          v8::Local<v8::Context> context,
                          v8::Local<v8::Object> global,
                          v8::Local<v8::String> name,
                          v8::Local<v8::Value> value) {
  v8::PropertyDescriptor desc(value, false);
  desc.set_configurable(false);
  desc.set_enumerable(true);
  global->DefineProperty(context, name, desc).Check();
}
#endif  // !defined(OFFICIAL_BUILD)

using AddFunc = int (*)(int, int);

#if BUILDFLAG(IS_WIN) && defined(COMPONENT_BUILD) && \
    !defined(OFFICIAL_BUILD)
base::NativeLibrary LoadRenderDllModule(
    const base::FilePath& path,
    base::NativeLibraryLoadError* load_error) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          kPreloadRenderDllSwitch)) {
    // PreSandboxStartup holds a process-lifetime reference. Acquire an
    // additional reference for this API instance without mapping a new image
    // after delayed CIG has been applied.
    HMODULE module = nullptr;
    if (::GetModuleHandleExW(0, path.value().c_str(), &module)) {
      return module;
    }
  }
  return base::LoadNativeLibrary(path, load_error);
}
#endif

void ThrowJsError(v8::Isolate* isolate, const std::string& message) {
  isolate->ThrowException(
      v8::Exception::Error(gin::StringToV8(isolate, message)));
}

v8::Local<v8::Object> MakeLoadSuccessObject(v8::Isolate* isolate,
                                            v8::Local<v8::Context> context,
                                            const std::string& path_str,
                                            base::NativeLibrary handle,
                                            bool already_loaded) {
  v8::Local<v8::Object> result = v8::Object::New(isolate);
  result
      ->Set(context, gin::StringToV8(isolate, "success"),
            v8::Boolean::New(isolate, true))
      .Check();
  result
      ->Set(context, gin::StringToV8(isolate, "path"),
            gin::StringToV8(isolate, path_str))
      .Check();
  result
      ->Set(context, gin::StringToV8(isolate, "handle"),
            gin::StringToV8(isolate, base::StringPrintf("%p", handle)))
      .Check();
  if (already_loaded) {
    result
        ->Set(context, gin::StringToV8(isolate, "alreadyLoaded"),
              v8::Boolean::New(isolate, true))
        .Check();
  }
  return result;
}

#if BUILDFLAG(IS_WIN)
std::string DescribeLoadError(const base::NativeLibraryLoadError& load_error,
                              const std::string& module_name) {
  switch (load_error.code) {
    case ERROR_ACCESS_DENIED:
      return "ACCESS_DENIED (5): sandbox blocked open of " + module_name +
             " - ensure stem is in render_dll_names.h, file sits next to "
             "exe, and browser was fully restarted after rebuild";
    case ERROR_INVALID_IMAGE_HASH:
#if defined(COMPONENT_BUILD) && !defined(OFFICIAL_BUILD)
      return "INVALID_IMAGE_HASH (577): CIG blocked unsigned image " +
             module_name +
             " - fully restart browser after rebuild; component Debug "
             "preloads only the render-dll-test renderer before delayed CIG, "
             "while non-component builds whitelist via AllowExtraDll";
#else
      return "INVALID_IMAGE_HASH (577): CIG blocked unsigned image " +
             module_name +
             " - AllowExtraDll must cover this stem in PreSpawnChild "
             "(non-component build)";
#endif
    case ERROR_MOD_NOT_FOUND:
      return "MOD_NOT_FOUND (126): missing " + module_name +
             " or one of its imports. Add stems to render_dll_names.h and "
             "build next to the exe";
    case ERROR_PROC_NOT_FOUND:
      return "PROC_NOT_FOUND (127): " + module_name +
             " loaded but a dependency export is missing";
    default:
      return module_name + ": " + load_error.ToString();
  }
}
#endif

v8::Local<v8::Object> MakeLoadFailureObject(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    const base::NativeLibraryLoadError& load_error,
    const std::string& module_name) {
  v8::Local<v8::Object> result = v8::Object::New(isolate);
  result
      ->Set(context, gin::StringToV8(isolate, "success"),
            v8::Boolean::New(isolate, false))
      .Check();
#if BUILDFLAG(IS_WIN)
  const std::string error_msg = DescribeLoadError(load_error, module_name);
#else
  const std::string error_msg = module_name + ": " + load_error.ToString();
#endif
  result
      ->Set(context, gin::StringToV8(isolate, "error"),
            gin::StringToV8(isolate, error_msg))
      .Check();
  result
      ->Set(context, gin::StringToV8(isolate, "code"),
            v8::Integer::New(isolate, load_error.code))
      .Check();
  result
      ->Set(context, gin::StringToV8(isolate, "module"),
            gin::StringToV8(isolate, module_name))
      .Check();
  return result;
}

}  // namespace

// static
void JSRenderDllApi::Install(content::RenderFrame* render_frame) {
#if defined(OFFICIAL_BUILD)
  (void)render_frame;
#else
  CHECK(render_frame);
  blink::WebLocalFrame* web_frame = render_frame->GetWebFrame();
  v8::Isolate* isolate = web_frame->GetAgentGroupScheduler()->Isolate();
  v8::HandleScope handle_scope(isolate);
  v8::Local<v8::Context> context = web_frame->MainWorldScriptContext();
  if (context.IsEmpty()) {
    return;
  }

  v8::MicrotasksScope microtasks(
      isolate, context->GetMicrotaskQueue(),
      v8::MicrotasksScope::kDoNotRunMicrotasks);
  v8::Context::Scope context_scope(context);

  v8::Local<v8::Object> global = context->Global();
  v8::Local<v8::Value> existing =
      global->Get(context, gin::StringToV8(isolate, kGlobalApiName))
          .ToLocalChecked();
  if (!existing->IsUndefined()) {
    return;
  }

  JSRenderDllApi* api = cppgc::MakeGarbageCollected<JSRenderDllApi>(
      isolate->GetCppHeap()->GetAllocationHandle(), render_frame);
  v8::Local<v8::Object> wrapper;
  if (!api->GetWrapper(isolate).ToLocal(&wrapper)) {
    return;
  }

  DefineGlobalReadOnly(isolate, context, global,
                       gin::StringToV8(isolate, kGlobalApiName), wrapper);
#endif  // defined(OFFICIAL_BUILD)
}

JSRenderDllApi::JSRenderDllApi(content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {}

JSRenderDllApi::~JSRenderDllApi() = default;

const gin::WrapperInfo* JSRenderDllApi::wrapper_info() const {
  return &kWrapperInfo;
}

void JSRenderDllApi::Trace(cppgc::Visitor* visitor) const {
  gin::Wrappable<JSRenderDllApi>::Trace(visitor);
}

gin::ObjectTemplateBuilder JSRenderDllApi::GetObjectTemplateBuilder(
    v8::Isolate* isolate) {
  return gin::Wrappable<JSRenderDllApi>::GetObjectTemplateBuilder(isolate)
      .SetMethod("loadDll", &JSRenderDllApi::LoadDll)
      .SetMethod("unloadDll", &JSRenderDllApi::UnloadDll)
      .SetMethod("invokeAdd", &JSRenderDllApi::InvokeAdd)
      .SetMethod("getStatus", &JSRenderDllApi::GetStatus);
}

void JSRenderDllApi::ClearLoadedLibraries() {
  // Drop main before deps so the loader can release imports cleanly.
  library_ = base::ScopedNativeLibrary();
  dependency_libraries_.clear();
  loaded_path_.clear();
}

void JSRenderDllApi::WillReleaseScriptContext(v8::Local<v8::Context>,
                                              int32_t world_id) {
  if (world_id != content::ISOLATED_WORLD_ID_GLOBAL) {
    return;
  }
  ClearLoadedLibraries();
}

void JSRenderDllApi::OnDestruct() {
  ClearLoadedLibraries();
}

void JSRenderDllApi::LoadDll(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();

  std::string path_str;
  if (!args->GetNext(&path_str) || path_str.empty()) {
    args->ThrowTypeError("loadDll requires a non-empty file path string");
    return;
  }

  const base::FilePath path = base::FilePath::FromUTF8Unsafe(path_str);
  if (!IsAllowedRenderDllLoadPath(path)) {
    const base::FilePath expected = RenderDllPathNextToExe();
    args->ThrowTypeError(
        "loadDll only allows the absolute path next to the browser exe: " +
        (expected.empty() ? std::string("(DIR_EXE unavailable)")
                          : expected.AsUTF8Unsafe()));
    return;
  }

  // Already holding this module: LoadLibrary would return the same handle.
  // ScopedNativeLibrary::reset aborts on self-reset (crbug.com/162971).
  if (library_.is_valid() && loaded_path_ == path) {
    args->Return(MakeLoadSuccessObject(isolate, context, path_str,
                                       library_.get(),
                                       /*already_loaded=*/true));
    return;
  }

  ClearLoadedLibraries();

  // Load private deps by the same absolute exe-dir paths the browser
  // allowlists. Avoids PE-loader search opening imports via a path form that
  // does not match the Win32 AllowFileAccess rule.
  std::vector<base::ScopedNativeLibrary> loaded_deps;
  for (size_t i = 0; i + 1 < kRenderDllModuleStemCount; ++i) {
    const char* stem = kRenderDllModuleStems[i];
    const base::FilePath dep_path = RenderDllPathNextToExeForStem(stem);
    if (dep_path.empty()) {
      args->ThrowTypeError(
          "DIR_EXE unavailable while resolving dependency " +
          base::GetNativeLibraryName(stem));
      return;
    }
    base::NativeLibraryLoadError dep_error;
#if BUILDFLAG(IS_WIN) && defined(COMPONENT_BUILD) && \
    !defined(OFFICIAL_BUILD)
    base::NativeLibrary dep_lib =
        LoadRenderDllModule(dep_path, &dep_error);
#else
    base::NativeLibrary dep_lib =
        base::LoadNativeLibrary(dep_path, &dep_error);
#endif
    if (!dep_lib) {
      args->Return(MakeLoadFailureObject(isolate, context, dep_error,
                                         base::GetNativeLibraryName(stem)));
      return;
    }
    loaded_deps.emplace_back(dep_lib);
  }

  base::NativeLibraryLoadError load_error;
#if BUILDFLAG(IS_WIN) && defined(COMPONENT_BUILD) && \
    !defined(OFFICIAL_BUILD)
  base::NativeLibrary native_lib = LoadRenderDllModule(path, &load_error);
#else
  base::NativeLibrary native_lib =
      base::LoadNativeLibrary(path, &load_error);
#endif

  if (!native_lib) {
    args->Return(MakeLoadFailureObject(isolate, context, load_error,
                                       base::GetNativeLibraryName(
                                           kRenderDllMainStem)));
    return;
  }

  dependency_libraries_ = std::move(loaded_deps);
  if (library_.get() == native_lib) {
    // Same handle already owned (path compare missed); drop extra ref.
    base::UnloadNativeLibrary(native_lib);
  } else {
    library_ = base::ScopedNativeLibrary(native_lib);
  }
  loaded_path_ = path;
  args->Return(MakeLoadSuccessObject(isolate, context, path_str, library_.get(),
                                     /*already_loaded=*/false));
}

void JSRenderDllApi::UnloadDll(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  // Releases API ownership of the on-demand LoadNativeLibrary handle(s).
  const bool was_loaded = library_.is_valid();
  ClearLoadedLibraries();

  v8::Local<v8::Object> result = v8::Object::New(isolate);
  result
      ->Set(args->GetHolderCreationContext(),
            gin::StringToV8(isolate, "unloaded"),
            v8::Boolean::New(isolate, was_loaded))
      .Check();
  args->Return(result);
}

void JSRenderDllApi::InvokeAdd(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();

  if (!library_.is_valid()) {
    ThrowJsError(isolate, "No native library currently loaded");
    return;
  }

  int a = 0;
  int b = 0;
  if (!args->GetNext(&a) || !args->GetNext(&b)) {
    args->ThrowTypeError("invokeAdd requires two integer arguments: (a, b)");
    return;
  }

  const AddFunc add_func =
      reinterpret_cast<AddFunc>(library_.GetFunctionPointer("Add"));
  if (!add_func) {
    ThrowJsError(isolate,
                 "Exported function 'Add' not found in loaded library");
    return;
  }

  args->Return(add_func(a, b));
}

void JSRenderDllApi::GetStatus(gin::Arguments* args) {
  v8::Isolate* isolate = args->isolate();
  v8::Local<v8::Context> context = args->GetHolderCreationContext();

  v8::Local<v8::Object> result = v8::Object::New(isolate);
  result
      ->Set(context, gin::StringToV8(isolate, "loaded"),
            v8::Boolean::New(isolate, library_.is_valid()))
      .Check();
  result
      ->Set(context, gin::StringToV8(isolate, "path"),
            gin::StringToV8(isolate, loaded_path_.AsUTF8Unsafe()))
      .Check();

  args->Return(result);
}

}  // namespace beijing
