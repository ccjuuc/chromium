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
#include <vector>

#include "base/files/file_path.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "gin/public/isolate_holder.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace gin {
class Arguments;
}

namespace xenon {
struct LoadedNodeAddon;
}

namespace xenon::ipc {

// Utility-process owner of the Electron-compatible main JavaScript context.
// Its lifetime is independent of every renderer Document, so ipcMain handlers
// and JavaScript globals survive renderer navigation and crashes.
class XenonIpcMainContainer {
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
                                 const std::string& function_name,
                                 const base::Value& args,
                                 base::Value* result,
                                 std::string* error)>
        invoke;
    base::RepeatingCallback<bool(const std::string& path,
                                 const std::string& export_path,
                                 const base::Value& args,
                                 int32_t* instance_id,
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
    base::RepeatingCallback<void(int32_t window_id)> close;
    base::RepeatingCallback<bool(const std::string& title,
                                 bool directory,
                                 bool allow_multi,
                                 const std::vector<std::string>& extensions,
                                 std::vector<std::string>* paths)>
        show_open_dialog;
  };

  XenonIpcMainContainer();
  ~XenonIpcMainContainer();

  XenonIpcMainContainer(const XenonIpcMainContainer&) = delete;
  XenonIpcMainContainer& operator=(const XenonIpcMainContainer&) = delete;

  bool Initialize();
  bool Initialize(EmbeddedMainModule main_module);
  void SetNativeAddonHooks(NativeAddonHooks hooks);
  void SetWindowHooks(WindowHooks hooks);
  void MarkAppReady();
  void Shutdown();
  bool is_initialized() const { return initialized_; }
  const std::string& startup_error() const { return startup_error_; }

  std::string AddRenderer(
      mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer,
      int32_t process_id,
      int32_t frame_id,
      int32_t window_id = 0);
  void AddRenderer(
      const std::string& endpoint_id,
      mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer,
      int32_t process_id,
      int32_t frame_id,
      int32_t window_id = 0);
  void RemoveRenderer(const std::string& endpoint_id);
  void DispatchToRenderer(const std::string& endpoint_id,
                          const std::string& channel,
                          base::Value arguments);
  void DispatchWindowEvent(int32_t window_id,
                           const std::string& event_name,
                           base::Value arguments);

  void Send(const std::string& endpoint_id,
            const std::string& channel,
            base::Value arguments);
  void Invoke(const std::string& endpoint_id,
              const std::string& channel,
              base::Value arguments,
              InvokeCallback callback);
  xenon::ipc::mojom::IpcResultPtr SendSync(const std::string& endpoint_id,
                                           const std::string& channel,
                                           base::Value arguments);
  // Executes the privileged backend for renderer node:fs compatibility. The
  // renderer keeps Chromium's sandbox; Electron-compatible sync/callback/
  // promise APIs are layered over this operation in the renderer bootstrap.
  xenon::ipc::mojom::IpcResultPtr FileSystemCall(base::Value arguments);

 private:
  struct PromiseReplyContext;
  struct RendererEndpoint;
  struct TimerInfo {
    int id = 0;
    bool is_interval = false;
    int delay_ms = 0;
    v8::Global<v8::Function> callback;
    std::vector<v8::Global<v8::Value>> args;
  };

  bool InitializeInternal(std::optional<EmbeddedMainModule> main_module);
  bool RunBootstrap();
  bool ResolveConfiguredMainScript();
  bool MaybeLoadConfiguredMainScript();
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
  v8::MaybeLocal<v8::Value> RequireModule(const std::string& request,
                                          const base::FilePath& parent_file,
                                          std::string* error);

  v8::MaybeLocal<v8::Value> ValueToV8(const base::Value& value);
  bool V8ToValue(v8::Local<v8::Value> value,
                 base::Value* output,
                 std::string* error);
  v8::Local<v8::Object> CreateSenderMetadata(const std::string& endpoint_id);
  void DispatchRendererEvent(const std::string& endpoint_id, bool attached);

  void NativeLog(gin::Arguments* args);
  void NativeGetPath(gin::Arguments* args);
  void NativeNetworkInterfaces(gin::Arguments* args);
  void NativeSendToRenderer(gin::Arguments* args);
  void NativeFsExists(gin::Arguments* args);
  void NativeFsReadFile(gin::Arguments* args);
  void NativeFsWriteFile(gin::Arguments* args);
  void NativeFsStat(gin::Arguments* args);
  void NativeFsReaddir(gin::Arguments* args);
  void NativeFsMkdir(gin::Arguments* args);
  void NativeFsUnlink(gin::Arguments* args);
  void NativeCryptoCipher(gin::Arguments* args);
  void NativeShowOpenDialog(gin::Arguments* args);
  void NativeCreateBrowserWindow(gin::Arguments* args);
  void NativeLoadBrowserWindowURL(gin::Arguments* args);
  void NativeSetBrowserWindowVisible(gin::Arguments* args);
  void NativeBrowserWindowCall(gin::Arguments* args);
  void NativeCloseBrowserWindow(gin::Arguments* args);
  void NativeInvokeExport(gin::Arguments* args);
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

  void HandleSetTimer(const v8::FunctionCallbackInfo<v8::Value>& info,
                      bool is_interval);
  void HandleClearTimer(const v8::FunctionCallbackInfo<v8::Value>& info);
  void OnTimerTriggered(int id);
  void CompletePromise(PromiseReplyContext* reply,
                       bool success,
                       v8::Local<v8::Value> value);
  void FailAllPendingPromises(const std::string& error);

  static void OnPromiseResolved(
      const v8::FunctionCallbackInfo<v8::Value>& info);
  static void OnPromiseRejected(
      const v8::FunctionCallbackInfo<v8::Value>& info);

  xenon::ipc::mojom::IpcResultPtr ErrorResult(const std::string& error) const;

  std::unique_ptr<gin::IsolateHolder> isolate_holder_;
  raw_ptr<v8::Isolate> isolate_ = nullptr;
  v8::Global<v8::Context> context_;
  v8::Global<v8::Function> dispatch_send_;
  v8::Global<v8::Function> dispatch_invoke_;
  v8::Global<v8::Function> dispatch_sync_;
  v8::Global<v8::Function> dispatch_window_event_;
  v8::Global<v8::Function> dispatch_renderer_event_;
  v8::Global<v8::Function> mark_app_ready_;
  v8::Global<v8::Function> shutdown_app_;

  std::map<std::string, v8::Global<v8::Value>> module_cache_;
  std::map<std::string, std::unique_ptr<xenon::LoadedNodeAddon>>
      loaded_node_addons_;
  std::map<std::string, RendererEndpoint> renderers_;
  std::set<PromiseReplyContext*> pending_promise_replies_;
  int next_timer_id_ = 1;
  std::map<int, std::unique_ptr<TimerInfo>> timers_;

  base::FilePath main_script_path_;
  base::FilePath module_root_;
  base::FilePath app_path_;
  base::FilePath executable_path_;
  std::optional<std::string> embedded_main_source_;
  NativeAddonHooks native_addon_hooks_;
  WindowHooks window_hooks_;
  std::string app_name_ = "Application";
  std::string app_version_ = "0.0.0";
  std::string default_user_agent_;
  std::vector<std::pair<std::string, std::string>> renderer_url_mappings_;
  std::string renderer_base_url_;
  uint64_t next_endpoint_id_ = 1;
  bool initialized_ = false;
  bool shutting_down_ = false;
  std::string startup_error_;

  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<XenonIpcMainContainer> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_IPC_MAIN_CONTAINER_H_
