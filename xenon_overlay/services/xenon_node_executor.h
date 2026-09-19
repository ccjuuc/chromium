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
#include "base/time/time.h"
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
                                   std::vector<base::Value> args,
                                   base::Value receiver)>;
  using NodeCallbackReleased =
      base::RepeatingCallback<void(int32_t client_id, int32_t callback_id)>;
  using DeferPromiseCallback = base::OnceCallback<void(uint64_t promise_id)>;

  void SetCallbackHandlers(NodeCallback callback,
                           NodeCallbackReleased callback_released);
  void ReleaseCallbacksForClient(int32_t client_id);
  // Owner zero belongs to main/legacy callers and is never released as a
  // renderer connection. Nonzero owners must be registered before use.
  void RegisterInstanceOwner(uint64_t owner);
  void ReleaseInstanceOwner(uint64_t owner);
  std::string GetInstanceOwnerToken(uint64_t owner) const;
  bool ValidateInstanceOwnerToken(uint64_t owner,
                                  const std::string& token) const;
  // Sets the private runtime root for native dependencies loaded by addons.
  // Electron applications commonly derive absolute DLL paths from the host
  // executable. Xenon runs those addons in a shared executable image, so the
  // loader redirects such paths to this per-container directory.
  void SetRuntimeDirectory(const base::FilePath& runtime_directory);
  // Metadata is optional for callers that request an exact root descriptor.
  void LoadAddon(const std::string& path,
                 LoadAddonCallback callback,
                 bool include_export_tree = true);
  void InspectExport(const std::string& module_path,
                     const std::string& export_path,
                     InspectExportCallback callback);
  void ConstructExport(const std::string& module_path,
                       const std::string& export_path,
                       int32_t client_id,
                       std::vector<mojom::NodeInvokeArgPtr> args,
                       ConstructExportCallback callback,
                       uint64_t owner = 0,
                       base::DictValue prototype_properties = {});
  void InvokeInstance(const std::string& module_path,
                      int32_t instance_id,
                      const std::string& method_name,
                      int32_t client_id,
                      std::vector<mojom::NodeInvokeArgPtr> args,
                      InvokeFunctionCallback callback,
                      bool allow_pending_promise = true,
                      DeferPromiseCallback defer_promise = {},
                      uint64_t promise_owner = 0);
  void GetInstanceProperty(const std::string& module_path,
                           int32_t instance_id,
                           const std::string& property_name,
                           GetPropertyCallback callback,
                           uint64_t owner = 0);
  void InspectInstanceMember(const std::string& module_path,
                             int32_t instance_id,
                             const std::string& property_name,
                             GetPropertyCallback callback,
                             uint64_t owner = 0);
  void SetInstanceProperty(const std::string& module_path,
                           int32_t instance_id,
                           const std::string& property_name,
                           base::Value value,
                           SetPropertyCallback callback,
                           uint64_t owner = 0);
  void ReleaseInstance(const std::string& module_path,
                       int32_t instance_id,
                       uint64_t owner = 0,
                       const std::string& expected_token = {});
  void InvokeFunction(const std::string& module_path,
                      const std::string& function_name,
                      int32_t client_id,
                      std::vector<mojom::NodeInvokeArgPtr> args,
                      InvokeFunctionCallback callback,
                      bool allow_pending_promise = true,
                      DeferPromiseCallback defer_promise = {},
                      uint64_t promise_owner = 0);
  // A deferred token belongs to one renderer connection and is consumed once.
  void AwaitDeferredPromise(uint64_t promise_id,
                            uint64_t promise_owner,
                            InvokeFunctionCallback callback);
  void CancelPromisesForOwner(uint64_t promise_owner);
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
  bool InspectExportFromCurrentThread(const std::string& path,
                                      const std::string& export_path,
                                      mojom::NodeExportInfoPtr* description,
                                      std::string* error);
  bool InvokeExportFromCurrentThread(const std::string& path,
                                     const std::string& function_name,
                                     const base::Value& args,
                                     base::Value* result,
                                     std::string* error);
  bool ConstructExportFromCurrentThread(const std::string& path,
                                        const std::string& export_path,
                                        const base::Value& args,
                                        base::Value* instance,
                                        std::string* error);
  bool InvokeInstanceFromCurrentThread(const std::string& path,
                                       int32_t instance_id,
                                       const std::string& method_name,
                                       const base::Value& args,
                                       base::Value* result,
                                       std::string* error);

  bool HasModule(const std::string& module_path) const;

 private:
  friend class XenonNodeExecutorTestPeer;

  struct AddonModule {
    AddonModule();
    ~AddonModule();

    AddonModule(AddonModule&&);
    AddonModule& operator=(AddonModule&&);

    AddonModule(const AddonModule&) = delete;
    AddonModule& operator=(const AddonModule&) = delete;

    v8::Global<v8::Value> exports;
    std::vector<mojom::NodeExportInfoPtr> export_tree;
    bool export_tree_initialized = false;
    std::unique_ptr<LoadedNodeAddon> loaded_addon;
    std::unique_ptr<NativeAddonResourceRedirect> resource_redirect;
  };

  std::vector<mojom::NodeExportInfoPtr> GetCachedExportTree(
      AddonModule* module);

  struct AddonInstance {
    AddonInstance();
    ~AddonInstance();
    AddonInstance(AddonInstance&&);
    AddonInstance& operator=(AddonInstance&&);

    base::FilePath module_path;
    uint64_t owner = 0;
    int identity_hash = 0;
    v8::Global<v8::Object> object;
  };

  struct AddonCallback {
    AddonCallback();
    ~AddonCallback();

    int32_t client_id = 0;
    int32_t callback_id = 0;
    uint64_t generation = 0;
    uint64_t owner = 0;
    std::string module_path;
    base::WeakPtr<XenonNodeExecutor> executor;
    v8::Global<v8::Function> function;
  };

  static void FirstWeakCallback(
      const v8::WeakCallbackInfo<AddonCallback>& data);
  v8::Local<v8::Function> GetOrCreateNativeCallback(
      v8::Local<v8::Context> context,
      int32_t client_id,
      int32_t callback_id,
      const std::string& module_path,
      uint64_t owner = 0);
  void OnNativeCallback(int32_t client_id,
                        int32_t callback_id,
                        uint64_t generation,
                        gin::Arguments* arguments);
  void OnNativeCallbackCollected(int32_t client_id,
                                 int32_t callback_id,
                                 uint64_t generation);
  void InvokeResolvedFunction(v8::Local<v8::Context> context,
                              const std::string& module_path,
                              const std::string& function_name,
                              v8::Local<v8::Function> function,
                              v8::Local<v8::Value> receiver,
                              int32_t client_id,
                              const std::vector<mojom::NodeInvokeArgPtr>& args,
                              InvokeFunctionCallback callback,
                              bool allow_pending_promise = true,
                              DeferPromiseCallback defer_promise = {},
                              uint64_t promise_owner = 0);
  void AwaitPromise(v8::Local<v8::Context> context,
                    const std::string& module_path,
                    v8::Local<v8::Promise> promise,
                    InvokeFunctionCallback callback,
                    std::optional<uint64_t> promise_owner);
  void ExpireDeferredPromises();
  void OnAsyncPromiseResolved(uint64_t promise_id, gin::Arguments* arguments);
  void OnAsyncPromiseRejected(uint64_t promise_id, gin::Arguments* arguments);
  std::optional<base::Value> MaybeAdoptNativeReturn(
      v8::Local<v8::Context> context,
      const std::string& module_path,
      v8::Local<v8::Value> result,
      int depth,
      uint64_t owner);
  int32_t RegisterNativeInstance(const std::string& module_path,
                                 v8::Local<v8::Object> object,
                                 uint64_t owner);
  void RemoveNativeInstance(int32_t instance_id);
  void AttachNativeInstanceFields(v8::Local<v8::Context> context,
                                  const std::string& module_path,
                                  v8::Local<v8::Object> object,
                                  base::DictValue& dict,
                                  int depth,
                                  uint64_t owner);
  // Adopts native handles at any depth (e.g. findRepeatTask() -> Task[]).
  std::optional<base::Value> ConvertNativeValue(v8::Local<v8::Context> context,
                                                const std::string& module_path,
                                                v8::Local<v8::Value> value,
                                                std::string* error_msg,
                                                int depth,
                                                uint64_t owner = 0);
  v8::MaybeLocal<v8::Value> WireValueToV8(v8::Local<v8::Context> context,
                                          const base::Value& value,
                                          int32_t client_id,
                                          const std::string& module_path,
                                          std::string* error_msg,
                                          int depth = 0,
                                          uint64_t owner = 0);
  void EnsureUvLoopPolling();
  void PumpUvLoops();

  bool EnsureIsolate();
  bool IsInstanceOwnerActive(uint64_t owner) const;
  AddonModule* FindModule(const std::string& module_path);
  const AddonModule* FindModule(const std::string& module_path) const;
  base::FilePath GetModuleCacheKey(const std::string& module_path) const;
  void RegisterModulePath(const base::FilePath& requested_path,
                          const base::FilePath& canonical_path);

  // A dedicated V8 isolate for running addon code
  std::unique_ptr<gin::IsolateHolder> addon_isolate_holder_;
  RAW_PTR_EXCLUSION v8::Isolate* addon_isolate_ = nullptr;
  v8::Global<v8::Context> addon_context_;
  std::map<base::FilePath, AddonModule> addon_modules_;
  std::map<base::FilePath, base::FilePath> addon_path_aliases_;
  std::map<int32_t, AddonInstance> addon_instances_;
  // V8 identity hashes may collide; validate owner, module, and StrictEquals.
  std::multimap<int, int32_t> addon_instance_ids_by_hash_;
  std::map<uint64_t, std::string> instance_owners_;
  std::map<std::pair<int32_t, int32_t>, std::unique_ptr<AddonCallback>>
      addon_callbacks_;
  base::FilePath runtime_directory_;
  int32_t next_instance_id_ = 1;
  uint64_t next_callback_generation_ = 1;
  NodeCallback callback_handler_;
  NodeCallbackReleased callback_released_handler_;
  base::RepeatingTimer uv_loop_timer_;
  struct PendingPromise {
    std::string module_path;
    InvokeFunctionCallback callback;
    std::optional<uint64_t> owner;
  };
  struct DeferredPromise {
    std::string module_path;
    uint64_t owner;
    v8::Global<v8::Promise> promise;
    base::TimeTicks expires_at;
  };
  uint64_t next_promise_id_ = 1;
  std::map<uint64_t, PendingPromise> pending_promises_;
  std::map<uint64_t, DeferredPromise> deferred_promises_;
  base::OneShotTimer deferred_promise_timer_;

  base::WeakPtrFactory<XenonNodeExecutor> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_SERVICES_XENON_NODE_EXECUTOR_H_
