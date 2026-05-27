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
#include "chrome/renderer/beijing/js_beijing_api.h"
#include "third_party/blink/public/web/web_document.h"
#include "third_party/blink/public/web/web_local_frame.h"
#include "url/origin.h"

namespace beijing {

namespace {

// 判断主机名 host 是否匹配模式 pattern (例如 "*.so.com" 匹配 "www.so.com" 或 "so.com")
bool MatchDomainPattern(const std::string& host, const std::string& pattern) {
  std::string trimmed_pattern(base::TrimWhitespaceASCII(pattern, base::TRIM_ALL));
  if (trimmed_pattern == "*") {
    return true;
  }
  if (base::StartsWith(trimmed_pattern, "*.", base::CompareCase::INSENSITIVE_ASCII)) {
    std::string domain = trimmed_pattern.substr(2);
    return host == domain || base::EndsWith(host, "." + domain, base::CompareCase::INSENSITIVE_ASCII);
  }
  return base::EqualsCaseInsensitiveASCII(host, trimmed_pattern);
}

// 检查是否为本地回环或受信任的 *.so.com 域名
bool IsLocalOrBeijingHost(std::string_view host) {
  return host == "localhost" ||
         host == "127.0.0.1" ||
         host == "::1" ||
         MatchDomainPattern(std::string(host), "*.so.com");
}

// 独立域名鉴权校验，完全不引用任何 shenzhen 相关的文件与头文件
bool IsBeijingUrlAllowed(const GURL& url) {
  if (url.is_empty() || !url.is_valid()) {
    return false;
  }

  static base::NoDestructor<std::vector<std::string>> allowed_domains([]() {
    auto* cmd_line = base::CommandLine::ForCurrentProcess();
    std::string val = cmd_line->GetSwitchValueASCII("beijing-allowed-domains");
    return base::SplitString(val, ",", base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY);
  }());

  std::string host(url.host());
  for (const std::string& pattern : *allowed_domains) {
    if (MatchDomainPattern(host, pattern)) {
      return true;
    }
  }

  // 默认对安全可靠的 *.so.com 域，以及本地测试域名 localhost / 127.0.0.1 公开
  return IsLocalOrBeijingHost(host);
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

bool BeijingRenderFrameObserver::IsPageUrlEligibleForApi(
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

  GURL url = last_navigation_url_;
  if (url.is_empty() || !url.is_valid() || url.spec() == "about:blank") {
    url = url::Origin(web_frame->GetSecurityOrigin()).GetURL();
  }

  if (url.is_empty() || !url.is_valid()) {
    return false;
  }

  // 校验安全上下文：如果是 HTTPS / localhost 则是安全上下文；
  // 为了方便本地调试 http://so.com，特放行回环地址或匹配的 HTTP 测试域
  bool is_secure = web_frame->GetDocument().IsSecureContext();
  if (!is_secure) {
    is_secure = IsLocalOrBeijingHost(url.host());
  }

  if (!is_secure) {
    return false;
  }

  return IsPageUrlEligibleForApi(url);
}

void BeijingRenderFrameObserver::DidClearWindowObject() {
  if (!ShouldExposeBeijingApi()) {
    return;
  }
  JSBeijingApi::Install(render_frame());
}

}  // namespace beijing
