// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <array>

#include "base/auto_reset.h"
#include "gin/arguments.h"
#include "gin/converter.h"
#include "net/base/filename_util.h"
#include "url/gurl.h"
#include "v8/include/v8.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.h"

namespace xenon::ipc {
namespace {

using ModuleInfo = XenonEsmModuleLoader::ModuleInfo;
using Format = XenonEsmModuleLoader::Format;
using Mode = XenonModuleResolver::Mode;

v8::Local<v8::String> String(v8::Isolate* isolate, std::string_view value) {
  return gin::StringToV8(isolate, value).As<v8::String>();
}

void ThrowModuleError(v8::Isolate* isolate, const std::string& message) {
  const auto separator = message.find(':');
  const auto code = separator == std::string::npos
                        ? "ERR_MODULE_NOT_FOUND"
                        : message.substr(0, separator);
  auto error = v8::Exception::Error(String(isolate, message));
  error.As<v8::Object>()
      ->CreateDataProperty(isolate->GetCurrentContext(),
                           String(isolate, "code"), String(isolate, code))
      .Check();
  isolate->ThrowException(error);
}

bool Property(v8::Local<v8::Context> context,
              v8::Local<v8::Object> object,
              const char* name,
              v8::Local<v8::Value>* result) {
  return object->Get(context, String(v8::Isolate::GetCurrent(), name))
      .ToLocal(result);
}

std::string FormatName(XenonModuleResolver::Format format,
                       const base::FilePath& filename) {
  if (filename.MatchesExtension(FILE_PATH_LITERAL(".node"))) {
    return "addon";
  }
  switch (format) {
    case XenonModuleResolver::Format::kCommonJs:
      return "commonjs";
    case XenonModuleResolver::Format::kModule:
      return "module";
    case XenonModuleResolver::Format::kJson:
      return "json";
    case XenonModuleResolver::Format::kBuiltin:
      return "builtin";
  }
}

bool HasDefaultImportFormat(const base::FilePath& filename) {
  const auto extension = filename.FinalExtension();
  return extension.empty() || extension == FILE_PATH_LITERAL(".js") ||
         extension == FILE_PATH_LITERAL(".mjs") ||
         extension == FILE_PATH_LITERAL(".cjs") ||
         extension == FILE_PATH_LITERAL(".json");
}

bool SetFormat(const std::string& name,
               ModuleInfo* module,
               std::string* error) {
  module->hook_format = name;
  if (name == "module") {
    module->format = Format::kModule;
  } else if (name == "commonjs" || name == "addon") {
    module->format = Format::kCommonJs;
  } else if (name == "json") {
    module->format = Format::kJson;
  } else if (name == "builtin") {
    module->format = Format::kBuiltin;
  } else {
    *error = "ERR_UNKNOWN_MODULE_FORMAT: Unsupported module format '" + name +
             "': " + module->url;
    return false;
  }
  return true;
}

base::FilePath ModuleFilename(const ModuleInfo& info) {
  return info.filename.empty() ? base::FilePath::FromUTF8Unsafe(info.url)
                               : info.filename;
}

std::string SourceKey(const ModuleInfo& info) {
  return info.hook_require ? info.CacheKey() + '\0' + "require"
                           : info.CacheKey();
}

v8::Local<v8::Object> Attributes(
    v8::Isolate* isolate,
    const std::map<std::string, std::string>& attributes) {
  auto object = v8::Object::New(isolate);
  object->SetPrototype(isolate->GetCurrentContext(), v8::Null(isolate)).Check();
  for (const auto& [name, value] : attributes) {
    object
        ->CreateDataProperty(isolate->GetCurrentContext(),
                             String(isolate, name), String(isolate, value))
        .Check();
  }
  return object;
}

bool ParseAttributes(v8::Local<v8::Context> context,
                     v8::Local<v8::Value> value,
                     std::map<std::string, std::string>* result,
                     std::string* error) {
  if (value->IsUndefined()) {
    return true;
  }
  if (!value->IsObject()) {
    *error =
        "ERR_INVALID_RETURN_PROPERTY_VALUE: importAttributes must be an object";
    return false;
  }
  auto* isolate = v8::Isolate::GetCurrent();
  auto object = value.As<v8::Object>();
  v8::Local<v8::Array> keys;
  if (!object->GetOwnPropertyNames(context).ToLocal(&keys)) {
    return false;
  }
  result->clear();
  for (uint32_t i = 0; i < keys->Length(); ++i) {
    v8::Local<v8::Value> key, item;
    std::string name, text;
    if (!keys->Get(context, i).ToLocal(&key) ||
        !object->Get(context, key).ToLocal(&item)) {
      return false;
    }
    if (!gin::ConvertFromV8(isolate, key, &name) ||
        !gin::ConvertFromV8(isolate, item, &text)) {
      *error =
          "ERR_INVALID_RETURN_PROPERTY_VALUE: importAttributes values must be "
          "strings";
      return false;
    }
    result->emplace(std::move(name), std::move(text));
  }
  return true;
}

v8::Local<v8::Object> HookContext(
    v8::Isolate* isolate,
    const std::map<std::string, std::string>& attributes,
    Mode mode) {
  auto context = isolate->GetCurrentContext();
  auto object = v8::Object::New(isolate);
  auto conditions = v8::Array::New(isolate, 4);
  const std::array values = {
      mode == Mode::kRequire ? "require" : "node",
      mode == Mode::kRequire ? "node" : "import",
      mode == Mode::kRequire ? "node-addons" : "module-sync",
      mode == Mode::kRequire ? "module-sync" : "node-addons"};
  for (uint32_t i = 0; i < std::size(values); ++i) {
    conditions->Set(context, i, String(isolate, values[i])).Check();
  }
  object->CreateDataProperty(context, String(isolate, "conditions"), conditions)
      .Check();
  object
      ->CreateDataProperty(
          context, String(isolate, "importAttributes"),
          mode == Mode::kRequire && attributes.empty()
              ? v8::Undefined(isolate).As<v8::Value>()
              : Attributes(isolate, attributes).As<v8::Value>())
      .Check();
  return object;
}

}  // namespace

void XenonIpcMainContainer::NativeModuleHooksChanged(gin::Arguments* args) {
  if (!args->GetNext(&has_resolve_hooks_) || !args->GetNext(&has_load_hooks_)) {
    args->ThrowTypeError("Module hook state requires two booleans");
    return;
  }
  module_resolution_cache_.clear();
}

void XenonIpcMainContainer::NativeDefaultModuleResolve(gin::Arguments* args) {
  std::string specifier;
  v8::Local<v8::Object> options;
  int mode_number;
  if (!args->GetNext(&specifier) || !args->GetNext(&options) ||
      !args->GetNext(&mode_number) || mode_number < 0 ||
      mode_number > static_cast<int>(Mode::kMetaResolve)) {
    args->ThrowTypeError(
        "Default module resolve requires specifier, context, and mode");
    return;
  }
  auto context = context_.Get(isolate_);
  v8::Local<v8::Value> parent_value, conditions_value;
  if (!Property(context, options, "parentURL", &parent_value) ||
      !Property(context, options, "conditions", &conditions_value)) {
    return;
  }
  std::string parent_url;
  base::FilePath parent = main_script_path_;
  if (!parent_value->IsUndefined()) {
    if (!gin::ConvertFromV8(isolate_, parent_value, &parent_url)) {
      args->ThrowTypeError("Module parentURL must be a string");
      return;
    }
    base::FilePath parsed_parent;
    if (net::FileURLToFilePath(GURL(parent_url), &parsed_parent)) {
      parent = parsed_parent;
    } else if (!IsBuiltinModuleRequest(specifier) &&
               !GURL(specifier).SchemeIsFile() &&
               !base::FilePath::FromUTF8Unsafe(specifier).IsAbsolute()) {
      ThrowModuleError(isolate_,
                       "ERR_UNSUPPORTED_ESM_URL_SCHEME: Default resolution "
                       "requires a file parent URL");
      return;
    }
  }
  std::vector<std::string> conditions;
  if (!gin::ConvertFromV8(isolate_, conditions_value, &conditions)) {
    ThrowModuleError(
        isolate_,
        "ERR_INVALID_ARG_VALUE: Module conditions must be an array of strings");
    return;
  }
  XenonModuleResolver::Resolution result;
  std::string error;
  // A synchronous require hook may redirect to a file URL even though the
  // public require API normally takes paths or package specifiers.
  if (static_cast<Mode>(mode_number) == Mode::kRequire &&
      GURL(specifier).SchemeIsFile()) {
    base::FilePath file;
    if (!net::FileURLToFilePath(GURL(specifier), &file)) {
      ThrowModuleError(isolate_,
                       "ERR_INVALID_MODULE_SPECIFIER: Invalid file URL");
      return;
    }
    specifier = file.AsUTF8Unsafe();
  }
  v8::TryCatch caught(isolate_);
  if (!module_resolver_->Resolve(specifier, parent,
                                 static_cast<Mode>(mode_number), &result,
                                 &error, &conditions)) {
    if (!caught.HasCaught()) {
      ThrowModuleError(isolate_, error);
    }
    if (caught.HasCaught()) {
      caught.ReThrow();
    }
    return;
  }
  auto object = v8::Object::New(isolate_);
  object
      ->CreateDataProperty(context, String(isolate_, "url"),
                           String(isolate_, result.url))
      .Check();
  object
      ->CreateDataProperty(
          context, String(isolate_, "format"),
          static_cast<Mode>(mode_number) != Mode::kRequire &&
                  !HasDefaultImportFormat(result.filename)
              ? v8::Undefined(isolate_).As<v8::Value>()
              : String(isolate_, FormatName(result.format, result.filename))
                    .As<v8::Value>())
      .Check();
  args->Return(object);
}

void XenonIpcMainContainer::NativeDefaultModuleLoad(gin::Arguments* args) {
  std::string url;
  v8::Local<v8::Object> options;
  bool require_mode;
  if (!args->GetNext(&url) || !args->GetNext(&options) ||
      !args->GetNext(&require_mode)) {
    args->ThrowTypeError("Default module load requires URL and context");
    return;
  }
  auto context = context_.Get(isolate_);
  v8::Local<v8::Value> requested_format;
  if (!Property(context, options, "format", &requested_format)) {
    return;
  }
  XenonModuleResolver::Resolution resolved;
  std::string error;
  {
    v8::TryCatch caught(isolate_);
    if (!module_resolver_->Resolve(url, main_script_path_, Mode::kImport,
                                   &resolved, &error)) {
      if (!caught.HasCaught()) {
        ThrowModuleError(isolate_, error);
      }
      if (caught.HasCaught()) {
        caught.ReThrow();
      }
      return;
    }
  }
  std::string format = FormatName(resolved.format, resolved.filename);
  if (!require_mode && requested_format->IsUndefined() &&
      !HasDefaultImportFormat(resolved.filename)) {
    ThrowModuleError(
        isolate_,
        "ERR_UNKNOWN_FILE_EXTENSION: Unsupported module extension: " + url);
    return;
  }
  if (!requested_format->IsUndefined() &&
      !gin::ConvertFromV8(isolate_, requested_format, &format)) {
    args->ThrowTypeError("Module format must be a string");
    return;
  }
  ModuleInfo info;
  info.url = resolved.url;
  if (!SetFormat(format, &info, &error)) {
    ThrowModuleError(isolate_, error);
    return;
  }
  v8::Local<v8::Value> attributes;
  if (!Property(context, options, "importAttributes", &attributes)) {
    return;
  }
  {
    v8::TryCatch caught(isolate_);
    if (!ParseAttributes(context, attributes, &info.import_attributes,
                         &error)) {
      if (!caught.HasCaught()) {
        ThrowModuleError(isolate_, error);
      }
      if (caught.HasCaught()) {
        caught.ReThrow();
      }
      return;
    }
  }
  std::string type;
  for (const auto& [name, value] : info.import_attributes) {
    if (name != "type" || value != "json") {
      ThrowModuleError(
          isolate_,
          "ERR_IMPORT_ATTRIBUTE_UNSUPPORTED: Unsupported import attribute");
      return;
    }
    type = value;
  }
  if (!require_mode && format == "json" && type.empty()) {
    ThrowModuleError(
        isolate_,
        "ERR_IMPORT_ATTRIBUTE_MISSING: JSON module requires type: json");
    return;
  }
  if (format != "json" && !type.empty()) {
    ThrowModuleError(
        isolate_,
        "ERR_IMPORT_ATTRIBUTE_TYPE_INCOMPATIBLE: Module is not JSON: " + url);
    return;
  }
  auto result = v8::Object::New(isolate_);
  result
      ->CreateDataProperty(context, String(isolate_, "format"),
                           String(isolate_, format))
      .Check();
  v8::Local<v8::Value> source = v8::Null(isolate_);
  if (format != "builtin" && format != "addon") {
    std::string text;
    if (!ReadModuleSourceFile(resolved.filename, &text)) {
      ThrowModuleError(isolate_, "ERR_MODULE_NOT_FOUND: Cannot read " + url);
      return;
    }
    source = String(isolate_, text);
  }
  result->CreateDataProperty(context, String(isolate_, "source"), source)
      .Check();
  args->Return(result);
}

bool XenonIpcMainContainer::ResolveWithModuleHooks(
    const std::string& specifier,
    const std::string& parent_url,
    Mode mode,
    const std::map<std::string, std::string>& attributes,
    ModuleInfo* result,
    std::string* error) {
  auto context = context_.Get(isolate_);
  auto options = HookContext(isolate_, attributes, mode);
  options
      ->CreateDataProperty(context, String(isolate_, "parentURL"),
                           String(isolate_, parent_url))
      .Check();
  v8::Local<v8::Value> function;
  if (!context->Global()
           ->Get(context, String(isolate_, "__xenonResolveWithHooks"))
           .ToLocal(&function) ||
      !function->IsFunction()) {
    *error = "ERR_INVALID_STATE: Synchronous module hooks are unavailable";
    return false;
  }
  v8::Local<v8::Value> argv[] = {
      String(isolate_, specifier), options,
      v8::Integer::New(isolate_, static_cast<int>(mode))};
  v8::Local<v8::Value> value;
  if (!function.As<v8::Function>()
           ->Call(context, v8::Undefined(isolate_), std::size(argv), argv)
           .ToLocal(&value)) {
    return false;
  }
  if (!value->IsObject()) {
    *error =
        "ERR_INVALID_RETURN_PROPERTY_VALUE: Resolve hook must return an object";
    return false;
  }
  v8::Local<v8::Value> url, format, returned_attributes;
  auto object = value.As<v8::Object>();
  if (!Property(context, object, "url", &url) ||
      !Property(context, object, "format", &format) ||
      !Property(context, object, "importAttributes", &returned_attributes)) {
    return false;
  }
  if (!gin::ConvertFromV8(isolate_, url, &result->url)) {
    *error =
        "ERR_INVALID_RETURN_PROPERTY_VALUE: Resolve hook url must be a string";
    return false;
  }
  const GURL parsed(result->url);
  if (!parsed.is_valid() && !IsBuiltinModuleRequest(result->url)) {
    *error =
        "ERR_INVALID_RETURN_PROPERTY_VALUE: Resolve hook must return an "
        "absolute URL";
    return false;
  }
  if (parsed.SchemeIsFile()) {
    if (!net::FileURLToFilePath(parsed, &result->filename)) {
      *error = "ERR_INVALID_MODULE_SPECIFIER: Invalid resolved file URL";
      return false;
    }
  }
  if (!format->IsUndefined() &&
      !gin::ConvertFromV8(isolate_, format, &result->hook_format)) {
    *error =
        "ERR_INVALID_RETURN_PROPERTY_VALUE: Resolve hook format must be a "
        "string";
    return false;
  }
  result->import_attributes = attributes;
  if (!ParseAttributes(context, returned_attributes, &result->import_attributes,
                       error)) {
    return false;
  }
  result->use_module_hooks = true;
  result->hook_require = mode == Mode::kRequire;
  // A load hook may supply or replace the format. Do not reject a custom
  // extension or format hint until the load chain has had that opportunity.
  if (!result->hook_format.empty()) {
    std::string ignored;
    SetFormat(result->hook_format, result, &ignored);
  }
  return true;
}

bool XenonIpcMainContainer::PrepareHookedModule(ModuleInfo* module,
                                                std::string* error) {
  if (!module->use_module_hooks) {
    return true;
  }
  if (const auto existing = hooked_module_sources_.find(SourceKey(*module));
      existing != hooked_module_sources_.end()) {
    const auto attributes = module->import_attributes;
    const bool require = module->hook_require;
    *module = existing->second.info;
    module->import_attributes = attributes;
    module->hook_require = require;
    return true;
  }
  auto context = context_.Get(isolate_);
  auto options =
      HookContext(isolate_, module->import_attributes,
                  module->hook_require ? Mode::kRequire : Mode::kImport);
  options
      ->CreateDataProperty(
          context, String(isolate_, "format"),
          module->hook_format.empty()
              ? v8::Undefined(isolate_).As<v8::Value>()
              : String(isolate_, module->hook_format).As<v8::Value>())
      .Check();
  v8::Local<v8::Value> function, value;
  if (!context->Global()
           ->Get(context, String(isolate_, "__xenonLoadWithHooks"))
           .ToLocal(&function) ||
      !function->IsFunction()) {
    *error = "ERR_INVALID_STATE: Synchronous module load hooks are unavailable";
    return false;
  }
  v8::Local<v8::Value> argv[] = {
      String(isolate_, module->url), options,
      v8::Boolean::New(isolate_, module->hook_require)};
  if (!function.As<v8::Function>()
           ->Call(context, v8::Undefined(isolate_), std::size(argv), argv)
           .ToLocal(&value)) {
    return false;
  }
  if (!value->IsObject()) {
    *error =
        "ERR_INVALID_RETURN_PROPERTY_VALUE: Load hook must return an object";
    return false;
  }
  v8::Local<v8::Value> format, source;
  if (!Property(context, value.As<v8::Object>(), "format", &format) ||
      !Property(context, value.As<v8::Object>(), "source", &source)) {
    return false;
  }
  std::string format_name = module->hook_format;
  if (!format->IsUndefined() &&
      !gin::ConvertFromV8(isolate_, format, &format_name)) {
    *error =
        "ERR_INVALID_RETURN_PROPERTY_VALUE: Load hook format must be a string";
    return false;
  }
  if (!SetFormat(format_name, module, error)) {
    return false;
  }
  HookedModuleSource loaded{*module, {}};
  if (format_name != "builtin" && format_name != "addon" &&
      !gin::ConvertFromV8(isolate_, source, &loaded.source)) {
    *error =
        "ERR_INVALID_RETURN_PROPERTY_VALUE: Load hook source must be text or "
        "binary source";
    return false;
  }
  hooked_module_sources_.emplace(SourceKey(*module), std::move(loaded));
  return true;
}

bool XenonIpcMainContainer::ReadEsmModuleSource(const ModuleInfo& module,
                                                std::string* source,
                                                std::string* error) {
  if (module.use_module_hooks) {
    const auto loaded = hooked_module_sources_.find(SourceKey(module));
    if (loaded != hooked_module_sources_.end()) {
      *source = loaded->second.source;
      return true;
    }
  }
  if (ReadModuleSourceFile(module.filename, source)) {
    return true;
  }
  *error = "ERR_MODULE_NOT_FOUND: Cannot read " + module.url;
  return false;
}

v8::MaybeLocal<v8::Value> XenonIpcMainContainer::LoadHookedModule(
    ModuleInfo module,
    std::string* error) {
  auto context = context_.Get(isolate_);
  const auto filename = ModuleFilename(module);
  const auto key = filename.AsUTF8Unsafe();
  if (auto cached = module_cache_.find(key); cached != module_cache_.end()) {
    return cached->second.Get(isolate_).As<v8::Object>()->Get(
        context, String(isolate_, "exports"));
  }
  if (!PrepareHookedModule(&module, error)) {
    return {};
  }
  if (module.format == Format::kBuiltin) {
    return BuiltinModuleValue(module.url, error);
  }
  if (module.hook_format == "addon") {
    if (module.filename.empty() ||
        !module.filename.MatchesExtension(FILE_PATH_LITERAL(".node"))) {
      *error =
          "ERR_DLOPEN_FAILED: Native addons require an application file path";
      return {};
    }
    XenonModuleResolver::Resolution resolved;
    if (!module_resolver_->Resolve(module.filename.AsUTF8Unsafe(),
                                   main_script_path_, Mode::kRequire, &resolved,
                                   error)) {
      return {};
    }
    return LoadResolvedCommonJsModule(resolved.filename, error);
  }
  if (module.format == Format::kModule) {
    base::AutoReset<bool> requiring(&requiring_esm_, true);
    return esm_loader_->Require(module, error);
  }
  const auto source = hooked_module_sources_.find(SourceKey(module));
  if (source == hooked_module_sources_.end()) {
    *error = "ERR_INVALID_STATE: Hooked module source is missing";
    return {};
  }
  if (module.format == Format::kJson) {
    v8::Local<v8::Value> value;
    if (!v8::JSON::Parse(context, String(isolate_, source->second.source))
             .ToLocal(&value)) {
      hooked_module_sources_.erase(SourceKey(module));
      return {};
    }
    auto record = v8::Object::New(isolate_);
    record->CreateDataProperty(context, String(isolate_, "exports"), value)
        .Check();
    module_cache_.emplace(key, v8::Global<v8::Value>(isolate_, record));
    return value;
  }
  v8::Local<v8::Value> value;
  if (!LoadCommonJsSource(filename, source->second.source, error)
           .ToLocal(&value)) {
    hooked_module_sources_.erase(SourceKey(module));
    return {};
  }
  return value;
}

}  // namespace xenon::ipc
