// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_SERVICE_WORKER_RENDERER_H_
#define XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_SERVICE_WORKER_RENDERER_H_

#include <map>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "gin/arguments.h"
#include "mojo/public/cpp/bindings/associated_remote.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "v8/include/v8-persistent-handle.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace blink {
class WebServiceWorkerContextProxy;
}

namespace xenon::ipc {

class XenonContextBridge;

// Lives entirely on one worker thread. The worker's main world never receives
// require/process or the transport; app-authorized scripts run in a separate
// context and explicitly expose APIs through copying/function proxies.
class XenonServiceWorkerRenderer final
    : public mojom::IpcRenderer,
      public base::RefCounted<XenonServiceWorkerRenderer> {
 public:
  static void Install(blink::WebServiceWorkerContextProxy* context_proxy,
                      v8::Local<v8::Context> main_context);
  static void Destroy();
  explicit XenonServiceWorkerRenderer(v8::Local<v8::Context> main_context);

 private:
  friend class base::RefCounted<XenonServiceWorkerRenderer>;
  ~XenonServiceWorkerRenderer() override;
  bool Initialize(blink::WebServiceWorkerContextProxy* context_proxy);
  bool InstallScheduler(v8::Local<v8::Context> main,
                        v8::Local<v8::Context> preload);
  void Shutdown();
  void OnDisconnected();
  bool RunScript(v8::Local<v8::Context> context,
                 const std::string& source,
                 const std::string& path);
  void Send(gin::Arguments* args);
  void Invoke(gin::Arguments* args);
  bool ReadArguments(gin::Arguments* args,
                     std::string* channel,
                     base::Value* arguments);
  void OnInvoke(uint64_t id, mojom::IpcResultPtr result);
  void SetDispatchHandler(gin::Arguments* args);
  void Dispatch(const std::string& channel, base::Value arguments) override;
  void ThrowError(const std::string& message);
  v8::Local<v8::Function> BindFunction(auto method, auto... bound_args);

  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> main_context_;
  v8::Global<v8::Context> preload_context_;
  mojo::AssociatedRemote<mojom::ServiceWorkerIpcHost> host_;
  mojo::Receiver<mojom::IpcRenderer> receiver_{this};
  v8::Global<v8::Function> dispatch_handler_;
  v8::Global<v8::Function> revoke_scheduler_;
  std::map<uint64_t, v8::Global<v8::Promise::Resolver>> pending_invokes_;
  scoped_refptr<XenonContextBridge> bridge_;
  uint64_t next_id_ = 1;
  bool closed_ = false;
  base::WeakPtrFactory<XenonServiceWorkerRenderer> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_SERVICE_WORKER_RENDERER_H_
