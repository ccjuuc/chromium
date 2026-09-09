// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_SERVICES_XENON_NODE_EXECUTOR_H_
#define XENON_OVERLAY_SERVICES_XENON_NODE_EXECUTOR_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "gin/public/isolate_holder.h"
#include "v8/include/v8.h"
#include "xenon_overlay/public/mojom/xenon_service.mojom.h"

struct napi_env__;

namespace gin {
class Arguments;
}

namespace xenon {

struct LoadedNodeAddon;
class NativeAddonResourceRedirect;

class XenonNodeExecutor {
 public:
  XenonNodeExecutor();
  ~XenonNodeExecutor();

  XenonNodeExecutor(const XenonNodeExecutor&) = delete;
  XenonNodeExecutor& operator=(const XenonNodeExecutor&) = delete;

  using LoadAddonCallback =
      base::OnceCallback<void(bool success,
                              const std::string& error_msg,
                              std::vector<mojom::NodeExportInfoPtr> exports)>;
  using InspectExportCallback =
      base::OnceCallback<void(bool success,
                              const std::string& error_msg,
                              mojom::NodeExportInfoPtr info)>;
  using InvokeFunctionCallback = base::OnceCallback<void(
      bool success,
      base::Value result,
      std::vector<mojom::NodeCallbackResultPtr> callback_results,
      const std::string& error_msg)>;
  using ConstructExportCallback = base::OnceCallback<
      void(bool success, int32_t instance_id, const std::string& error_msg)>;
  using GetPropertyCallback = base::OnceCallback<
      void(bool success, base::Value result, const std::string& error_msg)>;
  using SetPropertyCallback =
      base::OnceCallback<void(bool success, const std::string& error_msg)>;
  using InvokeManyCallback =
      base::OnceCallback<void(std::vector<mojom::NodeInvokeCallResultPtr>)>;
  using NodeCallback =
      base::RepeatingCallback<void(int32_t client_id,
                                   int32_t callback_id,
                                   std::vector<base::Value> args)>;
  using NodeCallbackReleased =
      base::RepeatingCallback<void(int32_t client_id, int32_t callback_id)>;

  void SetCallbackHandlers(NodeCallback callback,
                           NodeCallbackReleased callback_released);
  // Sets the private runtime root for native dependencies loaded by addons.
  // Electron applications commonly derive absolute DLL paths from the host
  // executable. Xenon runs those addons in a shared executable image, so the
  // loader redirects such paths to this per-container directory.
  void SetRuntimeDirectory(const base::FilePath& runtime_directory);
  void LoadAddon(const std::string& path, LoadAddonCallback callback);
  void InspectExport(const std::string& module_path,
                     const std::string& export_path,
                     InspectExportCallback callback);
  void ConstructExport(const std::string& module_path,
                       const std::string& export_path,
                       int32_t client_id,
                       std::vector<mojom::NodeInvokeArgPtr> args,
                       ConstructExportCallback callback);
  void InvokeInstance(const std::string& module_path,
                      int32_t instance_id,
                      const std::string& method_name,
                      int32_t client_id,
                      std::vector<mojom::NodeInvokeArgPtr> args,
                      InvokeFunctionCallback callback,
                      bool allow_pending_promise = true);
  void GetInstanceProperty(const std::string& module_path,
                           int32_t instance_id,
                           const std::string& property_name,
                           GetPropertyCallback callback);
  void InspectInstanceMember(const std::string& module_path,
                             int32_t instance_id,
                             const std::string& property_name,
                             GetPropertyCallback callback);
  void SetInstanceProperty(const std::string& module_path,
                           int32_t instance_id,
                           const std::string& property_name,
                           base::Value value,
                           SetPropertyCallback callback);
  void ReleaseInstance(const std::string& module_path, int32_t instance_id);
  void InvokeFunction(const std::string& module_path,
                      const std::string& function_name,
                      int32_t client_id,
                      std::vector<mojom::NodeInvokeArgPtr> args,
                      InvokeFunctionCallback callback,
                      bool allow_pending_promise = true);
  void GetExportProperty(const std::string& module_path,
                         const std::string& object_path,
                         const std::string& property_name,
                         GetPropertyCallback callback);
  void SetExportProperty(const std::string& module_path,
                         const std::string& object_path,
                         const std::string& property_name,
                         base::Value value,
                         SetPropertyCallback callback);
  void InvokeMany(const std::string& module_path,
                  std::vector<mojom::NodeInvokeCallPtr> calls,
                  InvokeManyCallback callback);

  // Load (or return a cached) addon on the caller's thread. The caller must
  // not hold another V8 isolate locker (ipcMain uses v8::Unlocker first).
  bool LoadAddonFromCurrentThread(const std::string& path, std::string* error);
  bool InvokeExportFromCurrentThread(const std::string& path,
                                     const std::string& function_name,
                                     const base::Value& args,
                                     base::Value* result,
                                     std::string* error);
  bool ConstructExportFromCurrentThread(const std::string& path,
                                        const std::string& export_path,
                                        const base::Value& args,
                                        int32_t* instance_id,
                                        std::string* error);
  bool InvokeInstanceFromCurrentThread(const std::string& path,
                                       int32_t instance_id,
                                       const std::string& method_name,
                                       const base::Value& args,
                                       base::Value* result,
                                       std::string* error);

  bool HasModule(const std::string& module_path) const;

 private:
  struct AddonModule {
    AddonModule();
    ~AddonModule();

    AddonModule(AddonModule&&);
    AddonModule& operator=(AddonModule&&);

    AddonModule(const AddonModule&) = delete;
    AddonModule& operator=(const AddonModule&) = delete;

    v8::Global<v8::Value> exports;
    std::vector<mojom::NodeExportInfoPtr> export_tree;
    std::unique_ptr<LoadedNodeAddon> loaded_addon;
    std::unique_ptr<NativeAddonResourceRedirect> resource_redirect;
  };

  struct AddonInstance {
    AddonInstance();
    ~AddonInstance();
    AddonInstance(AddonInstance&&);
    AddonInstance& operator=(AddonInstance&&);

    base::FilePath module_path;
    v8::Global<v8::Object> object;
  };

  struct AddonCallback {
    AddonCallback();
    ~AddonCallback();

    int32_t client_id = 0;
    int32_t callback_id = 0;
    std::string module_path;
    base::WeakPtr<XenonNodeExecutor> executor;
    v8::Global<v8::Function> function;
  };

  static void FirstWeakCallback(
      const v8::WeakCallbackInfo<AddonCallback>& data);
  static void SecondWeakCallback(
      const v8::WeakCallbackInfo<AddonCallback>& data);

  v8::Local<v8::Function> GetOrCreateNativeCallback(
      v8::Local<v8::Context> context,
      int32_t client_id,
      int32_t callback_id,
      const std::string& module_path);
  void OnNativeCallback(int32_t client_id,
                        int32_t callback_id,
                        gin::Arguments* arguments);
  void OnNativeCallbackCollected(AddonCallback* callback);
  void InvokeResolvedFunction(v8::Local<v8::Context> context,
                              const std::string& module_path,
                              const std::string& function_name,
                              v8::Local<v8::Function> function,
                              v8::Local<v8::Value> receiver,
                              int32_t client_id,
                              const std::vector<mojom::NodeInvokeArgPtr>& args,
                              InvokeFunctionCallback callback,
                              bool allow_pending_promise = true);
  void OnAsyncPromiseResolved(uint64_t promise_id, gin::Arguments* arguments);
  void OnAsyncPromiseRejected(uint64_t promise_id, gin::Arguments* arguments);
  std::optional<base::Value> MaybeAdoptNativeReturn(
      v8::Local<v8::Context> context,
      const std::string& module_path,
      v8::Local<v8::Value> result,
      int depth);
  void AttachNativeInstanceFields(v8::Local<v8::Context> context,
                                  const std::string& module_path,
                                  v8::Local<v8::Object> object,
                                  base::DictValue& dict,
                                  int depth);
  // Adopts native handles at any depth (e.g. findRepeatTask() -> Task[]).
  std::optional<base::Value> ConvertNativeValue(
      v8::Local<v8::Context> context,
      const std::string& module_path,
      v8::Local<v8::Value> value,
      std::string* error_msg,
      int depth);
  v8::MaybeLocal<v8::Value> WireValueToV8(v8::Local<v8::Context> context,
                                          const base::Value& value,
                                          int32_t client_id,
                                          const std::string& module_path,
                                          std::string* error_msg,
                                          int depth = 0);
  void EnsureUvLoopPolling();
  void PumpUvLoops();

  bool EnsureIsolate();
  AddonModule* FindModule(const std::string& module_path);
  const AddonModule* FindModule(const std::string& module_path) const;
  void RegisterModulePath(const base::FilePath& requested_path,
                          const base::FilePath& canonical_path);

  // A dedicated V8 isolate for running addon code
  std::unique_ptr<gin::IsolateHolder> addon_isolate_holder_;
  RAW_PTR_EXCLUSION v8::Isolate* addon_isolate_ = nullptr;
  v8::Global<v8::Context> addon_context_;
  std::map<base::FilePath, AddonModule> addon_modules_;
  std::map<base::FilePath, base::FilePath> addon_path_aliases_;
  std::map<int32_t, AddonInstance> addon_instances_;
  std::map<std::pair<int32_t, int32_t>, std::unique_ptr<AddonCallback>>
      addon_callbacks_;
  base::FilePath runtime_directory_;
  int32_t next_instance_id_ = 1;
  NodeCallback callback_handler_;
  NodeCallbackReleased callback_released_handler_;
  base::RepeatingTimer uv_loop_timer_;
  struct PendingPromise {
    std::string module_path;
    InvokeFunctionCallback callback;
  };
  uint64_t next_promise_id_ = 1;
  std::map<uint64_t, PendingPromise> pending_promises_;

  base::WeakPtrFactory<XenonNodeExecutor> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_SERVICES_XENON_NODE_EXECUTOR_H_
