// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_TRACING_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_TRACING_H_

#include <string>

#include "base/functional/callback.h"
#include "base/values.h"

namespace xenon::ipc {

using ElectronTracingReply =
    base::OnceCallback<void(base::Value value, std::string error)>;

// UI sequence. Owner is Browser-authored container/generation, never supplied
// by application JS. Only the owner can stop its recording; unrelated Chrome
// or DevTools recordings are left alone.
void CallElectronTracing(const std::string& owner,
                         const base::Value& request,
                         ElectronTracingReply reply);
void CloseElectronTracing(const std::string& owner);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_ELECTRON_TRACING_H_
