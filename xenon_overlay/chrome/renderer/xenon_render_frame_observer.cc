// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/xenon_render_frame_observer.h"

#include <optional>

#include "content/public/common/url_constants.h"
#include "xenon_overlay/chrome/renderer/js_xenon_api.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_navigation_type.h"
#include "url/origin.h"

namespace xenon {

namespace {

// Must match xenon_webui_controller.h host constants.
constexpr char kXenonOverlayWebUIHost[] = "xenon-overlay";
constexpr char kXenonLoginWebUIHost[] = "xenon-login";

}  // namespace

XenonRenderFrameObserver::XenonRenderFrameObserver(
    content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {}

void XenonRenderFrameObserver::OnDestruct() {
  delete this;
}

void XenonRenderFrameObserver::DidStartNavigation(
    const GURL& url,
    std::optional<blink::WebNavigationType> navigation_type) {
  last_navigation_url_ = url;
}

bool XenonRenderFrameObserver::IsPageUrlEligibleForApi(
    const GURL& url) const {
  if (url.is_empty() || !url.is_valid() || url.spec() == "about:blank") {
    return false;
  }
  if (url.SchemeIsHTTPOrHTTPS() || url.SchemeIs("chrome-extension")) {
    return true;
  }
  // Trusted Xenon WebUIs (overlay + login gate).
  if (url.SchemeIs(content::kChromeUIScheme) &&
      (url.host() == kXenonOverlayWebUIHost ||
       url.host() == kXenonLoginWebUIHost)) {
    return true;
  }
  return false;
}

bool XenonRenderFrameObserver::ShouldExposeXenonApi() const {
  if (!render_frame() || !render_frame()->IsMainFrame()) {
    return false;
  }

  blink::WebLocalFrame* web_frame = render_frame()->GetWebFrame();
  if (!web_frame || web_frame->IsProvisional()) {
    return false;
  }

  if (!web_frame->GetDocument().IsSecureContext()) {
    return false;
  }

  GURL url = last_navigation_url_;
  if (url.is_empty() || !url.is_valid() || url.spec() == "about:blank") {
    url = url::Origin(web_frame->GetSecurityOrigin()).GetURL();
  }

  return IsPageUrlEligibleForApi(url);
}

void XenonRenderFrameObserver::DidClearWindowObject() {
  if (!ShouldExposeXenonApi()) {
    return;
  }
  JSXenonApi::Install(render_frame());
}

}  // namespace xenon
