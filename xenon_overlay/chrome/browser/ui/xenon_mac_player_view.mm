// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_view.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <pthread.h>

#include <utility>
#include <vector>

#include "base/check.h"
#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/sys_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/time/time.h"
#include "components/remote_cocoa/app_shim/bridged_content_view.h"
#include "components/remote_cocoa/app_shim/native_widget_ns_window_bridge.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "ui/views/cocoa/native_widget_mac_ns_window_host.h"
#include "ui/views/widget/widget.h"

// Keep the SDK rendering target inside an input-transparent root. The root is
// below the window compositor, which also draws the WebContents' controls.
@interface XenonMacPlayerRootView : NSView
@property(nonatomic, weak) NSView* renderView;
- (void)setDiagnosticsCallback:(base::RepeatingClosure)callback;
@end

@implementation XenonMacPlayerRootView {
  base::RepeatingClosure _diagnosticsCallback;
}

@synthesize renderView = _renderView;

- (BOOL)acceptsFirstResponder {
  return NO;
}

- (NSView*)hitTest:(NSPoint)point {
  // Input, focus and accessibility remain with the webpage above the video.
  return nil;
}

- (BOOL)isAccessibilityElement {
  return NO;
}

- (void)setFrameSize:(NSSize)size {
  if (NSEqualSizes(self.frame.size, size)) {
    return;
  }
  [super setFrameSize:size];
  self.renderView.frame = self.bounds;
  if (_diagnosticsCallback) {
    _diagnosticsCallback.Run();
  }
}

- (void)setDiagnosticsCallback:(base::RepeatingClosure)callback {
  _diagnosticsCallback = std::move(callback);
}

- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  self.layer.contentsScale = self.window.backingScaleFactor ?: 1;
}

- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  // NSView frames stay in points; the SDK owns the render view's layer tree.
  self.layer.contentsScale = self.window.backingScaleFactor ?: 1;
}

@end

namespace xenon {
namespace {

constexpr char kDebugMacPlayerView[] = "xenon-debug-mac-player-view";
constexpr size_t kMaxDiagnosticNodes = 80;
constexpr size_t kMaxDiagnosticDepth = 8;

bool Fail(const char* message, std::string* error) {
  if (error) {
    *error = message;
  }
  return false;
}

std::string ClassName(id object) {
  return object ? base::SysNSStringToUTF8(NSStringFromClass([object class]))
                : "nil";
}

std::string RectDescription(NSRect rect) {
  return base::SysNSStringToUTF8(NSStringFromRect(rect));
}

bool IsLayerWithin(CALayer* layer, CALayer* ancestor) {
  if (!ancestor) {
    return false;
  }
  for (; layer; layer = layer.superlayer) {
    if (layer == ancestor) {
      return true;
    }
  }
  return false;
}

}  // namespace

class XenonMacPlayerView::Impl : public content::WebContentsObserver {
 public:
  Impl(content::WebContents* contents,
       NSWindow* window,
       base::WeakPtr<remote_cocoa::NativeWidgetNSWindowBridge> window_bridge,
       base::OnceClosure on_contents_destroyed)
      : content::WebContentsObserver(contents),
        window_(window),
        window_bridge_(std::move(window_bridge)),
        diagnostics_enabled_(base::CommandLine::ForCurrentProcess()->HasSwitch(
            kDebugMacPlayerView)),
        on_contents_destroyed_(std::move(on_contents_destroyed)) {
    CHECK(pthread_main_np());
    root_ = [[XenonMacPlayerRootView alloc] initWithFrame:NSZeroRect];
    root_.wantsLayer = YES;
    root_.layer.backgroundColor = NSColor.blackColor.CGColor;
    root_.layer.masksToBounds = YES;
    render_view_ = [[NSView alloc] initWithFrame:NSZeroRect];
    render_view_.wantsLayer = YES;
    render_view_.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    root_.renderView = render_view_;
    [root_ addSubview:render_view_];
    if (diagnostics_enabled_) {
      [root_ setDiagnosticsCallback:base::BindRepeating(
                                        &Impl::ScheduleResizeDiagnostic,
                                        weak_factory_.GetWeakPtr())];
    }
  }

  ~Impl() override {
    CHECK(pthread_main_np());
    Detach();
  }

  uintptr_t native_view_for_in_process_addon() const {
    CHECK(pthread_main_np());
    return reinterpret_cast<uintptr_t>((__bridge void*)render_view_);
  }

  bool RefreshAttachment(std::string* error) {
    CHECK(pthread_main_np());
    if (!web_contents()) {
      return Fail("Mac player WebContents is unavailable", error);
    }
    NSView* container = web_contents()->GetNativeView().GetNativeNSView();
    NSView* window_content = window_bridge_ ? window_bridge_->ns_view() : nil;
    // GetNativeView is the in-process WebContentsViewCocoa, not the renderer's
    // short-lived native view. Reject remote mirrors and window migrations.
    if (!container || !window_content || !window_ ||
        container.window != window_ || window_content != window_.contentView ||
        ![container isDescendantOf:window_content]) {
      return Fail("Mac player requires its local Browser WebContents view",
                  error);
    }
    bool attachment_changed = false;
    if (!GeometryHierarchyMatches(container, window_content)) {
      ObserveGeometry(container, window_content);
      attachment_changed = true;
    }
    // Views-hosted WebContents draw through the window compositor, not through
    // their native RenderWidgetHostViewCocoa (whose display is disabled). A
    // video inside WebContentsViewCocoa would cover that compositor regardless
    // of its order relative to the renderer's empty native layer. Place only
    // our root below the compositor at the window content level instead.
    // SortSubviews may subsequently reorder native views; the root's negative
    // zPosition keeps it below all of the window's ordinary compositor layers.
    if (root_.layer.zPosition != -1) {
      root_.layer.zPosition = -1;
      attachment_changed = true;
    }
    if (root_.superview != window_content) {
      [window_content addSubview:root_ positioned:NSWindowBelow relativeTo:nil];
      attachment_changed = true;
    }
    NSRect visible_bounds =
        NSIntersectionRect(container.bounds, container.visibleRect);
    NSRect root_frame = [container convertRect:visible_bounds
                                        toView:window_content];
    if (!NSEqualRects(root_.frame, root_frame)) {
      root_.frame = root_frame;
      attachment_changed = true;
    }
    NSRect render_frame = [container convertRect:container.bounds toView:root_];
    if (!NSEqualRects(render_view_.frame, render_frame)) {
      render_view_.frame = render_frame;
      attachment_changed = true;
    }
    bool hidden =
        container.hiddenOrHasHiddenAncestor ||
        web_contents()->GetVisibility() != content::Visibility::VISIBLE;
    if (root_.hidden != hidden) {
      root_.hidden = hidden;
      attachment_changed = true;
    }
    if (diagnostics_enabled_) {
      if (attachment_changed) {
        DumpDiagnostics("attachment-changed");
      }
      if (!delayed_diagnostics_scheduled_) {
        delayed_diagnostics_scheduled_ = true;
        auto task_runner = base::SingleThreadTaskRunner::GetCurrentDefault();
        task_runner->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(&Impl::DumpDiagnostics, weak_factory_.GetWeakPtr(),
                           "after-attach-250ms"),
            base::Milliseconds(250));
        task_runner->PostDelayedTask(
            FROM_HERE,
            base::BindOnce(&Impl::DumpDiagnostics, weak_factory_.GetWeakPtr(),
                           "after-attach-3s"),
            base::Seconds(3));
      }
    }
    if (error) {
      error->clear();
    }
    return true;
  }

  void Detach() {
    CHECK(pthread_main_np());
    weak_factory_.InvalidateWeakPtrs();
    StopObservingGeometry();
    Observe(nullptr);
    [root_ removeFromSuperview];
  }

 private:
  struct ObservedGeometryView {
    NSView* __weak view;
    bool posted_frame_changes;
    bool posted_bounds_changes;
  };

  bool GeometryHierarchyMatches(NSView* container,
                                NSView* window_content) const {
    if (geometry_container_ != container) {
      return false;
    }
    NSView* view = container;
    for (const auto& observed : geometry_views_) {
      if (view != observed.view) {
        return false;
      }
      if (view == window_content) {
        return true;
      }
      view = view.superview;
    }
    return false;
  }

  void StopObservingGeometry() {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    for (id token in geometry_observers_) {
      [center removeObserver:token];
    }
    geometry_observers_ = nil;
    for (const auto& observed : geometry_views_) {
      NSView* view = observed.view;
      view.postsFrameChangedNotifications = observed.posted_frame_changes;
      view.postsBoundsChangedNotifications = observed.posted_bounds_changes;
    }
    geometry_views_.clear();
    geometry_container_ = nil;
  }

  void AddGeometryObserver(NSNotificationName name, id object) {
    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    id token = [NSNotificationCenter.defaultCenter
        addObserverForName:name
                    object:object
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification* notification) {
                  if (weak_this) {
                    weak_this->ScheduleRefresh();
                  }
                }];
    [geometry_observers_ addObject:token];
  }

  void ObserveGeometry(NSView* container, NSView* window_content) {
    StopObservingGeometry();
    geometry_container_ = container;
    geometry_observers_ = [[NSMutableArray alloc] init];
    // Ancestors may reposition WebContents without changing its own frame.
    // Observe just this local hierarchy, and restore its notification flags
    // when detaching. All queued callbacks hold only a main-thread WeakPtr.
    for (NSView* view = container; view; view = view.superview) {
      geometry_views_.push_back({view, !!view.postsFrameChangedNotifications,
                                 !!view.postsBoundsChangedNotifications});
      view.postsFrameChangedNotifications = YES;
      view.postsBoundsChangedNotifications = YES;
      AddGeometryObserver(NSViewFrameDidChangeNotification, view);
      AddGeometryObserver(NSViewBoundsDidChangeNotification, view);
      if (view == window_content) {
        break;
      }
    }
    AddGeometryObserver(NSWindowDidResizeNotification, window_);
    AddGeometryObserver(NSWindowDidChangeOcclusionStateNotification, window_);
    AddGeometryObserver(NSWindowDidChangeBackingPropertiesNotification,
                        window_);
    AddGeometryObserver(NSWindowDidEnterFullScreenNotification, window_);
    AddGeometryObserver(NSWindowDidExitFullScreenNotification, window_);
  }

  const char* ViewRole(NSView* view, NSView* container, NSView* page) const {
    if (!view) {
      return "nil";
    }
    if (view == root_) {
      return "video-root";
    }
    if (view == render_view_) {
      return "sdk-target";
    }
    if (view == page) {
      return "page";
    }
    if (view == window_.contentView) {
      return "window-content";
    }
    return view == container ? "container" : "other";
  }

  const char* LayerRole(CALayer* layer, NSView* container, NSView* page) const {
    if (!layer) {
      return "nil";
    }
    if (layer == root_.layer) {
      return "video-root";
    }
    if (layer == render_view_.layer) {
      return "sdk-target";
    }
    if (layer == page.layer) {
      return "page";
    }
    if (layer == window_.contentView.layer) {
      return "window-content";
    }
    return layer == container.layer ? "container" : "other";
  }

  void DumpViewTree(NSView* view,
                    NSView* container,
                    NSView* page,
                    const std::string& path,
                    size_t depth,
                    size_t* remaining) const {
    if (!view || !*remaining || depth > kMaxDiagnosticDepth) {
      return;
    }
    --*remaining;
    LOG(WARNING) << "[xenon-mac-player-view] view " << path
                 << " role=" << ViewRole(view, container, page)
                 << " class=" << ClassName(view)
                 << " frame=" << RectDescription(view.frame)
                 << " bounds=" << RectDescription(view.bounds)
                 << " layer=" << view.wantsLayer << " opaque=" << view.opaque
                 << " hidden=" << view.hidden
                 << " hiddenAncestor=" << view.hiddenOrHasHiddenAncestor
                 << " alpha=" << view.alphaValue
                 << " layerRole=" << LayerRole(view.layer, container, page)
                 << " layerParent="
                 << LayerRole(view.layer.superlayer, container, page)
                 << " layerIndex="
                 << (view.layer.superlayer
                         ? [view.layer.superlayer.sublayers
                               indexOfObjectIdenticalTo:view.layer]
                         : NSNotFound);
    size_t index = 0;
    for (NSView* child in view.subviews) {
      DumpViewTree(child, container, page,
                   path + "/" + base::NumberToString(index++), depth + 1,
                   remaining);
    }
  }

  void DumpLayerTree(CALayer* layer,
                     NSView* container,
                     NSView* page,
                     const std::string& path,
                     size_t depth,
                     size_t* remaining) const {
    if (!layer || !*remaining || depth > kMaxDiagnosticDepth) {
      return;
    }
    --*remaining;
    LOG(WARNING) << "[xenon-mac-player-view] layer " << path
                 << " role=" << LayerRole(layer, container, page)
                 << " class=" << ClassName(layer)
                 << " frame=" << RectDescription(NSRectFromCGRect(layer.frame))
                 << " bounds="
                 << RectDescription(NSRectFromCGRect(layer.bounds))
                 << " z=" << layer.zPosition << " opaque=" << layer.opaque
                 << " hidden=" << layer.hidden << " opacity=" << layer.opacity
                 << " scale=" << layer.contentsScale
                 << " masksToBounds=" << layer.masksToBounds;
    size_t index = 0;
    for (CALayer* child in layer.sublayers) {
      DumpLayerTree(child, container, page,
                    path + "/" + base::NumberToString(index++), depth + 1,
                    remaining);
    }
  }

  void DumpDiagnostics(const char* reason) {
    if (!diagnostics_enabled_ || !web_contents()) {
      return;
    }
    NSView* container = web_contents()->GetNativeView().GetNativeNSView();
    content::RenderWidgetHostView* page_host =
        web_contents()->GetRenderWidgetHostView();
    NSView* page =
        page_host ? page_host->GetNativeView().GetNativeNSView() : nil;
    NSWindow* window = window_;
    NSView* content_view = window.contentView;
    NSPoint window_point = window.mouseLocationOutsideOfEventStream;
    // Match RenderWidgetHostViewCocoa's use of superview coordinates.
    NSPoint hit_point = [content_view.superview convertPoint:window_point
                                                    fromView:nil];
    NSView* hit = [content_view hitTest:hit_point];
    const char* hit_role = "other";
    for (NSView* ancestor = hit; ancestor; ancestor = ancestor.superview) {
      const char* role = ViewRole(ancestor, container, page);
      if (ancestor == page || ancestor == root_ || ancestor == render_view_) {
        hit_role = role;
        break;
      }
    }
    LOG(WARNING) << "[xenon-mac-player-view] snapshot=" << reason
                 << " appActive=" << NSApp.active
                 << " windowVisible=" << window.visible
                 << " key=" << window.keyWindow << " main=" << window.mainWindow
                 << " ignoresMouse=" << window.ignoresMouseEvents
                 << " acceptsMouseMoved=" << window.acceptsMouseMovedEvents
                 << " firstResponder=" << ClassName(window.firstResponder)
                 << " mouse=" << window_point.x << "," << window_point.y
                 << " hit=" << ClassName(hit) << " hitRole=" << hit_role
                 << " sharesWindowContentLayer="
                 << (root_.layer.superlayer &&
                     root_.layer.superlayer == content_view.layer);
    size_t remaining = kMaxDiagnosticNodes;
    DumpViewTree(content_view, container, page, "window-content", 0,
                 &remaining);
    remaining = kMaxDiagnosticNodes;
    CALayer* layer_root = content_view.layer;
    DumpLayerTree(layer_root, container, page, "window-content", 0, &remaining);
    if (!IsLayerWithin(root_.layer, layer_root)) {
      DumpLayerTree(root_.layer, container, page, "video-detached", 0,
                    &remaining);
    }
    if (!IsLayerWithin(page.layer, layer_root)) {
      DumpLayerTree(page.layer, container, page, "page-detached", 0,
                    &remaining);
    }
  }

  void ScheduleResizeDiagnostic() {
    if (resize_diagnostic_scheduled_) {
      return;
    }
    resize_diagnostic_scheduled_ = true;
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE, base::BindOnce(&Impl::DumpResizeDiagnostic,
                                  weak_factory_.GetWeakPtr()));
  }

  void DumpResizeDiagnostic() {
    resize_diagnostic_scheduled_ = false;
    DumpDiagnostics("resize");
  }

  void RenderViewHostChanged(content::RenderViewHost* old_host,
                             content::RenderViewHost* new_host) override {
    ScheduleRefresh();
  }

  void PrimaryPageChanged(content::Page& page) override { ScheduleRefresh(); }

  void OnVisibilityChanged(content::Visibility visibility) override {
    ScheduleRefresh();
  }

  void DidToggleFullscreenModeForTab(bool entered_fullscreen,
                                     bool will_cause_resize) override {
    ScheduleRefresh();
  }

  void WebContentsDestroyed() override {
    // The owner can synchronously closePlayWindow before Cocoa releases the
    // parent. The callback may also destroy this helper or call Detach().
    base::WeakPtr<Impl> weak_this = weak_factory_.GetWeakPtr();
    if (on_contents_destroyed_) {
      std::move(on_contents_destroyed_).Run();
    }
    if (!weak_this) {
      return;
    }
    // Keep render_view_ alive until the owning player runtime has detached its
    // SDK target, even if WebContents teardown wins the host's shutdown race.
    weak_factory_.InvalidateWeakPtrs();
    StopObservingGeometry();
    Observe(nullptr);
    [root_ removeFromSuperview];
  }

  void ScheduleRefresh() {
    if (refresh_scheduled_) {
      return;
    }
    refresh_scheduled_ = true;
    // AppKit can emit several ancestor/window notifications for one layout.
    // Coalesce them into one update per display interval; no-op updates avoid
    // reattaching or writing frames that would restart native tracking/layout.
    base::SingleThreadTaskRunner::GetCurrentDefault()->PostDelayedTask(
        FROM_HERE,
        base::BindOnce(&Impl::RefreshAfterPageChange,
                       weak_factory_.GetWeakPtr()),
        base::Milliseconds(16));
  }

  void RefreshAfterPageChange() {
    refresh_scheduled_ = false;
    if (!RefreshAttachment(nullptr)) {
      root_.hidden = YES;
    }
  }

  __weak NSWindow* window_;
  base::WeakPtr<remote_cocoa::NativeWidgetNSWindowBridge> window_bridge_;
  __weak NSView* geometry_container_;
  NSMutableArray* __strong geometry_observers_;
  std::vector<ObservedGeometryView> geometry_views_;
  XenonMacPlayerRootView* __strong root_;
  NSView* __strong render_view_;
  bool refresh_scheduled_ = false;
  const bool diagnostics_enabled_;
  bool delayed_diagnostics_scheduled_ = false;
  bool resize_diagnostic_scheduled_ = false;
  base::OnceClosure on_contents_destroyed_;
  base::WeakPtrFactory<Impl> weak_factory_{this};
};

// static
std::unique_ptr<XenonMacPlayerView> XenonMacPlayerView::Create(
    views::Widget* widget,
    content::WebContents* web_contents,
    std::string* error,
    base::OnceClosure on_contents_destroyed) {
  CHECK(pthread_main_np());
  if (!widget || !web_contents) {
    Fail("Mac player window and WebContents are required", error);
    return nullptr;
  }
  const auto native_window = widget->GetNativeWindow();
  const auto* host =
      views::NativeWidgetMacNSWindowHost::GetFromNativeWindow(native_window);
  if (!host || !host->GetInProcessNSWindowBridge()) {
    Fail("Mac player requires a local Browser window", error);
    return nullptr;
  }
  NSWindow* window = native_window.GetNativeNSWindow();
  auto impl = std::make_unique<Impl>(
      web_contents, window, host->GetInProcessNSWindowBridge()->GetWeakPtr(),
      std::move(on_contents_destroyed));
  if (!impl->RefreshAttachment(error)) {
    return nullptr;
  }
  return std::unique_ptr<XenonMacPlayerView>(
      new XenonMacPlayerView(std::move(impl)));
}

XenonMacPlayerView::XenonMacPlayerView(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

XenonMacPlayerView::~XenonMacPlayerView() = default;

uintptr_t XenonMacPlayerView::native_view_for_in_process_addon() const {
  return impl_->native_view_for_in_process_addon();
}

bool XenonMacPlayerView::RefreshAttachment(std::string* error) {
  return impl_->RefreshAttachment(error);
}

void XenonMacPlayerView::Detach() {
  impl_->Detach();
}

}  // namespace xenon
