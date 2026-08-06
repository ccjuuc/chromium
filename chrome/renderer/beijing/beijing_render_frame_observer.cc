// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/renderer/beijing/beijing_render_frame_observer.h"

#include <optional>
#include <string>
#include <vector>

#include "base/command_line.h"
#include "base/no_destructor.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "chrome/common/beijing/render_dll_names.h"
#include "chrome/renderer/beijing/js_beijing_api.h"
#include "chrome/renderer/beijing/js_render_dll_api.h"
#include "content/public/common/url_constants.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "url/origin.h"

namespace beijing {

namespace {

// 判断主机名 host 是否匹配模式 pattern (例如 "*.so.com" 匹配 "www.so.com" 或 "so.com")
bool MatchDomainPattern(const std::string& host, const std::string& pattern) {
  std::string trimmed_pattern(
      base::TrimWhitespaceASCII(pattern, base::TRIM_ALL));
  if (trimmed_pattern == "*") {
    return true;
  }
  if (base::StartsWith(trimmed_pattern, "*.",
                       base::CompareCase::INSENSITIVE_ASCII)) {
    std::string domain = trimmed_pattern.substr(2);
    return host == domain ||
           base::EndsWith(host, "." + domain,
                          base::CompareCase::INSENSITIVE_ASCII);
  }
  return base::EqualsCaseInsensitiveASCII(host, trimmed_pattern);
}

// 检查是否为本地回环或受信任的 *.so.com 域名
bool IsLocalOrBeijingHost(std::string_view host) {
  return host == "localhost" || host == "127.0.0.1" || host == "::1" ||
         MatchDomainPattern(std::string(host), "*.so.com");
}

bool MatchesAllowedDomainSwitch(const GURL& url) {
  static base::NoDestructor<std::vector<std::string>> allowed_domains([]() {
    auto* cmd_line = base::CommandLine::ForCurrentProcess();
    std::string val = cmd_line->GetSwitchValueASCII("beijing-allowed-domains");
    return base::SplitString(val, ",", base::TRIM_WHITESPACE,
                             base::SPLIT_WANT_NONEMPTY);
  }());

  std::string host(url.host());
  for (const std::string& pattern : *allowed_domains) {
    if (MatchDomainPattern(host, pattern)) {
      return true;
    }
  }
  return false;
}

// window.beijing：不放行 file://（避免本地测试页顺带暴露 beijing）。
bool IsBeijingUrlAllowed(const GURL& url) {
  if (url.is_empty() || !url.is_valid()) {
    return false;
  }
  if (MatchesAllowedDomainSwitch(url)) {
    return true;
  }
  return IsLocalOrBeijingHost(url.host());
}

#if !defined(OFFICIAL_BUILD)
// window.renderDll：beijing 白名单 + file:// + chrome://render-dll-test/。
bool IsRenderDllUrlAllowed(const GURL& url) {
  if (url.is_empty() || !url.is_valid()) {
    return false;
  }
  if (url.SchemeIsFile()) {
    return true;
  }
  if (url.SchemeIs(content::kChromeUIScheme) &&
      url.host() == kRenderDllTestHost) {
    return true;
  }
  return IsBeijingUrlAllowed(url);
}
#endif

GURL ResolvePageUrl(content::RenderFrame* render_frame,
                    const GURL& last_navigation_url) {
  blink::WebLocalFrame* web_frame = render_frame->GetWebFrame();
  if (!web_frame) {
    return GURL();
  }

  GURL url = last_navigation_url;
  if (url.is_empty() || !url.is_valid() || url.spec() == "about:blank") {
    url = url::Origin(web_frame->GetSecurityOrigin()).GetURL();
  }
  return url;
}

bool IsSecureEnoughForApi(blink::WebLocalFrame* web_frame, const GURL& url) {
  bool is_secure = web_frame->GetDocument().IsSecureContext();
  if (is_secure) {
    return true;
  }
#if !defined(OFFICIAL_BUILD)
  return url.SchemeIsFile() || IsLocalOrBeijingHost(url.host());
#else
  return IsLocalOrBeijingHost(url.host());
#endif
}

}  // namespace

BeijingRenderFrameObserver::BeijingRenderFrameObserver(
    content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {}

void BeijingRenderFrameObserver::OnDestruct() {
  delete this;
}

void BeijingRenderFrameObserver::DidStartNavigation(
    const GURL& url,
    std::optional<blink::WebNavigationType> navigation_type) {
  last_navigation_url_ = url;
}

bool BeijingRenderFrameObserver::IsPageUrlEligibleForBeijingApi(
    const GURL& url) const {
  return IsBeijingUrlAllowed(url);
}

bool BeijingRenderFrameObserver::ShouldExposeBeijingApi() const {
  if (!render_frame() || !render_frame()->IsMainFrame()) {
    return false;
  }

  blink::WebLocalFrame* web_frame = render_frame()->GetWebFrame();
  if (!web_frame) {
    return false;
  }

  const GURL url = ResolvePageUrl(render_frame(), last_navigation_url_);
  if (url.is_empty() || !url.is_valid()) {
    return false;
  }

  if (!IsSecureEnoughForApi(web_frame, url)) {
    return false;
  }

  return IsPageUrlEligibleForBeijingApi(url);
}

#if !defined(OFFICIAL_BUILD)
bool BeijingRenderFrameObserver::IsPageUrlEligibleForRenderDllApi(
    const GURL& url) const {
  return IsRenderDllUrlAllowed(url);
}

bool BeijingRenderFrameObserver::ShouldExposeRenderDllApi() const {
  if (!render_frame() || !render_frame()->IsMainFrame()) {
    return false;
  }

  blink::WebLocalFrame* web_frame = render_frame()->GetWebFrame();
  if (!web_frame) {
    return false;
  }

  const GURL url = ResolvePageUrl(render_frame(), last_navigation_url_);
  if (url.is_empty() || !url.is_valid()) {
    return false;
  }

  if (!IsSecureEnoughForApi(web_frame, url)) {
    return false;
  }

  return IsPageUrlEligibleForRenderDllApi(url);
}
#endif

void BeijingRenderFrameObserver::DidClearWindowObject() {
  if (ShouldExposeBeijingApi()) {
    JSBeijingApi::Install(render_frame());
  }
#if !defined(OFFICIAL_BUILD)
  if (ShouldExposeRenderDllApi()) {
    JSRenderDllApi::Install(render_frame());
  }
#endif
}

}  // namespace beijing
