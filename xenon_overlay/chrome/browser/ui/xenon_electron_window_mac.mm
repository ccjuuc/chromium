// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_electron_window_mac.h"

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include <algorithm>

#include "ui/views/cocoa/native_widget_mac_ns_window_host.h"
#include "ui/views/widget/widget.h"

// Retain native traffic-light layout with the NSWindow, so resize/fullscreen
// transitions also preserve setWindowButtonVisibility and hiddenInset.
@interface XenonElectronWindowChrome : NSObject
@property(nonatomic, weak) NSWindow* window;
@property(nonatomic) BOOL insetButtons;
@property(nonatomic) BOOL buttonsVisible;
- (instancetype)initWithWindow:(NSWindow*)window;
- (void)updateButtons;
@end

@implementation XenonElectronWindowChrome

@synthesize window = _window;
@synthesize insetButtons = _insetButtons;
@synthesize buttonsVisible = _buttonsVisible;

- (instancetype)initWithWindow:(NSWindow*)window {
  if ((self = [super init])) {
    _window = window;
    _buttonsVisible = YES;
    for (NSNotificationName name in @[
           NSWindowDidResizeNotification, NSWindowDidExitFullScreenNotification
         ]) {
      [NSNotificationCenter.defaultCenter addObserver:self
                                             selector:@selector(windowChanged:)
                                                 name:name
                                               object:window];
    }
  }
  return self;
}

- (void)dealloc {
  [NSNotificationCenter.defaultCenter removeObserver:self];
}

- (void)windowChanged:(NSNotification*)notification {
  [self updateButtons];
}

- (void)updateButtons {
  NSWindow* window = self.window;
  NSButton* close = [window standardWindowButton:NSWindowCloseButton];
  if (self.insetButtons && close) {
    // Keep AppKit's standard controls and spacing. Only their inset changes.
    const CGFloat delta_x = 12 - NSMinX(close.frame);
    NSView* container = close.superview;
    // Older AppKit title bars can be shorter; keep the full native button
    // inside its titlebar rather than clipping it below the container.
    const CGFloat desired_y = std::max<CGFloat>(
        0, NSHeight(container.bounds) - 12 - NSHeight(close.frame));
    const CGFloat delta_y = desired_y - NSMinY(close.frame);
    for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton,
                                NSWindowZoomButton}) {
      NSButton* button = [window standardWindowButton:kind];
      NSRect frame = button.frame;
      frame.origin.x += delta_x;
      frame.origin.y += delta_y;
      button.frame = frame;
    }
  }
  for (NSWindowButton kind : {NSWindowCloseButton, NSWindowMiniaturizeButton,
                              NSWindowZoomButton}) {
    NSButton* button = [window standardWindowButton:kind];
    button.hidden = !self.buttonsVisible;
  }
}

@end

namespace xenon {
namespace {

const char kWindowChromeKey = 0;

XenonElectronWindowChrome* GetWindowChrome(NSWindow* window) {
  XenonElectronWindowChrome* chrome =
      objc_getAssociatedObject(window, &kWindowChromeKey);
  if (!chrome) {
    chrome = [[XenonElectronWindowChrome alloc] initWithWindow:window];
    objc_setAssociatedObject(window, &kWindowChromeKey, chrome,
                             OBJC_ASSOCIATION_RETAIN_NONATOMIC);
  }
  return chrome;
}

NSWindow* GetLocalWindow(views::Widget* widget, std::string* error) {
  const auto native_window = widget->GetNativeWindow();
  const auto* host =
      views::NativeWidgetMacNSWindowHost::GetFromNativeWindow(native_window);
  // A remote Cocoa window exposes a local mirror. Updating the mirror would
  // report success without changing the actual window's WindowServer shadow.
  if (!host || !host->GetInProcessNSWindowBridge()) {
    *error = "ERR_NOT_SUPPORTED: BrowserWindow chrome requires a local Mac window";
    return nil;
  }
  NSWindow* window = native_window.GetNativeNSWindow();
  if (!window) {
    *error = "BrowserWindow native Mac window is unavailable";
  }
  return window;
}

}  // namespace

bool SetMacWindowShadow(views::Widget* widget, bool enabled, std::string* error) {
  NSWindow* window = GetLocalWindow(widget, error);
  if (!window) {
    return false;
  }
  [window setHasShadow:enabled];
  [window invalidateShadow];
  return true;
}

bool GetMacWindowShadow(views::Widget* widget,
                        bool* enabled,
                        std::string* error) {
  NSWindow* window = GetLocalWindow(widget, error);
  if (!window) {
    return false;
  }
  *enabled = [window hasShadow];
  return true;
}

bool ConfigureMacWindowTitleBar(views::Widget* widget,
                                const std::string& style,
                                std::string* error) {
  if (style == "default") {
    return true;
  }
  NSWindow* window = GetLocalWindow(widget, error);
  if (!window) {
    return false;
  }
  const NSRect frame = window.frame;
  const BOOL has_shadow = window.hasShadow;
  // Hidden title bars retain AppKit's real standard buttons over the web
  // content. Merely removing the native frame loses the close button entirely.
  window.styleMask |= NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                      NSWindowStyleMaskMiniaturizable |
                      NSWindowStyleMaskFullSizeContentView;
  window.titleVisibility = NSWindowTitleHidden;
  window.titlebarAppearsTransparent = YES;
  [window setFrame:frame display:NO];
  window.hasShadow = has_shadow;
  XenonElectronWindowChrome* chrome = GetWindowChrome(window);
  chrome.insetButtons = style == "hiddenInset";
  [chrome updateButtons];
  [window invalidateShadow];
  return true;
}

bool SetMacWindowButtonVisibility(views::Widget* widget,
                                  bool visible,
                                  std::string* error) {
  NSWindow* window = GetLocalWindow(widget, error);
  if (!window) {
    return false;
  }
  XenonElectronWindowChrome* chrome = GetWindowChrome(window);
  chrome.buttonsVisible = visible;
  [chrome updateButtons];
  return true;
}

}  // namespace xenon
