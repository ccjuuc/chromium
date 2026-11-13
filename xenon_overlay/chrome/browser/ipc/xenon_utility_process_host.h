// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_UTILITY_PROCESS_HOST_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_UTILITY_PROCESS_HOST_H_

#include <array>
#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/process/process.h"
#include "base/timer/timer.h"
#include "content/public/browser/browser_child_process_observer.h"
#include "content/public/browser/service_process_host.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_net.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_utility_process_pipe.h"
#include "xenon_overlay/public/mojom/xenon_utility_process.mojom.h"

namespace xenon::ipc {

// Browser-side owner of one dedicated Electron utilityProcess. Its lifetime is
// subordinate to the authenticated main-service connection, never a renderer.
class XenonUtilityProcessHost final
    : public mojom::UtilityProcessControl,
      public mojom::UtilityProcessServiceObserver,
      public content::ServiceProcessHost::Observer,
      public content::BrowserChildProcessObserver {
 public:
  using StartCallback = base::OnceCallback<void(bool, const std::string&)>;
  XenonUtilityProcessHost(
      mojo::PendingReceiver<mojom::UtilityProcessControl> control,
      mojo::PendingRemote<mojom::UtilityProcessClient> client,
      ElectronNetEngine::FactoryProvider network_factory,
      ElectronNetEngine::OnlineProvider network_online,
      base::OnceClosure disconnected);
  ~XenonUtilityProcessHost() override;

  void Start(
      mojom::IpcMainConfigPtr application,
      mojom::UtilityProcessOptionsPtr options,
      blink::MessagePortDescriptor parent_port,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> network_factory,
      StartCallback callback);

 private:
  struct PreparedLaunch;
  static std::unique_ptr<PreparedLaunch> Prepare(
      mojom::IpcMainConfigPtr application,
      mojom::UtilityProcessOptionsPtr options,
      blink::MessagePortDescriptor parent_port,
      mojo::PendingRemote<network::mojom::URLLoaderFactory> network_factory);
  void Launch(std::unique_ptr<PreparedLaunch> prepared);
  void OnStarted(bool success, const std::string& error);
  void OnServiceDisconnected();
  void OnParentDisconnected();
  void Finish(int code, const std::string& reason);
  void ObserveExit(const content::ChildProcessData& data,
                   const content::ChildProcessTerminationInfo& info,
                   const char* reason);

  void Kill(KillCallback callback) override;
  void ReadOutput(int32_t fd,
                  uint32_t maximum_bytes,
                  ReadOutputCallback callback) override;
  void CloseOutput(int32_t fd) override;
  void OnExitRequested(int32_t exit_code) override;
  void CreateNetworkFactory(
      const std::string& partition,
      mojo::PendingRemote<mojom::ElectronNetworkHeadersObserver> headers,
      CreateNetworkFactoryCallback callback) override;
  void GetNetworkOnline(GetNetworkOnlineCallback callback) override;
  void OnServiceProcessLaunched(
      const content::ServiceProcessInfo& info) override;
  void BrowserChildProcessCrashed(
      const content::ChildProcessData& data,
      const content::ChildProcessTerminationInfo& info) override;
  void BrowserChildProcessKilled(
      const content::ChildProcessData& data,
      const content::ChildProcessTerminationInfo& info) override;
  void BrowserChildProcessExitedNormally(
      const content::ChildProcessData& data,
      const content::ChildProcessTerminationInfo& info) override;

  mojo::Receiver<mojom::UtilityProcessControl> control_{this};
  mojo::PendingReceiver<mojom::UtilityProcessControl> pending_control_;
  mojo::Remote<mojom::UtilityProcessClient> client_;
  mojo::Receiver<mojom::UtilityProcessServiceObserver> observer_{this};
  mojo::Remote<mojom::UtilityProcessService> service_;
  std::array<scoped_refptr<UtilityProcessPipe>, 2> output_;
  ElectronNetEngine::FactoryProvider network_factory_;
  ElectronNetEngine::OnlineProvider network_online_;
  base::Process process_;
  base::OnceClosure disconnected_;
  StartCallback start_callback_;
  base::OneShotTimer startup_timeout_;
  bool finished_ = false;
  bool exit_requested_ = false;
  bool cancelled_ = false;
  bool prepare_failed_ = false;
  base::WeakPtrFactory<XenonUtilityProcessHost> weak_factory_{this};
};

}  // namespace xenon::ipc
#endif
