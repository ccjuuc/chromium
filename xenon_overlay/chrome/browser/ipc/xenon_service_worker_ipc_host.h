// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_SERVICE_WORKER_IPC_HOST_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_SERVICE_WORKER_IPC_HOST_H_

#include <string>

#include "content/public/browser/service_worker_version_base_info.h"
#include "mojo/public/cpp/bindings/pending_associated_receiver.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom.h"

namespace content {
class BrowserContext;
}

namespace xenon::ipc {

// Every pipe belongs to the service-worker version identified by Chromium's
// associated interface registry. Worker JavaScript cannot select an app or
// partition.
class XenonServiceWorkerIpcHost final : public mojom::ServiceWorkerIpcHost {
 public:
  static void Create(
      content::BrowserContext* browser_context,
      const content::ServiceWorkerVersionBaseInfo& worker,
      mojo::PendingAssociatedReceiver<mojom::ServiceWorkerIpcHost> receiver);
  explicit XenonServiceWorkerIpcHost(
      const content::ServiceWorkerVersionBaseInfo& worker);
  ~XenonServiceWorkerIpcHost() override;

 private:
  void Initialize(mojo::PendingRemote<mojom::IpcRenderer> renderer,
                  InitializeCallback callback) override;
  void Send(const std::string& channel, base::Value arguments) override;
  void Invoke(const std::string& channel,
              base::Value arguments,
              InvokeCallback callback) override;
  bool IsAvailable(const std::string& channel) const;

  const content::ServiceWorkerVersionBaseInfo worker_;
  std::string container_id_;
  std::string partition_;
  std::string endpoint_id_;
  uint64_t service_generation_ = 0;
};

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_SERVICE_WORKER_IPC_HOST_H_
