// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license.
#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PROTOCOL_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_PROTOCOL_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/values.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"

namespace content {
class BrowserContext;
class WebContents;
}  // namespace content
namespace xenon::ipc {
bool SetElectronProtocolHandler(content::BrowserContext* context,
                                const std::string& container_id,
                                uint64_t generation,
                                const base::Value& arguments,
                                std::string* error);
void RemoveElectronProtocolHandlers(const std::string& container_id);
std::vector<std::string> GetElectronProtocolSchemes(
    content::WebContents* contents);
mojo::PendingRemote<network::mojom::URLLoaderFactory>
CreateElectronProtocolURLLoaderFactory(content::WebContents* contents,
                                       const std::string& scheme);
}  // namespace xenon::ipc
#endif
