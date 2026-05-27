// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_RENDERER_BEIJING_BEIJING_RENDER_FRAME_OBSERVER_H_
#define CHROME_RENDERER_BEIJING_BEIJING_RENDER_FRAME_OBSERVER_H_

#include "content/public/renderer/render_frame.h"
#include "content/public/renderer/render_frame_observer.h"
#include "third_party/blink/public/web/web_navigation_type.h"
#include "url/gurl.h"

namespace beijing {

// 渲染器端观察器：在 DidClearWindowObject() 时向允许的域名注入全局 window.beijing 对象
class BeijingRenderFrameObserver : public content::RenderFrameObserver {
 public:
  explicit BeijingRenderFrameObserver(content::RenderFrame* render_frame);

  BeijingRenderFrameObserver(const BeijingRenderFrameObserver&) = delete;
  BeijingRenderFrameObserver& operator=(const BeijingRenderFrameObserver&) = delete;

 private:
  void OnDestruct() override;

  void DidStartNavigation(
      const GURL& url,
      std::optional<blink::WebNavigationType> navigation_type) override;

  void DidClearWindowObject() override;

  bool IsPageUrlEligibleForApi(const GURL& url) const;
  bool ShouldExposeBeijingApi() const;

  GURL last_navigation_url_;
};

}  // namespace beijing

#endif  // CHROME_RENDERER_BEIJING_BEIJING_RENDER_FRAME_OBSERVER_H_
