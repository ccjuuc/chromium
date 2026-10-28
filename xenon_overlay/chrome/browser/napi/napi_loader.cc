// Copyright 2026 The Chromium Authors. All rights reserved.
// Use of this source code is governed by BSD-style license that can be
// found in the LICENSE file.

#include "napi_loader.h"

#include <cstdint>

#include "js_native_api_v8.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/logging.h"
#include "base/native_library.h"
#include "base/path_service.h"
#include "build/build_config.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/napi/napi_switches.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#elif BUILDFLAG(IS_MAC)
#include <dlfcn.h>
#endif

namespace xenon {

LoadedNodeAddon::LoadedNodeAddon() = default;
LoadedNodeAddon::~LoadedNodeAddon() = default;
LoadedNodeAddon::LoadedNodeAddon(LoadedNodeAddon&&) = default;
LoadedNodeAddon& LoadedNodeAddon::operator=(LoadedNodeAddon&&) = default;

#if BUILDFLAG(IS_WIN)
base::NativeLibrary GetLoadedNodeExeModule() {
  HMODULE module = ::GetModuleHandleW(L"node.exe");
  if (!module) {
    base::FilePath exe_dir;
    if (base::PathService::Get(base::DIR_EXE, &exe_dir)) {
      base::FilePath node_exe = exe_dir.Append(FILE_PATH_LITERAL("node.exe"));
      module = ::LoadLibraryW(node_exe.value().c_str());
    }
  }
  return reinterpret_cast<base::NativeLibrary>(module);
}

void EnsureNodeHostLibraryLoaded() {
  GetLoadedNodeExeModule();
}
#endif

namespace {

using RegisterFunc = napi_value (*)(napi_env, napi_value);

#if BUILDFLAG(IS_WIN) && BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
using GetRegisteredModuleCountFunc = size_t (*)();
using GetRegisteredModuleFunc = napi_module* (*)(size_t);

base::NativeLibrary GetChromeDllModule() {
#if defined(CUSTOM_CHROME_DLL_NAME_W)
  HMODULE module = ::GetModuleHandleW(CUSTOM_CHROME_DLL_NAME_W);
#else
  HMODULE module = ::GetModuleHandleW(L"chrome.dll");
#endif
  return reinterpret_cast<base::NativeLibrary>(module);
}

base::NativeLibrary GetCurrentProcessExeModule() {
  return reinterpret_cast<base::NativeLibrary>(::GetModuleHandleW(nullptr));
}

size_t GetRegisteredModuleCount(base::NativeLibrary library) {
  if (!library) {
    return 0;
  }
  auto* count_func = reinterpret_cast<GetRegisteredModuleCountFunc>(
      base::GetFunctionPointerFromNativeLibrary(
          library, "xenon_napi_get_registered_module_count"));
  if (!count_func) {
    return 0;
  }
  return count_func();
}

napi_module* GetRegisteredModule(base::NativeLibrary library, size_t index) {
  if (!library) {
    return nullptr;
  }
  auto* get_func = reinterpret_cast<GetRegisteredModuleFunc>(
      base::GetFunctionPointerFromNativeLibrary(
          library, "xenon_napi_get_registered_module"));
  if (!get_func) {
    return nullptr;
  }
  return get_func(index);
}

bool IsAddressInModule(base::NativeLibrary library, const void* address) {
  if (!library || !address) {
    return false;
  }

  const uintptr_t module_start = reinterpret_cast<uintptr_t>(library);
  const auto* dos_header =
      reinterpret_cast<const IMAGE_DOS_HEADER*>(module_start);
  if (dos_header->e_magic != IMAGE_DOS_SIGNATURE) {
    return false;
  }

  // Reject clearly invalid e_lfanew before reading NT headers.
  if (dos_header->e_lfanew <= 0) {
    return false;
  }

  const auto* nt_headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(
      module_start + static_cast<uintptr_t>(dos_header->e_lfanew));
  if (nt_headers->Signature != IMAGE_NT_SIGNATURE) {
    return false;
  }

  const uintptr_t module_end =
      module_start + nt_headers->OptionalHeader.SizeOfImage;
  const uintptr_t address_value = reinterpret_cast<uintptr_t>(address);
  return address_value >= module_start && address_value < module_end;
}

bool IsRegisteredModuleOwnedByAddon(napi_module* registered_module,
                                    base::NativeLibrary addon_library) {
  if (!registered_module) {
    return false;
  }

  // Node's static registration macros place the napi_module in the addon
  // image. Checking the record address before dereferencing it also avoids
  // touching stale entries left by a previously failed library load.
  return IsAddressInModule(addon_library, registered_module);
}

napi_module* FindRegisteredModuleForAddon(
    base::NativeLibrary host_library,
    const char* host_name,
    base::NativeLibrary addon_library,
    const char** registered_host_name) {
  const size_t module_count = GetRegisteredModuleCount(host_library);
  for (size_t index = 0; index < module_count; ++index) {
    napi_module* registered_module = GetRegisteredModule(host_library, index);
    if (IsRegisteredModuleOwnedByAddon(registered_module, addon_library)) {
      if (registered_host_name) {
        *registered_host_name = host_name;
      }
      return registered_module;
    }
  }
  return nullptr;
}

napi_module* FindRegisteredModuleAcrossHosts(
    base::NativeLibrary addon_library,
    const char** registered_host_name) {
  const base::NativeLibrary chrome_dll = GetChromeDllModule();
  const base::NativeLibrary process_exe = GetCurrentProcessExeModule();
  const base::NativeLibrary node_exe = GetLoadedNodeExeModule();

  napi_module* found = FindRegisteredModuleForAddon(
      chrome_dll, "xenon/chrome dll", addon_library, registered_host_name);
  if (found) {
    return found;
  }
  if (process_exe && process_exe != chrome_dll) {
    found = FindRegisteredModuleForAddon(
        process_exe, "process exe", addon_library, registered_host_name);
    if (found) {
      return found;
    }
  }
  if (node_exe && node_exe != chrome_dll && node_exe != process_exe) {
    found = FindRegisteredModuleForAddon(
        node_exe, "node.exe sidecar", addon_library, registered_host_name);
    if (found) {
      return found;
    }
  }
  return nullptr;
}

size_t GetRegisteredModuleCountAcrossHosts() {
  return GetRegisteredModuleCount(GetChromeDllModule()) +
         GetRegisteredModuleCount(GetCurrentProcessExeModule()) +
         GetRegisteredModuleCount(GetLoadedNodeExeModule());
}
#elif BUILDFLAG(IS_MAC) && BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
bool IsRegisteredModuleOwnedByAddon(napi_module* registered_module,
                                    base::NativeLibrary addon_library) {
  if (!registered_module || !addon_library ||
      addon_library->type != base::DYNAMIC_LIB) {
    return false;
  }

  // 注册表可能保留已卸载模块的地址，先询问 dyld，不能先解引用记录。
  Dl_info info = {};
  if (!dladdr(registered_module, &info) || !info.dli_fname) {
    return false;
  }
  // 仅查找现有 image，避免误加载同名文件或触发其他 addon 的构造函数。
  void* owner = dlopen(info.dli_fname, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
  if (!owner) {
    return false;
  }
  const bool matches = owner == addon_library->dylib;
  dlclose(owner);
  return matches;
}

napi_module* FindRegisteredModuleAcrossHosts(
    base::NativeLibrary addon_library,
    const char** registered_host_name) {
  const size_t count = xenon_napi_get_registered_module_count();
  for (size_t index = 0; index < count; ++index) {
    napi_module* module = xenon_napi_get_registered_module(index);
    if (IsRegisteredModuleOwnedByAddon(module, addon_library)) {
      *registered_host_name = "xenon host";
      return module;
    }
  }
  return nullptr;
}

size_t GetRegisteredModuleCountAcrossHosts() {
  return xenon_napi_get_registered_module_count();
}
#endif

#if (BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)) && \
    BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
bool IsNodeStaticRegistrationEnabled() {
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();
  return !command_line->HasSwitch(
      napi_switches::kDisableNodeStaticRegistration);
}
#endif

}  // namespace

base::ScopedNativeLibrary LoadNodeAddonLibrary(
    const base::FilePath& addon_path,
    base::NativeLibraryLoadError* error) {
  base::ScopedNativeLibrary library(base::LoadNativeLibrary(addon_path, error));
  if (!library.is_valid()) {
    return {};
  }
#if BUILDFLAG(IS_MAC)
  // env 的同步清理可能排队 uv_close 回调。保留 image，而非延长 env 寿命，
  // 确保后续正常 loop 泵执行 native-only close 时仍有有效代码和静态注册记录。
  void* retained = dlopen(addon_path.value().c_str(),
                          RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD | RTLD_NODELETE);
  if (!retained) {
    if (error) {
      const char* message = dlerror();
      error->message =
          message ? message : "Failed to retain native addon image";
    }
    return {};
  }
  const bool matches = library.get()->type == base::DYNAMIC_LIB &&
                       retained == library.get()->dylib;
  dlclose(retained);
  if (!matches) {
    if (error) {
      error->message = "Native addon image changed during loading";
    }
    return {};
  }
#endif
  return library;
}

v8::Local<v8::Value> LoadAndInitializeNodeAddon(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    const base::FilePath& addon_path,
    LoadedNodeAddon* loaded_addon) {
  VLOG(1) << "[NapiLoader] Loading native library from: " << addon_path.value();
#if BUILDFLAG(IS_WIN)
  EnsureNodeHostLibraryLoaded();
#endif
  base::NativeLibraryLoadError error;
  base::ScopedNativeLibrary library = LoadNodeAddonLibrary(addon_path, &error);
  if (!library.is_valid()) {
    LOG(ERROR) << "[NapiLoader] Failed to load Node-API addon: "
               << error.ToString();
    return {};
  }
  return InitializeLoadedNodeAddon(isolate, context, std::move(library),
                                   loaded_addon);
}

v8::Local<v8::Value> InitializeLoadedNodeAddon(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    base::ScopedNativeLibrary library,
    LoadedNodeAddon* loaded_addon) {
  if (!loaded_addon || !library.is_valid()) {
    return {};
  }
  base::NativeLibrary library_handle = library.get();
  VLOG(1) << "[NapiLoader] Resolving module entry point...";
  RegisterFunc register_func =
      reinterpret_cast<RegisterFunc>(base::GetFunctionPointerFromNativeLibrary(
          library_handle, "napi_register_module_v1"));

  if (!register_func) {
    register_func = reinterpret_cast<RegisterFunc>(
        base::GetFunctionPointerFromNativeLibrary(library_handle,
                                                  "node_register_module_v1"));
  }

#if (BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)) && \
    BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (!register_func) {
    const char* registered_host_name = nullptr;
    napi_module* registered_module =
        FindRegisteredModuleAcrossHosts(library_handle, &registered_host_name);

    if (registered_module) {
      RegisterFunc registered_func = registered_module->nm_register_func;
      const char* registered_module_name = registered_module->nm_modname;
      if (registered_func && IsNodeStaticRegistrationEnabled()) {
        VLOG(1) << "[NapiLoader] Using napi_module_register entry point from "
                << registered_host_name << ": "
                << (registered_module_name ? registered_module_name
                                           : "<unnamed>");
        register_func = registered_func;
      } else if (!IsNodeStaticRegistrationEnabled()) {
        LOG(ERROR) << "[NapiLoader] Addon registered through "
                      "napi_module_register, but --"
                   << napi_switches::kDisableNodeStaticRegistration
                   << " is set.";
      }
    }
  }
#elif BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)
  if (!register_func) {
    LOG(ERROR) << "[NapiLoader] Node/uv compatibility is disabled by GN arg "
                  "enable_xenon_node_uv_compat.";
  }
#endif

  if (!register_func) {
#if (BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC)) && \
    BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
    LOG(ERROR) << "[NapiLoader] Addon does not export napi_register_module_v1/"
                  "node_register_module_v1, and no napi_module_register record "
                  "belongs to this image. registered_count="
               << GetRegisteredModuleCountAcrossHosts();
#else
    LOG(ERROR) << "[NapiLoader] Addon does not export entry point.";
#endif
    return v8::Local<v8::Value>();
  }
  VLOG(1) << "[NapiLoader] Entry point resolved. Creating N-API env...";

  auto env = std::make_unique<napi_env__>(isolate, context);
  napi_handle_scope initialization_scope = nullptr;
  if (napi_open_handle_scope(env.get(), &initialization_scope) != napi_ok) {
    return {};
  }

  v8::Local<v8::Object> exports_obj = v8::Object::New(isolate);
  napi_value exports = env->CreateValue(exports_obj);

  VLOG(1) << "[NapiLoader] Invoking addon entry point (register_func)...";
  napi_value result = register_func(env.get(), exports);

  if (!result) {
    LOG(ERROR) << "[NapiLoader] Failed to initialize Node-API addon: register_func returned null";
    napi_close_handle_scope(env.get(), initialization_scope);
    return {};
  }

  v8::Global<v8::Value> persistent_result(isolate, result->Get());
  napi_close_handle_scope(env.get(), initialization_scope);
  v8::Local<v8::Value> exports_result = persistent_result.Get(isolate);
  persistent_result.Reset();
  loaded_addon->library = std::move(library);
  loaded_addon->env = std::move(env);
  VLOG(1) << "[NapiLoader] Addon initialization complete.";
  return exports_result;
}

}  // namespace xenon
