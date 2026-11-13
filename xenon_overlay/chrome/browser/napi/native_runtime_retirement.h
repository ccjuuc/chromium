// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_NAPI_NATIVE_RUNTIME_RETIREMENT_H_
#define XENON_OVERLAY_CHROME_BROWSER_NAPI_NATIVE_RUNTIME_RETIREMENT_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/time/time.h"

namespace xenon {

// The complete object containing native JS bindings, environments, libraries,
// context and isolate. Retaining just an isolate does not retain the `this`
// pointers captured by its bindings.
class NativeRuntimeShutdown {
 public:
  virtual ~NativeRuntimeShutdown() = default;
  virtual void BeginNativeShutdown() = 0;
  // Nonblocking. Returns true only after all native callbacks/requests have
  // drained and the runtime has completed its final teardown outside V8 scopes.
  virtual bool PollNativeShutdown() = 0;
  virtual std::string DescribeNativeShutdown() const = 0;
};

struct NativeRuntimeRetirementOptions {
  base::TimeDelta poll_interval = base::Milliseconds(10);
  base::TimeDelta deadline = base::Seconds(30);
  // Required. A Utility owner should terminate its isolated process if native
  // resources cannot drain. Returning does NOT authorize freeing busy state;
  // polling continues and this notification is delivered at most once.
  base::OnceCallback<void(std::string)> on_stalled;
  // Released after runtime destruction, normally a process keepalive token.
  // The caller acquires that token before handing ownership to retirement.
  base::OnceClosure on_complete;
};

// Takes ownership immediately and starts on the next task, so a call initiated
// by a native/JS callback cannot destroy its own stack. No nested run loop,
// thread join, forced uv_close, or off-sequence V8 disposal is performed.
void RetireNativeRuntime(std::unique_ptr<NativeRuntimeShutdown> runtime,
                         NativeRuntimeRetirementOptions options);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_NAPI_NATIVE_RUNTIME_RETIREMENT_H_
