// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "components/remote_cocoa/app_shim/browser_native_widget_window_mac.h"

#include <AppKit/AppKit.h>

#include <memory>
#include <string>
#include <vector>

#include "base/test/scoped_feature_list.h"
#import "components/remote_cocoa/app_shim/bridged_content_view.h"
#import "components/remote_cocoa/app_shim/features.h"
#import "components/remote_cocoa/app_shim/native_widget_mac_nswindow.h"
#import "components/remote_cocoa/app_shim/native_widget_ns_window_bridge.h"
#import "components/remote_cocoa/app_shim/native_widget_ns_window_host_helper.h"
#import "components/remote_cocoa/app_shim/window_move_loop.h"
#include "components/remote_cocoa/common/native_widget_ns_window_host.mojom.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "testing/platform_test.h"
#include "ui/accelerated_widget_mac/ca_transaction_observer.h"
#include "ui/base/cocoa/window_size_constants.h"
#include "ui/display/screen.h"

using NativeWidgetMacOverlayNSWindowTest = PlatformTest;

@interface NSWindow (PrivateTestAPI)
- (void)_setFrame:(NSRect)frameRect
    fromAdjustmentToScreen:(NSScreen*)screen
            anchorIfNeeded:(BOOL)anchor
                   animate:(BOOL)animate;
- (void)_setFrameAfterMove:(NSRect)frameRect;
@end

@interface NSThemeFrame (PrivateTrafficLightsAPI)
- (void)setButtonRevealAmount:(double)amount;
@property(readonly) NSView* closeButton;
@property(readonly) NSView* minimizeButton;
@property(readonly) NSView* zoomButton;
@end

// Test that private NSThemeFrame traffic light methods can be used to always
// show the traffic lights.
TEST(BrowserNativeWidgetWindowTest, AlwaysShowTrafficLights) {
  BrowserNativeWidgetWindow* browser_window = [[BrowserNativeWidgetWindow alloc]
      initWithContentRect:ui::kWindowSizeDeterminedLater
                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskMiniaturizable |
                          NSWindowStyleMaskResizable
                  backing:NSBackingStoreBuffered
                    defer:NO];
  NSThemeFrame* theme_frame = base::apple::ObjCCastStrict<NSThemeFrame>(
      browser_window.contentView.superview);

  // Make sure the `NSThemeFrame` responds to the private API we are declaring.
  ASSERT_TRUE([theme_frame respondsToSelector:@selector(closeButton)]);
  ASSERT_TRUE([theme_frame respondsToSelector:@selector(minimizeButton)]);
  ASSERT_TRUE([theme_frame respondsToSelector:@selector(zoomButton)]);
  ASSERT_TRUE(
      [theme_frame respondsToSelector:@selector(setButtonRevealAmount:)]);

  // Ensure the traffic lights are visible.
  EXPECT_EQ(theme_frame.closeButton.alphaValue, 1.0);
  EXPECT_EQ(theme_frame.minimizeButton.alphaValue, 1.0);
  EXPECT_EQ(theme_frame.zoomButton.alphaValue, 1.0);

  // Hide the traffic lights.
  [theme_frame setButtonRevealAmount:0];
  EXPECT_EQ(theme_frame.closeButton.alphaValue, 0.0);
  EXPECT_EQ(theme_frame.minimizeButton.alphaValue, 0.0);
  EXPECT_EQ(theme_frame.zoomButton.alphaValue, 0.0);

  // Always show the traffic lights.
  [browser_window setAlwaysShowTrafficLights:YES];
  EXPECT_EQ(theme_frame.closeButton.alphaValue, 1.0);
  EXPECT_EQ(theme_frame.minimizeButton.alphaValue, 1.0);
  EXPECT_EQ(theme_frame.zoomButton.alphaValue, 1.0);

  // Try to hide the traffic lights again, they should remain visible.
  [theme_frame setButtonRevealAmount:0.0];
  EXPECT_EQ(theme_frame.closeButton.alphaValue, 1.0);
  EXPECT_EQ(theme_frame.minimizeButton.alphaValue, 1.0);
  EXPECT_EQ(theme_frame.zoomButton.alphaValue, 1.0);

  // Remove the always show pin, the traffic lights should be able hide again.
  [browser_window setAlwaysShowTrafficLights:NO];

  // Subsequent calls to `-setButtonRevealAmount:` with the same parameter value
  // will have no effect. Call with a temporary value, then with the desired
  // value.
  [theme_frame setButtonRevealAmount:1.0];
  [theme_frame setButtonRevealAmount:0];
  EXPECT_EQ(theme_frame.closeButton.alphaValue, 0.0);
  EXPECT_EQ(theme_frame.minimizeButton.alphaValue, 0.0);
  EXPECT_EQ(theme_frame.zoomButton.alphaValue, 0.0);
}

// Test that a window with enforceNeverMadeVisible cannot be ordered in.
// This prevents external frameworks (e.g., AuthenticationServicesCore)
// from making the invisible proxy window visible as a side effect of
// presenting modal dialogs. See https://crbug.com/325931972.
TEST(NativeWidgetMacNSWindowTest, EnforceNeverMadeVisibleBlocksOrderIn) {
  NativeWidgetMacNSWindow* window = [[NativeWidgetMacNSWindow alloc]
      initWithContentRect:ui::kWindowSizeDeterminedLater
                styleMask:NSWindowStyleMaskBorderless
                  backing:NSBackingStoreBuffered
                    defer:NO];

  [window enforceNeverMadeVisible];
  EXPECT_FALSE([window isVisible]);

  // orderWindow:NSWindowAbove should be blocked.
  [window orderWindow:NSWindowAbove relativeTo:0];
  EXPECT_FALSE([window isVisible]);

  // orderWindow:NSWindowBelow should also be blocked.
  [window orderWindow:NSWindowBelow relativeTo:0];
  EXPECT_FALSE([window isVisible]);

  // orderWindow:NSWindowOut should still work (ordering out is fine).
  [window orderWindow:NSWindowOut relativeTo:0];
  EXPECT_FALSE([window isVisible]);
}

namespace {

class DummyNativeWidgetNSWindowHost
    : public remote_cocoa::mojom::NativeWidgetNSWindowHost {
 public:
  DummyNativeWidgetNSWindowHost() = default;
  ~DummyNativeWidgetNSWindowHost() override = default;

  void set_hit_test_result(remote_cocoa::mojom::HitTestResult result) {
    hit_test_result_ = result;
  }

 private:
  void OnVisibilityChanged(bool visible) override {}
  void OnSpaceActivationChanged(bool is_space_active) override {}
  void OnWindowNativeThemeChanged() override {}
  void OnViewSizeChanged(const gfx::Size& new_size) override {}
  void SetKeyboardAccessible(bool enabled) override {}
  void OnIsFirstResponderChanged(bool is_first_responder) override {}
  void OnMouseCaptureActiveChanged(bool capture_is_active) override {}
  void OnScrollEvent(std::unique_ptr<ui::Event> event) override {}
  void OnMouseEvent(std::unique_ptr<ui::Event> event) override {}
  void OnGestureEvent(std::unique_ptr<ui::Event> event) override {}
  void OnWindowGeometryChanged(
      const gfx::Rect& window_bounds_in_screen_dips,
      const gfx::Rect& content_bounds_in_screen_dips) override {}
  void OnWindowWillMove() override {}
  void OnWindowDidEndMove() override {}
  void OnWindowWillStartLiveResize() override {}
  void OnWindowDidEndLiveResize() override {}
  void OnWindowFullscreenTransitionStart(
      bool target_fullscreen_state) override {}
  void OnWindowFullscreenTransitionComplete(bool is_fullscreen) override {}
  void OnWindowMiniaturizedChanged(bool miniaturized) override {}
  void OnWindowZoomedChanged(bool zoomed) override {}
  void OnWindowDisplayChanged(const display::Display& display) override {}
  void OnWindowWillClose() override {}
  void OnWindowHasClosed() override {}
  void OnWindowKeyStatusChanged(bool is_key,
                                bool is_content_first_responder,
                                bool full_keyboard_access_enabled) override {}
  void OnWindowStateRestorationDataChanged(
      const std::vector<uint8_t>& data) override {}
  void OnVisibleOnAllWorkspacesChanged(bool visible) override {}
  void OnSheetModalShown() override {}
  void OnSheetModalClosed() override {}
  void OnImmersiveFullscreenToolbarRevealChanged(bool is_revealed) override {}
  void OnImmersiveFullscreenMenuBarRevealChanged(
      double reveal_amount) override {}
  void OnAutohidingMenuBarHeightChanged(int menu_bar_height) override {}
  void DoDialogButtonAction(ui::mojom::DialogButton button) override {}
  void OnFocusWindowToolbar() override {}
  void SetRemoteAccessibilityTokens(
      const std::vector<uint8_t>& window_token,
      const std::vector<uint8_t>& view_token) override {}
  void GetSheetOffsetY(GetSheetOffsetYCallback callback) override {
    std::move(callback).Run(0);
  }
  void DispatchKeyEventRemote(
      std::unique_ptr<ui::Event> event,
      DispatchKeyEventRemoteCallback callback) override {
    std::move(callback).Run(false);
  }
  void DispatchKeyEventToMenuControllerRemote(
      std::unique_ptr<ui::Event> event,
      DispatchKeyEventToMenuControllerRemoteCallback callback) override {
    std::move(callback).Run(false, false);
  }
  void DispatchMonitorEvent(std::unique_ptr<ui::Event> event,
                            bool target_is_this_window,
                            DispatchMonitorEventCallback callback) override {
    std::move(callback).Run(false);
  }
  void GetHasMenuController(GetHasMenuControllerCallback callback) override {
    std::move(callback).Run(false);
  }
  void GetHitTestResult(const gfx::Point& location_in_content,
                        GetHitTestResultCallback callback) override {
    std::move(callback).Run(hit_test_result_);
  }
  void GetTooltipTextAt(const gfx::Point& location_in_content,
                        GetTooltipTextAtCallback callback) override {
    std::move(callback).Run(std::u16string());
  }
  void GetIsFocusedViewTextual(
      GetIsFocusedViewTextualCallback callback) override {
    std::move(callback).Run(false);
  }
  void GetWidgetIsModal(GetWidgetIsModalCallback callback) override {
    std::move(callback).Run(false);
  }
  void GetDialogButtonInfo(ui::mojom::DialogButton button,
                           GetDialogButtonInfoCallback callback) override {
    std::move(callback).Run(false, std::u16string(), false, false);
  }
  void GetDoDialogButtonsExist(
      GetDoDialogButtonsExistCallback callback) override {
    std::move(callback).Run(false);
  }
  void GetShouldShowWindowTitle(
      GetShouldShowWindowTitleCallback callback) override {
    std::move(callback).Run(false);
  }
  void GetCanWindowBecomeKey(GetCanWindowBecomeKeyCallback callback) override {
    std::move(callback).Run(false);
  }
  void GetAlwaysRenderWindowAsKey(
      GetAlwaysRenderWindowAsKeyCallback callback) override {
    std::move(callback).Run(false);
  }
  void OnWindowCloseRequested(
      OnWindowCloseRequestedCallback callback) override {
    std::move(callback).Run(false);
  }
  void GetWindowFrameTitlebarHeight(
      GetWindowFrameTitlebarHeightCallback callback) override {
    std::move(callback).Run(false, 0.0f);
  }
  void GetRootViewAccessibilityToken(
      GetRootViewAccessibilityTokenCallback callback) override {
    std::move(callback).Run(base::kNullProcessId, std::vector<uint8_t>());
  }
  void ValidateUserInterfaceItem(
      int32_t command,
      ValidateUserInterfaceItemCallback callback) override {
    std::move(callback).Run(
        remote_cocoa::mojom::ValidateUserInterfaceItemResult::New());
  }
  void WillExecuteCommand(int32_t command,
                          WindowOpenDisposition window_open_disposition,
                          bool is_before_first_responder,
                          ExecuteCommandCallback callback) override {
    std::move(callback).Run(false);
  }
  void ExecuteCommand(int32_t command,
                      WindowOpenDisposition window_open_disposition,
                      bool is_before_first_responder,
                      ExecuteCommandCallback callback) override {
    std::move(callback).Run(false);
  }
  void HandleAccelerator(const ui::Accelerator& accelerator,
                         bool require_priority_handler,
                         HandleAcceleratorCallback callback) override {
    std::move(callback).Run(false);
  }

  remote_cocoa::mojom::HitTestResult hit_test_result_ =
      remote_cocoa::mojom::HitTestResult::kOther;
};

class DummyNativeWidgetNSWindowHostHelper
    : public remote_cocoa::NativeWidgetNSWindowHostHelper {
 public:
  DummyNativeWidgetNSWindowHostHelper() = default;
  ~DummyNativeWidgetNSWindowHostHelper() override = default;

 private:
  id GetNativeViewAccessible() override { return nil; }
  void DispatchKeyEvent(ui::KeyEvent* event) override {}
  bool DispatchKeyEventToMenuController(ui::KeyEvent* event) override {
    return false;
  }
  void GetWordAt(const gfx::Point& location_in_content,
                 bool* found_word,
                 gfx::DecoratedText* decorated_word,
                 gfx::Point* baseline_point) override {
    *found_word = false;
  }
  remote_cocoa::DragDropClient* GetDragDropClient() override { return nullptr; }
  ui::TextInputClient* GetTextInputClient() override { return nullptr; }
};

}  // namespace

TEST(NativeWidgetMacNSWindowTest, DraggableRegionPreservesHoverHitTesting) {
  display::ScopedNativeScreen screen;
  @autoreleasepool {
    NativeWidgetMacNSWindow* window = [[NativeWidgetMacNSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 100, 100)
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];
    DummyNativeWidgetNSWindowHost dummy_host;
    DummyNativeWidgetNSWindowHostHelper dummy_helper;
    auto bridge = std::make_unique<remote_cocoa::NativeWidgetNSWindowBridge>(
        4, &dummy_host, &dummy_helper, nullptr);
    bridge->SetWindow(window);
    bridge->CreateContentView(4, gfx::Rect(0, 0, 100, 100), std::nullopt);
    BridgedContentView* view = bridge->ns_view();
    NSView* child = [[NSView alloc] initWithFrame:view.bounds];
    [view addSubview:child];
    NSView* overlay = [[NSView alloc] initWithFrame:NSMakeRect(10, 10, 20, 20)];
    [view addSubview:overlay];
    NSPoint child_point = [view.superview convertPoint:NSMakePoint(50, 50)
                                              fromView:view];
    NSPoint overlay_point = [view.superview convertPoint:NSMakePoint(20, 20)
                                                fromView:view];
    NSPoint outside_point = [view.superview convertPoint:NSMakePoint(-10, -10)
                                                fromView:view];

    auto make_event = [window](NSEventType type) {
      if (type == NSEventTypeMouseEntered || type == NSEventTypeMouseExited) {
        return [NSEvent enterExitEventWithType:type
                                      location:NSZeroPoint
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:window.windowNumber
                                       context:nil
                                   eventNumber:0
                                trackingNumber:0
                                      userData:nullptr];
      }
      return [NSEvent mouseEventWithType:type
                                location:NSZeroPoint
                           modifierFlags:0
                               timestamp:0
                            windowNumber:window.windowNumber
                                 context:nil
                             eventNumber:0
                              clickCount:1
                                pressure:0];
    };

    dummy_host.set_hit_test_result(
        remote_cocoa::mojom::HitTestResult::kDraggableBackground);
    EXPECT_EQ(nil, [view hitTest:child_point]);
    EXPECT_EQ(nil, [view hitTest:child_point forMouseEvent:nil]);
    for (NSEventType type : {NSEventTypeMouseMoved, NSEventTypeMouseEntered,
                             NSEventTypeMouseExited}) {
      SCOPED_TRACE(type);
      NSEvent* event = make_event(type);
      ASSERT_TRUE(event);
      EXPECT_EQ(child, [view hitTest:child_point forMouseEvent:event]);
      // Preserve normal frontmost-subview and out-of-bounds filtering.
      EXPECT_EQ(overlay, [view hitTest:overlay_point forMouseEvent:event]);
      EXPECT_EQ(nil, [view hitTest:outside_point forMouseEvent:event]);
    }
    for (NSEventType type :
         {NSEventTypeLeftMouseDown, NSEventTypeLeftMouseUp,
          NSEventTypeLeftMouseDragged, NSEventTypeRightMouseDown,
          NSEventTypeRightMouseUp, NSEventTypeRightMouseDragged,
          NSEventTypeOtherMouseDown, NSEventTypeOtherMouseUp,
          NSEventTypeOtherMouseDragged}) {
      SCOPED_TRACE(type);
      NSEvent* event = make_event(type);
      ASSERT_TRUE(event);
      EXPECT_EQ(nil, [view hitTest:child_point forMouseEvent:event]);
    }

    // RootView and non-draggable content keep their original dispatch targets.
    NSEvent* mouse_moved = make_event(NSEventTypeMouseMoved);
    dummy_host.set_hit_test_result(
        remote_cocoa::mojom::HitTestResult::kRootView);
    EXPECT_EQ(view, [view hitTest:child_point forMouseEvent:mouse_moved]);
    dummy_host.set_hit_test_result(remote_cocoa::mojom::HitTestResult::kOther);
    EXPECT_EQ(child, [view hitTest:child_point]);
    EXPECT_EQ(child, [view hitTest:child_point forMouseEvent:mouse_moved]);
    [window close];
  }
}

// Tests that AppKit frame adjustments (such as clamping window position to
// screen boundaries) are suppressed when a window move loop is active, allowing
// windows to be dragged smoothly across display boundaries without coordinate
// jumping.
TEST(NativeWidgetMacNSWindowTest,
     SuppressAppKitFrameAdjustmentsDuringMoveLoop) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      remote_cocoa::features::kSuppressAppKitFrameAdjustmentsDuringMoveLoop);

  display::ScopedNativeScreen screen;
  @autoreleasepool {
    NativeWidgetMacNSWindow* window = [[NativeWidgetMacNSWindow alloc]
        initWithContentRect:ui::kWindowSizeDeterminedLater
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];

    DummyNativeWidgetNSWindowHost dummy_host;
    DummyNativeWidgetNSWindowHostHelper dummy_helper;
    auto bridge = std::make_unique<remote_cocoa::NativeWidgetNSWindowBridge>(
        1, &dummy_host, &dummy_helper, nullptr);
    bridge->SetWindow(window);
    bridge->SetWindowMoveLoopForTesting(
        std::make_unique<remote_cocoa::CocoaWindowMoveLoop>(bridge.get(),
                                                            NSZeroPoint));

    // Verify that constraining the frame rect returns the requested offscreen
    // frame unchanged without AppKit clamping while the move loop is active.
    NSScreen* main_screen = [NSScreen mainScreen];
    NSRect offscreen_rect = NSMakeRect(-5000, -5000, 400, 400);
    NSRect constrained = [window constrainFrameRect:offscreen_rect
                                           toScreen:main_screen];
    EXPECT_TRUE(NSEqualRects(constrained, offscreen_rect));

    // Save initial frame and verify that AppKit internal frame adjustment
    // callbacks do not modify the frame while the move loop is active.
    NSRect initial_frame = [window frame];

    [window _setFrame:NSMakeRect(200, 200, 400, 400)
        fromAdjustmentToScreen:main_screen
                anchorIfNeeded:NO
                       animate:NO];
    EXPECT_TRUE(NSEqualRects([window frame], initial_frame));

    [window _setFrameAfterMove:NSMakeRect(300, 300, 400, 400)];
    EXPECT_TRUE(NSEqualRects([window frame], initial_frame));

    bridge->EndMoveLoop();
    [window close];
  }
}

// Tests that when the feature flag is disabled, default AppKit frame
// constraining is performed even when a move loop is active.
TEST(NativeWidgetMacNSWindowTest,
     SuppressAppKitFrameAdjustmentsDuringMoveLoop_Disabled) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndDisableFeature(
      remote_cocoa::features::kSuppressAppKitFrameAdjustmentsDuringMoveLoop);

  display::ScopedNativeScreen screen;
  @autoreleasepool {
    NativeWidgetMacNSWindow* window = [[NativeWidgetMacNSWindow alloc]
        initWithContentRect:ui::kWindowSizeDeterminedLater
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];

    DummyNativeWidgetNSWindowHost dummy_host;
    DummyNativeWidgetNSWindowHostHelper dummy_helper;
    auto bridge = std::make_unique<remote_cocoa::NativeWidgetNSWindowBridge>(
        2, &dummy_host, &dummy_helper, nullptr);
    bridge->SetWindow(window);

    NSRect new_frame = NSMakeRect(200, 200, 400, 400);
    [window _setFrameAfterMove:new_frame];
    // When feature is disabled, _setFrameAfterMove forwards to super and
    // updates frame.
    EXPECT_TRUE(NSEqualRects([window frame], new_frame));

    [window close];
  }
}

// Tests that AppKit frame adjustments are suppressed specifically when dragging
// across display boundaries, ensuring position is not clamped or locked.
TEST(NativeWidgetMacNSWindowTest,
     SuppressAppKitFrameAdjustmentsAcrossDisplayBoundaries) {
  base::test::ScopedFeatureList feature_list;
  feature_list.InitAndEnableFeature(
      remote_cocoa::features::kSuppressAppKitFrameAdjustmentsDuringMoveLoop);

  display::ScopedNativeScreen screen;
  @autoreleasepool {
    NativeWidgetMacNSWindow* window = [[NativeWidgetMacNSWindow alloc]
        initWithContentRect:ui::kWindowSizeDeterminedLater
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO];

    DummyNativeWidgetNSWindowHost dummy_host;
    DummyNativeWidgetNSWindowHostHelper dummy_helper;
    auto bridge = std::make_unique<remote_cocoa::NativeWidgetNSWindowBridge>(
        3, &dummy_host, &dummy_helper, nullptr);
    bridge->SetWindow(window);

    // Set up an active move loop.
    bridge->SetWindowMoveLoopForTesting(
        std::make_unique<remote_cocoa::CocoaWindowMoveLoop>(bridge.get(),
                                                            NSZeroPoint));

    NSScreen* main_screen = [NSScreen mainScreen];

    // Simulate window dragging across display boundary line (e.g. x = 1728).
    NSRect boundary_frame = NSMakeRect(1728, 200, 800, 600);
    [window setFrame:boundary_frame display:NO];

    // Verify constrainFrameRect returns the unconstrained boundary frame.
    NSRect constrained = [window constrainFrameRect:boundary_frame
                                           toScreen:main_screen];
    EXPECT_TRUE(NSEqualRects(constrained, boundary_frame));

    // Simulate stale AppKit frame adjustment events firing during drag across
    // boundary.
    NSRect stale_appkit_frame = NSMakeRect(1600, 200, 800, 600);

    [window _setFrame:stale_appkit_frame
        fromAdjustmentToScreen:main_screen
                anchorIfNeeded:NO
                       animate:NO];
    EXPECT_TRUE(NSEqualRects([window frame], boundary_frame));

    [window _setFrameAfterMove:stale_appkit_frame];
    EXPECT_TRUE(NSEqualRects([window frame], boundary_frame));

    // End the move loop and verify normal frame updates resume.
    bridge->EndMoveLoop();

    [window _setFrameAfterMove:stale_appkit_frame];
    EXPECT_TRUE(NSEqualRects([window frame], stale_appkit_frame));

    [window close];
  }
}
