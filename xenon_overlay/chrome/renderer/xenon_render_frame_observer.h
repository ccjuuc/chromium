// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_RENDERER_XENON_RENDER_FRAME_OBSERVER_H_
#define XENON_OVERLAY_CHROME_RENDERER_XENON_RENDER_FRAME_OBSERVER_H_

#include <optional>

#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "third_party/blink/public/web/web_navigation_type.h"
#include "url/gurl.h"

namespace xenon {

// Installs `window.xenon` on each navigation when the document is eligible
// (see ShouldExposeXenonApi), aligned with Brave wallet's
// BraveWalletRenderFrameObserver::DidClearWindowObject timing.
class XenonRenderFrameObserver : public content::RenderFrameObserver {
 public:
  explicit XenonRenderFrameObserver(content::RenderFrame* render_frame);
  ~XenonRenderFrameObserver() override;

  XenonRenderFrameObserver(const XenonRenderFrameObserver&) = delete;
  XenonRenderFrameObserver& operator=(const XenonRenderFrameObserver&) = delete;

 private:
  void OnDestruct() override;

  void DidStartNavigation(
      const GURL& url,
      std::optional<blink::WebNavigationType> navigation_type) override;

  void DidClearWindowObject() override;
  void DidCreateScriptContext(v8::Local<v8::Context> context,
                              int32_t world_id) override;

  bool IsPageUrlEligibleForApi(const GURL& url) const;
  bool ShouldExposeXenonApi() const;
  bool ShouldAttemptElectronIpcInstall() const;

  GURL last_navigation_url_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_RENDERER_XENON_RENDER_FRAME_OBSERVER_H_
