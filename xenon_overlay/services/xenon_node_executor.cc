// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_node_executor.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <utility>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/compiler_specific.h"
#include "base/containers/span.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/process/process_handle.h"
#include "base/scoped_native_library.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/strings/sys_string_conversions.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/unguessable_token.h"
#include "base/values.h"
#include "build/build_config.h"
#include "gin/arguments.h"
#include "gin/array_buffer.h"
#include "gin/converter.h"
#include "gin/dictionary.h"
#include "gin/function_template.h"
#include "gin/public/isolate_holder.h"
#include "gin/try_catch.h"
#include "gin/v8_initializer.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/napi/js_native_api_v8.h"
#include "xenon_overlay/chrome/browser/napi/napi_loader.h"
#include "xenon_overlay/chrome/browser/napi/napi_switches.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <delayimp.h>
#include <intrin.h>
#endif

#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
#include "uv.h"
#endif

namespace xenon {

class NativeAddonResourceRedirect {
 public:
  NativeAddonResourceRedirect(base::NativeLibrary module,
                              const base::FilePath& runtime_directory);
  ~NativeAddonResourceRedirect();

  NativeAddonResourceRedirect(const NativeAddonResourceRedirect&) = delete;
  NativeAddonResourceRedirect& operator=(const NativeAddonResourceRedirect&) =
      delete;

 private:
#if BUILDFLAG(IS_WIN)
  void PatchImports();
  void RestoreImports();

  HMODULE module_ = nullptr;
  std::vector<std::pair<ULONG_PTR*, ULONG_PTR>> import_patches_;
#endif
};

namespace {

constexpr int kMaxValueConversionDepth = 32;
constexpr char kWireTypeKey[] = "__xenon_node_wire_type__";
constexpr char kWireValueKey[] = "value";
constexpr char kNativeInstanceWireType[] = "native_instance";
constexpr char kNativeFunctionWireType[] = "native_function";
constexpr char kInvokePathPrefix[] = "$xenonInvokePath:";
constexpr base::TimeDelta kUvLoopPollInterval = base::Milliseconds(10);
constexpr size_t kMaxPendingNativePromises = 1024;
constexpr base::TimeDelta kDeferredPromiseLifetime = base::Seconds(30);

#if BUILDFLAG(IS_WIN)
struct NativeAddonRedirectState {
  base::Lock lock;
  std::map<HMODULE, base::FilePath> runtime_directories GUARDED_BY(lock);
};

NativeAddonRedirectState& GetNativeAddonRedirectState() {
  static base::NoDestructor<NativeAddonRedirectState> state;
  return *state;
}

std::optional<base::FilePath> GetRuntimeDirectoryForCaller(
    const void* caller_address) {
  HMODULE caller_module = nullptr;
  if (!caller_address ||
      !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(caller_address),
                            &caller_module)) {
    return std::nullopt;
  }

  NativeAddonRedirectState& state = GetNativeAddonRedirectState();
  base::AutoLock lock(state.lock);
  const auto it = state.runtime_directories.find(caller_module);
  if (it == state.runtime_directories.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::optional<base::FilePath> GetRedirectedLibraryPath(
    const wchar_t* requested_path,
    const void* caller_address) {
  if (!requested_path || !*requested_path) {
    return std::nullopt;
  }

  std::optional<base::FilePath> runtime_directory =
      GetRuntimeDirectoryForCaller(caller_address);
  if (!runtime_directory || runtime_directory->empty()) {
    return std::nullopt;
  }

  const base::FilePath original_path(requested_path);
  if (!original_path.IsAbsolute()) {
    return std::nullopt;
  }

  base::FilePath executable_path;
  if (!base::PathService::Get(base::FILE_EXE, &executable_path)) {
    return std::nullopt;
  }

  base::FilePath relative_path;
  if (!executable_path.DirName().AppendRelativePath(original_path,
                                                    &relative_path)) {
    return std::nullopt;
  }

  const base::FilePath redirected_path =
      runtime_directory->Append(relative_path);
  if (redirected_path == original_path || !base::PathExists(redirected_path)) {
    return std::nullopt;
  }

  LOG(INFO) << "[NapiLoader] Redirecting hosted addon library "
            << original_path.AsUTF8Unsafe() << " -> "
            << redirected_path.AsUTF8Unsafe();
  return redirected_path;
}

__declspec(noinline) HMODULE WINAPI HostedLoadLibraryW(LPCWSTR requested_path) {
  const std::optional<base::FilePath> redirected =
      GetRedirectedLibraryPath(requested_path, _ReturnAddress());
  return ::LoadLibraryW(redirected ? redirected->value().c_str()
                                   : requested_path);
}

__declspec(noinline) HMODULE WINAPI HostedLoadLibraryExW(LPCWSTR requested_path,
                                                         HANDLE file,
                                                         DWORD flags) {
  const std::optional<base::FilePath> redirected =
      GetRedirectedLibraryPath(requested_path, _ReturnAddress());
  return ::LoadLibraryExW(
      redirected ? redirected->value().c_str() : requested_path, file, flags);
}

__declspec(noinline) HMODULE WINAPI HostedLoadLibraryA(LPCSTR requested_path) {
  const std::wstring wide_path =
      requested_path ? base::SysNativeMBToWide(requested_path) : std::wstring();
  const std::optional<base::FilePath> redirected = GetRedirectedLibraryPath(
      wide_path.empty() ? nullptr : wide_path.c_str(), _ReturnAddress());
  if (redirected) {
    return ::LoadLibraryW(redirected->value().c_str());
  }
  return ::LoadLibraryA(requested_path);
}

__declspec(noinline) HMODULE WINAPI HostedLoadLibraryExA(LPCSTR requested_path,
                                                         HANDLE file,
                                                         DWORD flags) {
  const std::wstring wide_path =
      requested_path ? base::SysNativeMBToWide(requested_path) : std::wstring();
  const std::optional<base::FilePath> redirected = GetRedirectedLibraryPath(
      wide_path.empty() ? nullptr : wide_path.c_str(), _ReturnAddress());
  if (redirected) {
    return ::LoadLibraryExW(redirected->value().c_str(), file, flags);
  }
  return ::LoadLibraryExA(requested_path, file, flags);
}

void* GetHostedLibraryReplacement(std::string_view function_name) {
  if (function_name == "LoadLibraryW") {
    return reinterpret_cast<void*>(&HostedLoadLibraryW);
  }
  if (function_name == "LoadLibraryExW") {
    return reinterpret_cast<void*>(&HostedLoadLibraryExW);
  }
  if (function_name == "LoadLibraryA") {
    return reinterpret_cast<void*>(&HostedLoadLibraryA);
  }
  if (function_name == "LoadLibraryExA") {
    return reinterpret_cast<void*>(&HostedLoadLibraryExA);
  }
  return nullptr;
}

// Native engines create HWNDs on this Utility thread (getAplayerWnd, etc.).
// Chromium's service process uses an IO pump, not GetMessage, so those
// windows never see WM_PAINT / WM_TIMER unless we drain the queue — Electron
// main does this implicitly. Cap the batch so Mojo/uv stay responsive.
void PumpWin32Messages() {
  MSG msg = {};
  int pumped = 0;
  constexpr int kMaxMessagesPerPump = 32;
  while (pumped < kMaxMessagesPerPump &&
         ::PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) {
      ::PostQuitMessage(static_cast<int>(msg.wParam));
      break;
    }
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
    ++pumped;
  }
}
#endif

std::vector<mojom::NodeInvokeArgPtr> ListValueToInvokeArgs(
    const base::Value& arguments) {
  std::vector<mojom::NodeInvokeArgPtr> invoke_args;
  if (!arguments.is_list()) {
    return invoke_args;
  }
  invoke_args.reserve(arguments.GetList().size());
  for (const base::Value& argument : arguments.GetList()) {
    auto invoke_arg = mojom::NodeInvokeArg::New();
    invoke_arg->is_callback = false;
    invoke_arg->callback_id = 0;
    invoke_arg->value = argument.Clone();
    invoke_args.push_back(std::move(invoke_arg));
  }
  return invoke_args;
}

#if BUILDFLAG(IS_WIN)
LONG CALLBACK LogDelayLoadFailure(PEXCEPTION_POINTERS exception) {
  if (!exception || !exception->ExceptionRecord) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const DWORD code = exception->ExceptionRecord->ExceptionCode;
  if (code !=
          VcppException(ERROR_SEVERITY_ERROR, ERROR_PROC_NOT_FOUND) &&
      code != VcppException(ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND)) {
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const EXCEPTION_RECORD* record = exception->ExceptionRecord;
  if (record->NumberParameters < 1 || !record->ExceptionInformation[0]) {
    LOG(ERROR) << "[NapiLoader] Delay-load exception code=" << code
               << " without DelayLoadInfo";
    return EXCEPTION_CONTINUE_SEARCH;
  }

  const auto* info = reinterpret_cast<const DelayLoadInfo*>(
      record->ExceptionInformation[0]);
  const char* dll_name = info->szDll ? info->szDll : "<unknown>";
  if (info->dlp.fImportByName) {
    LOG(ERROR) << "[NapiLoader] Delay-load failure: dll=" << dll_name
               << ", proc="
               << (info->dlp.szProcName ? info->dlp.szProcName : "<unknown>")
               << ", win_error=" << info->dwLastError;
  } else {
    LOG(ERROR) << "[NapiLoader] Delay-load failure: dll=" << dll_name
               << ", ordinal=" << info->dlp.dwOrdinal
               << ", win_error=" << info->dwLastError;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

std::optional<base::Value> V8ValueToBaseValue(v8::Isolate* isolate,
                                              v8::Local<v8::Context> context,
                                              v8::Local<v8::Value> value,
                                              std::string* error_msg,
                                              int depth);

v8::MaybeLocal<v8::Value> BaseValueToV8Value(v8::Isolate* isolate,
                                             v8::Local<v8::Context> context,
                                             const base::Value& value,
                                             std::string* error_msg,
                                             int depth);

base::FilePath ResolveAddonPath(const std::string& path) {
  base::FilePath addon_path = base::FilePath::FromUTF8Unsafe(path);
  if (!addon_path.IsAbsolute()) {
    base::FilePath exe_dir;
    if (base::PathService::Get(base::DIR_EXE, &exe_dir)) {
      addon_path = exe_dir.Append(addon_path);
    }
  }

  // This helper is also used by invocation hot paths and must stay free of
  // filesystem access. PrepareAddon() performs canonicalization and security
  // checks on a blocking-capable ThreadPool sequence before the first load.
  return addon_path.StripTrailingSeparators();
}

struct PreparedAddon {
  PreparedAddon() = default;
  ~PreparedAddon() = default;
  PreparedAddon(PreparedAddon&&) = default;
  PreparedAddon& operator=(PreparedAddon&&) = default;

  base::FilePath path;
  base::ScopedNativeLibrary library;
  std::string error;
};

PreparedAddon PrepareAddon(const base::FilePath& requested_path,
                           bool allow_external_addons) {
  PreparedAddon prepared;
  if (!base::FilePath::CompareEqualIgnoreCase(requested_path.Extension(),
                                              FILE_PATH_LITERAL(".node"))) {
    prepared.error = "Native addon must have a .node extension";
    return prepared;
  }

  if (!base::NormalizeFilePath(requested_path, &prepared.path)) {
    prepared.error = "Native addon does not exist or cannot be resolved";
    return prepared;
  }

  if (!allow_external_addons) {
    base::FilePath executable_directory;
    base::FilePath normalized_executable_directory;
    if (!base::PathService::Get(base::DIR_EXE, &executable_directory) ||
        !base::NormalizeFilePath(executable_directory,
                                 &normalized_executable_directory)) {
      prepared.error = "Failed to resolve executable directory";
      return prepared;
    }
    if (!normalized_executable_directory.IsParent(prepared.path)) {
      prepared.error =
          "Native addon is outside the executable directory; start with --" +
          std::string(napi_switches::kAllowExternalNodeAddons) +
          " to allow external development modules";
      return prepared;
    }
  }

#if BUILDFLAG(IS_WIN)
  EnsureNodeHostLibraryLoaded();
  PVOID delay_load_handler =
      AddVectoredExceptionHandler(/*First=*/1, &LogDelayLoadFailure);
#endif
  prepared.library = base::ScopedNativeLibrary(prepared.path);
#if BUILDFLAG(IS_WIN)
  if (delay_load_handler) {
    RemoveVectoredExceptionHandler(delay_load_handler);
  }
#endif
  if (!prepared.library.is_valid()) {
    const base::NativeLibraryLoadError* error = prepared.library.GetError();
    prepared.error = error ? error->ToString() : "Failed to load native addon";
  }
  return prepared;
}

// Top-level exports are depth 0. One additional level exposes class prototypes
// and nested export descriptors without recursively walking arbitrary graphs.
constexpr int kMaxExportInspectDepth = 1;

std::string DescribeExportKind(v8::Isolate* isolate,
                               v8::Local<v8::Value> value) {
  if (value.IsEmpty() || value->IsUndefined()) {
    return "undefined";
  }
  if (value->IsNull()) {
    return "null";
  }
  // Prefer callable detection before IsObject (functions are objects too).
  if (value->IsFunction()) {
    return "function";
  }
  if (value->IsObject()) {
    return "object";
  }
  // Surface the real primitive kind so UI/debug is not a opaque "value".
  if (value->IsNumber()) {
    return "number";
  }
  if (value->IsString()) {
    return "string";
  }
  if (value->IsBoolean()) {
    return "boolean";
  }
  if (value->IsBigInt()) {
    return "bigint";
  }
  if (value->IsSymbol()) {
    return "symbol";
  }
  if (value->IsExternal()) {
    return "external";
  }
  v8::String::Utf8Value type_name(isolate, value->TypeOf(isolate));
  if (*type_name) {
    return *type_name;
  }
  return "undefined";
}

mojom::NodeExportInfoPtr DescribeExportValue(v8::Isolate* isolate,
                                             v8::Local<v8::Context> context,
                                             const std::string& name,
                                             v8::Local<v8::Value> value,
                                             int depth,
                                             int max_depth);

void SetExportValue(v8::Isolate* isolate,
                    v8::Local<v8::Context> context,
                    v8::Local<v8::Value> value,
                    mojom::NodeExportInfo* info) {
  if (!info || value.IsEmpty() || value->IsObject() || value->IsSymbol() ||
      value->IsExternal()) {
    return;
  }

  if (value->IsBigInt()) {
    v8::Local<v8::String> string;
    if (value.As<v8::BigInt>()->ToString(context).ToLocal(&string)) {
      v8::String::Utf8Value utf8(isolate, string);
      info->has_value = true;
      info->value = base::Value(std::string(*utf8 ? *utf8 : ""));
    }
    return;
  }

  std::string error_msg;
  std::optional<base::Value> converted =
      V8ValueToBaseValue(isolate, context, value, &error_msg, 0);
  if (converted) {
    info->has_value = true;
    info->value = std::move(*converted);
  }
}

bool HasExportChildNamed(const std::vector<mojom::NodeExportInfoPtr>& children,
                         const std::string& name) {
  for (const auto& child : children) {
    if (child && child->name == name) {
      return true;
    }
  }
  return false;
}

bool IsBuiltinCtorPrototype(v8::Isolate* isolate,
                            v8::Local<v8::Context> context,
                            v8::Local<v8::Object> candidate,
                            const char* ctor_name) {
  v8::Local<v8::Value> ctor;
  if (!context->Global()
           ->Get(context, gin::StringToV8(isolate, ctor_name))
           .ToLocal(&ctor) ||
      !ctor->IsObject()) {
    return false;
  }
  v8::Local<v8::Value> prototype;
  if (!ctor.As<v8::Object>()
           ->Get(context, gin::StringToV8(isolate, "prototype"))
           .ToLocal(&prototype) ||
      !prototype->IsObject()) {
    return false;
  }
  return candidate->SameValue(prototype);
}

bool IsBuiltinPrototype(v8::Isolate* isolate,
                        v8::Local<v8::Context> context,
                        v8::Local<v8::Object> candidate) {
  static constexpr const char* kCtors[] = {"Object",  "Function", "Number",
                                           "String",  "Boolean",  "Array",
                                           "Date",    "RegExp",   "Error"};
  for (const char* ctor : kCtors) {
    if (IsBuiltinCtorPrototype(isolate, context, candidate, ctor)) {
      return true;
    }
  }
  return false;
}

bool IsPrototypeConstructorLink(v8::Isolate* isolate,
                                v8::Local<v8::Context> context,
                                v8::Local<v8::Object> object,
                                const std::string& name,
                                v8::Local<v8::Value> value) {
  if (name != "constructor" || !value->IsFunction()) {
    return false;
  }
  v8::Local<v8::Value> descriptor_value;
  if (!value.As<v8::Object>()
           ->GetOwnPropertyDescriptor(context,
                                      gin::StringToV8(isolate, "prototype"))
           .ToLocal(&descriptor_value) ||
      !descriptor_value->IsObject()) {
    return false;
  }
  gin::Dictionary descriptor(isolate, descriptor_value.As<v8::Object>());
  v8::Local<v8::Value> prototype;
  return descriptor.Get("value", &prototype) && prototype->SameValue(object);
}

// Collect only own properties. Prototype members are kept in a separate list.
void CollectOwnChildExports(v8::Isolate* isolate,
                            v8::Local<v8::Context> context,
                            v8::Local<v8::Object> object,
                            int child_depth,
                            int max_depth,
                            std::vector<mojom::NodeExportInfoPtr>* children) {
  if (!children || child_depth > max_depth) {
    return;
  }

  v8::Local<v8::Array> keys;
  if (!object
           ->GetOwnPropertyNames(context, v8::PropertyFilter::ALL_PROPERTIES,
                                 v8::KeyConversionMode::kConvertToString)
           .ToLocal(&keys)) {
    return;
  }

  for (uint32_t i = 0; i < keys->Length(); ++i) {
    v8::Local<v8::Value> key;
    if (!keys->Get(context, i).ToLocal(&key)) {
      continue;
    }

    std::string key_str;
    if (!gin::ConvertFromV8(isolate, key, &key_str) ||
        HasExportChildNamed(*children, key_str)) {
      continue;
    }

    v8::Local<v8::Value> descriptor_value;
    if (!key->IsName() ||
        !object->GetOwnPropertyDescriptor(context, key.As<v8::Name>())
             .ToLocal(&descriptor_value) ||
        !descriptor_value->IsObject()) {
      continue;
    }

    gin::Dictionary descriptor(isolate, descriptor_value.As<v8::Object>());
    bool enumerable = false;
    bool writable = false;
    descriptor.Get("enumerable", &enumerable);

    v8::Local<v8::Value> getter;
    v8::Local<v8::Value> setter;
    const bool has_getter =
        descriptor.Get("get", &getter) && getter->IsFunction();
    const bool has_setter =
        descriptor.Get("set", &setter) && setter->IsFunction();
    if (has_getter || has_setter) {
      auto info = mojom::NodeExportInfo::New();
      info->name = key_str;
      info->kind = "property";
      info->enumerable = enumerable;
      info->writable = has_setter;
      children->push_back(std::move(info));
      continue;
    }

    v8::Local<v8::Value> child_value;
    if (!descriptor.Get("value", &child_value)) {
      continue;
    }
    // Ignore only the constructor's structural back-reference. A native own
    // method named bind/call/apply/toString is still part of its real API;
    // inherited built-ins are excluded by the prototype traversal boundary.
    if (IsPrototypeConstructorLink(isolate, context, object, key_str,
                                   child_value)) {
      continue;
    }
    descriptor.Get("writable", &writable);

    auto info = DescribeExportValue(isolate, context, key_str, child_value,
                                    child_depth, max_depth);
    info->enumerable = enumerable;
    info->writable = writable;
    children->push_back(std::move(info));
  }
}

// napi_define_class exports a constructor Function; instance APIs live on
// `Constructor.prototype` as own properties. This is bridge metadata only:
// invocation still uses the original V8 constructor and instance.
bool GetConstructorPrototypeData(v8::Isolate* isolate,
                                 v8::Local<v8::Context> context,
                                 v8::Local<v8::Function> constructor,
                                 v8::Local<v8::Object>* prototype) {
  v8::Local<v8::Value> descriptor_value;
  if (!constructor
           ->GetOwnPropertyDescriptor(context,
                                      gin::StringToV8(isolate, "prototype"))
           .ToLocal(&descriptor_value) ||
      !descriptor_value->IsObject()) {
    return false;
  }
  gin::Dictionary descriptor(isolate, descriptor_value.As<v8::Object>());
  v8::Local<v8::Value> value;
  if (!descriptor.Get("value", &value) || !value->IsObject()) {
    return false;
  }
  *prototype = value.As<v8::Object>();
  return true;
}

bool ConstructorHasInstanceMethods(v8::Isolate* isolate,
                                   v8::Local<v8::Context> context,
                                   v8::Local<v8::Function> constructor) {
  v8::Local<v8::Object> current;
  if (!GetConstructorPrototypeData(isolate, context, constructor, &current)) {
    return false;
  }
  while (!IsBuiltinPrototype(isolate, context, current)) {
    v8::Local<v8::Array> keys;
    if (current
            ->GetOwnPropertyNames(context, v8::PropertyFilter::ALL_PROPERTIES,
                                  v8::KeyConversionMode::kConvertToString)
            .ToLocal(&keys)) {
      for (uint32_t i = 0; i < keys->Length(); ++i) {
        v8::Local<v8::Value> key;
        if (!keys->Get(context, i).ToLocal(&key)) {
          continue;
        }
        std::string key_str;
        if (gin::ConvertFromV8(isolate, key, &key_str) &&
            key_str != "constructor") {
          return true;
        }
      }
    }
    v8::Local<v8::Value> parent = current->GetPrototype();
    if (!parent->IsObject()) {
      break;
    }
    current = parent.As<v8::Object>();
  }
  return false;
}

void CollectPrototypeExports(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Function> function,
    int child_depth,
    int max_depth,
    std::vector<mojom::NodeExportInfoPtr>* prototype_children) {
  v8::Local<v8::Object> current;
  if (!GetConstructorPrototypeData(isolate, context, function, &current)) {
    return;
  }
  while (!IsBuiltinPrototype(isolate, context, current)) {
    CollectOwnChildExports(isolate, context, current, child_depth, max_depth,
                           prototype_children);
    v8::Local<v8::Value> parent = current->GetPrototype();
    if (!parent->IsObject()) {
      break;
    }
    current = parent.As<v8::Object>();
  }
}

mojom::NodeExportInfoPtr DescribeExportValue(v8::Isolate* isolate,
                                             v8::Local<v8::Context> context,
                                             const std::string& name,
                                             v8::Local<v8::Value> value,
                                             int depth,
                                             int max_depth) {
  auto info = mojom::NodeExportInfo::New();
  info->name = name;
  info->kind = DescribeExportKind(isolate, value);
  info->enumerable = true;
  info->writable = false;
  SetExportValue(isolate, context, value, info.get());

  if (value.IsEmpty()) {
    return info;
  }

  bool is_class_export = false;
  if (value->IsFunction() && ConstructorHasInstanceMethods(
                                 isolate, context, value.As<v8::Function>())) {
    is_class_export = true;
    info->kind = "class";
  }

  if (depth >= max_depth) {
    return info;
  }

  if (value->IsFunction()) {
    v8::Local<v8::Function> function = value.As<v8::Function>();
    // Function own properties are static members. Instance members must remain
    // on the proxy prototype so instanceof and method lookup behave normally.
    CollectOwnChildExports(isolate, context, function, depth + 1, max_depth,
                           &info->children);
    if (is_class_export) {
      CollectPrototypeExports(isolate, context, function, depth + 1, max_depth,
                              &info->prototype);
    }
    return info;
  }

  if (!value->IsObject()) {
    return info;
  }

  v8::Local<v8::Object> object = value.As<v8::Object>();
  CollectOwnChildExports(isolate, context, object, depth + 1, max_depth,
                         &info->children);

  v8::Local<v8::Value> proto_value = object->GetPrototype();
  if (proto_value->IsObject()) {
    v8::Local<v8::Object> proto = proto_value.As<v8::Object>();
    if (!IsBuiltinPrototype(isolate, context, proto)) {
      CollectOwnChildExports(isolate, context, proto, depth + 1, max_depth,
                             &info->prototype);
    }
  }

  return info;
}

std::vector<mojom::NodeExportInfoPtr> CloneExportTree(
    const std::vector<mojom::NodeExportInfoPtr>& tree) {
  std::vector<mojom::NodeExportInfoPtr> out;
  out.reserve(tree.size());
  for (const auto& item : tree) {
    out.push_back(item.Clone());
  }
  return out;
}

// The main context needs the exact own shape, including names such as `then`,
// `length` and `toString`. Inspect descriptors instead of evaluating accessors;
// nested objects are inspected only when the main context reads them.
mojom::NodeExportInfoPtr DescribeExactExportValue(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    const std::string& name,
    v8::Local<v8::Value> value) {
  auto describe_leaf = [&](const std::string& leaf_name,
                           v8::Local<v8::Value> leaf) {
    auto info = mojom::NodeExportInfo::New();
    info->name = leaf_name;
    info->kind = DescribeExportKind(isolate, leaf);
    info->enumerable = true;
    SetExportValue(isolate, context, leaf, info.get());
    if (leaf->IsArray()) {
      info->kind = "array";
    }
    if (leaf->IsFunction()) {
      // Looking at a function's shape must not invoke a custom `prototype`
      // getter. Only a data descriptor can supply class prototype metadata.
      v8::Local<v8::Value> descriptor_value;
      if (leaf.As<v8::Object>()
              ->GetOwnPropertyDescriptor(context,
                                         gin::StringToV8(isolate, "prototype"))
              .ToLocal(&descriptor_value) &&
          descriptor_value->IsObject()) {
        v8::Local<v8::Value> prototype;
        gin::Dictionary descriptor(isolate, descriptor_value.As<v8::Object>());
        if (descriptor.Get("value", &prototype) && prototype->IsObject()) {
          v8::Local<v8::Value> current = prototype;
          while (
              current->IsObject() &&
              !IsBuiltinPrototype(isolate, context, current.As<v8::Object>())) {
            v8::Local<v8::Array> names;
            // node-addon-api defines prototype methods as non-enumerable.
            // Classification must use the same complete property set as the
            // prototype metadata collector below.
            if (current.As<v8::Object>()
                    ->GetOwnPropertyNames(
                        context, v8::PropertyFilter::ALL_PROPERTIES,
                        v8::KeyConversionMode::kConvertToString)
                    .ToLocal(&names)) {
              for (uint32_t index = 0; index < names->Length(); ++index) {
                v8::Local<v8::Value> key;
                std::string name;
                if (names->Get(context, index).ToLocal(&key) &&
                    gin::ConvertFromV8(isolate, key, &name) &&
                    name != "constructor") {
                  info->kind = "class";
                  break;
                }
              }
            }
            if (info->kind == "class") {
              break;
            }
            current = current.As<v8::Object>()->GetPrototype();
          }
        }
      }
    }
    return info;
  };
  auto info = describe_leaf(name, value);
  if (!value->IsObject()) {
    return info;
  }
  v8::Local<v8::Object> object = value.As<v8::Object>();
  v8::Local<v8::Array> keys;
  if (!object
           ->GetOwnPropertyNames(context, v8::PropertyFilter::ALL_PROPERTIES,
                                 v8::KeyConversionMode::kConvertToString)
           .ToLocal(&keys)) {
    return nullptr;
  }
  for (uint32_t i = 0; i < keys->Length(); ++i) {
    v8::Local<v8::Value> key;
    v8::Local<v8::Value> descriptor_value;
    std::string key_name;
    if (!keys->Get(context, i).ToLocal(&key) || !key->IsName() ||
        !gin::ConvertFromV8(isolate, key, &key_name) ||
        !object->GetOwnPropertyDescriptor(context, key.As<v8::Name>())
             .ToLocal(&descriptor_value) ||
        !descriptor_value->IsObject()) {
      return nullptr;
    }
    gin::Dictionary descriptor(isolate, descriptor_value.As<v8::Object>());
    v8::Local<v8::Value> child_value;
    mojom::NodeExportInfoPtr child;
    if (descriptor_value.As<v8::Object>()
            ->HasOwnProperty(context, gin::StringToV8(isolate, "value"))
            .FromMaybe(false) &&
        descriptor.Get("value", &child_value)) {
      child = describe_leaf(key_name, child_value);
      descriptor.Get("writable", &child->writable);
    } else {
      child = mojom::NodeExportInfo::New();
      child->name = key_name;
      child->kind = "property";
    }
    descriptor.Get("enumerable", &child->enumerable);
    info->children.push_back(std::move(child));
  }
  if (value->IsFunction() && info->kind == "class") {
    CollectPrototypeExports(isolate, context, value.As<v8::Function>(), 0, 0,
                            &info->prototype);
  }
  return info;
}

std::vector<mojom::NodeExportInfoPtr> BuildExportTree(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Object> exports,
    int max_depth) {
  std::vector<mojom::NodeExportInfoPtr> tree;
  CollectOwnChildExports(isolate, context, exports, /*child_depth=*/0, max_depth,
                         &tree);
  return tree;
}

// Must match GetWrapPrivateKey() in js_native_api_v8.cc. napi_wrap() stores
// the native pointer as v8::Private("napi::wrap"), so objects created with
// napi_create_object + napi_wrap have InternalFieldCount() == 0.
constexpr char kNapiWrapPrivateName[] = "napi::wrap";

bool HasNapiWrap(v8::Isolate* isolate,
                 v8::Local<v8::Context> context,
                 v8::Local<v8::Object> object) {
  v8::Local<v8::Private> key = v8::Private::ForApi(
      isolate, v8::String::NewFromUtf8Literal(isolate, kNapiWrapPrivateName));
  return object->HasPrivate(context, key).FromMaybe(false);
}

bool ObjectHasCallableMembers(v8::Isolate* isolate,
                              v8::Local<v8::Context> context,
                              v8::Local<v8::Object> object) {
  std::vector<mojom::NodeExportInfoPtr> members;
  CollectOwnChildExports(isolate, context, object, 0, 0, &members);
  for (const auto& member : members) {
    if (member && (member->kind == "function" || member->kind == "class")) {
      return true;
    }
  }
  return false;
}

void CollectPrototypeChainMembers(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Object> object,
    std::vector<mojom::NodeExportInfoPtr>* members) {
  CollectOwnChildExports(isolate, context, object, 0, 0, members);
  v8::Local<v8::Value> proto_value = object->GetPrototype();
  while (proto_value->IsObject()) {
    v8::Local<v8::Object> proto = proto_value.As<v8::Object>();
    if (IsBuiltinPrototype(isolate, context, proto)) {
      break;
    }
    CollectOwnChildExports(isolate, context, proto, 0, 0, members);
    proto_value = proto->GetPrototype();
  }
}

bool LooksLikeNativeHandle(v8::Isolate* isolate,
                           v8::Local<v8::Context> context,
                           v8::Local<v8::Value> value) {
  if (value.IsEmpty() || !value->IsObject() || value->IsArray() ||
      value->IsFunction() || value->IsDate() || value->IsPromise() ||
      value->IsArrayBuffer() || value->IsArrayBufferView() || value->IsMap() ||
      value->IsSet() || value->IsRegExp() || value->IsProxy()) {
    return false;
  }

  v8::Local<v8::Object> object = value.As<v8::Object>();
  if (object->InternalFieldCount() > 0 ||
      HasNapiWrap(isolate, context, object)) {
    return true;
  }
  if (ObjectHasCallableMembers(isolate, context, object)) {
    return true;
  }

  v8::Local<v8::Value> proto_value = object->GetPrototype();
  while (proto_value->IsObject()) {
    v8::Local<v8::Object> proto = proto_value.As<v8::Object>();
    if (IsBuiltinPrototype(isolate, context, proto)) {
      break;
    }
    if (ObjectHasCallableMembers(isolate, context, proto)) {
      return true;
    }
    proto_value = proto->GetPrototype();
  }
  return false;
}

base::ListValue NativePrototypeMembersToWire(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    v8::Local<v8::Object> object) {
  std::vector<mojom::NodeExportInfoPtr> members;
  CollectPrototypeChainMembers(isolate, context, object, &members);

  base::ListValue list;
  for (const auto& member : members) {
    if (!member ||
        (member->kind != "function" && member->kind != "class")) {
      continue;
    }
    base::DictValue item;
    item.Set("name", member->name);
    item.Set("kind", "function");
    list.Append(std::move(item));
  }
  return list;
}

bool ResolveExportValue(v8::Isolate* isolate,
                        v8::Local<v8::Context> context,
                        v8::Local<v8::Object> exports,
                        const std::string& export_path,
                        v8::Local<v8::Value>* value,
                        v8::Local<v8::Object>* receiver,
                        std::string* error_msg) {
  if (export_path.empty()) {
    *value = exports;
    *receiver = exports;
    return true;
  }
  const std::vector<std::string> parts = base::SplitString(
      export_path, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (parts.empty()) {
    *error_msg = "Export path is empty";
    return false;
  }

  v8::Local<v8::Value> cursor = exports;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (!cursor->IsObject()) {
      *error_msg = "Export path is not an object: " + export_path;
      return false;
    }

    v8::Local<v8::Object> object = cursor.As<v8::Object>();
    v8::Local<v8::Value> next;
    if (!object->Get(context, gin::StringToV8(isolate, parts[i]))
             .ToLocal(&next) ||
        next->IsUndefined()) {
      *error_msg = "Export is missing: " + export_path;
      return false;
    }

    if (i + 1 == parts.size()) {
      *value = next;
      *receiver = object;
      return true;
    }

    cursor = next;
  }

  *error_msg = "Export is missing: " + export_path;
  return false;
}

bool ResolveExportFunction(v8::Isolate* isolate,
                           v8::Local<v8::Context> context,
                           v8::Local<v8::Object> exports,
                           const std::string& function_name,
                           v8::Local<v8::Function>* function,
                           v8::Local<v8::Object>* receiver,
                           std::string* error_msg) {
  v8::Local<v8::Value> value;
  if (!ResolveExportValue(isolate, context, exports, function_name, &value,
                          receiver, error_msg)) {
    return false;
  }
  if (!value->IsFunction()) {
    *error_msg = "Export is not a function: " + function_name;
    return false;
  }
  *function = value.As<v8::Function>();
  return true;
}

void InstallNodeLikeProcess(v8::Isolate* isolate,
                            v8::Local<v8::Context> context) {
  v8::Local<v8::Object> global = context->Global();
  v8::Local<v8::String> process_key =
      v8::String::NewFromUtf8Literal(isolate, "process");
  v8::Local<v8::Value> existing;
  if (global->Get(context, process_key).ToLocal(&existing) &&
      existing->IsObject()) {
    return;
  }

  base::FilePath exe_path;
  base::PathService::Get(base::FILE_EXE, &exe_path);
  base::FilePath exe_dir;
  base::PathService::Get(base::DIR_EXE, &exe_dir);
  base::FilePath home;
  base::PathService::Get(base::DIR_HOME, &home);
  base::FilePath temp;
  base::PathService::Get(base::DIR_TEMP, &temp);

  v8::Local<v8::Object> process = v8::Object::New(isolate);
  auto set_string = [&](const char* key, const std::string& value) {
    process
        ->Set(context, gin::StringToV8(isolate, key),
              gin::StringToV8(isolate, value))
        .Check();
  };
  set_string("execPath", exe_path.AsUTF8Unsafe());
#if BUILDFLAG(IS_WIN)
  set_string("platform", "win32");
#elif BUILDFLAG(IS_MAC)
  set_string("platform", "darwin");
#else
  set_string("platform", "linux");
#endif
  set_string("arch", "x64");
  process
      ->Set(context, gin::StringToV8(isolate, "pid"),
            v8::Integer::NewFromUnsigned(isolate, base::GetCurrentProcId()))
      .Check();

  v8::Local<v8::Array> argv = v8::Array::New(isolate, 1);
  argv->Set(context, 0, gin::StringToV8(isolate, exe_path.AsUTF8Unsafe()))
      .Check();
  process->Set(context, gin::StringToV8(isolate, "argv"), argv).Check();

  v8::Local<v8::Object> env = v8::Object::New(isolate);
  auto set_env = [&](const char* key, const std::string& value) {
    env->Set(context, gin::StringToV8(isolate, key),
             gin::StringToV8(isolate, value))
        .Check();
  };
  set_env("APP_BASE_DIR", exe_dir.AsUTF8Unsafe());
  set_env("NODE_ENV", "production");
  if (!home.empty()) {
    set_env("USERPROFILE", home.AsUTF8Unsafe());
    set_env("HOME", home.AsUTF8Unsafe());
#if BUILDFLAG(IS_WIN)
    const base::FilePath appdata = home.Append(FILE_PATH_LITERAL("AppData"))
                                       .Append(FILE_PATH_LITERAL("Roaming"));
    const base::FilePath local_appdata =
        home.Append(FILE_PATH_LITERAL("AppData"))
            .Append(FILE_PATH_LITERAL("Local"));
    set_env("APPDATA", appdata.AsUTF8Unsafe());
    set_env("LOCALAPPDATA", local_appdata.AsUTF8Unsafe());
#endif
  }
  if (!temp.empty()) {
    set_env("TEMP", temp.AsUTF8Unsafe());
    set_env("TMP", temp.AsUTF8Unsafe());
  }
  process->Set(context, gin::StringToV8(isolate, "env"), env).Check();

  v8::Local<v8::Function> cwd =
      gin::CreateFunctionTemplate(
          isolate, base::BindRepeating([](gin::Arguments* arguments) {
                     base::FilePath dir;
                     base::PathService::Get(base::DIR_EXE, &dir);
                     arguments->Return(dir.AsUTF8Unsafe());
                   }))
          ->GetFunction(context)
          .ToLocalChecked();
  process->Set(context, gin::StringToV8(isolate, "cwd"), cwd).Check();
  global->Set(context, process_key, process).Check();
}

struct AutoV8Scope {
  explicit AutoV8Scope(v8::Isolate* isolate, v8::Global<v8::Context>& global_context)
      : locker(isolate),
        isolate_scope(isolate),
        handle_scope(isolate) {
    if (global_context.IsEmpty()) {
      v8::Local<v8::Context> local_ctx = v8::Context::New(isolate);
      global_context.Reset(isolate, local_ctx);
    }
    context = global_context.Get(isolate);
    context_scope = std::make_unique<v8::Context::Scope>(context);
    InstallNodeLikeProcess(isolate, context);
  }

  v8::Locker locker;
  v8::Isolate::Scope isolate_scope;
  v8::HandleScope handle_scope;
  v8::Local<v8::Context> context;
  std::unique_ptr<v8::Context::Scope> context_scope;
};

std::optional<base::Value> V8ArrayBufferToBaseValue(
    v8::Local<v8::ArrayBuffer> array_buffer,
    size_t byte_offset,
    size_t byte_length) {
  std::shared_ptr<v8::BackingStore> backing_store =
      array_buffer->GetBackingStore();
  CHECK_LE(byte_offset, backing_store->ByteLength());
  CHECK_LE(byte_length, backing_store->ByteLength() - byte_offset);
  // SAFETY: Offset/length validated against the ArrayBuffer BackingStore size.
  base::span<const uint8_t> bytes = UNSAFE_BUFFERS(base::span(
      static_cast<const uint8_t*>(backing_store->Data()),
      backing_store->ByteLength()));
  auto slice = bytes.subspan(byte_offset, byte_length);
  return base::Value(base::Value::BlobStorage(slice.begin(), slice.end()));
}

base::Value MakeTaggedWireValue(const std::string& type) {
  base::DictValue dict;
  dict.Set(kWireTypeKey, type);
  return base::Value(std::move(dict));
}

base::Value MakeTaggedWireValue(const std::string& type, base::Value value) {
  base::DictValue dict;
  dict.Set(kWireTypeKey, type);
  dict.Set(kWireValueKey, std::move(value));
  return base::Value(std::move(dict));
}

template <typename T>
v8::Local<v8::Value> CreateBinaryTypedArray(v8::Local<v8::ArrayBuffer> buffer,
                                            size_t length) {
  return T::New(buffer, 0, length).template As<v8::Value>();
}

struct BinaryArrayType {
  std::string_view name;
  size_t element_size;
  v8::Local<v8::Value> (*create)(v8::Local<v8::ArrayBuffer>, size_t);
};

constexpr BinaryArrayType kBinaryArrayTypes[] = {
    {"Int8Array", 1, &CreateBinaryTypedArray<v8::Int8Array>},
    {"Uint8Array", 1, &CreateBinaryTypedArray<v8::Uint8Array>},
    {"Uint8ClampedArray", 1, &CreateBinaryTypedArray<v8::Uint8ClampedArray>},
    {"Int16Array", 2, &CreateBinaryTypedArray<v8::Int16Array>},
    {"Uint16Array", 2, &CreateBinaryTypedArray<v8::Uint16Array>},
    {"Int32Array", 4, &CreateBinaryTypedArray<v8::Int32Array>},
    {"Uint32Array", 4, &CreateBinaryTypedArray<v8::Uint32Array>},
    {"Float16Array", 2, &CreateBinaryTypedArray<v8::Float16Array>},
    {"Float32Array", 4, &CreateBinaryTypedArray<v8::Float32Array>},
    {"Float64Array", 8, &CreateBinaryTypedArray<v8::Float64Array>},
    {"BigInt64Array", 8, &CreateBinaryTypedArray<v8::BigInt64Array>},
    {"BigUint64Array", 8, &CreateBinaryTypedArray<v8::BigUint64Array>},
};

std::string BinaryValueKind(v8::Local<v8::Context> context,
                            v8::Local<v8::Value> value) {
  if (value->IsArrayBuffer()) {
    return "ArrayBuffer";
  }
  if (v8impl::IsMarkedBuffer(context, value)) {
    return "Buffer";
  }
  if (value->IsDataView()) {
    return "DataView";
  }
  if (value->IsInt8Array()) {
    return "Int8Array";
  }
  if (value->IsUint8Array()) {
    return "Uint8Array";
  }
  if (value->IsUint8ClampedArray()) {
    return "Uint8ClampedArray";
  }
  if (value->IsInt16Array()) {
    return "Int16Array";
  }
  if (value->IsUint16Array()) {
    return "Uint16Array";
  }
  if (value->IsInt32Array()) {
    return "Int32Array";
  }
  if (value->IsUint32Array()) {
    return "Uint32Array";
  }
  if (value->IsFloat16Array()) {
    return "Float16Array";
  }
  if (value->IsFloat32Array()) {
    return "Float32Array";
  }
  if (value->IsFloat64Array()) {
    return "Float64Array";
  }
  if (value->IsBigInt64Array()) {
    return "BigInt64Array";
  }
  if (value->IsBigUint64Array()) {
    return "BigUint64Array";
  }
  return {};
}

v8::MaybeLocal<v8::ArrayBuffer> BlobToArrayBuffer(
    v8::Isolate* isolate,
    const base::Value::BlobStorage& blob,
    std::string* error_msg) {
  if (blob.size() > v8::ArrayBuffer::kMaxByteLength) {
    *error_msg = "Binary argument exceeds the V8 ArrayBuffer size limit";
    return {};
  }
  auto backing_store = v8::ArrayBuffer::NewBackingStore(
      isolate, blob.size(), v8::BackingStoreInitializationMode::kUninitialized,
      v8::BackingStoreOnFailureMode::kReturnNull);
  if (!backing_store) {
    *error_msg = "Failed to allocate binary argument";
    return {};
  }
  if (!blob.empty()) {
    // SAFETY: BackingStore size equals blob.size(); Data() is writable for
    // that many bytes.
    UNSAFE_BUFFERS(
        base::span(static_cast<uint8_t*>(backing_store->Data()), blob.size()))
        .copy_from(base::as_byte_span(blob));
  }
  return v8::ArrayBuffer::New(isolate, std::move(backing_store));
}

v8::MaybeLocal<v8::Value> BinaryWireValueToV8(v8::Isolate* isolate,
                                              v8::Local<v8::Context> context,
                                              const base::DictValue& dict,
                                              const base::Value& payload,
                                              std::string* error_msg) {
  const std::string* kind = dict.FindString("kind");
  if (!kind || !payload.is_blob()) {
    *error_msg = "Tagged binary argument requires a kind and binary value";
    return {};
  }
  const BinaryArrayType* array_type = nullptr;
  for (const auto& type : kBinaryArrayTypes) {
    if (*kind == type.name) {
      array_type = &type;
      break;
    }
  }
  if (!array_type && *kind != "ArrayBuffer" && *kind != "DataView" &&
      *kind != "Buffer") {
    *error_msg = "Unknown Xenon Node binary kind: " + *kind;
    return {};
  }
  const auto& blob = payload.GetBlob();
  if (array_type && blob.size() % array_type->element_size != 0) {
    *error_msg = "Tagged binary argument has a misaligned byte length";
    return {};
  }
  v8::Local<v8::ArrayBuffer> buffer;
  if (!BlobToArrayBuffer(isolate, blob, error_msg).ToLocal(&buffer)) {
    return {};
  }
  if (*kind == "ArrayBuffer") {
    return buffer.As<v8::Value>();
  }
  if (*kind == "DataView") {
    return v8::DataView::New(buffer, 0, blob.size()).As<v8::Value>();
  }
  if (*kind == "Buffer") {
    auto view = v8::Uint8Array::New(buffer, 0, blob.size());
    if (!v8impl::MarkBuffer(context, view)) {
      *error_msg = "Failed to mark native Buffer argument";
      return {};
    }
    return view.As<v8::Value>();
  }
  return array_type->create(buffer, blob.size() / array_type->element_size);
}

std::optional<base::Value> V8ValueToBaseValue(v8::Isolate* isolate,
                                              v8::Local<v8::Context> context,
                                              v8::Local<v8::Value> value,
                                              std::string* error_msg,
                                              int depth) {
  if (depth > kMaxValueConversionDepth) {
    *error_msg = "Native export returned a value that is nested too deeply";
    return std::nullopt;
  }

  if (value->IsUndefined()) {
    return MakeTaggedWireValue("undefined");
  }
  if (value->IsNull()) {
    return base::Value();
  }
  if (value->IsBoolean()) {
    return base::Value(value->BooleanValue(isolate));
  }
  if (value->IsInt32()) {
    return base::Value(value.As<v8::Int32>()->Value());
  }
  if (value->IsNumber()) {
    v8::Local<v8::Number> number;
    if (!value->ToNumber(context).ToLocal(&number)) {
      *error_msg = "Failed to convert native number result";
      return std::nullopt;
    }
    const double number_value = number->Value();
    if (std::isnan(number_value)) {
      return MakeTaggedWireValue("number", base::Value("nan"));
    }
    if (std::isinf(number_value)) {
      return MakeTaggedWireValue(
          "number", base::Value(number_value > 0 ? "infinity" : "-infinity"));
    }
    if (number_value == 0 && std::signbit(number_value)) {
      return MakeTaggedWireValue("number", base::Value("-0"));
    }
    return base::Value(number_value);
  }
  if (value->IsBigInt()) {
    v8::Local<v8::String> string;
    if (!value.As<v8::BigInt>()->ToString(context).ToLocal(&string)) {
      *error_msg = "Failed to convert native BigInt result";
      return std::nullopt;
    }
    v8::String::Utf8Value utf8(isolate, string);
    return MakeTaggedWireValue("bigint",
                               base::Value(std::string(*utf8 ? *utf8 : "")));
  }
  if (value->IsString()) {
    v8::String::Utf8Value string(isolate, value);
    return base::Value(std::string(*string ? *string : ""));
  }
  if (value->IsArrayBuffer() || value->IsArrayBufferView()) {
    const std::string kind = BinaryValueKind(context, value);
    if (kind.empty()) {
      *error_msg = "Native export returned an unsupported binary view";
      return std::nullopt;
    }
    std::optional<base::Value> bytes;
    if (value->IsArrayBuffer()) {
      auto buffer = value.As<v8::ArrayBuffer>();
      bytes = V8ArrayBufferToBaseValue(buffer, 0, buffer->ByteLength());
    } else {
      auto view = value.As<v8::ArrayBufferView>();
      bytes = V8ArrayBufferToBaseValue(view->Buffer(), view->ByteOffset(),
                                       view->ByteLength());
    }
    auto result = MakeTaggedWireValue("binary", std::move(*bytes));
    result.GetDict().Set("kind", kind);
    return result;
  }
  if (value->IsSharedArrayBuffer()) {
    *error_msg = "SharedArrayBuffer cannot cross the Xenon Node wire";
    return std::nullopt;
  }
  if (value->IsFunction()) {
    *error_msg =
        "Native export returned a function, which cannot cross the "
        "Xenon Node wire";
    return std::nullopt;
  }
  if (value->IsArray()) {
    v8::Local<v8::Array> array = value.As<v8::Array>();
    base::ListValue list;
    list.reserve(array->Length());
    for (uint32_t i = 0; i < array->Length(); ++i) {
      v8::Local<v8::Value> element;
      if (!array->Get(context, i).ToLocal(&element)) {
        *error_msg = "Failed to read native array result";
        return std::nullopt;
      }
      std::optional<base::Value> converted =
          V8ValueToBaseValue(isolate, context, element, error_msg, depth + 1);
      if (!converted) {
        return std::nullopt;
      }
      list.Append(std::move(*converted));
    }
    return base::Value(std::move(list));
  }
  if (value->IsDate()) {
    return MakeTaggedWireValue("date",
                               base::Value(value.As<v8::Date>()->ValueOf()));
  }
  if (value->IsMap()) {
    v8::Local<v8::Array> entries = value.As<v8::Map>()->AsArray();
    base::ListValue pairs;
    pairs.reserve(entries->Length() / 2);
    for (uint32_t i = 0; i < entries->Length(); i += 2) {
      v8::Local<v8::Value> key;
      v8::Local<v8::Value> item;
      if (!entries->Get(context, i).ToLocal(&key) ||
          !entries->Get(context, i + 1).ToLocal(&item)) {
        *error_msg = "Failed to read native Map result";
        return std::nullopt;
      }
      std::optional<base::Value> converted_key =
          V8ValueToBaseValue(isolate, context, key, error_msg, depth + 1);
      std::optional<base::Value> converted_item =
          V8ValueToBaseValue(isolate, context, item, error_msg, depth + 1);
      if (!converted_key || !converted_item) {
        return std::nullopt;
      }
      base::ListValue pair;
      pair.Append(std::move(*converted_key));
      pair.Append(std::move(*converted_item));
      pairs.Append(std::move(pair));
    }
    return MakeTaggedWireValue("map", base::Value(std::move(pairs)));
  }
  if (value->IsSet()) {
    v8::Local<v8::Array> entries = value.As<v8::Set>()->AsArray();
    base::ListValue items;
    items.reserve(entries->Length());
    for (uint32_t i = 0; i < entries->Length(); ++i) {
      v8::Local<v8::Value> item;
      if (!entries->Get(context, i).ToLocal(&item)) {
        *error_msg = "Failed to read native Set result";
        return std::nullopt;
      }
      std::optional<base::Value> converted =
          V8ValueToBaseValue(isolate, context, item, error_msg, depth + 1);
      if (!converted) {
        return std::nullopt;
      }
      items.Append(std::move(*converted));
    }
    return MakeTaggedWireValue("set", base::Value(std::move(items)));
  }
  if (value->IsObject()) {
    v8::Local<v8::Object> object;
    if (!value->ToObject(context).ToLocal(&object)) {
      *error_msg = "Failed to convert native object result";
      return std::nullopt;
    }

    v8::Local<v8::Array> keys;
    if (!object
             ->GetOwnPropertyNames(context, v8::PropertyFilter::ONLY_ENUMERABLE,
                                   v8::KeyConversionMode::kConvertToString)
             .ToLocal(&keys)) {
      *error_msg = "Failed to enumerate native object result";
      return std::nullopt;
    }

    base::DictValue dict;
    for (uint32_t i = 0; i < keys->Length(); ++i) {
      v8::Local<v8::Value> key;
      if (!keys->Get(context, i).ToLocal(&key)) {
        *error_msg = "Failed to read native object key";
        return std::nullopt;
      }

      v8::String::Utf8Value key_string(isolate, key);
      if (!*key_string) {
        continue;
      }

      v8::Local<v8::Value> property;
      if (!object->Get(context, key).ToLocal(&property)) {
        *error_msg = "Failed to read native object property";
        return std::nullopt;
      }
      if (property->IsFunction()) {
        *error_msg = "Native export returned an object containing a function";
        return std::nullopt;
      }

      std::optional<base::Value> converted = V8ValueToBaseValue(
          isolate, context, property, error_msg, depth + 1);
      if (!converted) {
        return std::nullopt;
      }
      dict.Set(std::string(*key_string), std::move(*converted));
    }
    return base::Value(std::move(dict));
  }

  *error_msg = "Native export returned an unsupported value type";
  return std::nullopt;
}

v8::MaybeLocal<v8::Value> BaseValueToV8Value(v8::Isolate* isolate,
                                             v8::Local<v8::Context> context,
                                             const base::Value& value,
                                             std::string* error_msg,
                                             int depth) {
  if (depth > kMaxValueConversionDepth) {
    *error_msg = "Argument is nested too deeply";
    return v8::MaybeLocal<v8::Value>();
  }

  switch (value.type()) {
    case base::Value::Type::NONE:
      return v8::Null(isolate);
    case base::Value::Type::BOOLEAN:
      return v8::Boolean::New(isolate, value.GetBool());
    case base::Value::Type::INTEGER:
      return v8::Integer::New(isolate, value.GetInt());
    case base::Value::Type::DOUBLE:
      return v8::Number::New(isolate, value.GetDouble());
    case base::Value::Type::STRING: {
      v8::Local<v8::String> string;
      if (!v8::String::NewFromUtf8(isolate, value.GetString().c_str(),
                                   v8::NewStringType::kNormal)
               .ToLocal(&string)) {
        *error_msg = "Failed to build V8 string argument";
        return v8::MaybeLocal<v8::Value>();
      }
      return string.As<v8::Value>();
    }
    case base::Value::Type::BINARY: {
      // Legacy raw binary callers retain their ArrayBuffer interpretation.
      v8::Local<v8::ArrayBuffer> buffer;
      if (!BlobToArrayBuffer(isolate, value.GetBlob(), error_msg)
               .ToLocal(&buffer)) {
        return {};
      }
      return buffer.As<v8::Value>();
    }
    case base::Value::Type::LIST: {
      const base::ListValue& list = value.GetList();
      v8::Local<v8::Array> array =
          v8::Array::New(isolate, static_cast<int>(list.size()));
      uint32_t index = 0;
      for (const base::Value& item : list) {
        v8::Local<v8::Value> converted;
        if (!BaseValueToV8Value(isolate, context, item, error_msg, depth + 1)
                 .ToLocal(&converted)) {
          return v8::MaybeLocal<v8::Value>();
        }
        if (!array->Set(context, index++, converted).FromMaybe(false)) {
          *error_msg = "Failed to build V8 array argument";
          return v8::MaybeLocal<v8::Value>();
        }
      }
      return array.As<v8::Value>();
    }
    case base::Value::Type::DICT: {
      const base::DictValue& dict = value.GetDict();
      const std::string* wire_type = dict.FindString(kWireTypeKey);
      if (wire_type) {
        if (*wire_type == "undefined") {
          return v8::Undefined(isolate);
        }
        if (*wire_type == kNativeInstanceWireType) {
          *error_msg =
              "native_instance arguments must be resolved by the executor";
          return v8::MaybeLocal<v8::Value>();
        }

        const base::Value* payload = dict.Find(kWireValueKey);
        if (!payload) {
          *error_msg = "Tagged argument is missing its value";
          return v8::MaybeLocal<v8::Value>();
        }

        if (*wire_type == "binary") {
          return BinaryWireValueToV8(isolate, context, dict, *payload,
                                     error_msg);
        }

        if (*wire_type == "number") {
          if (!payload->is_string()) {
            *error_msg = "Tagged number argument is invalid";
            return v8::MaybeLocal<v8::Value>();
          }
          const std::string& number = payload->GetString();
          if (number == "nan") {
            return v8::Number::New(isolate,
                                   std::numeric_limits<double>::quiet_NaN());
          }
          if (number == "infinity") {
            return v8::Number::New(isolate,
                                   std::numeric_limits<double>::infinity());
          }
          if (number == "-infinity") {
            return v8::Number::New(isolate,
                                   -std::numeric_limits<double>::infinity());
          }
          if (number == "-0") {
            return v8::Number::New(isolate, -0.0);
          }
          *error_msg = "Tagged number argument is invalid";
          return v8::MaybeLocal<v8::Value>();
        }

        if (*wire_type == "bigint") {
          if (!payload->is_string()) {
            *error_msg = "Tagged BigInt argument is invalid";
            return v8::MaybeLocal<v8::Value>();
          }
          v8::Local<v8::Value> bigint_value;
          if (!context->Global()
                   ->Get(context, gin::StringToV8(isolate, "BigInt"))
                   .ToLocal(&bigint_value) ||
              !bigint_value->IsFunction()) {
            *error_msg = "V8 BigInt constructor is unavailable";
            return v8::MaybeLocal<v8::Value>();
          }
          v8::Local<v8::Value> argument =
              gin::StringToV8(isolate, payload->GetString());
          return bigint_value.As<v8::Function>()->Call(
              context, v8::Undefined(isolate), 1, &argument);
        }

        if (*wire_type == "date") {
          v8::Local<v8::Value> converted;
          v8::Local<v8::Number> milliseconds;
          if (!BaseValueToV8Value(isolate, context, *payload, error_msg,
                                  depth + 1)
                   .ToLocal(&converted) ||
              !converted->ToNumber(context).ToLocal(&milliseconds)) {
            *error_msg = "Tagged Date argument is invalid";
            return v8::MaybeLocal<v8::Value>();
          }
          return v8::Date::New(context, milliseconds->Value());
        }

        if (*wire_type == "map") {
          if (!payload->is_list()) {
            *error_msg = "Tagged Map argument is invalid";
            return v8::MaybeLocal<v8::Value>();
          }
          v8::Local<v8::Map> map = v8::Map::New(isolate);
          for (const base::Value& pair_value : payload->GetList()) {
            if (!pair_value.is_list() || pair_value.GetList().size() != 2) {
              *error_msg = "Tagged Map entry is invalid";
              return v8::MaybeLocal<v8::Value>();
            }
            v8::Local<v8::Value> key;
            v8::Local<v8::Value> item;
            if (!BaseValueToV8Value(isolate, context, pair_value.GetList()[0],
                                    error_msg, depth + 1)
                     .ToLocal(&key) ||
                !BaseValueToV8Value(isolate, context, pair_value.GetList()[1],
                                    error_msg, depth + 1)
                     .ToLocal(&item) ||
                !map->Set(context, key, item).ToLocal(&map)) {
              return v8::MaybeLocal<v8::Value>();
            }
          }
          return map.As<v8::Value>();
        }

        if (*wire_type == "set") {
          if (!payload->is_list()) {
            *error_msg = "Tagged Set argument is invalid";
            return v8::MaybeLocal<v8::Value>();
          }
          v8::Local<v8::Set> set = v8::Set::New(isolate);
          for (const base::Value& item_value : payload->GetList()) {
            v8::Local<v8::Value> item;
            if (!BaseValueToV8Value(isolate, context, item_value, error_msg,
                                    depth + 1)
                     .ToLocal(&item) ||
                !set->Add(context, item).ToLocal(&set)) {
              return v8::MaybeLocal<v8::Value>();
            }
          }
          return set.As<v8::Value>();
        }

        *error_msg = "Unknown Xenon Node wire type: " + *wire_type;
        return v8::MaybeLocal<v8::Value>();
      }

      v8::Local<v8::Object> object = v8::Object::New(isolate);
      for (const auto [key, item] : dict) {
        v8::Local<v8::String> key_string;
        if (!v8::String::NewFromUtf8(isolate, std::string(key).c_str(),
                                     v8::NewStringType::kNormal)
                 .ToLocal(&key_string)) {
          *error_msg = "Failed to build V8 object key";
          return v8::MaybeLocal<v8::Value>();
        }

        v8::Local<v8::Value> converted;
        if (!BaseValueToV8Value(isolate, context, item, error_msg, depth + 1)
                 .ToLocal(&converted)) {
          return v8::MaybeLocal<v8::Value>();
        }
        if (!object->CreateDataProperty(context, key_string, converted)
                 .FromMaybe(false)) {
          *error_msg = "Failed to build V8 object argument";
          return v8::MaybeLocal<v8::Value>();
        }
      }
      return object.As<v8::Value>();
    }
  }

  *error_msg = "Argument has an unsupported value type";
  return v8::MaybeLocal<v8::Value>();
}

std::string DescribeCaughtException(const std::string& function_name,
                                    gin::TryCatch* try_catch) {
  std::string error = "Exception while invoking export: " + function_name;
  if (try_catch->HasCaught()) {
    std::string stack = try_catch->GetStackTrace();
    if (!stack.empty()) {
      error += "\n" + stack;
    }
  }
  return error;
}

}  // namespace

NativeAddonResourceRedirect::NativeAddonResourceRedirect(
    base::NativeLibrary module,
    const base::FilePath& runtime_directory) {
#if BUILDFLAG(IS_WIN)
  if (!module || runtime_directory.empty() || !runtime_directory.IsAbsolute()) {
    return;
  }

  // Hold an independent reference while the IAT is patched. Addon
  // initialization may fail and release its ScopedNativeLibrary before this
  // registration is destroyed.
  HMODULE retained_module = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                            reinterpret_cast<LPCWSTR>(module),
                            &retained_module)) {
    LOG(WARNING) << "[NapiLoader] Failed to retain addon for resource "
                    "redirection, error="
                 << ::GetLastError();
    return;
  }
  module_ = retained_module;

  NativeAddonRedirectState& state = GetNativeAddonRedirectState();
  {
    base::AutoLock lock(state.lock);
    state.runtime_directories[module_] =
        runtime_directory.StripTrailingSeparators();
  }
  PatchImports();
#else
  (void)module;
  (void)runtime_directory;
#endif
}

NativeAddonResourceRedirect::~NativeAddonResourceRedirect() {
#if BUILDFLAG(IS_WIN)
  if (!module_) {
    return;
  }

  NativeAddonRedirectState& state = GetNativeAddonRedirectState();
  {
    base::AutoLock lock(state.lock);
    state.runtime_directories.erase(module_);
  }
  RestoreImports();
  ::FreeLibrary(module_);
#endif
}

#if BUILDFLAG(IS_WIN)
void NativeAddonResourceRedirect::PatchImports() {
  // SAFETY: |module_| is a retained, successfully loaded PE image. Every RVA
  // and array walk below is bounded by the image's declared SizeOfImage before
  // it is dereferenced.
  UNSAFE_BUFFERS({
    auto* image = reinterpret_cast<uint8_t*>(module_);
    const auto* dos_header = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
    if (dos_header->e_magic != IMAGE_DOS_SIGNATURE ||
        dos_header->e_lfanew <= 0) {
      return;
    }

    const auto* nt_headers =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos_header->e_lfanew);
    if (nt_headers->Signature != IMAGE_NT_SIGNATURE ||
        nt_headers->OptionalHeader.NumberOfRvaAndSizes <=
            IMAGE_DIRECTORY_ENTRY_IMPORT) {
      return;
    }

    const size_t image_size = nt_headers->OptionalHeader.SizeOfImage;
    const IMAGE_DATA_DIRECTORY& imports =
        nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!imports.VirtualAddress || imports.VirtualAddress >= image_size ||
        imports.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
      return;
    }

    const size_t descriptor_count = std::min(
        static_cast<size_t>(imports.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR)),
        (image_size - imports.VirtualAddress) /
            sizeof(IMAGE_IMPORT_DESCRIPTOR));
    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        image + imports.VirtualAddress);
    for (size_t descriptor_index = 0;
         descriptor_index < descriptor_count && descriptor->FirstThunk;
         ++descriptor_index, ++descriptor) {
      if (!descriptor->OriginalFirstThunk ||
          descriptor->OriginalFirstThunk >= image_size ||
          descriptor->FirstThunk >= image_size) {
        continue;
      }

      auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(
          image + descriptor->OriginalFirstThunk);
      auto* functions =
          reinterpret_cast<IMAGE_THUNK_DATA*>(image + descriptor->FirstThunk);
      const size_t name_capacity =
          (image_size - descriptor->OriginalFirstThunk) /
          sizeof(IMAGE_THUNK_DATA);
      const size_t function_capacity =
          (image_size - descriptor->FirstThunk) / sizeof(IMAGE_THUNK_DATA);
      const size_t thunk_count = std::min(name_capacity, function_capacity);

      for (size_t thunk_index = 0;
           thunk_index < thunk_count && names[thunk_index].u1.AddressOfData;
           ++thunk_index) {
        if (IMAGE_SNAP_BY_ORDINAL(names[thunk_index].u1.Ordinal)) {
          continue;
        }
        const ULONG_PTR name_rva = names[thunk_index].u1.AddressOfData;
        if (name_rva >= image_size ||
            image_size - name_rva <= offsetof(IMAGE_IMPORT_BY_NAME, Name)) {
          continue;
        }
        const auto* import =
            reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image + name_rva);
        const char* function_name = reinterpret_cast<const char*>(import->Name);
        const size_t maximum_name_length =
            image_size - name_rva - offsetof(IMAGE_IMPORT_BY_NAME, Name);
        const void* terminator =
            std::memchr(function_name, '\0', maximum_name_length);
        if (!terminator) {
          continue;
        }
        void* replacement = GetHostedLibraryReplacement(std::string_view(
            function_name,
            static_cast<const char*>(terminator) - function_name));
        if (!replacement) {
          continue;
        }

        ULONG_PTR* slot = &functions[thunk_index].u1.Function;
        const ULONG_PTR replacement_value =
            reinterpret_cast<ULONG_PTR>(replacement);
        if (*slot == replacement_value) {
          continue;
        }

        DWORD old_protection = 0;
        if (!::VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE,
                              &old_protection)) {
          LOG(WARNING) << "[NapiLoader] Failed to patch addon import "
                       << function_name << ", error=" << ::GetLastError();
          continue;
        }
        const ULONG_PTR original_value = *slot;
        *slot = replacement_value;
        DWORD ignored = 0;
        ::VirtualProtect(slot, sizeof(*slot), old_protection, &ignored);
        ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(*slot));
        import_patches_.emplace_back(slot, original_value);
      }
    }
  });
}

void NativeAddonResourceRedirect::RestoreImports() {
  for (auto patch = import_patches_.rbegin(); patch != import_patches_.rend();
       ++patch) {
    DWORD old_protection = 0;
    if (!::VirtualProtect(patch->first, sizeof(*patch->first), PAGE_READWRITE,
                          &old_protection)) {
      continue;
    }
    *patch->first = patch->second;
    DWORD ignored = 0;
    ::VirtualProtect(patch->first, sizeof(*patch->first), old_protection,
                     &ignored);
    ::FlushInstructionCache(::GetCurrentProcess(), patch->first,
                            sizeof(*patch->first));
  }
  import_patches_.clear();
}
#endif

XenonNodeExecutor::AddonModule::AddonModule() = default;
XenonNodeExecutor::AddonModule::~AddonModule() {
  exports.Reset();
  resource_redirect.reset();
  loaded_addon.reset();
}
XenonNodeExecutor::AddonModule::AddonModule(AddonModule&&) = default;
XenonNodeExecutor::AddonModule& XenonNodeExecutor::AddonModule::operator=(
    AddonModule&&) = default;

XenonNodeExecutor::AddonInstance::AddonInstance() = default;
XenonNodeExecutor::AddonInstance::~AddonInstance() = default;
XenonNodeExecutor::AddonInstance::AddonInstance(AddonInstance&&) = default;
XenonNodeExecutor::AddonInstance& XenonNodeExecutor::AddonInstance::operator=(
    AddonInstance&&) = default;

XenonNodeExecutor::AddonCallback::AddonCallback() = default;
XenonNodeExecutor::AddonCallback::~AddonCallback() = default;

XenonNodeExecutor::XenonNodeExecutor() = default;

void XenonNodeExecutor::SetCallbackHandlers(
    NodeCallback callback,
    NodeCallbackReleased callback_released) {
  callback_handler_ = std::move(callback);
  callback_released_handler_ = std::move(callback_released);
}

void XenonNodeExecutor::SetRuntimeDirectory(
    const base::FilePath& runtime_directory) {
  runtime_directory_ = runtime_directory.StripTrailingSeparators();
}

void XenonNodeExecutor::ReleaseCallbacksForClient(int32_t client_id) {
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    return;
  }
  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  for (auto it = addon_callbacks_.begin(); it != addon_callbacks_.end();) {
    if (it->first.first == client_id) {
      it = addon_callbacks_.erase(it);
    } else {
      ++it;
    }
  }
}

void XenonNodeExecutor::RegisterInstanceOwner(uint64_t owner) {
  if (owner != 0 && !instance_owners_.contains(owner)) {
    instance_owners_.emplace(owner,
                             base::UnguessableToken::Create().ToString());
  }
}

std::string XenonNodeExecutor::GetInstanceOwnerToken(uint64_t owner) const {
  auto it = instance_owners_.find(owner);
  return it == instance_owners_.end() ? std::string() : it->second;
}

bool XenonNodeExecutor::ValidateInstanceOwnerToken(
    uint64_t owner,
    const std::string& token) const {
  if (owner == 0) {
    return token.empty();
  }
  auto it = instance_owners_.find(owner);
  return it != instance_owners_.end() && it->second == token;
}

bool XenonNodeExecutor::IsInstanceOwnerActive(uint64_t owner) const {
  return owner == 0 || instance_owners_.contains(owner);
}

void XenonNodeExecutor::ReleaseInstanceOwner(uint64_t owner) {
  if (owner == 0) {
    return;
  }
  instance_owners_.erase(owner);
  if (addon_isolate_ && !addon_context_.IsEmpty()) {
    AutoV8Scope v8_scope(addon_isolate_, addon_context_);
    for (auto it = addon_instances_.begin(); it != addon_instances_.end();) {
      const auto current = it++;
      if (current->second.owner == owner) {
        RemoveNativeInstance(current->first);
      }
    }
    std::erase_if(addon_callbacks_, [owner](const auto& item) {
      return item.second->owner == owner;
    });
  }
  // Mark the owner dead before invoking cancellation callbacks, which may
  // reenter the executor. Promise settlement must not create new handles.
  CancelPromisesForOwner(owner);
}

// static
void XenonNodeExecutor::FirstWeakCallback(
    const v8::WeakCallbackInfo<AddonCallback>& data) {
  AddonCallback* callback = data.GetParameter();
  callback->function.Reset();
  // Collection notifications may dispatch JavaScript through the service.
  // Leave the GC callback before doing that, and retain only stable ids so a
  // disconnect or a new registration can delete this cache entry immediately.
  base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
      FROM_HERE, base::BindOnce(&XenonNodeExecutor::OnNativeCallbackCollected,
                                callback->executor, callback->client_id,
                                callback->callback_id, callback->generation));
}

v8::Local<v8::Function> XenonNodeExecutor::GetOrCreateNativeCallback(
    v8::Local<v8::Context> context,
    int32_t client_id,
    int32_t callback_id,
    const std::string& module_path,
    uint64_t owner) {
  const auto key = std::make_pair(client_id, callback_id);
  auto existing = addon_callbacks_.find(key);
  if (existing != addon_callbacks_.end() &&
      !existing->second->function.IsEmpty() &&
      existing->second->owner == owner) {
    if (!module_path.empty() && existing->second->module_path.empty()) {
      existing->second->module_path = module_path;
    }
    return existing->second->function.Get(addon_isolate_);
  }

  const uint64_t generation = next_callback_generation_++;
  v8::Local<v8::Function> function =
      gin::CreateFunctionTemplate(
          addon_isolate_,
          base::BindRepeating(&XenonNodeExecutor::OnNativeCallback,
                              weak_factory_.GetWeakPtr(), client_id,
                              callback_id, generation))
          ->GetFunction(context)
          .ToLocalChecked();

  auto callback = std::make_unique<AddonCallback>();
  callback->client_id = client_id;
  callback->callback_id = callback_id;
  callback->generation = generation;
  callback->owner = owner;
  callback->module_path = module_path;
  callback->executor = weak_factory_.GetWeakPtr();
  callback->function.Reset(addon_isolate_, function);
  AddonCallback* callback_ptr = callback.get();
  callback->function.SetWeak(callback_ptr, FirstWeakCallback,
                             v8::WeakCallbackType::kParameter);
  addon_callbacks_.insert_or_assign(key, std::move(callback));
  return function;
}

void XenonNodeExecutor::OnNativeCallback(int32_t client_id,
                                         int32_t callback_id,
                                         uint64_t generation,
                                         gin::Arguments* arguments) {
  const auto key = std::make_pair(client_id, callback_id);
  auto callback_it = addon_callbacks_.find(key);
  // An addon may retain the JavaScript function after its renderer disconnects.
  // Drop it before serialization, including when the same ids are registered
  // again: the retained function belongs to an earlier registration.
  if (!callback_handler_ || callback_it == addon_callbacks_.end() ||
      callback_it->second->generation != generation ||
      !IsInstanceOwnerActive(callback_it->second->owner)) {
    return;
  }
  const std::string module_path = callback_it->second->module_path;
  const uint64_t owner = callback_it->second->owner;

  v8::Local<v8::Context> context = arguments->GetHolderCreationContext();
  if (context.IsEmpty() && !addon_context_.IsEmpty()) {
    context = addon_context_.Get(addon_isolate_);
  }
  v8::Local<v8::Object> receiver;
  if (context.IsEmpty() || !arguments->GetHolder(&receiver)) {
    LOG(ERROR) << "Failed to read Node callback receiver";
    return;
  }
  std::optional<base::Value> converted_receiver;
  if (receiver->StrictEquals(context->Global())) {
    // The global object contains the whole addon context and must not be
    // traversed or registered as a native instance.
    converted_receiver = MakeTaggedWireValue("global");
  } else {
    std::string error_msg;
    converted_receiver = ConvertNativeValue(context, module_path, receiver,
                                            &error_msg, 0, owner);
    if (!converted_receiver) {
      LOG(ERROR) << "Failed to serialize Node callback receiver: " << error_msg;
      return;
    }
  }
  std::vector<base::Value> converted_args;
  v8::Local<v8::Value> value;
  int adopted_count = 0;
  while (arguments->GetNext(&value)) {
    std::string error_msg;
    std::optional<base::Value> converted =
        ConvertNativeValue(context, module_path, value, &error_msg, 0, owner);
    if (!converted) {
      LOG(ERROR) << "Failed to serialize Node callback argument: " << error_msg;
      return;
    }
    if (converted->is_dict() &&
        converted->GetDict().FindString(kWireTypeKey) &&
        *converted->GetDict().FindString(kWireTypeKey) ==
            kNativeInstanceWireType) {
      ++adopted_count;
    }
    converted_args.push_back(std::move(*converted));
  }

  // Payload getters may disconnect or replace this callback while converting.
  callback_it = addon_callbacks_.find(key);
  if (callback_it == addon_callbacks_.end() ||
      callback_it->second->generation != generation ||
      !IsInstanceOwnerActive(owner)) {
    return;
  }
  LOG(INFO) << "OnNativeCallback client=" << client_id
            << " cb=" << callback_id << " args=" << converted_args.size()
            << " native_instances=" << adopted_count;
  if (callback_handler_) {
    callback_handler_.Run(client_id, callback_id, std::move(converted_args),
                          std::move(*converted_receiver));
  }
}

void XenonNodeExecutor::OnNativeCallbackCollected(int32_t client_id,
                                                  int32_t callback_id,
                                                  uint64_t generation) {
  const auto key = std::make_pair(client_id, callback_id);
  auto it = addon_callbacks_.find(key);
  if (it == addon_callbacks_.end() || it->second->generation != generation) {
    return;
  }

  addon_callbacks_.erase(it);
  if (callback_released_handler_) {
    callback_released_handler_.Run(client_id, callback_id);
  }
}

v8::MaybeLocal<v8::Value> XenonNodeExecutor::WireValueToV8(
    v8::Local<v8::Context> context,
    const base::Value& value,
    int32_t client_id,
    const std::string& module_path,
    std::string* error_msg,
    int depth,
    uint64_t owner) {
  if (!IsInstanceOwnerActive(owner)) {
    *error_msg = "Native instance owner was closed";
    return v8::MaybeLocal<v8::Value>();
  }
  if (depth > kMaxValueConversionDepth) {
    *error_msg = "Argument is nested too deeply";
    return v8::MaybeLocal<v8::Value>();
  }
  if (value.is_list()) {
    const base::ListValue& list = value.GetList();
    v8::Local<v8::Array> array =
        v8::Array::New(addon_isolate_, static_cast<int>(list.size()));
    uint32_t index = 0;
    for (const base::Value& item : list) {
      v8::Local<v8::Value> converted;
      if (!WireValueToV8(context, item, client_id, module_path, error_msg,
                         depth + 1, owner)
               .ToLocal(&converted) ||
          !array->Set(context, index++, converted).FromMaybe(false)) {
        if (error_msg->empty()) {
          *error_msg = "Failed to build V8 array argument";
        }
        return v8::MaybeLocal<v8::Value>();
      }
    }
    return array.As<v8::Value>();
  }
  if (value.is_dict()) {
    const base::DictValue& dict = value.GetDict();
    const std::string* wire_type = dict.FindString(kWireTypeKey);
    if (wire_type && *wire_type == "callback") {
      const std::optional<int> callback_id = dict.FindInt("callback_id");
      if (!callback_id || client_id == 0) {
        *error_msg = "callback argument is missing its renderer scope";
        return v8::MaybeLocal<v8::Value>();
      }
      return GetOrCreateNativeCallback(context, client_id, *callback_id,
                                       module_path, owner);
    }
    if (wire_type && (*wire_type == kNativeInstanceWireType ||
                      *wire_type == kNativeFunctionWireType)) {
      const std::optional<int> instance_id = dict.FindInt("instance_id");
      if (!instance_id) {
        *error_msg = "native_instance argument is missing instance_id";
        return v8::MaybeLocal<v8::Value>();
      }
      auto it = addon_instances_.find(*instance_id);
      if (it == addon_instances_.end() || it->second.object.IsEmpty()) {
        *error_msg =
            "Unknown native instance id: " + std::to_string(*instance_id);
        return v8::MaybeLocal<v8::Value>();
      }
      if (it->second.owner != owner) {
        *error_msg = "Native instance does not belong to this owner";
        return v8::MaybeLocal<v8::Value>();
      }
      const std::string* token = dict.FindString("owner_token");
      if (owner != 0 &&
          (!token || !ValidateInstanceOwnerToken(owner, *token))) {
        *error_msg = "Native instance owner token is invalid";
        return v8::MaybeLocal<v8::Value>();
      }
      return it->second.object.Get(addon_isolate_).As<v8::Value>();
    }
    if (!wire_type) {
      v8::Local<v8::Object> object = v8::Object::New(addon_isolate_);
      for (const auto [key, item] : dict) {
        v8::Local<v8::Value> converted;
        if (!WireValueToV8(context, item, client_id, module_path, error_msg,
                           depth + 1, owner)
                 .ToLocal(&converted) ||
            !object
                 ->CreateDataProperty(
                     context, gin::StringToV8(addon_isolate_, key), converted)
                 .FromMaybe(false)) {
          if (error_msg->empty()) {
            *error_msg = "Failed to build V8 object argument";
          }
          return v8::MaybeLocal<v8::Value>();
        }
      }
      return object.As<v8::Value>();
    }
  }
  return BaseValueToV8Value(addon_isolate_, context, value, error_msg, depth);
}

void XenonNodeExecutor::AttachNativeInstanceFields(
    v8::Local<v8::Context> context,
    const std::string& module_path,
    v8::Local<v8::Object> object,
    base::DictValue& dict,
    int depth,
    uint64_t owner) {
  if (depth + 1 > kMaxValueConversionDepth) {
    return;
  }

  // Snapshot only data descriptors. Reading inherited accessors here can run
  // addon code while an unrelated native callback is still on the stack.
  // Accessors remain available through explicit member inspection.
  v8::TryCatch try_catch(addon_isolate_);
  std::set<std::string> names;
  base::DictValue fields;
  v8::Local<v8::Object> current = object;
  while (IsInstanceOwnerActive(owner)) {
    v8::Local<v8::Array> keys;
    if (!current
             ->GetOwnPropertyNames(context, v8::PropertyFilter::ALL_PROPERTIES,
                                   v8::KeyConversionMode::kConvertToString)
             .ToLocal(&keys)) {
      return;
    }
    for (uint32_t i = 0; i < keys->Length(); ++i) {
      if (!IsInstanceOwnerActive(owner)) {
        return;
      }
      v8::Local<v8::Value> key;
      std::string name;
      if (!keys->Get(context, i).ToLocal(&key) || !key->IsName() ||
          !gin::ConvertFromV8(addon_isolate_, key, &name)) {
        return;
      }
      if (!names.insert(name).second || name == "constructor" ||
          name == "prototype" || name == "then") {
        continue;
      }
      v8::Local<v8::Value> descriptor;
      if (!current->GetOwnPropertyDescriptor(context, key.As<v8::Name>())
               .ToLocal(&descriptor)) {
        return;
      }
      if (!descriptor->IsObject()) {
        continue;
      }
      v8::Local<v8::Value> property;
      gin::Dictionary descriptor_dict(addon_isolate_,
                                      descriptor.As<v8::Object>());
      if (!descriptor_dict.Get("value", &property) || property.IsEmpty() ||
          property->IsUndefined() || property->IsFunction()) {
        continue;
      }
      std::string field_error;
      std::optional<base::Value> converted = ConvertNativeValue(
          context, module_path, property, &field_error, depth + 1, owner);
      if (try_catch.HasCaught()) {
        return;
      }
      if (converted) {
        fields.Set(name, std::move(*converted));
      }
    }
    v8::Local<v8::Value> prototype = current->GetPrototype();
    if (!prototype->IsObject() ||
        IsBuiltinPrototype(addon_isolate_, context,
                           prototype.As<v8::Object>())) {
      break;
    }
    current = prototype.As<v8::Object>();
  }
  if (!fields.empty()) {
    dict.Set("fields", std::move(fields));
  }
}

int32_t XenonNodeExecutor::RegisterNativeInstance(
    const std::string& module_path,
    v8::Local<v8::Object> object,
    uint64_t owner) {
  const base::FilePath resolved_path = ResolveAddonPath(module_path);
  const int identity_hash = object->GetIdentityHash();
  const auto range = addon_instance_ids_by_hash_.equal_range(identity_hash);
  for (auto it = range.first; it != range.second; ++it) {
    const auto instance = addon_instances_.find(it->second);
    if (instance != addon_instances_.end() && instance->second.owner == owner &&
        instance->second.module_path == resolved_path &&
        instance->second.object.Get(addon_isolate_)->StrictEquals(object)) {
      return instance->first;
    }
  }
  const int32_t instance_id = next_instance_id_++;
  AddonInstance stored;
  stored.module_path = resolved_path;
  stored.owner = owner;
  stored.identity_hash = identity_hash;
  stored.object.Reset(addon_isolate_, object);
  addon_instances_.emplace(instance_id, std::move(stored));
  addon_instance_ids_by_hash_.emplace(identity_hash, instance_id);
  return instance_id;
}

void XenonNodeExecutor::RemoveNativeInstance(int32_t instance_id) {
  const auto instance = addon_instances_.find(instance_id);
  if (instance == addon_instances_.end()) {
    return;
  }
  const auto range =
      addon_instance_ids_by_hash_.equal_range(instance->second.identity_hash);
  for (auto it = range.first; it != range.second; ++it) {
    if (it->second == instance_id) {
      addon_instance_ids_by_hash_.erase(it);
      break;
    }
  }
  addon_instances_.erase(instance);
}

std::optional<base::Value> XenonNodeExecutor::MaybeAdoptNativeReturn(
    v8::Local<v8::Context> context,
    const std::string& module_path,
    v8::Local<v8::Value> result,
    int depth,
    uint64_t owner) {
  const bool is_function = result->IsFunction();
  if (!is_function &&
      !LooksLikeNativeHandle(addon_isolate_, context, result)) {
    return std::nullopt;
  }
  if (!IsInstanceOwnerActive(owner)) {
    return std::nullopt;
  }

  v8::Local<v8::Object> object = result.As<v8::Object>();
  const int32_t instance_id =
      RegisterNativeInstance(module_path, object, owner);

  base::DictValue dict;
  dict.Set(kWireTypeKey,
           is_function ? kNativeFunctionWireType : kNativeInstanceWireType);
  dict.Set("module_path", module_path);
  dict.Set("instance_id", instance_id);
  if (owner != 0) {
    dict.Set("owner_token", GetInstanceOwnerToken(owner));
  }
  if (is_function) {
    v8::Local<v8::Value> name = result.As<v8::Function>()->GetName();
    if (!name.IsEmpty() && name->IsString()) {
      v8::String::Utf8Value utf8(addon_isolate_, name);
      if (*utf8) {
        dict.Set("name", std::string(*utf8, utf8.length()));
      }
    }
    return base::Value(std::move(dict));
  }
  v8::Local<v8::String> ctor_name = object->GetConstructorName();
  if (!ctor_name.IsEmpty()) {
    v8::String::Utf8Value utf8(addon_isolate_, ctor_name);
    if (*utf8) {
      const std::string_view name(*utf8, utf8.length());
      if (name != "Object" && name != "Function") {
        dict.Set("class_name", std::string(name));
      }
    }
  }
  dict.Set("prototype", NativePrototypeMembersToWire(addon_isolate_, context,
                                                     object));
  AttachNativeInstanceFields(context, module_path, object, dict, depth, owner);
  return base::Value(std::move(dict));
}

std::optional<base::Value> XenonNodeExecutor::ConvertNativeValue(
    v8::Local<v8::Context> context,
    const std::string& module_path,
    v8::Local<v8::Value> value,
    std::string* error_msg,
    int depth,
    uint64_t owner) {
  if (!IsInstanceOwnerActive(owner)) {
    *error_msg = "Native instance owner was closed";
    return std::nullopt;
  }
  if (depth > kMaxValueConversionDepth) {
    *error_msg = "Native export returned a value that is nested too deeply";
    return std::nullopt;
  }
  if (value.IsEmpty()) {
    return MakeTaggedWireValue("undefined");
  }
  if (!module_path.empty()) {
    if (std::optional<base::Value> adopted =
            MaybeAdoptNativeReturn(context, module_path, value, depth, owner)) {
      if (!IsInstanceOwnerActive(owner)) {
        *error_msg = "Native instance owner was closed";
        return std::nullopt;
      }
      return adopted;
    }
  }
  if (!IsInstanceOwnerActive(owner)) {
    *error_msg = "Native instance owner was closed";
    return std::nullopt;
  }
  if (value->IsArray()) {
    v8::Local<v8::Array> array = value.As<v8::Array>();
    base::ListValue list;
    list.reserve(array->Length());
    for (uint32_t i = 0; i < array->Length(); ++i) {
      v8::Local<v8::Value> element;
      if (!array->Get(context, i).ToLocal(&element)) {
        *error_msg = "Failed to read native array result";
        return std::nullopt;
      }
      std::optional<base::Value> converted = ConvertNativeValue(
          context, module_path, element, error_msg, depth + 1, owner);
      if (!converted) {
        return std::nullopt;
      }
      list.Append(std::move(*converted));
    }
    return base::Value(std::move(list));
  }
  std::optional<base::Value> converted =
      V8ValueToBaseValue(addon_isolate_, context, value, error_msg, depth);
  if (!IsInstanceOwnerActive(owner)) {
    *error_msg = "Native instance owner was closed";
    return std::nullopt;
  }
  return converted;
}

void XenonNodeExecutor::InvokeResolvedFunction(
    v8::Local<v8::Context> context,
    const std::string& module_path,
    const std::string& function_name,
    v8::Local<v8::Function> function,
    v8::Local<v8::Value> receiver,
    int32_t client_id,
    const std::vector<mojom::NodeInvokeArgPtr>& args,
    InvokeFunctionCallback callback,
    bool allow_pending_promise,
    DeferPromiseCallback defer_promise,
    uint64_t promise_owner) {
  if (!IsInstanceOwnerActive(promise_owner)) {
    std::move(callback).Run(false, base::Value(), {},
                            "Native instance owner was closed");
    return;
  }
  std::vector<v8::Local<v8::Value>> argv;
  argv.reserve(args.size());
  for (const auto& arg : args) {
    if (!IsInstanceOwnerActive(promise_owner)) {
      std::move(callback).Run(false, base::Value(), {},
                              "Native instance owner was closed");
      return;
    }
    if (!arg) {
      std::move(callback).Run(false, base::Value(), {},
                              "Invalid native argument");
      return;
    }
    if (arg->is_callback) {
      argv.push_back(GetOrCreateNativeCallback(
          context, client_id, arg->callback_id, module_path, promise_owner));
      continue;
    }

    std::string error_msg;
    v8::Local<v8::Value> converted;
    if (!WireValueToV8(context, arg->value, client_id, module_path, &error_msg,
                       0, promise_owner)
             .ToLocal(&converted)) {
      std::move(callback).Run(false, base::Value(), {}, error_msg);
      return;
    }
    argv.push_back(converted);
  }

  v8::Local<v8::Value> result;
  gin::TryCatch try_catch(addon_isolate_);
  const bool invoked =
      function
          ->Call(context, receiver, static_cast<int>(argv.size()), argv.data())
          .ToLocal(&result);
  EnsureUvLoopPolling();
  if (!invoked) {
    std::move(callback).Run(false, base::Value(), {},
                            DescribeCaughtException(function_name, &try_catch));
    return;
  }
  if (!IsInstanceOwnerActive(promise_owner)) {
    if (result->IsPromise()) {
      result.As<v8::Promise>()->MarkAsHandled();
    }
    std::move(callback).Run(false, base::Value(), {},
                            "Native instance owner was closed");
    return;
  }

  if (result->IsPromise()) {
    v8::Local<v8::Promise> promise = result.As<v8::Promise>();
    // This bridge forwards rejection to its caller, including rejections
    // produced by the checkpoint before the renderer can subscribe.
    promise->MarkAsHandled();
    addon_isolate_->PerformMicrotaskCheckpoint();
    if (!IsInstanceOwnerActive(promise_owner)) {
      std::move(callback).Run(false, base::Value(), {},
                              "Native instance owner was closed");
      return;
    }
    if (promise->State() == v8::Promise::kFulfilled) {
      result = promise->Result();
    } else if (promise->State() == v8::Promise::kRejected) {
      std::move(callback).Run(false, base::Value(), {},
                              "Native export Promise rejected");
      return;
    } else if (defer_promise) {
      const uint64_t promise_id = next_promise_id_++;
      // Retain the original Promise, including settlement before the renderer
      // subscribes. Mark it handled immediately to prevent spurious unhandled
      // rejection reports during the two-message handoff.
      deferred_promises_.emplace(
          promise_id,
          DeferredPromise{module_path, promise_owner,
                          v8::Global<v8::Promise>(addon_isolate_, promise),
                          base::TimeTicks::Now() + kDeferredPromiseLifetime});
      // Use one timer, not one delayed task per call: most tokens are consumed
      // immediately, and a high call rate must not accumulate timeout tasks.
      if (!deferred_promise_timer_.IsRunning()) {
        deferred_promise_timer_.Start(
            FROM_HERE, kDeferredPromiseLifetime, this,
            &XenonNodeExecutor::ExpireDeferredPromises);
      }
      std::move(defer_promise).Run(promise_id);
      return;
    } else if (allow_pending_promise) {
      AwaitPromise(context, module_path, promise, std::move(callback),
                   promise_owner == 0 ? std::nullopt
                                      : std::make_optional(promise_owner));
      return;
    } else {
      std::move(callback).Run(
          false, base::Value(), {},
          "Native export returned a pending Promise; only settled Promise "
          "results are supported");
      return;
    }
  }

  std::string error_msg;
  std::optional<base::Value> converted = ConvertNativeValue(
      context, module_path, result, &error_msg, 0, promise_owner);
  if (!converted) {
    std::move(callback).Run(false, base::Value(), {}, error_msg);
    return;
  }
  std::move(callback).Run(true, std::move(*converted), {}, "");
}

void XenonNodeExecutor::AwaitPromise(v8::Local<v8::Context> context,
                                     const std::string& module_path,
                                     v8::Local<v8::Promise> promise,
                                     InvokeFunctionCallback callback,
                                     std::optional<uint64_t> promise_owner) {
  const uint64_t promise_id = next_promise_id_++;
  pending_promises_.emplace(
      promise_id,
      PendingPromise{module_path, std::move(callback), promise_owner});
  v8::Local<v8::Function> resolved_fn =
      gin::CreateFunctionTemplate(
          addon_isolate_,
          base::BindRepeating(&XenonNodeExecutor::OnAsyncPromiseResolved,
                              weak_factory_.GetWeakPtr(), promise_id))
          ->GetFunction(context)
          .ToLocalChecked();
  v8::Local<v8::Function> rejected_fn =
      gin::CreateFunctionTemplate(
          addon_isolate_,
          base::BindRepeating(&XenonNodeExecutor::OnAsyncPromiseRejected,
                              weak_factory_.GetWeakPtr(), promise_id))
          ->GetFunction(context)
          .ToLocalChecked();
  if (promise->Then(context, resolved_fn, rejected_fn).IsEmpty()) {
    auto it = pending_promises_.find(promise_id);
    if (it != pending_promises_.end()) {
      auto cb = std::move(it->second.callback);
      pending_promises_.erase(it);
      std::move(cb).Run(false, base::Value(), {},
                        "Failed to attach Promise handlers");
    }
    return;
  }
  // Then() also handles Promises which settled before subscription.
  addon_isolate_->PerformMicrotaskCheckpoint();
  EnsureUvLoopPolling();
}

void XenonNodeExecutor::AwaitDeferredPromise(uint64_t promise_id,
                                             uint64_t promise_owner,
                                             InvokeFunctionCallback callback) {
  if (!IsInstanceOwnerActive(promise_owner)) {
    std::move(callback).Run(false, base::Value(), {},
                            "Native instance owner was closed");
    return;
  }
  auto it = deferred_promises_.find(promise_id);
  if (it == deferred_promises_.end() || it->second.owner != promise_owner) {
    std::move(callback).Run(false, base::Value(), {},
                            "Unknown or expired native Promise");
    return;
  }
  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  DeferredPromise deferred = std::move(it->second);
  deferred_promises_.erase(it);
  if (deferred_promises_.empty()) {
    deferred_promise_timer_.Stop();
  }
  AwaitPromise(v8_scope.context, deferred.module_path,
               deferred.promise.Get(addon_isolate_), std::move(callback),
               promise_owner);
}

void XenonNodeExecutor::ExpireDeferredPromises() {
  if (deferred_promises_.empty()) {
    return;
  }
  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  const base::TimeTicks now = base::TimeTicks::Now();
  std::erase_if(deferred_promises_, [now](const auto& item) {
    return item.second.expires_at <= now;
  });
  if (!deferred_promises_.empty()) {
    deferred_promise_timer_.Start(
        FROM_HERE, deferred_promises_.begin()->second.expires_at - now, this,
        &XenonNodeExecutor::ExpireDeferredPromises);
  }
}

void XenonNodeExecutor::CancelPromisesForOwner(uint64_t promise_owner) {
  if (!addon_isolate_) {
    return;
  }
  std::vector<InvokeFunctionCallback> callbacks;
  {
    AutoV8Scope v8_scope(addon_isolate_, addon_context_);
    std::erase_if(deferred_promises_, [promise_owner](const auto& item) {
      return item.second.owner == promise_owner;
    });
    if (deferred_promises_.empty()) {
      deferred_promise_timer_.Stop();
    }
    for (auto it = pending_promises_.begin(); it != pending_promises_.end();) {
      if (it->second.owner == promise_owner) {
        callbacks.push_back(std::move(it->second.callback));
        it = pending_promises_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& callback : callbacks) {
    std::move(callback).Run(false, base::Value(), {},
                            "Native Promise connection was closed");
  }
}

void XenonNodeExecutor::OnAsyncPromiseResolved(uint64_t promise_id,
                                               gin::Arguments* arguments) {
  auto it = pending_promises_.find(promise_id);
  if (it == pending_promises_.end()) {
    return;
  }
  PendingPromise pending = std::move(it->second);
  pending_promises_.erase(it);

  v8::Local<v8::Context> context = arguments->GetHolderCreationContext();
  if (context.IsEmpty() && !addon_context_.IsEmpty()) {
    context = addon_context_.Get(addon_isolate_);
  }
  v8::Local<v8::Value> result;
  if (!arguments->GetNext(&result) || result.IsEmpty()) {
    result = v8::Undefined(addon_isolate_);
  }

  std::string error_msg;
  std::optional<base::Value> converted =
      ConvertNativeValue(context, pending.module_path, result, &error_msg, 0,
                         pending.owner.value_or(0));
  if (!converted) {
    std::move(pending.callback).Run(false, base::Value(), {}, error_msg);
    return;
  }
  std::move(pending.callback).Run(true, std::move(*converted), {}, "");
}

void XenonNodeExecutor::OnAsyncPromiseRejected(uint64_t promise_id,
                                               gin::Arguments* arguments) {
  auto it = pending_promises_.find(promise_id);
  if (it == pending_promises_.end()) {
    return;
  }
  PendingPromise pending = std::move(it->second);
  pending_promises_.erase(it);

  std::string reason = "Native export Promise rejected";
  v8::Local<v8::Value> error_val;
  if (arguments->GetNext(&error_val) && !error_val.IsEmpty()) {
    v8::String::Utf8Value utf8(addon_isolate_, error_val);
    if (*utf8 && utf8.length() > 0) {
      reason = base::StringPrintf("Native export Promise rejected: %s", *utf8);
    }
  }
  std::move(pending.callback).Run(false, base::Value(), {}, reason);
}

XenonNodeExecutor::~XenonNodeExecutor() {
  weak_factory_.InvalidateWeakPtrs();
  deferred_promise_timer_.Stop();
  uv_loop_timer_.Stop();
  for (auto& [id, pending] : pending_promises_) {
    if (pending.callback) {
      std::move(pending.callback)
          .Run(false, base::Value(), {}, "Executor destroyed");
    }
  }
  pending_promises_.clear();
  if (addon_isolate_) {
    if (!addon_context_.IsEmpty()) {
      AutoV8Scope v8_scope(addon_isolate_, addon_context_);
      deferred_promises_.clear();
      addon_callbacks_.clear();
      addon_instances_.clear();
      addon_instance_ids_by_hash_.clear();
      addon_modules_.clear();
      addon_path_aliases_.clear();
      addon_context_.Reset();
    } else {
      v8::Locker locker(addon_isolate_);
      v8::Isolate::Scope isolate_scope(addon_isolate_);
      deferred_promises_.clear();
      addon_callbacks_.clear();
      addon_instances_.clear();
      addon_instance_ids_by_hash_.clear();
      addon_modules_.clear();
      addon_path_aliases_.clear();
    }
    addon_isolate_ = nullptr;
  }
  addon_isolate_holder_.reset();
}

void XenonNodeExecutor::EnsureUvLoopPolling() {
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (uv_loop_timer_.IsRunning() || addon_modules_.empty()) {
    return;
  }
  // Keep pumping for as long as any addon is loaded. Native engines often
  // schedule uv work (waitLoadFinish, threadpool completion) after the
  // invoking call has already returned, and many never call
  // napi_get_uv_event_loop so env->uv_loop stays null.
  uv_loop_timer_.Start(FROM_HERE, kUvLoopPollInterval, this,
                       &XenonNodeExecutor::PumpUvLoops);
#endif
}

void XenonNodeExecutor::PumpUvLoops() {
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (!addon_isolate_ || addon_modules_.empty()) {
    uv_loop_timer_.Stop();
    return;
  }
  if (addon_context_.IsEmpty()) {
    return;
  }

  struct UvLoopEntry {
    RAW_PTR_EXCLUSION uv_loop_t* loop;
    XenonUvRunFunction run;
  };
  std::vector<UvLoopEntry> loops;
  std::set<uv_loop_t*> seen;
  uv_loop_t* default_loop = uv_default_loop();
  if (default_loop) {
    seen.insert(default_loop);
    loops.push_back({default_loop, reinterpret_cast<XenonUvRunFunction>(&uv_run)});
  }
  for (const auto& [path, module] : addon_modules_) {
    if (!module.loaded_addon || !module.loaded_addon->env) {
      continue;
    }
    napi_env env = module.loaded_addon->env.get();
    if (!env->uv_loop || !env->uv_run_function ||
        !seen.insert(env->uv_loop).second) {
      continue;
    }
    loops.push_back({env->uv_loop, env->uv_run_function});
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  for (const auto& entry : loops) {
    entry.run(entry.loop, UV_RUN_NOWAIT);
  }
  addon_isolate_->PerformMicrotaskCheckpoint();
#if BUILDFLAG(IS_WIN)
  // Keep isolate entered: WndProc may invoke N-API / fire native callbacks.
  PumpWin32Messages();
#endif
#endif
}

bool XenonNodeExecutor::EnsureIsolate() {
  if (!addon_isolate_) {
    if (!gin::IsolateHolder::Initialized()) {
#if defined(V8_USE_EXTERNAL_STARTUP_DATA)
      gin::V8Initializer::LoadV8Snapshot();
#endif
      gin::IsolateHolder::Initialize(
          gin::IsolateHolder::kNonStrictMode,
          gin::ArrayBufferAllocator::SharedInstance());
    }

    addon_isolate_holder_ = std::make_unique<gin::IsolateHolder>(
        base::SingleThreadTaskRunner::GetCurrentDefault(),
        gin::IsolateHolder::kUseLocker,
        gin::IsolateHolder::IsolateType::kUtility);
    addon_isolate_ = addon_isolate_holder_->isolate();
  }
  return addon_isolate_ != nullptr;
}

XenonNodeExecutor::AddonModule* XenonNodeExecutor::FindModule(
    const std::string& module_path) {
  return const_cast<AddonModule*>(
      static_cast<const XenonNodeExecutor*>(this)->FindModule(module_path));
}

const XenonNodeExecutor::AddonModule* XenonNodeExecutor::FindModule(
    const std::string& module_path) const {
  const base::FilePath resolved = ResolveAddonPath(module_path);
  auto alias = addon_path_aliases_.find(resolved);
  const base::FilePath& key =
      alias != addon_path_aliases_.end() ? alias->second : resolved;
  auto cached = addon_modules_.find(key);
  if (cached != addon_modules_.end()) {
    return &cached->second;
  }
  auto by_resolved = addon_modules_.find(resolved);
  return by_resolved == addon_modules_.end() ? nullptr : &by_resolved->second;
}

void XenonNodeExecutor::RegisterModulePath(
    const base::FilePath& requested_path,
    const base::FilePath& canonical_path) {
  addon_path_aliases_[requested_path] = canonical_path;
  addon_path_aliases_[canonical_path] = canonical_path;
}

bool XenonNodeExecutor::HasModule(const std::string& module_path) const {
  const AddonModule* module = FindModule(module_path);
  return module && !module->exports.IsEmpty();
}

bool XenonNodeExecutor::LoadAddonFromCurrentThread(const std::string& path,
                                                   std::string* error) {
  if (!EnsureIsolate()) {
    if (error) {
      *error = "Failed to create V8 isolate in utility";
    }
    return false;
  }
  if (HasModule(path)) {
    return true;
  }

  const bool allow_external_addons =
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          napi_switches::kAllowExternalNodeAddons);
  const base::FilePath requested_path = ResolveAddonPath(path);
  PreparedAddon prepared = PrepareAddon(requested_path, allow_external_addons);
  if (!prepared.error.empty()) {
    if (error) {
      *error = prepared.error;
    }
    return false;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  auto loaded_addon = std::make_unique<LoadedNodeAddon>();
  auto resource_redirect = std::make_unique<NativeAddonResourceRedirect>(
      prepared.library.get(), runtime_directory_);
  v8::Local<v8::Value> exports_value = InitializeLoadedNodeAddon(
      addon_isolate_, context, std::move(prepared.library), loaded_addon.get());
  if (exports_value.IsEmpty() || !exports_value->IsObject()) {
    if (error) {
      *error = "Failed to initialize addon in utility";
    }
    return false;
  }

  RegisterModulePath(requested_path, prepared.path);
  AddonModule module;
  module.exports.Reset(addon_isolate_, exports_value);
  module.loaded_addon = std::move(loaded_addon);
  module.resource_redirect = std::move(resource_redirect);
  addon_modules_.insert_or_assign(prepared.path, std::move(module));
  EnsureUvLoopPolling();
  LOG(INFO) << "[XenonNodeExecutor] Loaded native addon "
            << prepared.path.AsUTF8Unsafe();
  return true;
}

bool XenonNodeExecutor::InspectExportFromCurrentThread(
    const std::string& path,
    const std::string& export_path,
    mojom::NodeExportInfoPtr* description,
    std::string* error) {
  AddonModule* module = FindModule(path);
  if (!module || !addon_isolate_ || addon_context_.IsEmpty() ||
      module->exports.IsEmpty()) {
    *error = "Node addon has not been loaded: " + path;
    return false;
  }
  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  v8::Local<v8::Value> value = module->exports.Get(addon_isolate_);
  gin::TryCatch try_catch(addon_isolate_);
  if (!export_path.empty()) {
    for (const auto& part : base::SplitString(
             export_path, ".", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL)) {
      v8::Local<v8::Value> next;
      if (!value->IsObject() ||
          !value.As<v8::Object>()
               ->Get(context, gin::StringToV8(addon_isolate_, part))
               .ToLocal(&next)) {
        *error = try_catch.HasCaught()
                     ? DescribeCaughtException(export_path, &try_catch)
                     : "Native export path is not an object: " + export_path;
        return false;
      }
      value = next;
    }
  }
  *description =
      DescribeExactExportValue(addon_isolate_, context, export_path, value);
  if (!*description) {
    *error = try_catch.HasCaught()
                 ? DescribeCaughtException(export_path, &try_catch)
                 : "Failed to inspect native export descriptors";
    return false;
  }
  return true;
}

bool XenonNodeExecutor::InvokeExportFromCurrentThread(
    const std::string& path,
    const std::string& function_name,
    const base::Value& args,
    base::Value* result,
    std::string* error) {
  std::string load_error;
  if (!LoadAddonFromCurrentThread(path, &load_error)) {
    if (error) {
      *error = load_error;
    }
    return false;
  }

  bool ok = false;
  InvokeFunction(
      path, function_name, /*client_id=*/0, ListValueToInvokeArgs(args),
      base::BindOnce(
          [](bool* ok, base::Value* result, std::string* error, bool success,
             base::Value value,
             std::vector<mojom::NodeCallbackResultPtr> /*callback_results*/,
             const std::string& error_msg) {
            *ok = success;
            if (success) {
              if (result) {
                *result = std::move(value);
              }
            } else if (error) {
              *error = error_msg.empty() ? "Native export invocation failed"
                                         : error_msg;
            }
          },
          &ok, result, error),
      /*allow_pending_promise=*/false);
  return ok;
}

bool XenonNodeExecutor::ConstructExportFromCurrentThread(
    const std::string& path,
    const std::string& export_path,
    const base::Value& args,
    base::Value* instance,
    std::string* error) {
  std::string load_error;
  if (!LoadAddonFromCurrentThread(path, &load_error)) {
    if (error) {
      *error = load_error;
    }
    return false;
  }

  bool ok = false;
  int32_t instance_id = 0;
  ConstructExport(
      path, export_path, /*client_id=*/0, ListValueToInvokeArgs(args),
      base::BindOnce(
          [](bool* ok, int32_t* instance_id, std::string* error, bool success,
             int32_t id, const std::string& error_msg) {
            *ok = success;
            if (success) {
              if (instance_id) {
                *instance_id = id;
              }
            } else if (error) {
              *error = error_msg.empty() ? "Native construct failed"
                                         : error_msg;
            }
          },
          &ok, &instance_id, error));
  if (!ok) {
    return false;
  }
  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  v8::Local<v8::Object> object =
      addon_instances_.at(instance_id).object.Get(addon_isolate_);
  base::DictValue wire;
  wire.Set(kWireTypeKey, kNativeInstanceWireType);
  wire.Set("instance_id", instance_id);
  wire.Set("prototype",
           NativePrototypeMembersToWire(addon_isolate_, context, object));
  // Snapshot data descriptors only: constructing a wrapper must not call
  // unrelated native getters just to discover an instance's fields.
  auto description =
      DescribeExactExportValue(addon_isolate_, context, "", object);
  if (!description) {
    *error = "Failed to inspect constructed native instance";
    return false;
  }
  base::DictValue fields;
  for (const auto& child : description->children) {
    if (child->has_value) {
      fields.Set(child->name,
                 child->kind == "bigint"
                     ? base::Value(base::DictValue()
                                       .Set(kWireTypeKey, "bigint")
                                       .Set("value", child->value.Clone()))
                     : child->value.Clone());
    }
  }
  wire.Set("fields", std::move(fields));
  *instance = base::Value(std::move(wire));
  return true;
}

bool XenonNodeExecutor::InvokeInstanceFromCurrentThread(
    const std::string& path,
    int32_t instance_id,
    const std::string& method_name,
    const base::Value& args,
    base::Value* result,
    std::string* error) {
  if (!addon_isolate_) {
    if (error) {
      *error = "No Node addon has been loaded";
    }
    return false;
  }

  bool ok = false;
  InvokeInstance(
      path, instance_id, method_name, /*client_id=*/0,
      ListValueToInvokeArgs(args),
      base::BindOnce(
          [](bool* ok, base::Value* result, std::string* error, bool success,
             base::Value value,
             std::vector<mojom::NodeCallbackResultPtr> /*callback_results*/,
             const std::string& error_msg) {
            *ok = success;
            if (success) {
              if (result) {
                *result = std::move(value);
              }
            } else if (error) {
              *error = error_msg.empty() ? "Native instance invocation failed"
                                         : error_msg;
            }
          },
          &ok, result, error),
      /*allow_pending_promise=*/false);
  return ok;
}

std::vector<mojom::NodeExportInfoPtr> XenonNodeExecutor::GetCachedExportTree(
    AddonModule* module) {
  if (!module->export_tree_initialized) {
    AutoV8Scope v8_scope(addon_isolate_, addon_context_);
    module->export_tree =
        BuildExportTree(addon_isolate_, v8_scope.context,
                        module->exports.Get(addon_isolate_).As<v8::Object>(),
                        kMaxExportInspectDepth);
    module->export_tree_initialized = true;
  }
  return CloneExportTree(module->export_tree);
}

void XenonNodeExecutor::LoadAddon(const std::string& path,
                                  LoadAddonCallback callback,
                                  bool include_export_tree) {
  if (!EnsureIsolate()) {
    LOG(ERROR) << "[XenonNodeExecutor] Failed to create V8 isolate";
    std::move(callback).Run(false, "Failed to create V8 isolate in utility", {});
    return;
  }

  if (AddonModule* cached_addon = FindModule(path);
      cached_addon && !cached_addon->exports.IsEmpty()) {
    std::move(callback).Run(true, "",
                            include_export_tree
                                ? GetCachedExportTree(cached_addon)
                                : std::vector<mojom::NodeExportInfoPtr>());
    return;
  }

  const bool allow_external_addons =
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          napi_switches::kAllowExternalNodeAddons);
  const base::FilePath addon_path = ResolveAddonPath(path);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&PrepareAddon, addon_path, allow_external_addons),
      base::BindOnce(
          [](base::WeakPtr<XenonNodeExecutor> self, base::FilePath addon_path,
             bool include_export_tree, LoadAddonCallback callback,
             PreparedAddon prepared) {
            if (!self) {
              std::move(callback).Run(
                  false, "Node executor was destroyed during load", {});
              return;
            }
            if (!prepared.error.empty()) {
              std::move(callback).Run(false, prepared.error, {});
              return;
            }

            if (AddonModule* cached =
                    self->FindModule(addon_path.AsUTF8Unsafe());
                cached && !cached->exports.IsEmpty()) {
              std::move(callback).Run(
                  true, "",
                  include_export_tree
                      ? self->GetCachedExportTree(cached)
                      : std::vector<mojom::NodeExportInfoPtr>());
              return;
            }

            AutoV8Scope v8_scope(self->addon_isolate_, self->addon_context_);
            v8::Local<v8::Context> context = v8_scope.context;
            auto loaded_addon = std::make_unique<LoadedNodeAddon>();
            auto resource_redirect =
                std::make_unique<NativeAddonResourceRedirect>(
                    prepared.library.get(), self->runtime_directory_);
            v8::Local<v8::Value> exports_value = InitializeLoadedNodeAddon(
                self->addon_isolate_, context, std::move(prepared.library),
                loaded_addon.get());
            if (exports_value.IsEmpty() || !exports_value->IsObject()) {
              LOG(ERROR)
                  << "[XenonNodeExecutor] InitializeLoadedNodeAddon failed";
              std::move(callback).Run(
                  false, "Failed to initialize addon in utility", {});
              return;
            }

            for (auto instance = self->addon_instances_.begin();
                 instance != self->addon_instances_.end();) {
              if (instance->second.module_path == prepared.path ||
                  instance->second.module_path == addon_path) {
                const int32_t instance_id = (instance++)->first;
                self->RemoveNativeInstance(instance_id);
              } else {
                ++instance;
              }
            }

            AddonModule module;
            module.exports.Reset(self->addon_isolate_, exports_value);
            module.loaded_addon = std::move(loaded_addon);
            module.resource_redirect = std::move(resource_redirect);
            self->RegisterModulePath(addon_path, prepared.path);
            auto entry = self->addon_modules_.insert_or_assign(
                prepared.path, std::move(module));
            self->EnsureUvLoopPolling();
            std::move(callback).Run(
                true, "",
                include_export_tree
                    ? self->GetCachedExportTree(&entry.first->second)
                    : std::vector<mojom::NodeExportInfoPtr>());
          },
          weak_factory_.GetWeakPtr(), addon_path, include_export_tree,
          std::move(callback)));
}

void XenonNodeExecutor::InspectExport(const std::string& module_path,
                                      const std::string& export_path,
                                      InspectExportCallback callback) {
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, "No Node addon has been loaded", nullptr);
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  AddonModule* cached_addon = FindModule(module_path);
  if (!cached_addon || cached_addon->exports.IsEmpty()) {
    std::move(callback).Run(
        false, "Node addon has not been loaded: " + module_path, nullptr);
    return;
  }

  v8::Local<v8::Value> exports_value =
      cached_addon->exports.Get(addon_isolate_);
  if (!exports_value->IsObject()) {
    std::move(callback).Run(false, "Node addon exports is not an object",
                            nullptr);
    return;
  }

  v8::Local<v8::Object> exports = exports_value.As<v8::Object>();
  v8::Local<v8::Value> target = exports;
  std::string leaf_name = export_path;
  if (!export_path.empty()) {
    const std::vector<std::string> parts = base::SplitString(
        export_path, ".", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
    if (parts.empty()) {
      std::move(callback).Run(false, "Export path is empty", nullptr);
      return;
    }
    for (size_t i = 0; i < parts.size(); ++i) {
      if (!target->IsObject()) {
        std::move(callback).Run(false, "Export path is not an object", nullptr);
        return;
      }
      v8::Local<v8::Value> next;
      if (!target.As<v8::Object>()
               ->Get(context, gin::StringToV8(addon_isolate_, parts[i]))
               .ToLocal(&next) ||
          next->IsUndefined()) {
        std::move(callback).Run(false, "Export is missing: " + export_path,
                                nullptr);
        return;
      }
      target = next;
      leaf_name = parts[i];
    }
  }

  auto info = DescribeExportValue(addon_isolate_, context, leaf_name, target,
                                  /*depth=*/0, kMaxExportInspectDepth);

  // Refresh cached shallow entry's children when inspecting a top-level name.
  if (export_path.find('.') == std::string::npos) {
    for (auto& cached : cached_addon->export_tree) {
      if (cached && cached->name == leaf_name) {
        cached = info.Clone();
        break;
      }
    }
  }

  std::move(callback).Run(true, "", std::move(info));
}

void XenonNodeExecutor::ConstructExport(
    const std::string& module_path,
    const std::string& export_path,
    int32_t client_id,
    std::vector<mojom::NodeInvokeArgPtr> args,
    ConstructExportCallback callback,
    uint64_t owner,
    base::DictValue prototype_properties) {
  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, 0, "Native instance owner was closed");
    return;
  }
  LOG(INFO) << "[XenonNodeExecutor] ConstructExport " << module_path << " "
            << export_path;
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, 0, "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  AddonModule* cached_addon = FindModule(module_path);
  if (!cached_addon || cached_addon->exports.IsEmpty()) {
    std::move(callback).Run(false, 0,
                            "Node addon has not been loaded: " + module_path);
    return;
  }

  v8::Local<v8::Value> exports_value =
      cached_addon->exports.Get(addon_isolate_);
  if (!exports_value->IsObject()) {
    std::move(callback).Run(false, 0, "Node addon exports is not an object");
    return;
  }

  v8::Local<v8::Function> constructor;
  v8::Local<v8::Object> unused_receiver;
  std::string resolve_error;
  if (!ResolveExportFunction(addon_isolate_, context,
                             exports_value.As<v8::Object>(), export_path,
                             &constructor, &unused_receiver, &resolve_error)) {
    std::move(callback).Run(false, 0, resolve_error);
    return;
  }
  if (!constructor->IsConstructor()) {
    std::move(callback).Run(false, 0,
                            "Node export is not constructable: " + export_path);
    return;
  }
  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, 0, "Native instance owner was closed");
    return;
  }

  std::vector<v8::Local<v8::Value>> argv;
  argv.reserve(args.size());
  for (const auto& arg : args) {
    if (!IsInstanceOwnerActive(owner)) {
      std::move(callback).Run(false, 0, "Native instance owner was closed");
      return;
    }
    if (!arg) {
      std::move(callback).Run(false, 0, "Invalid constructor argument");
      return;
    }
    if (arg->is_callback) {
      argv.push_back(GetOrCreateNativeCallback(
          context, client_id, arg->callback_id, module_path, owner));
      continue;
    }
    std::string error_msg;
    v8::Local<v8::Value> converted;
    if (!WireValueToV8(context, arg->value, client_id, module_path, &error_msg,
                       0, owner)
             .ToLocal(&converted)) {
      std::move(callback).Run(false, 0, error_msg);
      return;
    }
    argv.push_back(converted);
  }

  gin::TryCatch try_catch(addon_isolate_);
  v8::Local<v8::Object> instance;
  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, 0, "Native instance owner was closed");
    return;
  }
  if (prototype_properties.empty()) {
    if (!constructor
             ->NewInstance(context, static_cast<int>(argv.size()), argv.data())
             .ToLocal(&instance)) {
      std::move(callback).Run(false, 0,
                              DescribeCaughtException(export_path, &try_catch));
      return;
    }
  } else {
    // Userland wrappers (for example sqlite3's EventEmitter mixin) add methods
    // that native constructors may call immediately. Supply an independent
    // newTarget prototype before construction, never modify the shared addon
    // constructor.prototype or replace its native methods.
    v8::Local<v8::Value> native_prototype;
    if (!constructor->Get(context, gin::StringToV8(addon_isolate_, "prototype"))
             .ToLocal(&native_prototype) ||
        !native_prototype->IsObject()) {
      std::move(callback).Run(false, 0,
                              "Native constructor has no object prototype");
      return;
    }
    v8::Local<v8::Object> prototype = v8::Object::New(addon_isolate_);
    if (!prototype->SetPrototype(context, native_prototype).FromMaybe(false)) {
      std::move(callback).Run(false, 0,
                              "Failed to create native instance prototype");
      return;
    }
    for (const auto [name, value] : prototype_properties) {
      v8::Local<v8::String> key = gin::StringToV8(addon_isolate_, name);
      const v8::Maybe<bool> present =
          native_prototype.As<v8::Object>()->Has(context, key);
      if (present.IsNothing()) {
        std::move(callback).Run(
            false, 0, DescribeCaughtException(export_path, &try_catch));
        return;
      }
      if (present.FromJust() || name == "constructor") {
        continue;
      }
      std::string error;
      v8::Local<v8::Value> converted;
      if (!WireValueToV8(context, value, client_id, module_path, &error, 0,
                         owner)
               .ToLocal(&converted)) {
        std::move(callback).Run(false, 0, error);
        return;
      }
      if (!converted->IsFunction() ||
          !prototype->CreateDataProperty(context, key, converted)
               .FromMaybe(false)) {
        std::move(callback).Run(false, 0,
                                "Native prototype additions must be functions");
        return;
      }
    }
    v8::Local<v8::Function> new_target;
    v8::Local<v8::Value> reflect;
    v8::Local<v8::Value> reflect_construct;
    if (!v8::Function::New(context,
                           [](const v8::FunctionCallbackInfo<v8::Value>&) {})
             .ToLocal(&new_target) ||
        !new_target
             ->Set(context, gin::StringToV8(addon_isolate_, "prototype"),
                   prototype)
             .FromMaybe(false) ||
        !context->Global()
             ->Get(context, gin::StringToV8(addon_isolate_, "Reflect"))
             .ToLocal(&reflect) ||
        !reflect->IsObject() ||
        !reflect.As<v8::Object>()
             ->Get(context, gin::StringToV8(addon_isolate_, "construct"))
             .ToLocal(&reflect_construct) ||
        !reflect_construct->IsFunction()) {
      std::move(callback).Run(
          false, 0, "Failed to prepare native prototype construction");
      return;
    }
    v8::Local<v8::Array> arguments =
        v8::Array::New(addon_isolate_, static_cast<int>(argv.size()));
    for (size_t i = 0; i < argv.size(); ++i) {
      if (!arguments
               ->CreateDataProperty(context, static_cast<uint32_t>(i), argv[i])
               .FromMaybe(false)) {
        std::move(callback).Run(false, 0,
                                "Failed to prepare constructor arguments");
        return;
      }
    }
    if (!IsInstanceOwnerActive(owner)) {
      std::move(callback).Run(false, 0, "Native instance owner was closed");
      return;
    }
    v8::Local<v8::Value> construct_args[] = {constructor, arguments,
                                             new_target};
    v8::Local<v8::Value> constructed;
    if (!reflect_construct.As<v8::Function>()
             ->Call(context, reflect, 3, construct_args)
             .ToLocal(&constructed) ||
        !constructed->IsObject()) {
      std::move(callback).Run(false, 0,
                              DescribeCaughtException(export_path, &try_catch));
      return;
    }
    instance = constructed.As<v8::Object>();
  }
  EnsureUvLoopPolling();

  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, 0, "Native instance owner was closed");
    return;
  }
  const int32_t instance_id =
      RegisterNativeInstance(module_path, instance, owner);
  std::move(callback).Run(true, instance_id, "");
}

void XenonNodeExecutor::InvokeInstance(
    const std::string& module_path,
    int32_t instance_id,
    const std::string& method_name,
    int32_t client_id,
    std::vector<mojom::NodeInvokeArgPtr> args,
    InvokeFunctionCallback callback,
    bool allow_pending_promise,
    DeferPromiseCallback defer_promise,
    uint64_t promise_owner) {
  if (!IsInstanceOwnerActive(promise_owner)) {
    std::move(callback).Run(false, base::Value(), {},
                            "Native instance owner was closed");
    return;
  }
  // Check before resolving the method path, which can itself call native code.
  if ((allow_pending_promise || defer_promise) &&
      pending_promises_.size() + deferred_promises_.size() >=
          kMaxPendingNativePromises) {
    std::move(callback).Run(false, base::Value(), {},
                            "Too many pending native Promises");
    return;
  }
  LOG(INFO) << "[XenonNodeExecutor] InvokeInstance id=" << instance_id << " "
            << method_name;
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, base::Value(), {},
                            "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;

  auto it = addon_instances_.find(instance_id);
  if (it == addon_instances_.end() || it->second.object.IsEmpty()) {
    std::move(callback).Run(
        false, base::Value(), {},
        "Unknown instance id: " + std::to_string(instance_id));
    return;
  }

  base::FilePath addon_path = ResolveAddonPath(module_path);
  if (it->second.owner != promise_owner) {
    std::move(callback).Run(false, base::Value(), {},
                            "Native instance does not belong to this owner");
    return;
  }
  if (it->second.module_path != addon_path) {
    std::move(callback).Run(
        false, base::Value(), {},
        "Instance does not belong to module: " + module_path);
    return;
  }

  v8::Local<v8::Object> receiver = it->second.object.Get(addon_isolate_);
  std::string resolved_method_name = method_name;

  // Native addons commonly return another wrapped object (for example,
  // NativeAplayerStack.getCurrPlayMedia()). Such objects cannot be serialized
  // as ordinary Mojo values because their useful API lives on the prototype.
  // The WebUI bridge's reserved $invokePath helper encodes a no-argument
  // method chain here so the whole chain stays inside the addon isolate and
  // only the final primitive/result crosses Mojo.
  if (resolved_method_name.starts_with(kInvokePathPrefix)) {
    resolved_method_name.erase(0, std::strlen(kInvokePathPrefix));
    const std::vector<std::string> path = base::SplitString(
        resolved_method_name, ".", base::KEEP_WHITESPACE,
        base::SPLIT_WANT_NONEMPTY);
    if (path.size() < 2) {
      std::move(callback).Run(false, base::Value(), {},
                              "Native instance method path is invalid");
      return;
    }

    for (size_t i = 0; i + 1 < path.size(); ++i) {
      if (!IsInstanceOwnerActive(promise_owner)) {
        std::move(callback).Run(false, base::Value(), {},
                                "Native instance owner was closed");
        return;
      }
      v8::Local<v8::Value> getter_value;
      if (!receiver->Get(context, gin::StringToV8(addon_isolate_, path[i]))
               .ToLocal(&getter_value) ||
          !getter_value->IsFunction()) {
        std::move(callback).Run(
            false, base::Value(), {},
            "Instance method path segment is missing: " + path[i]);
        return;
      }

      if (!IsInstanceOwnerActive(promise_owner)) {
        std::move(callback).Run(false, base::Value(), {},
                                "Native instance owner was closed");
        return;
      }
      gin::TryCatch try_catch(addon_isolate_);
      v8::Local<v8::Value> nested_value;
      if (!getter_value.As<v8::Function>()
               ->Call(context, receiver, 0, nullptr)
               .ToLocal(&nested_value)) {
        std::move(callback).Run(
            false, base::Value(), {},
            DescribeCaughtException(path[i], &try_catch));
        return;
      }
      EnsureUvLoopPolling();
      if (!IsInstanceOwnerActive(promise_owner)) {
        std::move(callback).Run(false, base::Value(), {},
                                "Native instance owner was closed");
        return;
      }

      if (nested_value->IsPromise()) {
        addon_isolate_->PerformMicrotaskCheckpoint();
        if (!IsInstanceOwnerActive(promise_owner)) {
          std::move(callback).Run(false, base::Value(), {},
                                  "Native instance owner was closed");
          return;
        }
        v8::Local<v8::Promise> promise = nested_value.As<v8::Promise>();
        if (promise->State() != v8::Promise::kFulfilled) {
          std::move(callback).Run(
              false, base::Value(), {},
              "Instance method path returned an unsettled or rejected Promise: " +
                  path[i]);
          return;
        }
        nested_value = promise->Result();
      }

      if (!nested_value->IsObject()) {
        std::move(callback).Run(
            false, base::Value(), {},
            "Instance method path did not return an object: " + path[i]);
        return;
      }
      receiver = nested_value.As<v8::Object>();
    }
    resolved_method_name = path.back();
  }

  // Ordinary [[Get]] walks the prototype chain, same as JS instance.method.
  v8::Local<v8::Value> method_value;
  if (!receiver
           ->Get(context,
                 gin::StringToV8(addon_isolate_, resolved_method_name))
           .ToLocal(&method_value) ||
      !method_value->IsFunction()) {
    std::move(callback).Run(false, base::Value(), {},
                            "Instance method is missing: " +
                                resolved_method_name);
    return;
  }
  v8::Local<v8::Function> function = method_value.As<v8::Function>();

  InvokeResolvedFunction(context, module_path, resolved_method_name, function,
                         receiver, client_id, args, std::move(callback),
                         allow_pending_promise, std::move(defer_promise),
                         promise_owner);
}

void XenonNodeExecutor::GetInstanceProperty(const std::string& module_path,
                                            int32_t instance_id,
                                            const std::string& property_name,
                                            GetPropertyCallback callback,
                                            uint64_t owner) {
  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, base::Value(),
                            "Native instance owner was closed");
    return;
  }
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, base::Value(),
                            "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  auto it = addon_instances_.find(instance_id);
  if (it == addon_instances_.end() || it->second.object.IsEmpty()) {
    std::move(callback).Run(
        false, base::Value(),
        "Unknown instance id: " + std::to_string(instance_id));
    return;
  }
  if (it->second.owner != owner) {
    std::move(callback).Run(false, base::Value(),
                            "Native instance does not belong to this owner");
    return;
  }
  if (it->second.module_path != ResolveAddonPath(module_path)) {
    std::move(callback).Run(
        false, base::Value(),
        "Instance does not belong to module: " + module_path);
    return;
  }

  v8::Local<v8::Value> value;
  if (!it->second.object.Get(addon_isolate_)
           ->Get(context, gin::StringToV8(addon_isolate_, property_name))
           .ToLocal(&value)) {
    std::move(callback).Run(
        false, base::Value(),
        "Failed to read instance property: " + property_name);
    return;
  }

  std::string error_msg;
  std::optional<base::Value> converted =
      ConvertNativeValue(context, module_path, value, &error_msg, 0, owner);
  if (!converted) {
    std::move(callback).Run(false, base::Value(), error_msg);
    return;
  }
  std::move(callback).Run(true, std::move(*converted), "");
}

void XenonNodeExecutor::InspectInstanceMember(const std::string& module_path,
                                              int32_t instance_id,
                                              const std::string& property_name,
                                              GetPropertyCallback callback,
                                              uint64_t owner) {
  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, base::Value(),
                            "Native instance owner was closed");
    return;
  }
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, base::Value(),
                            "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  auto it = addon_instances_.find(instance_id);
  if (it == addon_instances_.end() || it->second.object.IsEmpty()) {
    std::move(callback).Run(
        false, base::Value(),
        "Unknown instance id: " + std::to_string(instance_id));
    return;
  }
  if (it->second.owner != owner) {
    std::move(callback).Run(false, base::Value(),
                            "Native instance does not belong to this owner");
    return;
  }
  if (it->second.module_path != ResolveAddonPath(module_path)) {
    std::move(callback).Run(
        false, base::Value(),
        "Instance does not belong to module: " + module_path);
    return;
  }

  gin::TryCatch try_catch(addon_isolate_);
  v8::Local<v8::Value> value;
  if (!it->second.object.Get(addon_isolate_)
           ->Get(context, gin::StringToV8(addon_isolate_, property_name))
           .ToLocal(&value)) {
    std::move(callback).Run(
        false, base::Value(),
        DescribeCaughtException(property_name, &try_catch));
    return;
  }

  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, base::Value(),
                            "Native instance owner was closed");
    return;
  }
  base::DictValue result;
  if (value->IsUndefined()) {
    result.Set("kind", "undefined");
  } else if (value->IsFunction()) {
    result.Set("kind", "function");
  } else {
    std::string error_msg;
    std::optional<base::Value> converted =
        ConvertNativeValue(context, module_path, value, &error_msg, 0, owner);
    if (!converted) {
      std::move(callback).Run(false, base::Value(), error_msg);
      return;
    }
    result.Set("kind", "value");
    result.Set("value", std::move(*converted));
  }
  std::move(callback).Run(true, base::Value(std::move(result)), "");
}

void XenonNodeExecutor::SetInstanceProperty(const std::string& module_path,
                                            int32_t instance_id,
                                            const std::string& property_name,
                                            base::Value value,
                                            SetPropertyCallback callback,
                                            uint64_t owner) {
  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, "Native instance owner was closed");
    return;
  }
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  auto it = addon_instances_.find(instance_id);
  if (it == addon_instances_.end() || it->second.object.IsEmpty()) {
    std::move(callback).Run(
        false, "Unknown instance id: " + std::to_string(instance_id));
    return;
  }
  if (it->second.owner != owner) {
    std::move(callback).Run(false,
                            "Native instance does not belong to this owner");
    return;
  }
  if (it->second.module_path != ResolveAddonPath(module_path)) {
    std::move(callback).Run(
        false, "Instance does not belong to module: " + module_path);
    return;
  }

  v8::Local<v8::Object> receiver = it->second.object.Get(addon_isolate_);
  std::string error_msg;
  v8::Local<v8::Value> converted;
  if (!WireValueToV8(context, value, /*client_id=*/0, module_path, &error_msg,
                     0, owner)
           .ToLocal(&converted)) {
    std::move(callback).Run(false, error_msg);
    return;
  }
  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, "Native instance owner was closed");
    return;
  }
  if (!receiver
           ->Set(context, gin::StringToV8(addon_isolate_, property_name),
                 converted)
           .FromMaybe(false)) {
    std::move(callback).Run(
        false, "Failed to write instance property: " + property_name);
    return;
  }
  if (!IsInstanceOwnerActive(owner)) {
    std::move(callback).Run(false, "Native instance owner was closed");
    return;
  }
  std::move(callback).Run(true, "");
}

void XenonNodeExecutor::ReleaseInstance(const std::string& module_path,
                                        int32_t instance_id,
                                        uint64_t owner,
                                        const std::string& expected_token) {
  if (!ValidateInstanceOwnerToken(owner, expected_token) || !addon_isolate_ ||
      addon_context_.IsEmpty()) {
    return;
  }
  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  auto it = addon_instances_.find(instance_id);
  if (it == addon_instances_.end() || it->second.owner != owner ||
      it->second.module_path != ResolveAddonPath(module_path)) {
    return;
  }
  RemoveNativeInstance(instance_id);
}

void XenonNodeExecutor::InvokeFunction(
    const std::string& module_path,
    const std::string& function_name,
    int32_t client_id,
    std::vector<mojom::NodeInvokeArgPtr> args,
    InvokeFunctionCallback callback,
    bool allow_pending_promise,
    DeferPromiseCallback defer_promise,
    uint64_t promise_owner) {
  if (!IsInstanceOwnerActive(promise_owner)) {
    std::move(callback).Run(false, base::Value(), {},
                            "Native instance owner was closed");
    return;
  }
  // Reject before entering native code, including export property getters.
  if ((allow_pending_promise || defer_promise) &&
      pending_promises_.size() + deferred_promises_.size() >=
          kMaxPendingNativePromises) {
    std::move(callback).Run(false, base::Value(), {},
                            "Too many pending native Promises");
    return;
  }
  LOG(INFO) << "[XenonNodeExecutor] InvokeFunction " << module_path << " "
            << function_name;
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, base::Value(), {},
                            "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;

  AddonModule* cached_addon = FindModule(module_path);
  if (!cached_addon || cached_addon->exports.IsEmpty()) {
    std::move(callback).Run(false, base::Value(), {},
                            "Node addon has not been loaded: " + module_path);
    return;
  }

  v8::Local<v8::Value> exports_value =
      cached_addon->exports.Get(addon_isolate_);
  if (!exports_value->IsObject()) {
    std::move(callback).Run(
        false, base::Value(), {},
        "Node addon exports is not an object: " + module_path);
    return;
  }

  v8::Local<v8::Object> exports = exports_value.As<v8::Object>();

  v8::Local<v8::Function> function;
  v8::Local<v8::Object> receiver;
  std::string resolve_error;
  if (!ResolveExportFunction(addon_isolate_, context, exports, function_name,
                             &function, &receiver, &resolve_error)) {
    std::move(callback).Run(false, base::Value(), {}, resolve_error);
    return;
  }

  InvokeResolvedFunction(context, module_path, function_name, function,
                         receiver, client_id, args, std::move(callback),
                         allow_pending_promise, std::move(defer_promise),
                         promise_owner);
}

void XenonNodeExecutor::GetExportProperty(const std::string& module_path,
                                          const std::string& object_path,
                                          const std::string& property_name,
                                          GetPropertyCallback callback) {
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, base::Value(),
                            "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  AddonModule* module = FindModule(module_path);
  if (!module || module->exports.IsEmpty()) {
    std::move(callback).Run(false, base::Value(),
                            "Node addon has not been loaded: " + module_path);
    return;
  }

  v8::Local<v8::Value> exports_value =
      module->exports.Get(addon_isolate_);
  if (!exports_value->IsObject()) {
    std::move(callback).Run(false, base::Value(),
                            "Node addon exports is not an object");
    return;
  }

  v8::Local<v8::Object> object = exports_value.As<v8::Object>();
  if (!object_path.empty()) {
    v8::Local<v8::Value> resolved;
    v8::Local<v8::Object> unused_receiver;
    std::string error_msg;
    if (!ResolveExportValue(addon_isolate_, context, object, object_path,
                            &resolved, &unused_receiver, &error_msg)) {
      std::move(callback).Run(false, base::Value(), error_msg);
      return;
    }
    if (!resolved->IsObject()) {
      std::move(callback).Run(false, base::Value(),
                              "Export path is not an object: " + object_path);
      return;
    }
    object = resolved.As<v8::Object>();
  }

  v8::Local<v8::Value> value;
  if (!object->Get(context, gin::StringToV8(addon_isolate_, property_name))
           .ToLocal(&value)) {
    std::move(callback).Run(false, base::Value(),
                            "Failed to read export property: " + property_name);
    return;
  }

  std::string error_msg;
  std::optional<base::Value> converted =
      ConvertNativeValue(context, module_path, value, &error_msg, 0);
  if (!converted) {
    std::move(callback).Run(false, base::Value(), error_msg);
    return;
  }
  std::move(callback).Run(true, std::move(*converted), "");
}

void XenonNodeExecutor::SetExportProperty(const std::string& module_path,
                                          const std::string& object_path,
                                          const std::string& property_name,
                                          base::Value value,
                                          SetPropertyCallback callback) {
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  AddonModule* module = FindModule(module_path);
  if (!module || module->exports.IsEmpty()) {
    std::move(callback).Run(false,
                            "Node addon has not been loaded: " + module_path);
    return;
  }

  v8::Local<v8::Value> exports_value =
      module->exports.Get(addon_isolate_);
  if (!exports_value->IsObject()) {
    std::move(callback).Run(false, "Node addon exports is not an object");
    return;
  }

  v8::Local<v8::Object> object = exports_value.As<v8::Object>();
  if (!object_path.empty()) {
    v8::Local<v8::Value> resolved;
    v8::Local<v8::Object> unused_receiver;
    std::string error_msg;
    if (!ResolveExportValue(addon_isolate_, context, object, object_path,
                            &resolved, &unused_receiver, &error_msg)) {
      std::move(callback).Run(false, error_msg);
      return;
    }
    if (!resolved->IsObject()) {
      std::move(callback).Run(false,
                              "Export path is not an object: " + object_path);
      return;
    }
    object = resolved.As<v8::Object>();
  }

  std::string error_msg;
  v8::Local<v8::Value> converted;
  if (!WireValueToV8(context, value, /*client_id=*/0, module_path, &error_msg)
           .ToLocal(&converted)) {
    std::move(callback).Run(false, error_msg);
    return;
  }
  if (!object
           ->Set(context, gin::StringToV8(addon_isolate_, property_name),
                 converted)
           .FromMaybe(false)) {
    std::move(callback).Run(
        false, "Failed to write export property: " + property_name);
    return;
  }
  std::move(callback).Run(true, "");
}

void XenonNodeExecutor::InvokeMany(const std::string& module_path,
                                   std::vector<mojom::NodeInvokeCallPtr> calls,
                                   InvokeManyCallback callback) {
  std::vector<mojom::NodeInvokeCallResultPtr> results;
  results.reserve(calls.size());

  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    for (size_t i = 0; i < calls.size(); ++i) {
      auto one = mojom::NodeInvokeCallResult::New();
      one->success = false;
      one->error_msg = "No Node addon has been loaded";
      results.push_back(std::move(one));
    }
    std::move(callback).Run(std::move(results));
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  AddonModule* cached_addon = FindModule(module_path);
  if (!cached_addon || cached_addon->exports.IsEmpty()) {
    for (size_t i = 0; i < calls.size(); ++i) {
      auto one = mojom::NodeInvokeCallResult::New();
      one->success = false;
      one->error_msg = "Node addon has not been loaded: " + module_path;
      results.push_back(std::move(one));
    }
    std::move(callback).Run(std::move(results));
    return;
  }

  v8::Local<v8::Value> exports_value =
      cached_addon->exports.Get(addon_isolate_);
  if (!exports_value->IsObject()) {
    for (size_t i = 0; i < calls.size(); ++i) {
      auto one = mojom::NodeInvokeCallResult::New();
      one->success = false;
      one->error_msg = "Node addon exports is not an object";
      results.push_back(std::move(one));
    }
    std::move(callback).Run(std::move(results));
    return;
  }
  v8::Local<v8::Object> exports = exports_value.As<v8::Object>();

  for (auto& call : calls) {
    auto one = mojom::NodeInvokeCallResult::New();
    if (!call) {
      one->success = false;
      one->error_msg = "Invalid invoke call";
      results.push_back(std::move(one));
      continue;
    }

    bool has_callback_arg = false;
    for (const auto& arg : call->args) {
      if (arg && arg->is_callback) {
        has_callback_arg = true;
        break;
      }
    }
    if (has_callback_arg) {
      one->success = false;
      one->error_msg =
          "InvokeMany does not support callback arguments; use InvokeFunction";
      results.push_back(std::move(one));
      continue;
    }

    v8::Local<v8::Function> function;
    v8::Local<v8::Object> receiver;
    std::string resolve_error;
    if (!ResolveExportFunction(addon_isolate_, context, exports,
                               call->function_name, &function, &receiver,
                               &resolve_error)) {
      one->success = false;
      one->error_msg = resolve_error;
      results.push_back(std::move(one));
      continue;
    }

    std::vector<v8::Local<v8::Value>> argv;
    argv.reserve(call->args.size());
    bool args_ok = true;
    for (const auto& arg : call->args) {
      if (!arg) {
        one->success = false;
        one->error_msg = "Invalid native argument";
        args_ok = false;
        break;
      }
      std::string error_msg;
      v8::Local<v8::Value> converted;
      if (!WireValueToV8(context, arg->value, /*client_id=*/0, module_path,
                         &error_msg)
               .ToLocal(&converted)) {
        one->success = false;
        one->error_msg = error_msg;
        args_ok = false;
        break;
      }
      argv.push_back(converted);
    }
    if (!args_ok) {
      results.push_back(std::move(one));
      continue;
    }

    v8::Local<v8::Value> result;
    gin::TryCatch try_catch(addon_isolate_);
    if (!function
             ->Call(context, receiver, static_cast<int>(argv.size()),
                    argv.data())
             .ToLocal(&result)) {
      one->success = false;
      one->error_msg =
          DescribeCaughtException(call->function_name, &try_catch);
      results.push_back(std::move(one));
      continue;
    }

    if (result->IsPromise()) {
      addon_isolate_->PerformMicrotaskCheckpoint();
      v8::Local<v8::Promise> promise = result.As<v8::Promise>();
      if (promise->State() == v8::Promise::kPending) {
        one->success = false;
        one->error_msg = "Native export returned a pending Promise";
        results.push_back(std::move(one));
        continue;
      }
      if (promise->State() == v8::Promise::kRejected) {
        one->success = false;
        one->error_msg = "Native export Promise rejected";
        results.push_back(std::move(one));
        continue;
      }
      result = promise->Result();
    }

    std::string error_msg;
    std::optional<base::Value> converted =
        ConvertNativeValue(context, module_path, result, &error_msg, 0);
    if (!converted) {
      one->success = false;
      one->error_msg = error_msg;
      results.push_back(std::move(one));
      continue;
    }

    one->success = true;
    one->result = std::move(*converted);
    results.push_back(std::move(one));
  }

  std::move(callback).Run(std::move(results));
}

}  // namespace xenon
