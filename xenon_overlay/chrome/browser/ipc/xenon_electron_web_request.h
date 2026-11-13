// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_WEB_REQUEST_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_WEB_REQUEST_H_

#include <cstdint>
#include <string>

#include "base/functional/callback.h"
#include "base/values.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/network_context.mojom-forward.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"

namespace content {
class BrowserContext;
class StoragePartition;
}  // namespace content
namespace network {
class URLLoaderFactoryBuilder;
}

namespace xenon::ipc {
using ElectronWebRequestReply =
    base::OnceCallback<void(base::Value, const std::string& error)>;
using ElectronWebRequestDispatch = base::RepeatingCallback<
    void(uint64_t generation, base::Value request, ElectronWebRequestReply)>;
using ElectronWebRequestCancel =
    base::RepeatingCallback<void(uint64_t generation, const std::string& id)>;
using ElectronWebRequestRelease = ElectronWebRequestCancel;

// UI sequence only. The caller must authenticate container and generation.
// Registration wire: {partition,event,registrationId,enabled,filter}.
bool SetElectronWebRequestHandler(content::BrowserContext* context,
                                  const std::string& container,
                                  uint64_t generation,
                                  const base::Value& arguments,
                                  ElectronWebRequestDispatch dispatch,
                                  ElectronWebRequestCancel cancel,
                                  ElectronWebRequestRelease release,
                                  std::string* error);
void RemoveElectronWebRequestHandlers(const std::string& container);

// Attach even if no listener exists yet: a Session may register after its
// renderer has received the factory. Unknown/non-Electron partitions are left
// alone. Existing extensions/DevTools header clients are chained, never lost.
bool AppendElectronWebRequestProxy(
    content::BrowserContext* context,
    content::StoragePartition* partition,
    network::URLLoaderFactoryBuilder& builder,
    bool is_for_network_service,
    mojo::PendingRemote<network::mojom::TrustedURLLoaderHeaderClient>*
        header_client);

enum class ElectronWebRequestFactoryKind {
  kNonNetwork,
  // The custom protocol's CORS check runs in this proxy after modified headers.
  kCustomProtocol,
};
mojo::PendingRemote<network::mojom::URLLoaderFactory>
WrapElectronWebRequestFactory(
    content::BrowserContext* context,
    content::StoragePartition* partition,
    mojo::PendingRemote<network::mojom::URLLoaderFactory> target,
    ElectronWebRequestFactoryKind kind =
        ElectronWebRequestFactoryKind::kNonNetwork);
}  // namespace xenon::ipc
#endif
