// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NET_FACTORY_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_NET_FACTORY_H_

#include <string>

#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"
#include "xenon_overlay/public/mojom/xenon_ipc.mojom-forward.h"

namespace content {
class BrowserContext;
}
namespace xenon::ipc {
// Browser UI sequence. Caller authenticates container/generation before entry.
// The returned factory is bound to the real Session and includes its blocking
// webRequest handlers, including trusted response headers before CORS.
mojo::PendingRemote<network::mojom::URLLoaderFactory> CreateElectronNetFactory(
    content::BrowserContext* context,
    const std::string& container,
    const std::string& partition,
    std::string* error,
    mojo::PendingRemote<mojom::ElectronNetworkHeadersObserver> headers = {});
bool IsElectronNetworkOnline();
}  // namespace xenon::ipc
#endif
