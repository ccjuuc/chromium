// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_VIEW_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_VIEW_H_

#include <cstdint>
#include <memory>
#include <string>

#include "base/functional/callback.h"

namespace content {
class WebContents;
}

namespace views {
class Widget;
}

namespace xenon {

// Owns a local Cocoa rendering target below the window compositor drawing a
// WebContents' transparent page.
// Construct, use and destroy on the Browser Cocoa main thread. The player SDK
// must release its target before destruction. This class neither owns the SDK
// nor changes the WebContents' background transparency. Native view/layer and
// input diagnostics are enabled by --xenon-debug-mac-player-view.
class XenonMacPlayerView {
 public:
  // on_contents_destroyed runs before the native container is released so the
  // owner can synchronously detach its SDK target. It may destroy this object.
  static std::unique_ptr<XenonMacPlayerView> Create(
      views::Widget* widget,
      content::WebContents* web_contents,
      std::string* error,
      base::OnceClosure on_contents_destroyed = {});

  ~XenonMacPlayerView();

  XenonMacPlayerView(const XenonMacPlayerView&) = delete;
  XenonMacPlayerView& operator=(const XenonMacPlayerView&) = delete;

  // An NSView address for a native addon in this Browser process only. Never
  // serialize this value or return it to a Utility/renderer process.
  uintptr_t native_view_for_in_process_addon() const;

  // Revalidates the local window and places the video below its compositor.
  // Tracks WebContents geometry, clipping and visibility without changing
  // compositor-owned views or layers.
  bool RefreshAttachment(std::string* error);

  // Stops observing and removes the view while retaining the rendering target
  // until this object is destroyed. Normally the SDK detaches first. Failed
  // SDK teardown may detach only this visual attachment if the owner retains
  // this object until SDK unInit succeeds or the Browser process exits.
  void Detach();

 private:
  class Impl;

  explicit XenonMacPlayerView(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_VIEW_H_
