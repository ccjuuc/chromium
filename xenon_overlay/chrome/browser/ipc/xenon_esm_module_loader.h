// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ESM_MODULE_LOADER_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ESM_MODULE_LOADER_H_

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-persistent-handle.h"

namespace xenon::ipc {

// Owns a module graph in an existing main-process isolate. The embedder retains
// package resolution, ASAR access, and CommonJS/native module cache ownership.
// Create and destroy with that isolate entered, before destroying its Context.
class XenonEsmModuleLoader {
 public:
  enum class Format { kModule, kCommonJs, kJson, kBuiltin };
  enum class ResolutionMode { kImport, kMetaResolve };
  struct ModuleInfo {
    std::string url;
    base::FilePath filename;
    Format format = Format::kModule;
    // Synchronous module hooks can override both source and format at load.
    std::string hook_format = {};
    std::map<std::string, std::string> import_attributes = {};
    bool use_module_hooks = false;
    bool hook_require = false;
    // Node separates ESM jobs by URL and the resolved import type attribute.
    std::string CacheKey() const {
      const auto type = import_attributes.find("type");
      return type == import_attributes.end() ? url : url + '\0' + type->second;
    }
  };
  using PrepareModuleCallback =
      base::RepeatingCallback<bool(ModuleInfo*, std::string*)>;
  using ResolveCallback =
      base::RepeatingCallback<bool(const std::string& specifier,
                                   const std::string& referrer,
                                   ResolutionMode mode,
                                   ModuleInfo* result,
                                   std::string* error)>;
  using ReadSourceCallback = base::RepeatingCallback<
      bool(const ModuleInfo&, std::string* source, std::string* error)>;
  using LoadValueCallback =
      base::RepeatingCallback<v8::MaybeLocal<v8::Value>(const ModuleInfo&,
                                                        std::string* error)>;
  // Must inspect CommonJS source without executing it. Return statically
  // declared named exports, including statically detected re-exports.
  using GetExportNamesCallback =
      base::RepeatingCallback<bool(const ModuleInfo&,
                                   std::vector<std::string>* names,
                                   std::string* error)>;

  XenonEsmModuleLoader(v8::Isolate* isolate,
                       v8::Local<v8::Context> context,
                       ResolveCallback resolve,
                       ReadSourceCallback read_source,
                       LoadValueCallback load_value,
                       GetExportNamesCallback get_export_names = {});
  ~XenonEsmModuleLoader();
  XenonEsmModuleLoader(const XenonEsmModuleLoader&) = delete;
  XenonEsmModuleLoader& operator=(const XenonEsmModuleLoader&) = delete;

  // The Promise is the real evaluation result, including top-level await and
  // rejection. The caller must not mark app ready while it is still pending.
  v8::MaybeLocal<v8::Promise> Evaluate(const ModuleInfo& info,
                                       std::string* error);
  v8::MaybeLocal<v8::Promise> Import(const std::string& specifier,
                                     const std::string& referrer,
                                     std::string* error);
  // Node's synchronous require(ESM) is permitted only for a synchronous graph.
  v8::MaybeLocal<v8::Value> Require(const ModuleInfo& info, std::string* error);
  void SetPrepareModuleCallback(PrepareModuleCallback callback);
  const std::map<std::string, std::string>& current_import_attributes() const {
    return current_import_attributes_;
  }

 private:
  struct ModuleRecord;
  static XenonEsmModuleLoader* ForIsolate(v8::Isolate* isolate);
  static v8::MaybeLocal<v8::Module> ResolveModule(
      v8::Local<v8::Context> context,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> attributes,
      v8::Local<v8::Module> referrer);
  static v8::MaybeLocal<v8::Promise> ImportDynamically(
      v8::Local<v8::Context> context,
      v8::Local<v8::Data> host_defined_options,
      v8::Local<v8::Value> resource_name,
      v8::Local<v8::String> specifier,
      v8::Local<v8::FixedArray> attributes);
  static void InitializeImportMeta(v8::Local<v8::Context> context,
                                   v8::Local<v8::Module> module,
                                   v8::Local<v8::Object> meta);
  static void ResolveImportMeta(
      const v8::FunctionCallbackInfo<v8::Value>& info);
  static v8::MaybeLocal<v8::Value> EvaluateSynthetic(
      v8::Local<v8::Context> context,
      v8::Local<v8::Module> module);

  bool Resolve(const std::string& specifier,
               const std::string& referrer,
               ResolutionMode mode,
               ModuleInfo* result,
               std::string* error,
               v8::Local<v8::FixedArray> attributes = {},
               bool dynamic_attributes = false);
  bool CheckAttributes(const ModuleInfo& info,
                       v8::Local<v8::FixedArray> attributes,
                       bool dynamic,
                       std::string* error);
  ModuleRecord* FindRecord(v8::Local<v8::Module> module);
  ModuleRecord* Load(const ModuleInfo& info, std::string* error);
  bool Instantiate(ModuleRecord* record, std::string* error);
  v8::MaybeLocal<v8::Promise> EvaluateRecord(ModuleRecord* record,
                                             std::string* error);
  v8::MaybeLocal<v8::Promise> ImportWithAttributes(
      const std::string& specifier,
      const std::string& referrer,
      v8::Local<v8::FixedArray> attributes,
      std::string* error);
  void ThrowFailure(const std::string& error);

  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  ResolveCallback resolve_;
  ReadSourceCallback read_source_;
  LoadValueCallback load_value_;
  GetExportNamesCallback get_export_names_;
  PrepareModuleCallback prepare_module_;
  std::map<std::string, std::string> current_import_attributes_;
  std::map<std::string, std::unique_ptr<ModuleRecord>> modules_;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ESM_MODULE_LOADER_H_
