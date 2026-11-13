// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_IPC_MAIN_CONTAINER_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_IPC_MAIN_CONTAINER_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "gin/public/isolate_holder.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"
#include "v8/include/v8-promise.h"
#include "xenon_overlay/buildflags/buildflags.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_net.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_esm_module_loader.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_module_resolver.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_worker_engine.h"
#include "xenon_overlay/chrome/browser/napi/native_runtime_retirement.h"
#include "xenon_overlay/common/ipc/xenon_crash_reporter_annotations.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"
#include "xenon_overlay/public/mojom/xenon_utility_process.mojom.h"

struct uv_loop_s;

namespace gin {
class Arguments;
}

namespace network {
class SharedURLLoaderFactory;
}

namespace blink {
struct TransferableMessage;
}

namespace xenon {
struct LoadedNodeAddon;
class NativeAddonEventLoop;
}  // namespace xenon

namespace xenon::ipc {

class XenonAppPaths;
class XenonFileDescriptors;
class MessagePortRuntime;
class XenonUdpSocketBridge;
class ZlibStream;

// Utility-process owner of the Electron-compatible main JavaScript context.
// Its lifetime is independent of every renderer Document, so ipcMain handlers
// and JavaScript globals survive renderer navigation and crashes.
class XenonIpcMainContainer : public xenon::NativeRuntimeShutdown {
 public:
  // Lets an embedder provide a packaged CommonJS main module without first
  // extracting it to disk. Relative requires are resolved from
  // `virtual_path`; the path itself does not need to exist.
  struct EmbeddedMainModule {
    std::string source;
    base::FilePath virtual_path;
    base::FilePath app_path;
    base::FilePath executable_path;
    std::string app_name = "Application";
    std::string app_version = "0.0.0";
    std::string default_user_agent;
    std::vector<std::pair<std::string, std::string>> renderer_url_mappings;
    std::string renderer_base_url;
    base::FilePath resources_directory;
    base::FilePath working_directory;
    std::vector<std::string> app_argv;
    std::optional<bool> is_packaged;
  };

  // Loads package.json and the original main module directly from a source
  // directory, release layout or ASAR. Empty metadata uses the manifest.
  struct AppMainModule {
    base::FilePath app_path;
    // Optional explicit entry for packages with an incorrect manifest. Must
    // remain inside the resolved application root; no source rewrite occurs.
    base::FilePath main_script_path;
    base::FilePath executable_path;
    base::FilePath resources_directory;
    base::FilePath working_directory;
    std::vector<std::string> app_argv;
    std::string app_name;
    std::string app_version;
    std::string default_user_agent;
    std::vector<std::pair<std::string, std::string>> renderer_url_mappings;
    std::string renderer_base_url;
    std::optional<bool> is_packaged;
  };

  using InvokeCallback =
      base::OnceCallback<void(xenon::ipc::mojom::IpcResultPtr)>;

  // Hosted N-API runtime lives in XenonNodeExecutor's isolate. ipcMain never
  // napi_module_register's the same .node; it loads through these hooks (with
  // v8::Unlocker) and talks to the real addon via a generic JS forwarder.
  struct NativeAddonHooks {
    base::RepeatingCallback<bool(const std::string& path, std::string* error)>
        load;
    base::RepeatingCallback<bool(const std::string& path,
                                 const std::string& export_path,
                                 base::Value* description,
                                 std::string* error)>
        describe;
    base::RepeatingCallback<bool(const std::string& path,
                                 const std::string& function_name,
                                 const base::Value& args,
                                 base::Value* result,
                                 std::string* error)>
        invoke;
    base::RepeatingCallback<bool(const std::string& path,
                                 const std::string& export_path,
                                 const base::Value& args,
                                 base::Value* instance,
                                 std::string* error)>
        construct;
    base::RepeatingCallback<bool(const std::string& path,
                                 int32_t instance_id,
                                 const std::string& method_name,
                                 const base::Value& args,
                                 base::Value* result,
                                 std::string* error)>
        invoke_instance;
  };

  struct WindowHooks {
    base::RepeatingCallback<bool(int width,
                                 int height,
                                 bool show,
                                 bool frame,
                                 bool transparent,
                                 int32_t parent_id,
                                 const std::string& title,
                                 int32_t* window_id,
                                 uint64_t* hwnd,
                                 std::string* error)>
        create;
    base::RepeatingCallback<void(int32_t window_id, const std::string& url)>
        load_url;
    base::RepeatingCallback<void(int32_t window_id, bool visible)> set_visible;
    base::RepeatingCallback<bool(int32_t window_id,
                                 const std::string& command,
                                 const base::Value& arguments,
                                 base::Value* result,
                                 std::string* error)>
        call;
    base::RepeatingCallback<void(
        int32_t window_id,
        const std::string& command,
        base::Value arguments,
        base::OnceCallback<void(base::Value, std::string)> reply)>
        call_async;
    base::RepeatingCallback<void(int32_t window_id)> close;
  };

  struct UtilityRuntimeOptions {
    std::vector<std::string> argv;
    std::vector<std::string> exec_argv;
    base::DictValue environment;
    blink::MessagePortDescriptor parent_port;
  };
  struct UtilityProcessHooks {
    base::RepeatingCallback<void(
        mojom::UtilityProcessOptionsPtr,
        blink::MessagePortDescriptor,
        mojo::PendingReceiver<mojom::UtilityProcessControl>,
        mojo::PendingRemote<mojom::UtilityProcessClient>,
        base::OnceCallback<void(bool, const std::string&)>)>
        create;
  };

  XenonIpcMainContainer();
  ~XenonIpcMainContainer() override;

  XenonIpcMainContainer(const XenonIpcMainContainer&) = delete;
  XenonIpcMainContainer& operator=(const XenonIpcMainContainer&) = delete;

  bool Initialize();
  bool Initialize(EmbeddedMainModule main_module);
  bool Initialize(AppMainModule main_module);
  void SetHostPaths(base::FilePath user_data, base::FilePath crash_dumps);
  void SetNativeAddonHooks(NativeAddonHooks hooks);
  void SetWindowHooks(WindowHooks hooks);
  void SetUtilityRuntimeOptions(UtilityRuntimeOptions options);
  void SetUtilityProcessHooks(UtilityProcessHooks hooks);
  void UtilityShutdown(int exit_code);
  void SetAppExitHandler(base::RepeatingCallback<void(int)> handler);
  void SetChildProcessCaller(
      base::RepeatingCallback<base::Value(const base::DictValue&)> caller);
  void SetNetPipeSender(
      base::RepeatingCallback<void(const std::string&, base::Value)> sender);
  void SetNetworkLoaderFactory(
      scoped_refptr<network::SharedURLLoaderFactory> factory);
  void SetElectronNetworkHooks(ElectronNetEngine::FactoryProvider factory,
                               ElectronNetEngine::OnlineProvider online);
  // Returns false while an ESM entry's top-level await is pending or failed.
  bool MarkAppReady();
  void SetMainEvaluationCompleteHandler(base::RepeatingClosure handler);
  // Synchronous convenience for runtimes without pending native work. Owners
  // of addon runtimes must transfer ownership to RetireNativeRuntime instead
  // of destroying a container whose PollNativeShutdown() is still false.
  void Shutdown();
  void BeginNativeShutdown() override;
  bool PollNativeShutdown() override;
  std::string DescribeNativeShutdown() const override;
  bool is_initialized() const { return initialized_; }
  bool uses_shared_native_addons() const {
    return !native_addon_hooks_.load.is_null();
  }
  const std::string& startup_error() const { return startup_error_; }

  std::string AddRenderer(
      mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer,
      int32_t process_id,
      int32_t frame_id,
      int32_t window_id = 0,
      const std::string& document_url = std::string(),
      bool is_main_frame = true);
  void AddRenderer(const std::string& endpoint_id,
                   mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer,
                   int32_t process_id,
                   int32_t frame_id,
                   int32_t window_id = 0,
                   const std::string& document_url = std::string(),
                   bool is_main_frame = true,
                   mojom::IpcServiceWorkerInfoPtr worker_info = nullptr);
  void UpdateRendererFrameUrl(const std::string& endpoint_id,
                              const std::string& document_url);
  void RemoveRenderer(const std::string& endpoint_id);
  void DispatchToRenderer(const std::string& endpoint_id,
                          const std::string& channel,
                          base::Value arguments);
  void DispatchWindowEvent(int32_t window_id,
                           const std::string& event_name,
                           base::Value arguments);
  bool DispatchBeforeInputEvent(int32_t window_id, base::Value input);
  base::expected<bool, std::string> CheckPermission(base::Value request);
  void DispatchAppEvent(const std::string& event_name, base::Value arguments);

  void Send(const std::string& endpoint_id,
            const std::string& channel,
            base::Value arguments);
  void ReceivePortMessage(const std::string& endpoint_id,
                          const std::string& channel,
                          blink::TransferableMessage message);
  void Invoke(const std::string& endpoint_id,
              const std::string& channel,
              base::Value arguments,
              InvokeCallback callback);
  xenon::ipc::mojom::IpcResultPtr SendSync(const std::string& endpoint_id,
                                           const std::string& channel,
                                           base::Value arguments);
  // Browser-owned protocol requests never pass through renderer ipcMain
  // handlers.
  void HandleProtocol(const std::string& partition,
                      const std::string& request_id,
                      base::Value request,
                      InvokeCallback callback);
  void CancelProtocol(const std::string& request_id);
  void HandleWebRequest(base::Value request, InvokeCallback callback);
  void CancelWebRequest(const std::string& request_id);
  void ReleaseWebRequestRegistration(const std::string& registration_id);

  // Executes the privileged backend for renderer node:fs compatibility. The
  // renderer keeps Chromium's sandbox; Electron-compatible sync/callback/
  // promise APIs are layered over this operation in the renderer bootstrap.
  xenon::ipc::mojom::IpcResultPtr FileSystemCall(base::Value arguments);

  // A Node worker reuses the module implementations, but has its own isolate,
  // context, timers and lifecycle and cannot acquire Electron capabilities.
  bool InitializeWorker(WorkerOptions options,
                        std::unique_ptr<WorkerMessage> data,
                        std::unique_ptr<v8::Isolate::CreateParams> params,
                        scoped_refptr<XenonFileDescriptors> files,
                        WorkerControl* control);
  bool WorkerHasActiveWork() const;
  int WorkerBeforeExit();
  int FinishWorker(int exit_code, bool terminated);
  void DispatchWorkerEvent(WorkerEngine::Event event);

 private:
  void FinalizeNativeShutdown();
  void ScheduleNativeEventLoopPump();
  void PumpNativeEventLoop();
  struct AsyncContextHooks;
  struct PromiseReplyContext;
  struct RendererEndpoint;
  struct TimerInfo {
    int id = 0;
    bool is_interval = false;
    bool referenced = true;
    int delay_ms = 0;
    v8::Global<v8::Function> callback;
    std::vector<v8::Global<v8::Value>> args;
  };

  bool InitializeInternal(
      std::optional<EmbeddedMainModule> main_module,
      std::optional<AppMainModule> app_module = std::nullopt);
  bool ResolveApplicationMain(AppMainModule main_module);
  bool RunBootstrap();
  bool ResolveConfiguredMainScript();
  bool MaybeLoadConfiguredMainScript();
  void InitializeModuleLoaders();
  bool ResolvePackageMap(const std::string& package_json,
                         const std::string& key,
                         bool importing,
                         bool imports,
                         std::string* target,
                         std::string* error);
  bool ResolveEsmModule(const std::string& specifier,
                        const std::string& parent_url,
                        XenonEsmModuleLoader::ResolutionMode mode,
                        XenonEsmModuleLoader::ModuleInfo* result,
                        std::string* error);
  bool ResolveWithModuleHooks(
      const std::string& specifier,
      const std::string& parent_url,
      XenonModuleResolver::Mode mode,
      const std::map<std::string, std::string>& attributes,
      XenonEsmModuleLoader::ModuleInfo* result,
      std::string* error);
  bool PrepareHookedModule(XenonEsmModuleLoader::ModuleInfo* module,
                           std::string* error);
  bool ReadEsmModuleSource(const XenonEsmModuleLoader::ModuleInfo& module,
                           std::string* source,
                           std::string* error);
  bool ReadModuleSourceFile(const base::FilePath& filename,
                            std::string* source);
  bool IsBuiltinModuleRequest(const std::string& name) const;
  v8::MaybeLocal<v8::Value> BuiltinModuleValue(const std::string& name,
                                               std::string* error);
  v8::MaybeLocal<v8::Value> LoadHookedModule(
      XenonEsmModuleLoader::ModuleInfo module,
      std::string* error);
  void NativeModuleHooksChanged(gin::Arguments* args);
  void NativeDefaultModuleResolve(gin::Arguments* args);
  void NativeDefaultModuleLoad(gin::Arguments* args);
  v8::MaybeLocal<v8::Value> LoadEsmValue(
      const XenonEsmModuleLoader::ModuleInfo& module,
      std::string* error);
  bool GetCommonJsExportNames(const XenonEsmModuleLoader::ModuleInfo& module,
                              std::vector<std::string>* names,
                              std::string* error);
  static void OnMainModuleEvaluated(
      const v8::FunctionCallbackInfo<v8::Value>& info);
  static void OnMainModuleRejected(
      const v8::FunctionCallbackInfo<v8::Value>& info);
  v8::MaybeLocal<v8::Object> CreateCommonJsParent(
      const base::FilePath& filename);
  v8::MaybeLocal<v8::Value> CallModuleMethod(const char* name,
                                             int argc,
                                             v8::Local<v8::Value> argv[]);
  v8::MaybeLocal<v8::Value> CallCommonJsLookupPaths(
      const std::string& request,
      v8::Local<v8::Object> parent);
  bool ResolveCommonJsLookupPaths(const std::string& request,
                                  const base::FilePath& parent_file,
                                  std::vector<base::FilePath>* paths,
                                  std::string* error);
  v8::MaybeLocal<v8::Function> CreateRequireFunction(
      const base::FilePath& parent_file,
      v8::Local<v8::Object> parent_module = {});
  v8::MaybeLocal<v8::Value> LoadCommonJsSource(
      const base::FilePath& virtual_path,
      const std::string& source_text,
      std::string* error);
  std::optional<base::FilePath> ResolveCommonJsPath(
      const base::FilePath& requested_path,
      std::string* error,
      int package_main_depth = 0);
  v8::MaybeLocal<v8::Value> LoadCommonJsModule(
      const base::FilePath& requested_path,
      std::string* error);
  v8::MaybeLocal<v8::Value> LoadResolvedCommonJsModule(
      const base::FilePath& normalized,
      std::string* error);
  v8::MaybeLocal<v8::Value> RequireModule(const std::string& request,
                                          const base::FilePath& parent_file,
                                          std::string* error,
                                          bool resolve_only = false);

  v8::MaybeLocal<v8::Value> ValueToV8(const base::Value& value);
  v8::MaybeLocal<v8::Value> IpcPayloadToV8(const base::Value& value,
                                           std::string* error);
  bool V8ToValue(v8::Local<v8::Value> value,
                 base::Value* output,
                 std::string* error);
  bool V8ToIpcPayload(v8::Local<v8::Value> value,
                      bool serialized,
                      base::Value* output,
                      std::string* error);
  v8::Local<v8::Object> CreateSenderMetadata(const std::string& endpoint_id);
  void DispatchRendererEvent(const std::string& endpoint_id, bool attached);

  void NativeLog(gin::Arguments* args);
  void NativeCreateRequire(gin::Arguments* args);
  void NativeProcessDirectory(gin::Arguments* args);
  void NativeIsBuiltinModule(gin::Arguments* args);
  void NativeIsPromise(gin::Arguments* args);
  void NativeCanParseURL(gin::Arguments* args);
  void NativeIsNativeError(gin::Arguments* args);
  void NativeUtilType(gin::Arguments* args);
  void NativeV8Serialize(gin::Arguments* args);
  void NativeStructuredClone(gin::Arguments* args);
  void NativeIsDataCloneError(gin::Arguments* args);
  void NativeInstallAsyncContextHooks(gin::Arguments* args);
  void NativeGetPath(gin::Arguments* args);
  void NativeSetPath(gin::Arguments* args);
  void NativeSetAppLogsPath(gin::Arguments* args);
  void NativeNetworkInterfaces(gin::Arguments* args);
  void NativeOsCall(gin::Arguments* args);
  void NativeCrashReporterCall(gin::Arguments* args);
  void NativeRegisterSchemesAsPrivileged(gin::Arguments* args);
  void NativeSendToRenderer(gin::Arguments* args);
  void NativeNetSend(gin::Arguments* args);
  void NativeDgramCall(gin::Arguments* args);
  void DispatchDgramEvent(base::Value event);
  uv_loop_s* EnsureNativeEventLoop(std::string* error);
  void NativeChildProcessCall(gin::Arguments* args);
  bool ReadFileSystemArguments(gin::Arguments* args, base::Value* arguments);
  void NativeFsCall(gin::Arguments* args);
  void NativeFsCallAsync(gin::Arguments* args);
  void OnFileSystemCallComplete(uint64_t request_id,
                                xenon::ipc::mojom::IpcResultPtr result);
  void NativeCryptoCipher(gin::Arguments* args);
  void NativeCryptoDigest(gin::Arguments* args);
  void NativeCryptoRandom(gin::Arguments* args);
  void NativeHttpRequest(gin::Arguments* args);
  void NativeHttpAbort(gin::Arguments* args);
  void NativeElectronNetStart(gin::Arguments* args);
  void NativeElectronNetControl(gin::Arguments* args);
  void NativeElectronNetAuth(gin::Arguments* args);
  void NativeElectronNetOnline(gin::Arguments* args);
  bool InitializeWorkerBindings(v8::Local<v8::Context> context);
  bool InitializeUtilityBindings(v8::Local<v8::Context> context);
  bool InstallUtilityParentPort();
  void NativeUtilityCreate(gin::Arguments* args);
  void NativeUtilityControl(gin::Arguments* args);
  void NativeUtilityRead(gin::Arguments* args);
  void NativeUtilityProcessCall(gin::Arguments* args);
  void NativeUtilityExit(gin::Arguments* args);
  void RequestUtilityExit(int exit_code);
  void DispatchUtilityEvent(int id,
                            const char* type,
                            int value,
                            const std::string& message);
  void CompleteUtilityRead(int id,
                           int fd,
                           const std::vector<uint8_t>& bytes,
                           bool eof,
                           const std::string& error);
  void NativeWorkerCreate(gin::Arguments* args);
  void NativeWorkerPost(gin::Arguments* args);
  void NativeWorkerControl(gin::Arguments* args);
  void NativeWorkerExit(gin::Arguments* args);
  void NativeMarkAsUncloneable(gin::Arguments* args);
  bool ReportNodeException(v8::Local<v8::Value> exception);
  static void OnWorkerV8Message(v8::Local<v8::Message> message,
                                v8::Local<v8::Value> data);
  static void OnWorkerPromiseRejected(v8::PromiseRejectMessage message);
  void ProcessWorkerPromiseRejection(uint64_t id);
  void DispatchElectronNetEvent(base::Value event);
  void OnHttpRequestComplete(uint64_t request_id,
                             xenon::ipc::mojom::IpcResultPtr result);
  void NativeZlibCall(gin::Arguments* args);
  void NativeZlibStreamCall(gin::Arguments* args);
  void OnZlibCallComplete(uint64_t request_id,
                          xenon::ipc::mojom::IpcResultPtr result);
  void NativeCreateBrowserWindow(gin::Arguments* args);
  void NativeLoadBrowserWindowURL(gin::Arguments* args);
  void NativeSetBrowserWindowVisible(gin::Arguments* args);
  void NativeBrowserWindowCall(gin::Arguments* args);
  void NativeBrowserWindowCallAsync(gin::Arguments* args);
  bool AddNativeRuntimeArguments(int32_t window_id,
                                 const std::string& command,
                                 base::Value* arguments,
                                 std::string* error);
  void OnBrowserWindowCallComplete(uint64_t id,
                                   base::Value result,
                                   std::string error);
  void NativeCloseBrowserWindow(gin::Arguments* args);
  void NativeExitApp(gin::Arguments* args);
  void NativeCanExitApp(gin::Arguments* args);
  void NotifyAppExit(int exit_code);
  void NativeInvokeExport(gin::Arguments* args);
  void NativeDescribeExport(gin::Arguments* args);
  void NativeConstructExport(gin::Arguments* args);
  void NativeInvokeInstance(gin::Arguments* args);
  v8::MaybeLocal<v8::Value> CreateNativeAddonForwarder(
      const std::string& module_path);
  static void NativeSetTimeoutCallback(
      const v8::FunctionCallbackInfo<v8::Value>& info);
  static void NativeSetIntervalCallback(
      const v8::FunctionCallbackInfo<v8::Value>& info);
  static void NativeClearTimerCallback(
      const v8::FunctionCallbackInfo<v8::Value>& info);
  void NativeSetTimerReferenced(int id, bool referenced);

  void HandleSetTimer(const v8::FunctionCallbackInfo<v8::Value>& info,
                      bool is_interval);
  void HandleClearTimer(const v8::FunctionCallbackInfo<v8::Value>& info);
  void OnTimerTriggered(int id);
  void CompletePromise(PromiseReplyContext* reply,
                       bool success,
                       v8::Local<v8::Value> value);
  void FailPendingPromisesForEndpoint(const std::string& endpoint_id,
                                      const std::string& error);
  void FailAllPendingPromises(const std::string& error);

  static void OnPromiseResolved(
      const v8::FunctionCallbackInfo<v8::Value>& info);
  static void OnPromiseRejected(
      const v8::FunctionCallbackInfo<v8::Value>& info);

  xenon::ipc::mojom::IpcResultPtr ErrorResult(const std::string& error) const;

  std::unique_ptr<gin::IsolateHolder> isolate_holder_;
  scoped_refptr<AsyncContextHooks> async_context_hooks_;
  std::optional<WorkerOptions> worker_options_;
  std::unique_ptr<WorkerMessage> worker_data_;
  std::unique_ptr<v8::Isolate::CreateParams> worker_params_;
  raw_ptr<WorkerControl> worker_control_ = nullptr;
  std::unique_ptr<WorkerEngine> worker_engine_;
  std::unique_ptr<MessagePortRuntime> message_ports_;
  std::optional<UtilityRuntimeOptions> utility_options_;
  UtilityProcessHooks utility_process_hooks_;
  struct UtilityProcessState : mojom::UtilityProcessClient {
    UtilityProcessState(base::WeakPtr<XenonIpcMainContainer> owner, int id);
    ~UtilityProcessState() override;
    void OnSpawn(int32_t pid) override;
    void OnExit(int32_t exit_code, const std::string& reason) override;
    void OnError(const std::string& error) override;
    base::WeakPtr<XenonIpcMainContainer> owner;
    int id;
    mojo::Remote<mojom::UtilityProcessControl> control;
    mojo::Receiver<mojom::UtilityProcessClient> receiver{this};
    std::map<int, v8::Global<v8::Promise::Resolver>> reads;
  };
  std::map<int, std::unique_ptr<UtilityProcessState>> utility_processes_;
  int next_utility_process_id_ = 1;
  v8::Global<v8::Function> dispatch_worker_event_;
  v8::Global<v8::Function> shutdown_workers_;
  struct WorkerRejection {
    v8::Global<v8::Promise> promise;
    v8::Global<v8::Value> reason;
  };
  std::map<uint64_t, WorkerRejection> worker_rejections_;
  uint64_t next_worker_rejection_id_ = 0;
  ElectronCrashReporterAnnotations crash_reporter_annotations_{true};
  raw_ptr<v8::Isolate> isolate_ = nullptr;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Function> dispatch_send_;
  v8::Global<v8::Function> dispatch_invoke_;
  v8::Global<v8::Function> dispatch_protocol_;
  v8::Global<v8::Function> cancel_protocol_;
  v8::Global<v8::Function> dispatch_web_request_;
  v8::Global<v8::Function> cancel_web_request_;
  v8::Global<v8::Function> release_web_request_registration_;
  v8::Global<v8::Function> shutdown_web_requests_;
  v8::Global<v8::Function> dispatch_electron_net_event_;
  v8::Global<v8::Function> shutdown_electron_net_;
  v8::Global<v8::Function> dispatch_sync_;
  v8::Global<v8::Function> dispatch_window_event_;
  v8::Global<v8::Function> dispatch_app_event_;
  v8::Global<v8::Function> dispatch_renderer_event_;
  v8::Global<v8::Function> mark_app_ready_;
  v8::Global<v8::Function> shutdown_app_;
  std::unique_ptr<XenonModuleResolver> module_resolver_;
  std::unique_ptr<XenonEsmModuleLoader> esm_loader_;
  bool main_evaluation_pending_ = false;
  bool requiring_esm_ = false;
  bool app_ready_requested_ = false;
  base::RepeatingClosure main_evaluation_complete_handler_;

  std::map<std::string, v8::Global<v8::Value>> module_cache_;
  struct HookedModuleSource {
    XenonEsmModuleLoader::ModuleInfo info;
    std::string source;
  };
  bool has_resolve_hooks_ = false;
  bool has_load_hooks_ = false;
  std::map<std::string, HookedModuleSource> hooked_module_sources_;
  // Active require calls retain the exact parent record, including synthetic
  // createRequire parents and application changes to their lookup paths.
  std::vector<std::pair<base::FilePath, v8::Global<v8::Object>>>
      commonjs_parent_stack_;
  // Default lookup only. The key includes the current ordered search paths;
  // application overrides never reuse an earlier resolution.
  std::map<std::pair<std::string, std::string>, std::string>
      module_resolution_cache_;
  std::map<std::string, std::unique_ptr<xenon::LoadedNodeAddon>>
      loaded_node_addons_;
  // Registration can schedule callbacks before throwing. Keep these failed
  // environments and their libraries until the same shutdown barrier passes.
  std::vector<std::unique_ptr<xenon::LoadedNodeAddon>> failed_node_addons_;
#if BUILDFLAG(ENABLE_XENON_NODE_UV_COMPAT)
  std::unique_ptr<xenon::NativeAddonEventLoop> native_event_loop_;
  base::OneShotTimer native_event_loop_timer_;
  bool native_event_loop_pumping_ = false;
  bool native_loop_draining_ = false;
#endif
  bool native_shutdown_complete_ = false;
  std::unique_ptr<XenonUdpSocketBridge> udp_sockets_;
  std::string native_shutdown_error_;
  std::map<std::string, RendererEndpoint> renderers_;
  std::set<PromiseReplyContext*> pending_promise_replies_;
  // Only accessed on the V8 sequence. Workers hold plain data and a weak reply,
  // never a V8 handle or a pointer into the container.
  std::map<uint64_t, v8::Global<v8::Promise::Resolver>> pending_fs_calls_;
  uint64_t next_fs_request_id_ = 1;
  std::map<uint64_t, v8::Global<v8::Promise::Resolver>> pending_window_calls_;
  uint64_t next_window_call_id_ = 1;
  std::map<uint64_t, v8::Global<v8::Promise::Resolver>> pending_zlib_calls_;
  uint64_t next_zlib_request_id_ = 1;
  struct ZlibStreamEntry {
    std::string owner;
    scoped_refptr<ZlibStream> stream;
  };
  std::map<int, ZlibStreamEntry> zlib_streams_;
  int next_zlib_stream_id_ = 1;
  struct HttpRequestInfo {
    v8::Global<v8::Promise::Resolver> resolver;
    base::OnceClosure cancel;
  };
  scoped_refptr<network::SharedURLLoaderFactory> network_loader_factory_;
  std::unique_ptr<ElectronNetEngine> electron_net_;
  ElectronNetEngine::OnlineProvider electron_net_online_;
  std::map<uint64_t, HttpRequestInfo> pending_http_requests_;
  uint64_t next_http_request_id_ = 1;
  int next_timer_id_ = 1;
  std::map<int, std::unique_ptr<TimerInfo>> timers_;

  base::FilePath main_script_path_;
  base::FilePath module_root_;
  base::FilePath app_path_;
  base::FilePath executable_path_;
  base::FilePath resources_directory_;
  base::FilePath working_directory_;
  std::vector<std::string> app_argv_;
  std::unique_ptr<XenonAppPaths> app_paths_;
  scoped_refptr<XenonFileDescriptors> file_descriptors_;
  base::FilePath host_user_data_path_;
  base::FilePath host_crash_dumps_path_;
  bool is_packaged_ = false;
  std::optional<std::string> embedded_main_source_;
  NativeAddonHooks native_addon_hooks_;
  WindowHooks window_hooks_;
  base::RepeatingCallback<void(int)> app_exit_handler_;
  base::RepeatingCallback<void(const std::string&, base::Value)>
      net_pipe_sender_;
  base::RepeatingCallback<base::Value(const base::DictValue&)>
      child_process_caller_;
  std::string app_name_ = "Application";
  std::string app_version_ = "0.0.0";
  std::string default_user_agent_;
  std::vector<std::pair<std::string, std::string>> renderer_url_mappings_;
  std::string renderer_base_url_;
  uint64_t next_endpoint_id_ = 1;
  bool initialized_ = false;
  bool shutting_down_ = false;
  bool app_exit_requested_ = false;
  std::string startup_error_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<XenonIpcMainContainer> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_IPC_MAIN_CONTAINER_H_
