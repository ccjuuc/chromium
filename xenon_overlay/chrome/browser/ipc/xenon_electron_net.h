// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NET_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NET_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom-forward.h"

namespace xenon::ipc {

// Utility-sequence HTTP(S) engine for Electron net/session.fetch. Browser
// supplies a factory for the authenticated container's actual Session. This
// deliberately does not reuse Node HTTP's proxy-bypassing request bridge.
class ElectronNetEngine {
 public:
  using FactoryCallback = base::OnceCallback<
      void(mojo::PendingRemote<network::mojom::URLLoaderFactory>, std::string)>;
  using FactoryProvider = base::RepeatingCallback<void(
      const std::string&,
      mojo::PendingRemote<mojom::ElectronNetworkHeadersObserver>,
      FactoryCallback)>;
  using EventCallback = base::RepeatingCallback<void(base::Value)>;
  using OnlineProvider = base::RepeatingCallback<bool(bool*, std::string*)>;

  ElectronNetEngine(FactoryProvider provider, EventCallback event);
  ~ElectronNetEngine();
  ElectronNetEngine(const ElectronNetEngine&) = delete;
  ElectronNetEngine& operator=(const ElectronNetEngine&) = delete;

  // Returns zero and a coded error on invalid/unsupported options. Events are
  // always posted, including factory errors, so JS can first record the ID.
  uint64_t Start(const base::DictValue& options, std::string* error);
  // Each response/data event consumes read credit. resume permits one next
  // chunk (at most 64 KiB); redirects wait for follow-redirect. abort silently
  // cancels the loader; JS owns its public abort/close events.
  bool Control(uint64_t id, const std::string& command, std::string* error);
  bool Auth(uint64_t id,
            uint64_t challenge,
            const std::optional<std::string>& username,
            const std::optional<std::string>& password);
  void Shutdown();

 private:
  class Request;
  void Begin(uint64_t id);
  void Emit(uint64_t id, base::DictValue event);
  void Remove(uint64_t id);

  FactoryProvider provider_;
  EventCallback event_;
  uint64_t next_id_ = 0;
  std::map<uint64_t, std::unique_ptr<Request>> requests_;
  base::WeakPtrFactory<ElectronNetEngine> weak_factory_{this};
};
}  // namespace xenon::ipc
#endif
