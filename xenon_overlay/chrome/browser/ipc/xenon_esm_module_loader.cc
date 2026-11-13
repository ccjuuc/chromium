// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_esm_module_loader.h"

#include <set>
#include <tuple>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "gin/converter.h"
#include "v8/include/v8-container.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-function.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-message.h"
#include "v8/include/v8-object.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-promise.h"
#include "v8/include/v8-script.h"

namespace xenon::ipc {
namespace {

struct Loaders {
  base::Lock lock;
  std::map<v8::Isolate*, XenonEsmModuleLoader*> instances;
};

Loaders& GetLoaders() {
  static base::NoDestructor<Loaders> loaders;
  return *loaders;
}

std::string Utf8(v8::Isolate* isolate, v8::Local<v8::Value> value) {
  v8::String::Utf8Value text(isolate, value);
  return *text ? std::string(*text, text.length()) : std::string();
}

v8::Local<v8::String> String(v8::Isolate* isolate, const std::string& value) {
  return gin::StringToV8(isolate, value).As<v8::String>();
}

}  // namespace

struct XenonEsmModuleLoader::ModuleRecord {
  ModuleInfo info;
  v8::Global<v8::Module> module;
  v8::Global<v8::Module> require_facade;
  v8::Global<v8::Promise> evaluation;
  v8::Global<v8::Value> value;
  std::vector<std::string> export_names;
};

XenonEsmModuleLoader::XenonEsmModuleLoader(
    v8::Isolate* isolate,
    v8::Local<v8::Context> context,
    ResolveCallback resolve,
    ReadSourceCallback read_source,
    LoadValueCallback load_value,
    GetExportNamesCallback get_export_names)
    : isolate_(isolate),
      context_(isolate, context),
      resolve_(std::move(resolve)),
      read_source_(std::move(read_source)),
      load_value_(std::move(load_value)),
      get_export_names_(std::move(get_export_names)) {
  auto& loaders = GetLoaders();
  {
    base::AutoLock lock(loaders.lock);
    CHECK(loaders.instances.emplace(isolate, this).second);
  }
  isolate_->SetHostImportModuleDynamicallyCallback(&ImportDynamically);
  isolate_->SetHostImportModuleWithPhaseDynamicallyCallback(
      [](v8::Local<v8::Context> context, v8::Local<v8::Data> options,
         v8::Local<v8::Value> resource_name, v8::Local<v8::String> specifier,
         v8::ModuleImportPhase phase,
         v8::Local<v8::FixedArray> attributes) -> v8::MaybeLocal<v8::Promise> {
        if (phase == v8::ModuleImportPhase::kEvaluation) {
          return ImportDynamically(context, options, resource_name, specifier,
                                   attributes);
        }
        auto* isolate = v8::Isolate::GetCurrent();
        v8::Local<v8::Promise::Resolver> resolver;
        if (!v8::Promise::Resolver::New(context).ToLocal(&resolver)) {
          return {};
        }
        auto exception = v8::Exception::Error(
            String(isolate, "Source-phase module imports are not supported"));
        if (exception.As<v8::Object>()
                ->CreateDataProperty(context, String(isolate, "code"),
                                     String(isolate, "ERR_NOT_SUPPORTED"))
                .IsNothing() ||
            resolver->Reject(context, exception).IsNothing()) {
          return {};
        }
        return resolver->GetPromise();
      });
  isolate_->SetHostInitializeImportMetaObjectCallback(&InitializeImportMeta);
}

XenonEsmModuleLoader::~XenonEsmModuleLoader() {
  isolate_->SetHostImportModuleDynamicallyCallback(nullptr);
  isolate_->SetHostImportModuleWithPhaseDynamicallyCallback(nullptr);
  isolate_->SetHostInitializeImportMetaObjectCallback(nullptr);
  auto& loaders = GetLoaders();
  base::AutoLock lock(loaders.lock);
  loaders.instances.erase(isolate_);
}

XenonEsmModuleLoader* XenonEsmModuleLoader::ForIsolate(v8::Isolate* isolate) {
  auto& loaders = GetLoaders();
  base::AutoLock lock(loaders.lock);
  const auto found = loaders.instances.find(isolate);
  return found == loaders.instances.end() ? nullptr : found->second;
}

void XenonEsmModuleLoader::ThrowFailure(const std::string& error) {
  const std::string message =
      error.empty() ? "Unable to load ES module" : error;
  const std::string code = message.starts_with("ERR_")
                               ? message.substr(0, message.find(':'))
                               : "ERR_MODULE_NOT_FOUND";
  auto context = context_.Get(isolate_);
  auto exception = v8::Exception::Error(String(isolate_, message));
  if (exception.As<v8::Object>()
          ->CreateDataProperty(context, String(isolate_, "code"),
                               String(isolate_, code))
          .IsNothing()) {
    return;
  }
  isolate_->ThrowException(exception);
}

bool XenonEsmModuleLoader::Resolve(const std::string& specifier,
                                   const std::string& referrer,
                                   ResolutionMode mode,
                                   ModuleInfo* result,
                                   std::string* error,
                                   v8::Local<v8::FixedArray> attributes,
                                   bool dynamic_attributes) {
  std::map<std::string, std::string> parsed_attributes;
  const int stride = dynamic_attributes ? 2 : 3;
  const int count = attributes.IsEmpty() ? 0 : attributes->Length();
  if (count % stride != 0) {
    *error = "ERR_IMPORT_ATTRIBUTE_UNSUPPORTED: Invalid import attributes";
    ThrowFailure(*error);
    return false;
  }
  for (int i = 0; i < count; i += stride) {
    auto key = attributes->Get(i).As<v8::Value>();
    auto value = attributes->Get(i + 1).As<v8::Value>();
    if (!key->IsString() || !value->IsString() ||
        !parsed_attributes.emplace(Utf8(isolate_, key), Utf8(isolate_, value))
             .second) {
      *error = "ERR_IMPORT_ATTRIBUTE_UNSUPPORTED: Invalid import attributes";
      ThrowFailure(*error);
      return false;
    }
  }
  base::AutoReset<std::map<std::string, std::string>> attributes_scope(
      &current_import_attributes_, std::move(parsed_attributes));
  v8::TryCatch caught(isolate_);
  if (resolve_.Run(specifier, referrer, mode, result, error)) {
    if (!result->use_module_hooks) {
      result->import_attributes = current_import_attributes_;
    }
    return true;
  }
  if (!caught.HasCaught()) {
    ThrowFailure(*error);
  }
  if (caught.HasCaught()) {
    caught.ReThrow();
  }
  return false;
}

void XenonEsmModuleLoader::SetPrepareModuleCallback(
    PrepareModuleCallback callback) {
  prepare_module_ = std::move(callback);
}

bool XenonEsmModuleLoader::CheckAttributes(const ModuleInfo& info,
                                           v8::Local<v8::FixedArray> attributes,
                                           bool dynamic,
                                           std::string* error) {
  std::string type;
  if (info.use_module_hooks) {
    // The default load step validates its input attributes. A synchronous
    // hook may replace them before nextLoad, or handle a custom type itself.
    return true;
  }
  const int stride = dynamic ? 2 : 3;
  const int length = attributes.IsEmpty() ? 0 : attributes->Length();
  if (length % stride != 0) {
    *error = "ERR_IMPORT_ATTRIBUTE_UNSUPPORTED: Invalid import attributes";
  } else {
    for (int i = 0; i < length; i += stride) {
      const auto key = attributes->Get(i).As<v8::Value>();
      const auto value = attributes->Get(i + 1).As<v8::Value>();
      if (!key->IsString() || !value->IsString() ||
          Utf8(isolate_, key) != "type" || !type.empty()) {
        *error =
            "ERR_IMPORT_ATTRIBUTE_UNSUPPORTED: Unsupported import attribute";
        break;
      }
      type = Utf8(isolate_, value);
      if (type != "json") {
        *error = "ERR_IMPORT_ATTRIBUTE_UNSUPPORTED: Unsupported module type: " +
                 type;
        break;
      }
    }
  }
  if (error->empty() && info.format == Format::kJson && type != "json") {
    *error = "ERR_IMPORT_ATTRIBUTE_MISSING: JSON module requires type: json";
  }
  if (error->empty() && info.format != Format::kJson && !type.empty()) {
    *error = "ERR_IMPORT_ATTRIBUTE_TYPE_INCOMPATIBLE: Module is not JSON: " +
             info.url;
  }
  if (!error->empty()) {
    ThrowFailure(*error);
    return false;
  }
  return true;
}

XenonEsmModuleLoader::ModuleRecord* XenonEsmModuleLoader::FindRecord(
    v8::Local<v8::Module> module) {
  for (const auto& [url, record] : modules_) {
    if (record->module.Get(isolate_) == module ||
        (!record->require_facade.IsEmpty() &&
         record->require_facade.Get(isolate_) == module)) {
      return record.get();
    }
  }
  return nullptr;
}

XenonEsmModuleLoader::ModuleRecord* XenonEsmModuleLoader::Load(
    const ModuleInfo& incoming,
    std::string* error) {
  auto existing = modules_.find(incoming.CacheKey());
  if (existing != modules_.end()) {
    return existing->second.get();
  }
  ModuleInfo info = incoming;
  if (prepare_module_ && info.use_module_hooks) {
    v8::TryCatch caught(isolate_);
    if (!prepare_module_.Run(&info, error)) {
      if (!caught.HasCaught()) {
        ThrowFailure(*error);
      }
      if (caught.HasCaught()) {
        caught.ReThrow();
      }
      return nullptr;
    }
  }
  auto context = context_.Get(isolate_);
  auto record = std::make_unique<ModuleRecord>();
  record->info = info;
  v8::Local<v8::Module> module;
  if (info.format == Format::kModule) {
    std::string source;
    v8::TryCatch caught(isolate_);
    if (!read_source_.Run(info, &source, error)) {
      if (!caught.HasCaught()) {
        ThrowFailure(*error);
      }
      if (caught.HasCaught()) {
        caught.ReThrow();
      }
      return nullptr;
    }
    v8::ScriptOrigin origin(String(isolate_, info.url), 0, 0, false, -1, {},
                            false, false, true);
    v8::ScriptCompiler::Source input(String(isolate_, source), origin);
    if (!v8::ScriptCompiler::CompileModule(isolate_, &input).ToLocal(&module)) {
      *error = "Unable to compile ES module: " + info.url;
      if (caught.HasCaught()) {
        caught.ReThrow();
      }
      return nullptr;
    }
  } else {
    std::set<std::string> names{"default"};
    if (info.format == Format::kCommonJs) {
      names.insert("module.exports");
      if (get_export_names_) {
        std::vector<std::string> detected;
        v8::TryCatch caught(isolate_);
        if (!get_export_names_.Run(info, &detected, error)) {
          if (!caught.HasCaught()) {
            ThrowFailure(*error);
          }
          if (caught.HasCaught()) {
            caught.ReThrow();
          }
          return nullptr;
        }
        names.insert(detected.begin(), detected.end());
      }
    } else if (info.format == Format::kBuiltin) {
      // Builtin facades already exist before user code. Reading their keys is
      // safe here; arbitrary CommonJS modules are not executed while linking.
      v8::Local<v8::Value> value;
      v8::TryCatch caught(isolate_);
      if (!load_value_.Run(info, error).ToLocal(&value)) {
        if (!caught.HasCaught()) {
          ThrowFailure(*error);
        }
        if (caught.HasCaught()) {
          caught.ReThrow();
        }
        return nullptr;
      }
      record->value.Reset(isolate_, value);
      if (value->IsObject()) {
        v8::Local<v8::Array> keys;
        if (!value.As<v8::Object>()
                 ->GetOwnPropertyNames(
                     context,
                     static_cast<v8::PropertyFilter>(v8::ONLY_ENUMERABLE |
                                                     v8::SKIP_SYMBOLS),
                     v8::KeyConversionMode::kConvertToString)
                 .ToLocal(&keys)) {
          if (caught.HasCaught()) {
            caught.ReThrow();
          }
          return nullptr;
        }
        for (uint32_t index = 0; index < keys->Length(); ++index) {
          v8::Local<v8::Value> key;
          if (!keys->Get(context, index).ToLocal(&key)) {
            if (caught.HasCaught()) {
              caught.ReThrow();
            }
            return nullptr;
          }
          names.insert(Utf8(isolate_, key));
        }
      }
    }
    record->export_names.assign(names.begin(), names.end());
    std::vector<v8::Local<v8::String>> exports;
    for (const auto& name : record->export_names) {
      exports.push_back(String(isolate_, name));
    }
    module = v8::Module::CreateSyntheticModule(
        isolate_, String(isolate_, info.url), exports, &EvaluateSynthetic);
  }
  record->module.Reset(isolate_, module);
  auto* result = record.get();
  modules_.emplace(info.CacheKey(), std::move(record));
  return result;
}

v8::MaybeLocal<v8::Module> XenonEsmModuleLoader::ResolveModule(
    v8::Local<v8::Context> context,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> attributes,
    v8::Local<v8::Module> referrer) {
  auto* loader = ForIsolate(v8::Isolate::GetCurrent());
  if (!loader) {
    return {};
  }
  auto* parent = loader->FindRecord(referrer);
  if (!parent) {
    loader->ThrowFailure("ERR_INVALID_STATE: Unknown ES module referrer");
    return {};
  }
  if (!parent->require_facade.IsEmpty() &&
      parent->require_facade.Get(loader->isolate_) == referrer) {
    // The facade contains only these fixed re-exports; application specifiers
    // still go through the host's normal resolution and access checks.
    CHECK(Utf8(loader->isolate_, specifier) == "xenon:original");
    return parent->module.Get(loader->isolate_);
  }
  ModuleInfo info;
  std::string error;
  if (!loader->Resolve(Utf8(loader->isolate_, specifier), parent->info.url,
                       ResolutionMode::kImport, &info, &error, attributes,
                       false)) {
    return {};
  }
  auto* record = loader->Load(info, &error);
  if (record) {
    info.format = record->info.format;
  }
  if (record && !loader->CheckAttributes(info, attributes, false, &error)) {
    return {};
  }
  return record
             ? v8::MaybeLocal<v8::Module>(record->module.Get(loader->isolate_))
             : v8::MaybeLocal<v8::Module>();
}

bool XenonEsmModuleLoader::Instantiate(ModuleRecord* record,
                                       std::string* error) {
  auto module = record->module.Get(isolate_);
  if (module->GetStatus() == v8::Module::kUninstantiated) {
    if (module
            ->InstantiateModule(
                context_.Get(isolate_), &ResolveModule,
                [](v8::Local<v8::Context> context,
                   v8::Local<v8::String> specifier,
                   v8::Local<v8::FixedArray> attributes,
                   v8::Local<v8::Module> referrer)
                    -> v8::MaybeLocal<v8::Object> {
                  auto* loader = ForIsolate(v8::Isolate::GetCurrent());
                  if (loader) {
                    loader->ThrowFailure(
                        "ERR_NOT_SUPPORTED: Source-phase module imports "
                        "are not supported");
                  }
                  return {};
                })
            .IsNothing()) {
      *error = "Unable to instantiate ES module: " + record->info.url;
      return false;
    }
  }
  if (module->GetStatus() == v8::Module::kErrored) {
    *error = "ES module evaluation failed: " + record->info.url;
    isolate_->ThrowException(module->GetException());
    return false;
  }
  return true;
}

v8::MaybeLocal<v8::Promise> XenonEsmModuleLoader::EvaluateRecord(
    ModuleRecord* record,
    std::string* error) {
  if (!record->evaluation.IsEmpty()) {
    return record->evaluation.Get(isolate_);
  }
  if (!Instantiate(record, error)) {
    return {};
  }
  v8::Local<v8::Promise> result;
  if (!record->module.Get(isolate_)
           ->Evaluate(context_.Get(isolate_))
           .ToLocal(&result)) {
    *error = "Unable to evaluate ES module: " + record->info.url;
    return {};
  }
  record->evaluation.Reset(isolate_, result);
  return result;
}

v8::MaybeLocal<v8::Promise> XenonEsmModuleLoader::Evaluate(
    const ModuleInfo& info,
    std::string* error) {
  error->clear();
  auto* record = Load(info, error);
  return record ? EvaluateRecord(record, error) : v8::MaybeLocal<v8::Promise>();
}

v8::MaybeLocal<v8::Value> XenonEsmModuleLoader::Require(const ModuleInfo& info,
                                                        std::string* error) {
  error->clear();
  auto* record = Load(info, error);
  if (!record || !Instantiate(record, error)) {
    return {};
  }
  auto module = record->module.Get(isolate_);
  if (module->GetStatus() == v8::Module::kEvaluating) {
    *error = "ERR_REQUIRE_CYCLE_MODULE: Cannot require ES module in a cycle: " +
             info.url;
    ThrowFailure(*error);
    return {};
  }
  if (module->IsGraphAsync()) {
    *error =
        "ERR_REQUIRE_ASYNC_MODULE: Cannot require a module graph with "
        "top-level await: " +
        info.url;
    ThrowFailure(*error);
    return {};
  }
  v8::Local<v8::Promise> result;
  if (!EvaluateRecord(record, error).ToLocal(&result)) {
    return {};
  }
  if (result->State() == v8::Promise::kRejected) {
    result->MarkAsHandled();
    isolate_->ThrowException(result->Result());
    *error = "ES module evaluation failed: " + info.url;
    return {};
  }
  if (result->State() != v8::Promise::kFulfilled) {
    *error = "ERR_REQUIRE_ASYNC_MODULE: ES module evaluation is pending: " +
             info.url;
    ThrowFailure(*error);
    return {};
  }
  auto module_namespace = module->GetModuleNamespace().As<v8::Object>();
  auto commonjs_name = String(isolate_, "module.exports");
  auto has_commonjs_export =
      module_namespace->HasOwnProperty(context_.Get(isolate_), commonjs_name);
  if (has_commonjs_export.IsNothing()) {
    return {};
  }
  if (has_commonjs_export.FromJust()) {
    return module_namespace->Get(context_.Get(isolate_), commonjs_name);
  }
  auto context = context_.Get(isolate_);
  auto has_default =
      module_namespace->HasOwnProperty(context, String(isolate_, "default"));
  auto has_marker =
      module_namespace->HasOwnProperty(context, String(isolate_, "__esModule"));
  if (has_default.IsNothing() || has_marker.IsNothing()) {
    return {};
  }
  if (!has_default.FromJust() || has_marker.FromJust()) {
    return module_namespace;
  }
  if (record->require_facade.IsEmpty()) {
    // Node annotates require(ESM) for transpiled CommonJS interop. A genuine
    // module facade preserves live bindings and namespace object semantics;
    // copying properties would snapshot exports and lose later updates. This
    // fixed host module never rewrites or re-evaluates the application source.
    v8::ScriptOrigin origin(String(isolate_, info.url), 0, 0, false, -1, {},
                            false, false, true);
    v8::ScriptCompiler::Source source(
        String(isolate_,
               "export * from 'xenon:original';"
               "export {default} from 'xenon:original';"
               "export const __esModule=true;"),
        origin);
    v8::Local<v8::Module> facade;
    if (!v8::ScriptCompiler::CompileModule(isolate_, &source)
             .ToLocal(&facade)) {
      *error = "Unable to compile require(ESM) facade: " + info.url;
      return {};
    }
    record->require_facade.Reset(isolate_, facade);
    if (facade->InstantiateModule(context, &ResolveModule).IsNothing()) {
      record->require_facade.Reset();
      *error = "Unable to instantiate require(ESM) facade: " + info.url;
      return {};
    }
    v8::Local<v8::Promise> evaluated;
    if (!facade->Evaluate(context).ToLocal(&evaluated)) {
      record->require_facade.Reset();
      *error = "Unable to evaluate require(ESM) facade: " + info.url;
      return {};
    }
    if (evaluated->State() != v8::Promise::kFulfilled) {
      record->require_facade.Reset();
      if (evaluated->State() == v8::Promise::kRejected) {
        evaluated->MarkAsHandled();
        isolate_->ThrowException(evaluated->Result());
        *error = "require(ESM) facade evaluation failed: " + info.url;
      } else {
        *error = "ERR_REQUIRE_ASYNC_MODULE: require(ESM) facade is pending: " +
                 info.url;
        ThrowFailure(*error);
      }
      return {};
    }
  }
  return record->require_facade.Get(isolate_)->GetModuleNamespace();
}

v8::MaybeLocal<v8::Value> XenonEsmModuleLoader::EvaluateSynthetic(
    v8::Local<v8::Context> context,
    v8::Local<v8::Module> module) {
  auto* loader = ForIsolate(v8::Isolate::GetCurrent());
  if (!loader) {
    return {};
  }
  auto* record = loader->FindRecord(module);
  CHECK(record);
  v8::Local<v8::Value> value;
  if (!record->value.IsEmpty()) {
    value = record->value.Get(loader->isolate_);
  } else {
    std::string error;
    v8::TryCatch caught(loader->isolate_);
    if (!loader->load_value_.Run(record->info, &error).ToLocal(&value)) {
      if (!caught.HasCaught()) {
        loader->ThrowFailure(error);
      }
      if (caught.HasCaught()) {
        caught.ReThrow();
      }
      return {};
    }
    record->value.Reset(loader->isolate_, value);
  }
  for (const auto& name : record->export_names) {
    v8::Local<v8::Value> exported = value;
    if (name != "default" && !(name == "module.exports" &&
                               record->info.format == Format::kCommonJs)) {
      exported = v8::Undefined(loader->isolate_);
      // CommonJS named exports are a snapshot of own exported properties;
      // inherited properties and statically detected-but-absent names are not
      // invented. JSON exposes only its default export.
      if (value->IsObject()) {
        auto key = String(loader->isolate_, name);
        auto own = value.As<v8::Object>()->HasOwnProperty(context, key);
        if (own.IsNothing()) {
          return {};
        }
        if (own.FromJust() &&
            !value.As<v8::Object>()->Get(context, key).ToLocal(&exported)) {
          return {};
        }
      }
    }
    if (module
            ->SetSyntheticModuleExport(loader->isolate_,
                                       String(loader->isolate_, name), exported)
            .IsNothing()) {
      return {};
    }
  }
  v8::Local<v8::Promise::Resolver> resolver;
  if (!v8::Promise::Resolver::New(context).ToLocal(&resolver) ||
      resolver->Resolve(context, v8::Undefined(loader->isolate_)).IsNothing()) {
    return {};
  }
  return resolver->GetPromise();
}

v8::MaybeLocal<v8::Promise> XenonEsmModuleLoader::ImportWithAttributes(
    const std::string& specifier,
    const std::string& referrer,
    v8::Local<v8::FixedArray> attributes,
    std::string* error) {
  ModuleInfo info;
  if (!Resolve(specifier, referrer, ResolutionMode::kImport, &info, error,
               attributes, true)) {
    return {};
  }
  auto* record = Load(info, error);
  if (record) {
    info.format = record->info.format;
  }
  if (record && !CheckAttributes(info, attributes, true, error)) {
    return {};
  }
  v8::Local<v8::Promise> evaluation;
  if (!record || !EvaluateRecord(record, error).ToLocal(&evaluation)) {
    return {};
  }
  auto context = context_.Get(isolate_);
  v8::Local<v8::Function> completed;
  if (!v8::Function::New(
           context,
           [](const v8::FunctionCallbackInfo<v8::Value>& args) {
             args.GetReturnValue().Set(args.Data());
           },
           record->module.Get(isolate_)->GetModuleNamespace())
           .ToLocal(&completed)) {
    return {};
  }
  return evaluation->Then(context, completed);
}

v8::MaybeLocal<v8::Promise> XenonEsmModuleLoader::Import(
    const std::string& specifier,
    const std::string& referrer,
    std::string* error) {
  error->clear();
  return ImportWithAttributes(specifier, referrer, {}, error);
}

v8::MaybeLocal<v8::Promise> XenonEsmModuleLoader::ImportDynamically(
    v8::Local<v8::Context> context,
    v8::Local<v8::Data> host_defined_options,
    v8::Local<v8::Value> resource_name,
    v8::Local<v8::String> specifier,
    v8::Local<v8::FixedArray> attributes) {
  auto* isolate = v8::Isolate::GetCurrent();
  auto* loader = ForIsolate(isolate);
  v8::Local<v8::Promise::Resolver> rejected;
  if (!v8::Promise::Resolver::New(context).ToLocal(&rejected)) {
    return {};
  }
  std::string error;
  v8::Local<v8::Value> exception;
  {
    v8::TryCatch caught(isolate);
    v8::Local<v8::Promise> result;
    if (loader && loader
                      ->ImportWithAttributes(Utf8(isolate, specifier),
                                             Utf8(isolate, resource_name),
                                             attributes, &error)
                      .ToLocal(&result)) {
      return result;
    }
    if (caught.HasCaught()) {
      exception = caught.Exception();
    } else {
      exception = v8::Exception::Error(String(
          isolate, error.empty() ? "ES module loader is unavailable" : error));
    }
  }
  if (rejected->Reject(context, exception).IsNothing()) {
    return {};
  }
  return rejected->GetPromise();
}

void XenonEsmModuleLoader::InitializeImportMeta(v8::Local<v8::Context> context,
                                                v8::Local<v8::Module> module,
                                                v8::Local<v8::Object> meta) {
  auto* loader = ForIsolate(v8::Isolate::GetCurrent());
  if (!loader) {
    return;
  }
  auto* record = loader->FindRecord(module);
  if (!record) {
    loader->ThrowFailure("ERR_INVALID_STATE: Unknown import.meta module");
    return;
  }
  auto url = String(loader->isolate_, record->info.url);
  if (meta->CreateDataProperty(context, String(loader->isolate_, "url"), url)
          .IsNothing()) {
    return;
  }
  if (!record->info.filename.empty()) {
    if (meta->CreateDataProperty(
                context, String(loader->isolate_, "filename"),
                String(loader->isolate_, record->info.filename.AsUTF8Unsafe()))
            .IsNothing() ||
        meta->CreateDataProperty(
                context, String(loader->isolate_, "dirname"),
                String(loader->isolate_,
                       record->info.filename.DirName().AsUTF8Unsafe()))
            .IsNothing()) {
      return;
    }
  }
  v8::Local<v8::Function> resolve;
  if (!v8::Function::New(context, &ResolveImportMeta, url).ToLocal(&resolve)) {
    return;
  }
  std::ignore = meta->CreateDataProperty(
      context, String(loader->isolate_, "resolve"), resolve);
}

void XenonEsmModuleLoader::ResolveImportMeta(
    const v8::FunctionCallbackInfo<v8::Value>& info) {
  auto* isolate = info.GetIsolate();
  auto* loader = ForIsolate(isolate);
  if (!loader) {
    return;
  }
  auto context = isolate->GetCurrentContext();
  v8::Local<v8::String> specifier;
  if (info.Length() < 1 || !info[0]->ToString(context).ToLocal(&specifier)) {
    if (info.Length() < 1) {
      loader->ThrowFailure(
          "ERR_INVALID_ARG_TYPE: Module specifier is required");
    }
    return;
  }
  ModuleInfo result;
  std::string error;
  if (loader->Resolve(Utf8(isolate, specifier), Utf8(isolate, info.Data()),
                      ResolutionMode::kMetaResolve, &result, &error)) {
    info.GetReturnValue().Set(String(isolate, result.url));
  }
}

}  // namespace xenon::ipc
