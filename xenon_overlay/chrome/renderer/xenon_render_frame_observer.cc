// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/renderer/xenon_render_frame_observer.h"

#include <optional>

#include "base/command_line.h"
#include "base/logging.h"
#include "content/public/common/url_constants.h"
#include "content/public/renderer/render_frame.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "third_party/blink/public/web/web_navigation_type.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/renderer/ipc/xenon_ipc_renderer.h"
#include "xenon_overlay/chrome/renderer/js_xenon_api.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

namespace xenon {

namespace {

// Must match xenon_webui_controller.h host constants.
constexpr char kXenonOverlayWebUIHost[] = "xenon-overlay";
constexpr char kXenonLoginWebUIHost[] = "xenon-login";
}  // namespace

XenonRenderFrameObserver::XenonRenderFrameObserver(
    content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame),
      ipc_hint_(*base::CommandLine::ForCurrentProcess()) {}

XenonRenderFrameObserver::~XenonRenderFrameObserver() = default;

void XenonRenderFrameObserver::OnDestruct() {
  delete this;
}

void XenonRenderFrameObserver::DidStartNavigation(
    const GURL& url,
    std::optional<blink::WebNavigationType> navigation_type) {
  last_navigation_url_ = url;
}

bool XenonRenderFrameObserver::IsPageUrlEligibleForApi(const GURL& url) const {
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

bool XenonRenderFrameObserver::ShouldAttemptElectronIpcInstall() const {
  if (!render_frame()) {
    return false;
  }
  blink::WebLocalFrame* web_frame = render_frame()->GetWebFrame();
  if (!web_frame || web_frame->IsProvisional()) {
    return false;
  }
  // This is only a negative fast path. An application partition, including
  // its HTTP/file/custom-scheme documents, still asks the document-scoped
  // Browser host for authorization before installing any globals.
  return ipc_hint_.MayRequestConfig(
      url::Origin(web_frame->GetDocument().GetSecurityOrigin()),
      render_frame()->IsMainFrame());
}

void XenonRenderFrameObserver::DidClearWindowObject() {
  if (ShouldExposeXenonApi()) {
    JSXenonApi::Install(render_frame());
  }
}

void XenonRenderFrameObserver::DidCreateScriptContext(
    v8::Local<v8::Context> context,
    int32_t world_id) {
  // This hook runs once the context exists but before page scripts. Posting a
  // task from DidClearWindowObject lets inline scripts race the preload.
  // Browser's document host still decides whether this frame is authorized.
  if (world_id == 0 && ShouldAttemptElectronIpcInstall()) {
    ipc::XenonIpcRenderer::Install(render_frame(), context);
  }
}

}  // namespace xenon
