// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/napi/native_addon_event_loop.h"

#include "base/memory/raw_ptr_exclusion.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "xenon_overlay/third_party/libuv/include/uv.h"

namespace xenon {
namespace {

struct TimerProbe {
  uv_timer_t timer = {};
  int calls = 0;
  bool closed = false;
  RAW_PTR_EXCLUSION NativeAddonEventLoop* owner = nullptr;
  bool rejected_reentry = false;
  bool referenced_work_in_callback = false;
};

void CloseTimer(uv_handle_t* handle) {
  static_cast<TimerProbe*>(handle->data)->closed = true;
}

void FireTimer(uv_timer_t* timer) {
  auto* probe = static_cast<TimerProbe*>(timer->data);
  ++probe->calls;
  if (probe->owner) {
    probe->referenced_work_in_callback = probe->owner->HasReferencedWork();
    probe->rejected_reentry = !probe->owner->Pump().has_value();
  }
  uv_close(reinterpret_cast<uv_handle_t*>(timer), CloseTimer);
}

TEST(NativeAddonEventLoopTest, IndependentLoopsDoNotRunEachOthersCallbacks) {
  auto first = NativeAddonEventLoop::Create();
  auto second = NativeAddonEventLoop::Create();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_NE((*first)->loop(), (*second)->loop());
  EXPECT_NE(uv_default_loop(), (*first)->loop());
  TimerProbe probe;
  probe.owner = first->get();
  ASSERT_EQ(0, uv_timer_init((*first)->loop(), &probe.timer));
  probe.timer.data = &probe;
  ASSERT_EQ(0, uv_timer_start(&probe.timer, FireTimer, 0, 0));
  ASSERT_TRUE((*second)->Pump().has_value());
  EXPECT_EQ(0, probe.calls);
  ASSERT_TRUE((*first)->Pump().has_value());
  EXPECT_EQ(1, probe.calls);
  EXPECT_TRUE(probe.rejected_reentry);
  EXPECT_FALSE(probe.referenced_work_in_callback);
  (*first)->BeginShutdown();
  auto closed = (*first)->PollShutdown();
  ASSERT_TRUE(closed.has_value());
  EXPECT_TRUE(*closed);
  EXPECT_TRUE(probe.closed);
}

TEST(NativeAddonEventLoopTest, UnreferencedHandlesStillBlockSafeDestruction) {
  auto owner = NativeAddonEventLoop::Create();
  ASSERT_TRUE(owner.has_value());
  TimerProbe probe;
  ASSERT_EQ(0, uv_timer_init((*owner)->loop(), &probe.timer));
  probe.timer.data = &probe;
  uv_unref(reinterpret_cast<uv_handle_t*>(&probe.timer));
  EXPECT_EQ(0, uv_loop_alive((*owner)->loop()));
  (*owner)->BeginShutdown();
  auto pending = (*owner)->PollShutdown();
  ASSERT_TRUE(pending.has_value());
  EXPECT_FALSE(*pending);
  const auto handles = (*owner)->InspectHandles();
  ASSERT_EQ(1u, handles.size());
  EXPECT_EQ("timer", handles[0].type);
  EXPECT_FALSE(handles[0].referenced);
  EXPECT_FALSE(handles[0].closing);
  // The addon selects its own close callback. A pending result did not replace
  // it or free the borrowed handle/loop storage.
  uv_close(reinterpret_cast<uv_handle_t*>(&probe.timer), CloseTimer);
  auto closed = (*owner)->PollShutdown();
  ASSERT_TRUE(closed.has_value());
  EXPECT_TRUE(*closed);
  EXPECT_TRUE(probe.closed);
  EXPECT_TRUE((*owner)->is_closed());
  EXPECT_EQ(nullptr, (*owner)->loop());
}

TEST(NativeAddonEventLoopTest,
     ExplicitPumpRunsUnreferencedCallbacksWithoutKeepingLoopAlive) {
  auto owner = NativeAddonEventLoop::Create();
  ASSERT_TRUE(owner.has_value());
  TimerProbe probe;
  probe.owner = owner->get();
  ASSERT_EQ(0, uv_timer_init((*owner)->loop(), &probe.timer));
  probe.timer.data = &probe;
  ASSERT_EQ(0, uv_timer_start(&probe.timer, FireTimer, 0, 0));
  uv_unref(reinterpret_cast<uv_handle_t*>(&probe.timer));
  EXPECT_EQ(0, uv_loop_alive((*owner)->loop()));
  EXPECT_FALSE((*owner)->HasReferencedWork());
  ASSERT_TRUE((*owner)->Pump().has_value());
  EXPECT_EQ(1, probe.calls);
  EXPECT_TRUE(probe.rejected_reentry);
  EXPECT_FALSE(probe.referenced_work_in_callback);
  EXPECT_FALSE((*owner)->HasReferencedWork());
  EXPECT_EQ(0, uv_loop_alive((*owner)->loop()));
  EXPECT_TRUE((*owner)->InspectHandles().empty());
  uv_stop((*owner)->loop());
  (*owner)->BeginShutdown();
  EXPECT_TRUE((*owner)->PollShutdown().value());
  EXPECT_TRUE(probe.closed);
}

TEST(NativeAddonEventLoopTest, ReferencedWorkFollowsActiveAndRefState) {
  auto owner = NativeAddonEventLoop::Create();
  ASSERT_TRUE(owner.has_value());
  EXPECT_FALSE((*owner)->HasReferencedWork());
  TimerProbe probe;
  ASSERT_EQ(0, uv_timer_init((*owner)->loop(), &probe.timer));
  probe.timer.data = &probe;
  EXPECT_FALSE((*owner)->HasReferencedWork());
  ASSERT_EQ(0, uv_timer_start(&probe.timer, FireTimer, 10000, 0));
  EXPECT_TRUE((*owner)->HasReferencedWork());
  auto* handle = reinterpret_cast<uv_handle_t*>(&probe.timer);
  uv_unref(handle);
  EXPECT_FALSE((*owner)->HasReferencedWork());
  uv_ref(handle);
  EXPECT_TRUE((*owner)->HasReferencedWork());
  ASSERT_EQ(0, uv_timer_stop(&probe.timer));
  EXPECT_FALSE((*owner)->HasReferencedWork());
  uv_close(handle, CloseTimer);
  EXPECT_TRUE((*owner)->HasReferencedWork());
  ASSERT_TRUE((*owner)->Pump().has_value());
  EXPECT_TRUE(probe.closed);
  EXPECT_FALSE((*owner)->HasReferencedWork());
  (*owner)->BeginShutdown();
  EXPECT_FALSE((*owner)->HasReferencedWork());
  EXPECT_TRUE((*owner)->PollShutdown().value());
  EXPECT_FALSE((*owner)->HasReferencedWork());
}

TEST(NativeAddonEventLoopTest, NativeRequestsKeepLoopAliveWithoutHandles) {
  auto owner = NativeAddonEventLoop::Create();
  ASSERT_TRUE(owner.has_value());
  bool completed = false;
  uv_work_t request = {};
  request.data = &completed;
  ASSERT_EQ(0, uv_queue_work(
                   (*owner)->loop(), &request, [](uv_work_t*) {},
                   [](uv_work_t* request, int status) {
                     EXPECT_EQ(0, status);
                     *static_cast<bool*>(request->data) = true;
                   }));
  EXPECT_TRUE((*owner)->InspectHandles().empty());
  EXPECT_TRUE((*owner)->HasReferencedWork());
  // Only the queued request is present, so run until its real completion.
  EXPECT_EQ(0, uv_run((*owner)->loop(), UV_RUN_DEFAULT));
  EXPECT_TRUE(completed);
  EXPECT_FALSE((*owner)->HasReferencedWork());
}

TEST(NativeAddonEventLoopTest, ShutdownIsExplicitAndIdempotent) {
  auto owner = NativeAddonEventLoop::Create();
  ASSERT_TRUE(owner.has_value());
  EXPECT_FALSE((*owner)->PollShutdown().has_value());
  (*owner)->BeginShutdown();
  EXPECT_FALSE((*owner)->Pump().has_value());
  auto closed = (*owner)->PollShutdown();
  ASSERT_TRUE(closed.has_value());
  EXPECT_TRUE(*closed);
  (*owner)->BeginShutdown();
  EXPECT_EQ(true, (*owner)->PollShutdown().value());
  EXPECT_TRUE((*owner)->InspectHandles().empty());
}

TEST(NativeAddonEventLoopTest, DefaultLoopLeaseExcludesOtherLegacyRuntimes) {
  auto legacy = NativeAddonEventLoop::AcquireDefault();
  ASSERT_TRUE(legacy.has_value());
  EXPECT_EQ(uv_default_loop(), (*legacy)->loop());
  EXPECT_FALSE(NativeAddonEventLoop::AcquireDefault().has_value());
  auto standard = NativeAddonEventLoop::Create();
  ASSERT_TRUE(standard.has_value());
  EXPECT_NE((*legacy)->loop(), (*standard)->loop());
  (*legacy)->BeginShutdown();
  EXPECT_TRUE((*legacy)->PollShutdown().value());
  // The owning runtime may still be releasing env/library objects.
  EXPECT_FALSE(NativeAddonEventLoop::AcquireDefault().has_value());
  legacy->reset();
  auto replacement = NativeAddonEventLoop::AcquireDefault();
  ASSERT_TRUE(replacement.has_value());
  EXPECT_EQ(uv_default_loop(), (*replacement)->loop());
}

TEST(NativeAddonEventLoopTest, DefaultLoopRejectsUnownedUnreferencedHandles) {
  uv_loop_t* loop = uv_default_loop();
  ASSERT_NE(nullptr, loop);
  TimerProbe probe;
  ASSERT_EQ(0, uv_timer_init(loop, &probe.timer));
  probe.timer.data = &probe;
  uv_unref(reinterpret_cast<uv_handle_t*>(&probe.timer));
  EXPECT_FALSE(NativeAddonEventLoop::AcquireDefault().has_value());
  uv_close(reinterpret_cast<uv_handle_t*>(&probe.timer), CloseTimer);
  uv_run(loop, UV_RUN_NOWAIT);
  EXPECT_TRUE(probe.closed);
  auto owner = NativeAddonEventLoop::AcquireDefault();
  EXPECT_TRUE(owner.has_value());
}

TEST(NativeAddonEventLoopTest, CloseCallbacksPrecedeEnvironmentDrainBarrier) {
  auto owner = NativeAddonEventLoop::Create();
  ASSERT_TRUE(owner.has_value());
  TimerProbe probe;
  ASSERT_EQ(0, uv_timer_init((*owner)->loop(), &probe.timer));
  probe.timer.data = &probe;
  uv_close(reinterpret_cast<uv_handle_t*>(&probe.timer), CloseTimer);
  (*owner)->BeginShutdown();
  auto pending = (*owner)->PollShutdown([&] {
    EXPECT_TRUE(probe.closed);
    return false;
  });
  ASSERT_TRUE(pending.has_value());
  EXPECT_FALSE(*pending);
  EXPECT_NE(nullptr, (*owner)->loop());
  EXPECT_TRUE((*owner)->PollShutdown([] { return true; }).value());
}

}  // namespace
}  // namespace xenon
