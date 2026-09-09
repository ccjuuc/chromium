// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_IPC_RENDERER_H_
#define XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_IPC_RENDERER_H_

#include <string>
#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/renderer/render_frame_observer.h"
#include "gin/arguments.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "v8/include/v8-forward.h"
#include "v8/include/v8-persistent-handle.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace content {
class RenderFrame;
}

namespace xenon::ipc {

// Document-scoped JavaScript binding installed as window.xenonIpcRenderer.
// The Electron compatibility bootstrap uses this object as the transport for
// require('electron').ipcRenderer.
class XenonIpcRenderer final : public xenon::ipc::mojom::IpcRenderer,
                               public content::RenderFrameObserver,
                               public base::RefCounted<XenonIpcRenderer> {
 public:
  static void Install(content::RenderFrame* render_frame,
                      v8::Local<v8::Context> context);

  XenonIpcRenderer(content::RenderFrame* render_frame,
                   v8::Local<v8::Context> context);

  XenonIpcRenderer(const XenonIpcRenderer&) = delete;
  XenonIpcRenderer& operator=(const XenonIpcRenderer&) = delete;

  void Shutdown();

 private:
  friend class base::RefCounted<XenonIpcRenderer>;
  ~XenonIpcRenderer() override;

  void WillReleaseScriptContext(v8::Local<v8::Context> context,
                                int32_t world_id) override;
  void OnDestruct() override;

  // JavaScript API.
  void GetRuntimeConfig(gin::Arguments* args);
  void AttachGuest(gin::Arguments* args);
  void Send(gin::Arguments* args);
  void Invoke(gin::Arguments* args);
  void SendSync(gin::Arguments* args);
  void RequireNodeModuleSync(gin::Arguments* args);
  void InvokeNodeExportSync(gin::Arguments* args);
  void ConstructNodeExportSync(gin::Arguments* args);
  void InvokeNodeInstanceSync(gin::Arguments* args);
  void InspectNodeInstanceMemberSync(gin::Arguments* args);
  void PostMessage(gin::Arguments* args);
  void SetDispatchHandler(gin::Arguments* args);

  // xenon::ipc::mojom::IpcRenderer:
  void Dispatch(const std::string& channel, base::Value arguments) override;

  bool EnsureHostConnected();
  bool EnsureRuntimeConfig();
  bool EnsureConnected();
  bool EnsureNodeAddonConnected();
  bool ReadChannelAndArguments(gin::Arguments* args,
                               std::string* channel,
                               base::Value* arguments);
  void DispatchNow(const std::string& channel, const base::Value& arguments);
  void OnHostDisconnected();
  void OnInvoke(v8::Global<v8::Context> global_context,
                v8::Global<v8::Promise::Resolver> resolver_global,
                v8::Isolate* isolate,
                xenon::ipc::mojom::IpcResultPtr result);

  // JavaScript method wrappers retain this binding, even if another context
  // keeps an old wrapper alive. The document observer revokes the transport
  // before its context/frame disappears; wrappers cannot reconnect afterward.
  const raw_ptr<v8::Isolate> isolate_;
  v8::Global<v8::Context> context_;
  mojo::Remote<xenon::ipc::mojom::IpcHost> host_;
  xenon::ipc::mojom::IpcRendererConfigPtr runtime_config_;
  mojo::Remote<xenon::ipc::mojom::NodeAddonHost> node_addon_host_;
  mojo::Receiver<xenon::ipc::mojom::IpcRenderer> receiver_{this};
  v8::Global<v8::Function> dispatch_handler_;
  std::vector<std::pair<std::string, base::Value>> queued_events_;

  base::WeakPtrFactory<XenonIpcRenderer> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_RENDERER_IPC_XENON_IPC_RENDERER_H_
