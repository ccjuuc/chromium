// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_ASAR_XENON_ASAR_URL_LOADER_FACTORY_H_
#define XENON_OVERLAY_CHROME_BROWSER_ASAR_XENON_ASAR_URL_LOADER_FACTORY_H_

#include <cstdint>

#include "base/memory/scoped_refptr.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader.mojom-forward.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"

namespace net {
class HttpResponseHeaders;
struct MutableNetworkTrafficAnnotationTag;
}  // namespace net
namespace network {
struct ResourceRequest;
}

namespace xenon {

// Creates a file: URL loader factory that reads standard and configured
// encrypted ASAR entries and delegates ordinary files to Chromium. Must be
// called on UI. Both paths retain the default file origin/CORS policy; this
// factory does not grant cross-origin read access to local resources.
mojo::PendingRemote<network::mojom::URLLoaderFactory>
CreateAsarURLLoaderFactory();

// Streams a path explicitly returned by an authenticated Electron main-process
// protocol handler. `request.url` is the internal file URL; its public response
// URL, CORS and session checks remain owned by the protocol loader. This entry
// point must never be exposed to renderer requests as an unrestricted factory.
// Ordinary files and ASAR entries retain streaming and single-range reads.
void CreateAsarProtocolFileURLLoader(
    const network::ResourceRequest& request,
    int32_t request_id,
    uint32_t options,
    const net::MutableNetworkTrafficAnnotationTag& traffic_annotation,
    mojo::PendingReceiver<network::mojom::URLLoader> loader,
    mojo::PendingRemote<network::mojom::URLLoaderClient> client,
    scoped_refptr<net::HttpResponseHeaders> response_headers);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_ASAR_XENON_ASAR_URL_LOADER_FACTORY_H_
