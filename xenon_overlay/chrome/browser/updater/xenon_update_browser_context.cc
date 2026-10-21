// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/updater/xenon_update_browser_context.h"

#include "chrome/browser/browser_process.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

namespace xenon::updater {

scoped_refptr<network::SharedURLLoaderFactory> GetUpdateURLLoaderFactory() {
  if (g_browser_process &&
      g_browser_process->system_network_context_manager()) {
    return g_browser_process->system_network_context_manager()
        ->GetSharedURLLoaderFactory();
  }
  return nullptr;
}

void ExitBrowserForUpdate() {
  chrome::ExitIgnoreUnloadHandlers();
}

}  // namespace xenon::updater
