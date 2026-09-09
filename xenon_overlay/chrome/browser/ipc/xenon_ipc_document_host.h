// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_IPC_DOCUMENT_HOST_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_IPC_DOCUMENT_HOST_H_

#include <cstdint>
#include <string>

#include "base/memory/weak_ptr.h"
#include "base/threading/sequence_bound.h"
#include "base/unguessable_token.h"
#include "content/public/browser/document_service.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace content {
class RenderFrameHost;
}

namespace xenon::ipc {

class XenonSqliteBridge;

// Per-Document Browser endpoint. It is automatically destroyed before a
// cross-document navigation commits, while XenonIpcMainContainer remains
// alive in the Utility process.
class XenonIpcDocumentHost final
    : public content::DocumentService<xenon::ipc::mojom::IpcHost> {
 public:
  static void Create(
      content::RenderFrameHost* render_frame_host,
      mojo::PendingReceiver<xenon::ipc::mojom::IpcHost> receiver);

  XenonIpcDocumentHost(
      content::RenderFrameHost& render_frame_host,
      mojo::PendingReceiver<xenon::ipc::mojom::IpcHost> receiver);
  ~XenonIpcDocumentHost() override;

  XenonIpcDocumentHost(const XenonIpcDocumentHost&) = delete;
  XenonIpcDocumentHost& operator=(const XenonIpcDocumentHost&) = delete;

 private:
  // xenon::ipc::mojom::IpcHost:
  void AttachGuest(const base::UnguessableToken& frame_token,
                   base::Value preferences,
                   AttachGuestCallback callback) override;
  void OnGuestFramePrepared(base::DictValue preferences,
                            AttachGuestCallback callback,
                            content::RenderFrameHost* frame);
  void GetRuntimeConfig(GetRuntimeConfigCallback callback) override;
  void BindRenderer(
      mojo::PendingRemote<xenon::ipc::mojom::IpcRenderer> renderer) override;
  void BindNodeAddonHost(
      mojo::PendingReceiver<xenon::ipc::mojom::NodeAddonHost> receiver)
      override;
  void Send(const std::string& channel, base::Value arguments) override;
  void Invoke(const std::string& channel,
              base::Value arguments,
              InvokeCallback callback) override;
  void SendSync(const std::string& channel,
                base::Value arguments,
                SendSyncCallback callback) override;
  bool IsAllowedDocument() const;
  std::string ResolveContainerId() const;
  xenon::ipc::mojom::IpcResultPtr UnavailableResult() const;

  std::string endpoint_id_;
  std::string container_id_;
  base::SequenceBound<XenonSqliteBridge> sqlite_bridge_;
  base::WeakPtrFactory<XenonIpcDocumentHost> weak_factory_{this};
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_IPC_DOCUMENT_HOST_H_
