// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_STORAGE_SERVICE_CONTEXT_H_
#define CONTENT_PUBLIC_BROWSER_STORAGE_SERVICE_CONTEXT_H_

#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include "base/functional/callback.h"
#include "content/common/content_export.h"

namespace mojo {
template <typename Interface>
class Remote;
}
namespace storage::mojom {
class StorageService;
}
namespace content {
class BrowserContext;

// UI sequence only, before any StoragePartition is created. Gives this context
// a separate sandboxed Storage Service whose filesystem broker is restricted
// to the context's own directory. Unmarked contexts keep the embedder service.
CONTENT_EXPORT void EnableDedicatedStorageServiceForBrowserContext(
    BrowserContext* context);

CONTENT_EXPORT mojo::Remote<storage::mojom::StorageService>&
GetStorageServiceForBrowserContext(BrowserContext* context);

// Invoke the returned closure after the context has been destroyed. It calls
// `callback` on this sequence after the context's dedicated service processes
// have reported termination. Pending launches retain the barrier until launch
// succeeds or fails. Uses service lifecycle notifications without blocking UI;
// contexts which use the shared service return the original callback unchanged.
CONTENT_EXPORT base::OnceClosure MakeStorageServiceShutdownClosure(
    BrowserContext* context,
    base::OnceClosure callback);
}  // namespace content
#endif

#endif  // CONTENT_PUBLIC_BROWSER_STORAGE_SERVICE_CONTEXT_H_
