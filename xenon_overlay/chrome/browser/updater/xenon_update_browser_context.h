// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_BROWSER_CONTEXT_H_
#define XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_BROWSER_CONTEXT_H_

#include "base/memory/scoped_refptr.h"

namespace network {
class SharedURLLoaderFactory;
}

namespace xenon::updater {

// Browser integration kept outside the updater so its state machine can be
// exercised without initializing the browser process and UI services.
scoped_refptr<network::SharedURLLoaderFactory> GetUpdateURLLoaderFactory();

// Requests normal browser shutdown after an installer has become ready.
void ExitBrowserForUpdate();

}  // namespace xenon::updater

#endif  // XENON_OVERLAY_CHROME_BROWSER_UPDATER_XENON_UPDATE_BROWSER_CONTEXT_H_
