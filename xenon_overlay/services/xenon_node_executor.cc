// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/services/xenon_node_executor.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
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
#include "base/path_service.h"
#include "base/scoped_native_library.h"
#include "base/strings/string_split.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
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
#endif

#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
#include "uv.h"
#endif

namespace xenon {

namespace {

constexpr int kMaxValueConversionDepth = 32;
constexpr char kWireTypeKey[] = "__xenon_node_wire_type__";
constexpr char kWireValueKey[] = "value";
constexpr char kInvokePathPrefix[] = "$xenonInvokePath:";
constexpr base::TimeDelta kUvLoopPollInterval = base::Milliseconds(10);

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

bool ShouldSkipExportName(const std::string& name) {
  // Built-ins from Object/Function/Number prototypes that must never appear as
  // addon "methods" in the export tree.
  static constexpr const char* kSkip[] = {
      "constructor",
      "caller",
      "arguments",
      "prototype",
      "__proto__",
      "toString",
      "valueOf",
      "toLocaleString",
      "hasOwnProperty",
      "isPrototypeOf",
      "propertyIsEnumerable",
      "__defineGetter__",
      "__defineSetter__",
      "__lookupGetter__",
      "__lookupSetter__",
      "toFixed",
      "toExponential",
      "toPrecision",
      "toLocaleString",
      "call",
      "apply",
      "bind",
      "name",
      "length",
  };
  for (const char* skip : kSkip) {
    if (name == skip) {
      return true;
    }
  }
  return false;
}

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
        ShouldSkipExportName(key_str) ||
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
bool ConstructorHasInstanceMethods(v8::Isolate* isolate,
                                   v8::Local<v8::Context> context,
                                   v8::Local<v8::Function> constructor) {
  v8::Local<v8::Value> prototype_value;
  if (!constructor->Get(context, gin::StringToV8(isolate, "prototype"))
           .ToLocal(&prototype_value) ||
      !prototype_value->IsObject()) {
    return false;
  }
  v8::Local<v8::Object> prototype = prototype_value.As<v8::Object>();
  if (IsBuiltinPrototype(isolate, context, prototype)) {
    return false;
  }
  v8::Local<v8::Array> keys;
  if (!prototype
           ->GetOwnPropertyNames(context, v8::PropertyFilter::ALL_PROPERTIES,
                                 v8::KeyConversionMode::kConvertToString)
           .ToLocal(&keys)) {
    return false;
  }
  for (uint32_t i = 0; i < keys->Length(); ++i) {
    v8::Local<v8::Value> key;
    if (!keys->Get(context, i).ToLocal(&key)) {
      continue;
    }
    std::string key_str;
    if (gin::ConvertFromV8(isolate, key, &key_str) &&
        !ShouldSkipExportName(key_str)) {
      return true;
    }
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
  v8::Local<v8::Value> prototype_value;
  if (!function->Get(context, gin::StringToV8(isolate, "prototype"))
           .ToLocal(&prototype_value) ||
      !prototype_value->IsObject()) {
    return;
  }

  v8::Local<v8::Object> prototype = prototype_value.As<v8::Object>();
  if (IsBuiltinPrototype(isolate, context, prototype)) {
    return;
  }
  CollectOwnChildExports(isolate, context, prototype, child_depth, max_depth,
                         prototype_children);
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

bool ResolveExportValue(v8::Isolate* isolate,
                        v8::Local<v8::Context> context,
                        v8::Local<v8::Object> exports,
                        const std::string& export_path,
                        v8::Local<v8::Value>* value,
                        v8::Local<v8::Object>* receiver,
                        std::string* error_msg) {
  const std::vector<std::string> parts = base::SplitString(
      export_path, ".", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
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
  if (value->IsArrayBuffer()) {
    v8::Local<v8::ArrayBuffer> array_buffer = value.As<v8::ArrayBuffer>();
    return V8ArrayBufferToBaseValue(array_buffer, 0,
                                    array_buffer->ByteLength());
  }
  if (value->IsArrayBufferView()) {
    v8::Local<v8::ArrayBufferView> view = value.As<v8::ArrayBufferView>();
    return V8ArrayBufferToBaseValue(view->Buffer(), view->ByteOffset(),
                                    view->ByteLength());
  }
  if (value->IsFunction()) {
    *error_msg = "Native export returned a function, which cannot cross the "
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
      const base::Value::BlobStorage& blob = value.GetBlob();
      std::unique_ptr<v8::BackingStore> backing_store =
          v8::ArrayBuffer::NewBackingStore(isolate, blob.size());
      if (!blob.empty()) {
        // SAFETY: BackingStore size equals blob.size(); Data() is writable for
        // that many bytes.
        UNSAFE_BUFFERS(base::span(static_cast<uint8_t*>(backing_store->Data()),
                                  blob.size()))
            .copy_from(base::as_byte_span(blob));
      }
      return v8::ArrayBuffer::New(isolate, std::move(backing_store))
          .As<v8::Value>();
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

        const base::Value* payload = dict.Find(kWireValueKey);
        if (!payload) {
          *error_msg = "Tagged argument is missing its value";
          return v8::MaybeLocal<v8::Value>();
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

XenonNodeExecutor::AddonModule::AddonModule() = default;
XenonNodeExecutor::AddonModule::~AddonModule() {
  exports.Reset();
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

// static
void XenonNodeExecutor::FirstWeakCallback(
    const v8::WeakCallbackInfo<AddonCallback>& data) {
  data.GetParameter()->function.Reset();
  data.SetSecondPassCallback(SecondWeakCallback);
}

// static
void XenonNodeExecutor::SecondWeakCallback(
    const v8::WeakCallbackInfo<AddonCallback>& data) {
  AddonCallback* callback = data.GetParameter();
  if (callback->executor) {
    callback->executor->OnNativeCallbackCollected(callback);
  }
}

v8::Local<v8::Function> XenonNodeExecutor::GetOrCreateNativeCallback(
    v8::Local<v8::Context> context,
    int32_t client_id,
    int32_t callback_id) {
  const auto key = std::make_pair(client_id, callback_id);
  auto existing = addon_callbacks_.find(key);
  if (existing != addon_callbacks_.end() &&
      !existing->second->function.IsEmpty()) {
    return existing->second->function.Get(addon_isolate_);
  }

  v8::Local<v8::Function> function =
      gin::CreateFunctionTemplate(
          addon_isolate_,
          base::BindRepeating(&XenonNodeExecutor::OnNativeCallback,
                              weak_factory_.GetWeakPtr(), client_id,
                              callback_id))
          ->GetFunction(context)
          .ToLocalChecked();

  auto callback = std::make_unique<AddonCallback>();
  callback->client_id = client_id;
  callback->callback_id = callback_id;
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
                                         gin::Arguments* arguments) {
  std::vector<base::Value> converted_args;
  v8::Local<v8::Value> value;
  while (arguments->GetNext(&value)) {
    std::string error_msg;
    std::optional<base::Value> converted = V8ValueToBaseValue(
        arguments->isolate(), arguments->GetHolderCreationContext(), value,
        &error_msg, 0);
    if (!converted) {
      LOG(ERROR) << "Failed to serialize Node callback argument: " << error_msg;
      return;
    }
    converted_args.push_back(std::move(*converted));
  }

  if (callback_handler_) {
    callback_handler_.Run(client_id, callback_id, std::move(converted_args));
  }
}

void XenonNodeExecutor::OnNativeCallbackCollected(AddonCallback* callback) {
  const auto key = std::make_pair(callback->client_id, callback->callback_id);
  auto it = addon_callbacks_.find(key);
  if (it == addon_callbacks_.end() || it->second.get() != callback) {
    return;
  }

  const int32_t client_id = callback->client_id;
  const int32_t callback_id = callback->callback_id;
  addon_callbacks_.erase(it);
  if (callback_released_handler_) {
    callback_released_handler_.Run(client_id, callback_id);
  }
}

void XenonNodeExecutor::InvokeResolvedFunction(
    v8::Local<v8::Context> context,
    const std::string& function_name,
    v8::Local<v8::Function> function,
    v8::Local<v8::Value> receiver,
    int32_t client_id,
    const std::vector<mojom::NodeInvokeArgPtr>& args,
    InvokeFunctionCallback callback) {
  std::vector<v8::Local<v8::Value>> argv;
  argv.reserve(args.size());
  for (const auto& arg : args) {
    if (!arg) {
      std::move(callback).Run(false, base::Value(), {},
                              "Invalid native argument");
      return;
    }
    if (arg->is_callback) {
      argv.push_back(
          GetOrCreateNativeCallback(context, client_id, arg->callback_id));
      continue;
    }

    std::string error_msg;
    v8::Local<v8::Value> converted;
    if (!BaseValueToV8Value(addon_isolate_, context, arg->value, &error_msg, 0)
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

  if (result->IsPromise()) {
    addon_isolate_->PerformMicrotaskCheckpoint();
    v8::Local<v8::Promise> promise = result.As<v8::Promise>();
    if (promise->State() == v8::Promise::kPending) {
      std::move(callback).Run(
          false, base::Value(), {},
          "Native export returned a pending Promise; only settled Promise "
          "results are supported");
      return;
    }
    if (promise->State() == v8::Promise::kRejected) {
      std::move(callback).Run(false, base::Value(), {},
                              "Native export Promise rejected");
      return;
    }
    result = promise->Result();
  }

  std::string error_msg;
  std::optional<base::Value> converted =
      V8ValueToBaseValue(addon_isolate_, context, result, &error_msg, 0);
  if (!converted) {
    std::move(callback).Run(false, base::Value(), {}, error_msg);
    return;
  }
  std::move(callback).Run(true, std::move(*converted), {}, "");
}

XenonNodeExecutor::~XenonNodeExecutor() {
  uv_loop_timer_.Stop();
  if (addon_isolate_) {
    if (!addon_context_.IsEmpty()) {
      AutoV8Scope v8_scope(addon_isolate_, addon_context_);
      addon_callbacks_.clear();
      addon_instances_.clear();
      addon_modules_.clear();
      addon_context_.Reset();
    } else {
      v8::Locker locker(addon_isolate_);
      v8::Isolate::Scope isolate_scope(addon_isolate_);
      addon_callbacks_.clear();
      addon_instances_.clear();
      addon_modules_.clear();
    }
    addon_isolate_ = nullptr;
  }
  addon_isolate_holder_.reset();
}

void XenonNodeExecutor::EnsureUvLoopPolling() {
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (uv_loop_timer_.IsRunning()) {
    return;
  }
  for (const auto& [path, module] : addon_modules_) {
    if (module.loaded_addon && module.loaded_addon->env &&
        module.loaded_addon->env->uv_loop &&
        module.loaded_addon->env->uv_run_function) {
      uv_loop_timer_.Start(FROM_HERE, kUvLoopPollInterval, this,
                           &XenonNodeExecutor::PumpUvLoops);
      return;
    }
  }
#endif
}

void XenonNodeExecutor::PumpUvLoops() {
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    uv_loop_timer_.Stop();
    return;
  }

  struct UvLoopEntry {
    RAW_PTR_EXCLUSION uv_loop_t* loop;
    XenonUvRunFunction run;
    XenonUvLoopAliveFunction alive;
  };
  std::vector<UvLoopEntry> loops;
  std::set<uv_loop_t*> seen;
  for (const auto& [path, module] : addon_modules_) {
    if (!module.loaded_addon || !module.loaded_addon->env) {
      continue;
    }
    napi_env env = module.loaded_addon->env.get();
    if (!env->uv_loop || !env->uv_run_function ||
        !seen.insert(env->uv_loop).second) {
      continue;
    }
    loops.push_back(
        {env->uv_loop, env->uv_run_function, env->uv_loop_alive_function});
  }

  if (loops.empty()) {
    uv_loop_timer_.Stop();
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  bool any_alive = false;
  for (const auto& entry : loops) {
    entry.run(entry.loop, UV_RUN_NOWAIT);
    any_alive |= !entry.alive || entry.alive(entry.loop) != 0;
  }
  addon_isolate_->PerformMicrotaskCheckpoint();
  if (!any_alive) {
    uv_loop_timer_.Stop();
  }
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
        gin::IsolateHolder::kSingleThread,
        gin::IsolateHolder::IsolateType::kUtility);
    addon_isolate_ = addon_isolate_holder_->isolate();
  }
  return addon_isolate_ != nullptr;
}

void XenonNodeExecutor::LoadAddon(const std::string& path,
                                  LoadAddonCallback callback) {
  base::FilePath addon_path = ResolveAddonPath(path);

  if (!EnsureIsolate()) {
    LOG(ERROR) << "[XenonNodeExecutor] Failed to create V8 isolate";
    std::move(callback).Run(false, "Failed to create V8 isolate in utility", {});
    return;
  }

  auto cached_addon = addon_modules_.find(addon_path);
  if (cached_addon != addon_modules_.end() &&
      !cached_addon->second.exports.IsEmpty()) {
    std::move(callback).Run(
        true, "", CloneExportTree(cached_addon->second.export_tree));
    return;
  }

  const bool allow_external_addons =
      base::CommandLine::ForCurrentProcess()->HasSwitch(
          napi_switches::kAllowExternalNodeAddons);
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&PrepareAddon, addon_path, allow_external_addons),
      base::BindOnce(
          [](base::WeakPtr<XenonNodeExecutor> self, base::FilePath addon_path,
             LoadAddonCallback callback, PreparedAddon prepared) {
            if (!self) {
              std::move(callback).Run(
                  false, "Node executor was destroyed during load", {});
              return;
            }
            if (!prepared.error.empty()) {
              std::move(callback).Run(false, prepared.error, {});
              return;
            }

            auto cached = self->addon_modules_.find(addon_path);
            if (cached != self->addon_modules_.end() &&
                !cached->second.exports.IsEmpty()) {
              std::move(callback).Run(
                  true, "", CloneExportTree(cached->second.export_tree));
              return;
            }

            AutoV8Scope v8_scope(self->addon_isolate_, self->addon_context_);
            v8::Local<v8::Context> context = v8_scope.context;
            auto loaded_addon = std::make_unique<LoadedNodeAddon>();
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
              if (instance->second.module_path == addon_path) {
                instance = self->addon_instances_.erase(instance);
              } else {
                ++instance;
              }
            }

            v8::Local<v8::Object> exports = exports_value.As<v8::Object>();
            std::vector<mojom::NodeExportInfoPtr> export_tree = BuildExportTree(
                self->addon_isolate_, context, exports, kMaxExportInspectDepth);

            AddonModule module;
            module.exports.Reset(self->addon_isolate_, exports_value);
            module.export_tree = CloneExportTree(export_tree);
            module.loaded_addon = std::move(loaded_addon);
            self->addon_modules_.insert_or_assign(addon_path,
                                                  std::move(module));
            self->EnsureUvLoopPolling();
            std::move(callback).Run(true, "", std::move(export_tree));
          },
          weak_factory_.GetWeakPtr(), addon_path, std::move(callback)));
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
  base::FilePath addon_path = ResolveAddonPath(module_path);
  auto cached_addon = addon_modules_.find(addon_path);
  if (cached_addon == addon_modules_.end() ||
      cached_addon->second.exports.IsEmpty()) {
    std::move(callback).Run(
        false, "Node addon has not been loaded: " + module_path, nullptr);
    return;
  }

  v8::Local<v8::Value> exports_value =
      cached_addon->second.exports.Get(addon_isolate_);
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
    for (auto& cached : cached_addon->second.export_tree) {
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
    ConstructExportCallback callback) {
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, 0, "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;
  base::FilePath addon_path = ResolveAddonPath(module_path);
  auto cached_addon = addon_modules_.find(addon_path);
  if (cached_addon == addon_modules_.end() ||
      cached_addon->second.exports.IsEmpty()) {
    std::move(callback).Run(false, 0,
                            "Node addon has not been loaded: " + module_path);
    return;
  }

  v8::Local<v8::Value> exports_value =
      cached_addon->second.exports.Get(addon_isolate_);
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

  std::vector<v8::Local<v8::Value>> argv;
  argv.reserve(args.size());
  for (const auto& arg : args) {
    if (!arg) {
      std::move(callback).Run(false, 0, "Invalid constructor argument");
      return;
    }
    if (arg->is_callback) {
      argv.push_back(
          GetOrCreateNativeCallback(context, client_id, arg->callback_id));
      continue;
    }
    std::string error_msg;
    v8::Local<v8::Value> converted;
    if (!BaseValueToV8Value(addon_isolate_, context, arg->value, &error_msg, 0)
             .ToLocal(&converted)) {
      std::move(callback).Run(false, 0, error_msg);
      return;
    }
    argv.push_back(converted);
  }

  gin::TryCatch try_catch(addon_isolate_);
  v8::Local<v8::Object> instance;
  if (!constructor
           ->NewInstance(context, static_cast<int>(argv.size()), argv.data())
           .ToLocal(&instance)) {
    std::move(callback).Run(false, 0,
                            DescribeCaughtException(export_path, &try_catch));
    return;
  }
  EnsureUvLoopPolling();

  const int32_t instance_id = next_instance_id_++;
  AddonInstance stored;
  stored.module_path = addon_path;
  stored.object.Reset(addon_isolate_, instance);
  addon_instances_.insert_or_assign(instance_id, std::move(stored));
  std::move(callback).Run(true, instance_id, "");
}

void XenonNodeExecutor::InvokeInstance(
    const std::string& module_path,
    int32_t instance_id,
    const std::string& method_name,
    int32_t client_id,
    std::vector<mojom::NodeInvokeArgPtr> args,
    InvokeFunctionCallback callback) {
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
      v8::Local<v8::Value> getter_value;
      if (!receiver->Get(context, gin::StringToV8(addon_isolate_, path[i]))
               .ToLocal(&getter_value) ||
          !getter_value->IsFunction()) {
        std::move(callback).Run(
            false, base::Value(), {},
            "Instance method path segment is missing: " + path[i]);
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

      if (nested_value->IsPromise()) {
        addon_isolate_->PerformMicrotaskCheckpoint();
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

  InvokeResolvedFunction(context, resolved_method_name, function, receiver,
                         client_id, args, std::move(callback));
}

void XenonNodeExecutor::GetInstanceProperty(const std::string& module_path,
                                            int32_t instance_id,
                                            const std::string& property_name,
                                            GetPropertyCallback callback) {
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
      V8ValueToBaseValue(addon_isolate_, context, value, &error_msg, 0);
  if (!converted) {
    std::move(callback).Run(false, base::Value(), error_msg);
    return;
  }
  std::move(callback).Run(true, std::move(*converted), "");
}

void XenonNodeExecutor::SetInstanceProperty(const std::string& module_path,
                                            int32_t instance_id,
                                            const std::string& property_name,
                                            base::Value value,
                                            SetPropertyCallback callback) {
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
  if (it->second.module_path != ResolveAddonPath(module_path)) {
    std::move(callback).Run(
        false, "Instance does not belong to module: " + module_path);
    return;
  }

  std::string error_msg;
  v8::Local<v8::Value> converted;
  if (!BaseValueToV8Value(addon_isolate_, context, value, &error_msg, 0)
           .ToLocal(&converted)) {
    std::move(callback).Run(false, error_msg);
    return;
  }
  if (!it->second.object.Get(addon_isolate_)
           ->Set(context, gin::StringToV8(addon_isolate_, property_name),
                 converted)
           .FromMaybe(false)) {
    std::move(callback).Run(
        false, "Failed to write instance property: " + property_name);
    return;
  }
  std::move(callback).Run(true, "");
}

void XenonNodeExecutor::ReleaseInstance(const std::string& module_path,
                                        int32_t instance_id) {
  auto it = addon_instances_.find(instance_id);
  if (it == addon_instances_.end() ||
      it->second.module_path != ResolveAddonPath(module_path)) {
    return;
  }
  addon_instances_.erase(it);
}

void XenonNodeExecutor::InvokeFunction(
    const std::string& module_path,
    const std::string& function_name,
    int32_t client_id,
    std::vector<mojom::NodeInvokeArgPtr> args,
    InvokeFunctionCallback callback) {
  if (!addon_isolate_ || addon_context_.IsEmpty()) {
    std::move(callback).Run(false, base::Value(), {},
                            "No Node addon has been loaded");
    return;
  }

  AutoV8Scope v8_scope(addon_isolate_, addon_context_);
  v8::Local<v8::Context> context = v8_scope.context;

  base::FilePath addon_path = ResolveAddonPath(module_path);
  auto cached_addon = addon_modules_.find(addon_path);
  if (cached_addon == addon_modules_.end() ||
      cached_addon->second.exports.IsEmpty()) {
    std::move(callback).Run(false, base::Value(), {},
                            "Node addon has not been loaded: " + module_path);
    return;
  }

  v8::Local<v8::Value> exports_value =
      cached_addon->second.exports.Get(addon_isolate_);
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

  InvokeResolvedFunction(context, function_name, function, receiver, client_id,
                         args, std::move(callback));
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
  const base::FilePath addon_path = ResolveAddonPath(module_path);
  auto module = addon_modules_.find(addon_path);
  if (module == addon_modules_.end() || module->second.exports.IsEmpty()) {
    std::move(callback).Run(false, base::Value(),
                            "Node addon has not been loaded: " + module_path);
    return;
  }

  v8::Local<v8::Value> exports_value =
      module->second.exports.Get(addon_isolate_);
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
      V8ValueToBaseValue(addon_isolate_, context, value, &error_msg, 0);
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
  const base::FilePath addon_path = ResolveAddonPath(module_path);
  auto module = addon_modules_.find(addon_path);
  if (module == addon_modules_.end() || module->second.exports.IsEmpty()) {
    std::move(callback).Run(false,
                            "Node addon has not been loaded: " + module_path);
    return;
  }

  v8::Local<v8::Value> exports_value =
      module->second.exports.Get(addon_isolate_);
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
  if (!BaseValueToV8Value(addon_isolate_, context, value, &error_msg, 0)
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
  base::FilePath addon_path = ResolveAddonPath(module_path);
  auto cached_addon = addon_modules_.find(addon_path);
  if (cached_addon == addon_modules_.end() ||
      cached_addon->second.exports.IsEmpty()) {
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
      cached_addon->second.exports.Get(addon_isolate_);
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
      std::string error_msg;
      v8::Local<v8::Value> converted;
      if (!BaseValueToV8Value(addon_isolate_, context, arg->value, &error_msg, 0)
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
        V8ValueToBaseValue(addon_isolate_, context, result, &error_msg, 0);
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
