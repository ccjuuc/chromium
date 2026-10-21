// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/scoped_refptr.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_browser_context.h"

// Stubs for browser process dependencies not needed in standalone updater unit
// tests.
namespace xenon::updater {
scoped_refptr<network::SharedURLLoaderFactory> GetUpdateURLLoaderFactory() {
  return nullptr;
}

namespace {
int g_update_exit_requests = 0;
}

int GetUpdateExitRequestsForTesting() {
  return g_update_exit_requests;
}

void ExitBrowserForUpdate() {
  ++g_update_exit_requests;
}

}  // namespace xenon::updater
