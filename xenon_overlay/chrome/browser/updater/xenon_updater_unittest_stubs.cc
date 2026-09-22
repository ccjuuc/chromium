// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/scoped_refptr.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"

// Stubs for browser process dependencies not needed in standalone updater unit tests.
namespace chrome {
void ExitIgnoreUnloadHandlers() {}
}  // namespace chrome

class SystemNetworkContextManager {
 public:
  scoped_refptr<network::SharedURLLoaderFactory> GetSharedURLLoaderFactory();
};

scoped_refptr<network::SharedURLLoaderFactory>
SystemNetworkContextManager::GetSharedURLLoaderFactory() {
  return nullptr;
}
