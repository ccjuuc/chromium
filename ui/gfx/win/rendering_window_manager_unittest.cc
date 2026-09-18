// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/gfx/win/rendering_window_manager.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE)
#include <memory>

#include "base/no_destructor.h"
#include "base/test/run_until.h"
#include "base/test/scoped_run_loop_timeout.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "base/win/scoped_gdi_object.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace gfx {
namespace {

class RenderingWindowManagerTest : public testing::Test {
 public:
  // The singleton retains its UI task runner, so all tests in this suite must
  // use the same task environment.
  static void SetUpTestSuite() {
    TaskEnvironment() =
        std::make_unique<base::test::SingleThreadTaskEnvironment>(
            base::test::SingleThreadTaskEnvironment::MainThreadType::UI);
  }

  static void TearDownTestSuite() { TaskEnvironment().reset(); }

 protected:
  void SetUp() override {
    previous_dpi_context_ = ::SetThreadDpiAwarenessContext(
        DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Visible off-screen windows exercise real WinEvents without activating a
    // window on the user's desktop. All geometry below is in physical pixels.
    parent_ =
        ::CreateWindowEx(0, L"STATIC", L"Rendering window fixture",
                         WS_POPUP | WS_CLIPCHILDREN, -32000, -32000, 320, 240,
                         nullptr, nullptr, ::GetModuleHandle(nullptr), nullptr);
    ASSERT_TRUE(parent_);
    ::ShowWindow(parent_, SW_SHOWNOACTIVATE);
    manager()->RegisterParent(parent_);
    gpu_ = CreateChild(5, 7, 280, 200);
    ASSERT_TRUE(gpu_);
    manager()->RegisterChild(parent_, gpu_, ::GetCurrentProcessId());
    ASSERT_TRUE(manager()->HasValidChildWindow(parent_));
  }

  void TearDown() override {
    if (parent_) {
      manager()->UnregisterParent(parent_);
      manager()->SetNativeChildClippingEnabled(parent_, false);
      ::DestroyWindow(parent_);
    }
    TaskEnvironment()->RunUntilIdle();
    if (previous_dpi_context_) {
      ::SetThreadDpiAwarenessContext(previous_dpi_context_);
    }
  }

  static RenderingWindowManager* manager() {
    return RenderingWindowManager::GetInstance();
  }

  HWND CreateChild(int x,
                   int y,
                   int width,
                   int height,
                   DWORD ex_style = 0,
                   bool visible = true,
                   HWND parent = nullptr) {
    HWND window =
        ::CreateWindowEx(ex_style, L"STATIC", L"Native child fixture",
                         WS_CHILD | (visible ? WS_VISIBLE : 0), x, y, width,
                         height, parent ? parent : parent_, nullptr,
                         ::GetModuleHandle(nullptr), nullptr);
    if (window) {
      // CreateWindowEx places a new child at the bottom of its siblings. These
      // fixtures represent native content displayed above the GPU child.
      EXPECT_TRUE(::SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
                                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
      EXPECT_EQ(window, ::GetTopWindow(parent ? parent : parent_));
    }
    return window;
  }

  bool HasNoRegion(HWND hwnd) {
    base::win::ScopedGDIObject<HRGN> region(::CreateRectRgn(0, 0, 0, 0));
    return ::GetWindowRgn(hwnd, region.get()) == ERROR;
  }

  bool Contains(HWND hwnd, int x, int y) {
    base::win::ScopedGDIObject<HRGN> region(::CreateRectRgn(0, 0, 0, 0));
    const int kind = ::GetWindowRgn(hwnd, region.get());
    if (kind == ERROR) {
      RECT bounds;
      if (!::GetWindowRect(hwnd, &bounds)) {
        return false;
      }
      return x >= 0 && y >= 0 && x < bounds.right - bounds.left &&
             y < bounds.bottom - bounds.top;
    }
    return ::PtInRegion(region.get(), x, y);
  }

  template <typename Predicate>
  bool WaitFor(Predicate condition) {
    base::test::ScopedRunLoopTimeout timeout(FROM_HERE, base::Seconds(5));
    return base::test::RunUntil(condition);
  }

  static std::unique_ptr<base::test::SingleThreadTaskEnvironment>&
  TaskEnvironment() {
    static base::NoDestructor<
        std::unique_ptr<base::test::SingleThreadTaskEnvironment>>
        environment;
    return *environment;
  }

  HWND parent_ = nullptr;
  HWND gpu_ = nullptr;
  DPI_AWARENESS_CONTEXT previous_dpi_context_ = nullptr;
};

TEST_F(RenderingWindowManagerTest, DefaultDoesNotChangeRenderingRegion) {
  ASSERT_TRUE(CreateChild(45, 47, 80, 60));
  TaskEnvironment()->RunUntilIdle();
  EXPECT_TRUE(HasNoRegion(gpu_));
  EXPECT_TRUE(Contains(gpu_, 50, 50));
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, false));
  TaskEnvironment()->RunUntilIdle();
  EXPECT_TRUE(HasNoRegion(gpu_));
}

TEST_F(RenderingWindowManagerTest, ClipsOnlyVisibleOpaqueDirectChildren) {
  ASSERT_TRUE(CreateChild(25, 27, 40, 40));
  ASSERT_TRUE(CreateChild(85, 27, 40, 40, 0, false));
  HWND transparent = CreateChild(145, 27, 40, 40, WS_EX_TRANSPARENT);
  ASSERT_TRUE(transparent);
  ASSERT_TRUE(CreateChild(0, 0, 40, 40, 0, true, transparent));
  ASSERT_TRUE(CreateChild(205, 27, 40, 40, WS_EX_LAYERED));
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 30, 30); }));
  EXPECT_TRUE(Contains(gpu_, 90, 30));
  EXPECT_TRUE(Contains(gpu_, 150, 30));
  EXPECT_TRUE(Contains(gpu_, 210, 30));
  EXPECT_TRUE(Contains(gpu_, 10, 10));
}

TEST_F(RenderingWindowManagerTest, PreservesHolesInComplexNativeRegion) {
  HWND native = CreateChild(45, 47, 80, 80);
  ASSERT_TRUE(native);
  // The horizontal arm intentionally extends past the native window bounds.
  base::win::ScopedGDIObject<HRGN> native_region(
      ::CreateRectRgn(0, 0, 120, 20));
  base::win::ScopedGDIObject<HRGN> left_region(::CreateRectRgn(0, 0, 20, 80));
  ASSERT_NE(ERROR, ::CombineRgn(native_region.get(), native_region.get(),
                                left_region.get(), RGN_OR));
  ASSERT_TRUE(::SetWindowRgn(native, native_region.get(), FALSE));
  (void)native_region.release();
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));
  EXPECT_FALSE(Contains(gpu_, 100, 50));
  EXPECT_FALSE(Contains(gpu_, 50, 100));
  EXPECT_TRUE(Contains(gpu_, 100, 100));
  EXPECT_TRUE(Contains(gpu_, 130, 50));
  EXPECT_TRUE(Contains(gpu_, 125, 125));
}

TEST_F(RenderingWindowManagerTest, UpdatesForMoveResizeHideShowAndDestroy) {
  HWND native = CreateChild(45, 47, 80, 60);
  ASSERT_TRUE(native);
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));

  ASSERT_TRUE(::SetWindowPos(native, nullptr, 145, 107, 40, 30,
                             SWP_NOACTIVATE | SWP_NOZORDER));
  ASSERT_TRUE(WaitFor([&] {
    return Contains(gpu_, 50, 50) && !Contains(gpu_, 150, 110) &&
           Contains(gpu_, 185, 135);
  }));

  ::ShowWindow(native, SW_HIDE);
  ASSERT_TRUE(WaitFor([&] { return Contains(gpu_, 150, 110); }));
  ::ShowWindow(native, SW_SHOWNOACTIVATE);
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 150, 110); }));

  ASSERT_TRUE(::DestroyWindow(native));
  ASSERT_TRUE(WaitFor([&] { return Contains(gpu_, 150, 110); }));
}

TEST_F(RenderingWindowManagerTest,
       ParentVisibilityAndGpuReplacementKeepClipping) {
  HWND native = CreateChild(45, 47, 80, 60);
  ASSERT_TRUE(native);
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));

  ::ShowWindow(parent_, SW_HIDE);
  TaskEnvironment()->RunUntilIdle();
  ::ShowWindow(parent_, SW_SHOWNOACTIVATE);
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));

  ASSERT_TRUE(::DestroyWindow(gpu_));
  gpu_ = CreateChild(5, 7, 280, 200);
  ASSERT_TRUE(gpu_);
  manager()->RegisterChild(parent_, gpu_, ::GetCurrentProcessId());
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));

  ASSERT_TRUE(::SetWindowPos(gpu_, nullptr, 5, 7, 300, 220,
                             SWP_NOACTIVATE | SWP_NOZORDER));
  ASSERT_TRUE(WaitFor(
      [&] { return Contains(gpu_, 290, 210) && !Contains(gpu_, 50, 50); }));
  ASSERT_TRUE(::SetWindowPos(native, nullptr, 145, 107, 40, 30,
                             SWP_NOACTIVATE | SWP_NOZORDER));
  ASSERT_TRUE(WaitFor(
      [&] { return Contains(gpu_, 50, 50) && !Contains(gpu_, 150, 110); }));
}

TEST_F(RenderingWindowManagerTest, NativeWindowBelowGpuDoesNotClip) {
  HWND native = CreateChild(45, 47, 80, 60);
  ASSERT_TRUE(native);
  ASSERT_TRUE(::SetWindowPos(native, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
  ASSERT_EQ(gpu_, ::GetTopWindow(parent_));
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  TaskEnvironment()->RunUntilIdle();
  EXPECT_TRUE(Contains(gpu_, 50, 50));

  ASSERT_TRUE(::SetWindowPos(native, HWND_TOP, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));
  ASSERT_TRUE(::SetWindowPos(native, HWND_BOTTOM, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE));
  ASSERT_TRUE(WaitFor([&] { return Contains(gpu_, 50, 50); }));
}

TEST_F(RenderingWindowManagerTest, DelayedDestroyEventPreservesCurrentGpu) {
  HWND native = CreateChild(45, 47, 80, 60);
  ASSERT_TRUE(native);
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));

  // Model a queued destroy notification whose numeric HWND now identifies the
  // current binding. It must not clear a still-valid GPU registration.
  ::NotifyWinEvent(EVENT_OBJECT_DESTROY, gpu_, OBJID_WINDOW, CHILDID_SELF);
  TaskEnvironment()->RunUntilIdle();
  EXPECT_TRUE(manager()->HasValidChildWindow(parent_));
  ASSERT_TRUE(::SetWindowPos(native, nullptr, 145, 107, 40, 30,
                             SWP_NOACTIVATE | SWP_NOZORDER));
  ASSERT_TRUE(WaitFor(
      [&] { return Contains(gpu_, 50, 50) && !Contains(gpu_, 150, 110); }));
}

TEST_F(RenderingWindowManagerTest, DisablingAndUnregisteringCancelLateUpdates) {
  HWND native = CreateChild(45, 47, 80, 60);
  ASSERT_TRUE(native);
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, false));
  EXPECT_TRUE(HasNoRegion(gpu_));
  ASSERT_TRUE(::SetWindowPos(native, nullptr, 145, 107, 40, 30,
                             SWP_NOACTIVATE | SWP_NOZORDER));
  TaskEnvironment()->RunUntilIdle();
  EXPECT_TRUE(HasNoRegion(gpu_));

  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 150, 110); }));
  ASSERT_TRUE(::SetWindowPos(native, nullptr, 45, 47, 80, 60,
                             SWP_NOACTIVATE | SWP_NOZORDER));
  manager()->UnregisterParent(parent_);
  TaskEnvironment()->RunUntilIdle();
  EXPECT_FALSE(manager()->HasValidChildWindow(parent_));
  EXPECT_TRUE(HasNoRegion(gpu_));
}

TEST_F(RenderingWindowManagerTest, EarlyOptInSurvivesCompositorUnregistration) {
  manager()->UnregisterParent(parent_);
  ASSERT_TRUE(CreateChild(45, 47, 80, 60));
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  EXPECT_TRUE(HasNoRegion(gpu_));

  manager()->RegisterParent(parent_);
  manager()->RegisterChild(parent_, gpu_, ::GetCurrentProcessId());
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));

  manager()->UnregisterParent(parent_);
  EXPECT_TRUE(HasNoRegion(gpu_));
  manager()->RegisterParent(parent_);
  manager()->RegisterChild(parent_, gpu_, ::GetCurrentProcessId());
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));

  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, false));
  manager()->UnregisterParent(parent_);
  manager()->RegisterParent(parent_);
  manager()->RegisterChild(parent_, gpu_, ::GetCurrentProcessId());
  TaskEnvironment()->RunUntilIdle();
  EXPECT_TRUE(HasNoRegion(gpu_));
}

TEST_F(RenderingWindowManagerTest, RestoresExistingRenderingRegion) {
  base::win::ScopedGDIObject<HRGN> original(::CreateRectRgn(0, 0, 200, 180));
  ASSERT_TRUE(::SetWindowRgn(gpu_, original.get(), FALSE));
  (void)original.release();
  ASSERT_TRUE(CreateChild(45, 47, 80, 60));
  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, true));
  ASSERT_TRUE(WaitFor([&] { return !Contains(gpu_, 50, 50); }));
  EXPECT_FALSE(Contains(gpu_, 220, 50));

  ASSERT_TRUE(manager()->SetNativeChildClippingEnabled(parent_, false));
  EXPECT_TRUE(Contains(gpu_, 50, 50));
  EXPECT_FALSE(Contains(gpu_, 220, 50));
  EXPECT_FALSE(HasNoRegion(gpu_));
}

}  // namespace
}  // namespace gfx
#endif  // BUILDFLAG(ENABLE_XENON_SERVICE)
