// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/napi/native_addon_event_loop.h"

#include <atomic>
#include <utility>

#include "base/auto_reset.h"
#include "base/check.h"
#include "xenon_overlay/third_party/libuv/include/uv.h"

namespace xenon {
namespace {
std::atomic<bool> g_default_loop_claimed{false};
}

NativeAddonEventLoop::NativeAddonEventLoop() = default;

base::expected<std::unique_ptr<NativeAddonEventLoop>, std::string>
NativeAddonEventLoop::Create() {
  auto owner =
      std::unique_ptr<NativeAddonEventLoop>(new NativeAddonEventLoop());
  owner->owned_loop_ = std::make_unique<uv_loop_t>();
  owner->loop_ = owner->owned_loop_.get();
  const int status = uv_loop_init(owner->loop_);
  if (status != 0) {
    return base::unexpected(std::string("uv_loop_init: ") +
                            uv_strerror(status));
  }
  owner->state_ = State::kRunning;
  return owner;
}

base::expected<std::unique_ptr<NativeAddonEventLoop>, std::string>
NativeAddonEventLoop::AcquireDefault() {
  bool expected = false;
  if (!g_default_loop_claimed.compare_exchange_strong(expected, true)) {
    return base::unexpected(
        "The legacy default libuv loop already belongs to another runtime");
  }
  uv_loop_t* loop = uv_default_loop();
  size_t handles = 0;
  if (loop) {
    uv_walk(
        loop, [](uv_handle_t*, void* data) { ++*static_cast<size_t*>(data); },
        &handles);
  }
  if (!loop || handles || uv_loop_alive(loop)) {
    g_default_loop_claimed.store(false);
    return base::unexpected(
        "The legacy default libuv loop has unowned native resources");
  }
  auto owner =
      std::unique_ptr<NativeAddonEventLoop>(new NativeAddonEventLoop());
  owner->loop_ = loop;
  owner->owns_default_ = true;
  owner->state_ = State::kRunning;
  return owner;
}

NativeAddonEventLoop::~NativeAddonEventLoop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kClosed) {
    // An idle loop can be destroyed normally. Busy native resources must never
    // outlive freed loop storage; the runtime owner must drain them first.
    CHECK(!pumping_);
    if (pump_guard_) {
      CHECK(InspectHandles().empty());
      CHECK_EQ(0, uv_loop_alive(loop_));
      ClosePumpGuard();
    }
    CHECK_EQ(0, uv_loop_close(loop_));
  }
  if (owns_default_) {
    g_default_loop_claimed.store(false);
  }
}

uv_loop_s* NativeAddonEventLoop::loop() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_ == State::kClosed ? nullptr : loop_;
}

bool NativeAddonEventLoop::HasReferencedWork() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kRunning) {
    return false;
  }
  // A callback may query liveness while PumpInternal has temporarily kept
  // uv_run alive. Exclude only our guard; libuv remains authoritative for
  // referenced handles, requests and pending close callbacks.
  auto* guard = reinterpret_cast<uv_handle_t*>(pump_guard_.get());
  const bool guard_referenced = guard && uv_has_ref(guard);
  if (guard_referenced) {
    uv_unref(guard);
  }
  const bool alive = uv_loop_alive(loop_) != 0;
  if (guard_referenced) {
    uv_ref(guard);
  }
  return alive;
}

base::expected<void, std::string> NativeAddonEventLoop::PumpInternal() {
  if (pumping_) {
    return base::unexpected("Cannot reenter the native event loop");
  }
  base::AutoReset<bool> pumping(&pumping_, true);
  if (!pump_guard_) {
    auto guard = std::make_unique<uv_idle_t>();
    const int status = uv_idle_init(loop_, guard.get());
    if (status != 0) {
      return base::unexpected(std::string("uv_idle_init: ") +
                              uv_strerror(status));
    }
    guard->data = this;
    pump_guard_closed_ = false;
    pump_guard_ = std::move(guard);
  }
  // Chromium explicitly pumps this loop while its own event loop is alive.
  // uv_run otherwise skips unreferenced handles when no libuv request keeps
  // the loop alive. The guard does not affect liveness between pump calls.
  uv_ref(reinterpret_cast<uv_handle_t*>(pump_guard_.get()));
  CHECK_EQ(0, uv_idle_start(pump_guard_.get(), [](uv_idle_t*) {}));
  uv_run(loop_, UV_RUN_NOWAIT);
  uv_idle_stop(pump_guard_.get());
  uv_unref(reinterpret_cast<uv_handle_t*>(pump_guard_.get()));
  return {};
}

void NativeAddonEventLoop::ClosePumpGuard() {
  if (!pump_guard_) {
    return;
  }
  CHECK(!pumping_);
  CHECK(InspectHandles().empty());
  CHECK_EQ(0, uv_loop_alive(loop_));
  base::AutoReset<bool> pumping(&pumping_, true);
  uv_close(
      reinterpret_cast<uv_handle_t*>(pump_guard_.get()),
      [](uv_handle_t* handle) {
        static_cast<NativeAddonEventLoop*>(handle->data)->pump_guard_closed_ =
            true;
      });
  // No application handle/request remains. A preexisting uv_stop may skip
  // the first iteration; retain the heap handle until its close callback runs.
  while (!pump_guard_closed_) {
    uv_run(loop_, UV_RUN_NOWAIT);
  }
  pump_guard_.reset();
}

base::expected<void, std::string> NativeAddonEventLoop::Pump() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ != State::kRunning) {
    return base::unexpected("Native event loop is shutting down");
  }
  return PumpInternal();
}

void NativeAddonEventLoop::BeginShutdown() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kRunning) {
    state_ = State::kDraining;
  }
}

base::expected<bool, std::string> NativeAddonEventLoop::PollShutdown() {
  return PollShutdown([] { return true; });
}

base::expected<bool, std::string> NativeAddonEventLoop::PollShutdown(
    base::FunctionRef<bool()> callbacks_drained) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (state_ == State::kClosed) {
    return true;
  }
  if (state_ != State::kDraining) {
    return base::unexpected("BeginShutdown must precede native loop draining");
  }
  if (auto pumped = PumpInternal(); !pumped.has_value()) {
    return base::unexpected(std::move(pumped.error()));
  }
  if (!callbacks_drained()) {
    return false;
  }
  if (pump_guard_) {
    if (uv_loop_alive(loop_) || !InspectHandles().empty()) {
      return false;
    }
    ClosePumpGuard();
  }
  // uv_loop_alive() alone misses inactive and unreferenced handles. Only
  // uv_loop_close() proves there are no handles or requests borrowing storage.
  const int status = uv_loop_close(loop_);
  if (status == UV_EBUSY) {
    return false;
  }
  if (status != 0) {
    return base::unexpected(std::string("uv_loop_close: ") +
                            uv_strerror(status));
  }
  state_ = State::kClosed;
  return true;
}

bool NativeAddonEventLoop::is_closed() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return state_ == State::kClosed;
}

std::vector<NativeAddonEventLoop::HandleState>
NativeAddonEventLoop::InspectHandles() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  std::vector<HandleState> handles;
  if (state_ != State::kClosed) {
    const auto* guard = reinterpret_cast<const uv_handle_t*>(pump_guard_.get());
    std::pair<const uv_handle_t*, std::vector<HandleState>*> walk_state(
        guard, &handles);
    uv_walk(
        loop_,
        [](uv_handle_t* handle, void* data) {
          auto* state = static_cast<decltype(walk_state)*>(data);
          if (handle == state->first) {
            return;
          }
          const char* type = uv_handle_type_name(uv_handle_get_type(handle));
          state->second->push_back(
              {type ? type : "unknown", uv_is_active(handle) != 0,
               uv_has_ref(handle) != 0, uv_is_closing(handle) != 0});
        },
        &walk_state);
  }
  return handles;
}

}  // namespace xenon
