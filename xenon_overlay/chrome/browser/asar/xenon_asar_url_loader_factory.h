// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_ASAR_XENON_ASAR_URL_LOADER_FACTORY_H_
#define XENON_OVERLAY_CHROME_BROWSER_ASAR_XENON_ASAR_URL_LOADER_FACTORY_H_

#include "mojo/public/cpp/bindings/pending_remote.h"
#include "services/network/public/mojom/url_loader_factory.mojom-forward.h"

namespace xenon {

// Creates a file: URL loader factory that transparently reads standard
// Electron ASAR entries and delegates ordinary files to Chromium.
mojo::PendingRemote<network::mojom::URLLoaderFactory>
CreateAsarURLLoaderFactory();

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_ASAR_XENON_ASAR_URL_LOADER_FACTORY_H_
