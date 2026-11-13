// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_NAPI_NATIVE_ADDON_EVENT_LOOP_H_
#define XENON_OVERLAY_CHROME_BROWSER_NAPI_NATIVE_ADDON_EVENT_LOOP_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/function_ref.h"
#include "base/memory/raw_ptr_exclusion.h"
#include "base/sequence_checker.h"
#include "base/types/expected.h"

struct uv_loop_s;
struct uv_idle_s;

namespace xenon {

// One libuv loop for one native JavaScript runtime. Standard runtimes own an
// independent loop; legacy runtimes exclusively lease the default loop.
//
// The owner must enter the loop's V8 isolate/context before Pump() and
// PollShutdown(), keep all addon environments and libraries alive throughout
// draining, and stop new native calls before BeginShutdown(). Cleanup hooks
// run with the environment still alive before polling. This class does not
// invent close callbacks for handles owned by an addon.
//
// A false PollShutdown() result is a lifecycle barrier, not permission to free
// the loop. Retain the entire owning runtime until it returns true. A deadline
// may terminate its isolated Utility process; freeing busy state is unsafe.
class NativeAddonEventLoop {
 public:
  struct HandleState {
    std::string type;
    bool active;
    bool referenced;
    bool closing;
  };

  static base::expected<std::unique_ptr<NativeAddonEventLoop>, std::string>
  Create();
  // Legacy addons may call uv_default_loop() themselves. Its callbacks cannot
  // be assigned to several V8 realms, so one runtime exclusively leases it.
  static base::expected<std::unique_ptr<NativeAddonEventLoop>, std::string>
  AcquireDefault();
  ~NativeAddonEventLoop();

  NativeAddonEventLoop(const NativeAddonEventLoop&) = delete;
  NativeAddonEventLoop& operator=(const NativeAddonEventLoop&) = delete;

  uv_loop_s* loop() const;
  // Active referenced handles, pending requests and closing callbacks keep a
  // running Worker alive. The internal pump guard is not application work.
  bool HasReferencedWork() const;
  // Nonblocking. Reentrant calls are rejected instead of reentering uv_run.
  base::expected<void, std::string> Pump();
  void BeginShutdown();
  base::expected<bool, std::string> PollShutdown();
  // Run after native callbacks and before closing the loop. The runtime uses
  // this barrier to finalize anything newly registered by those callbacks.
  base::expected<bool, std::string> PollShutdown(
      base::FunctionRef<bool()> callbacks_drained);
  bool is_closed() const;
  std::vector<HandleState> InspectHandles() const;

 private:
  enum class State { kRunning, kDraining, kClosed };
  NativeAddonEventLoop();
  base::expected<void, std::string> PumpInternal();
  void ClosePumpGuard();

  std::unique_ptr<uv_loop_s> owned_loop_;
  RAW_PTR_EXCLUSION uv_loop_s* loop_ = nullptr;
  bool owns_default_ = false;
  State state_ = State::kClosed;
  bool pumping_ = false;
  std::unique_ptr<uv_idle_s> pump_guard_;
  bool pump_guard_closed_ = false;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_NAPI_NATIVE_ADDON_EVENT_LOOP_H_
