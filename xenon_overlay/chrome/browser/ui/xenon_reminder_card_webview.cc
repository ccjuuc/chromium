// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_reminder_card_webview.h"

#include <algorithm>
#include <optional>
#include <string>

#include "base/strings/escape.h"
#include "base/values.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/navigator/browser_navigator.h"
#include "chrome/browser/ui/navigator/browser_navigator_params.h"
#include "chrome/browser/ui/browser_tabstrip.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ui/xenon_reminder_notification_group.h"

namespace xenon {

namespace {

// 未从页面获取到尺寸时的默认宽度
constexpr int kDefaultCardWidth = 360;
// 未加载完成前给卡片一个最小高度，否则 WebView 视口为 0，body(scrollHeight) 为 0，无法得到有效高度
constexpr int kMinCardHeightBeforeLoad = 120;

// 无 URL 时的内置测试页
std::string GetTestDataHTML(const ReminderMessage& message) {
  std::string title = message.title.empty() ? "标题" : message.title;
  std::string description =
      "消息内容。此为 xenon 提醒卡片测试页，用于验证 WebView 容器与高度上报。";

  return base::StringPrintf(
      R"HTML(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
  body { margin: 0; padding: 8px; font-family: system-ui, sans-serif; background: transparent; }
  .card {
    background: #fff;
    border-radius: 12px;
    box-shadow: 0 4px 16px rgba(0,0,0,0.12);
    padding: 16px;
    border: 1px solid rgba(0,0,0,0.03);
  }
  .title { font-size: 15px; font-weight: 600; color: #333; }
  .content { font-size: 13px; color: #666; margin-top: 8px; }
  .close-btn { width: 16px; height: 16px; cursor: pointer; color: #999; float: right; }
  .close-btn:hover { color: #666; }
</style>
<script>
  function notifyHeightChange() {
    if (window.resizeTo) window.resizeTo(document.body.scrollWidth, document.body.scrollHeight);
  }
  function dismissCard() { window.close(); }
  document.addEventListener('DOMContentLoaded', function() {
    notifyHeightChange();
    if (window.ResizeObserver) {
      new ResizeObserver(notifyHeightChange).observe(document.body);
    }
  });
</script>
</head>
<body>
<div class="card">
  <div class="close-btn" onclick="dismissCard()">
    <svg viewBox="0 0 12 12" width="12" height="12"><path d="M1.5 1.5L10.5 10.5M1.5 10.5L10.5 1.5" stroke="currentColor" stroke-width="1.5"/></svg>
  </div>
  <div class="title">%s</div>
  <div class="content">%s</div>
</div>
</body>
</html>)HTML",
      base::EscapeForHTML(title).c_str(),
      base::EscapeForHTML(description).c_str());
}

// 页面精简为仅内嵌 url 的 iframe。
// 使用 html,body 填满视口并 overflow:hidden，避免「外层 body」出现滚动条；
// iframe 内部若内容超出仍会有 iframe 自身滚动条。
std::string BuildCardHTML(const ReminderMessage& message) {
  if (message.js_component_url.empty()) {
    return GetTestDataHTML(message);
  }
  std::string escaped_url = base::EscapeForHTML(message.js_component_url);
  return base::StringPrintf(
      R"(<!DOCTYPE html>
<html>
<head><meta charset="utf-8">
<style>html,body{margin:0;padding:0;width:100%%;height:100%%;overflow:hidden;}</style>
</head>
<body>
<iframe src="%s" style="width:100%%;height:100%%;border:none;border-radius:8px;display:block;"></iframe>
</body>
</html>)",
      escaped_url.c_str());
}

// 可选：将 message_id 挂到 WebContents 上，便于外部通过 WebContents 反查卡片 id。
class XenonCardMessageIdUserData
    : public content::WebContentsUserData<XenonCardMessageIdUserData> {
 public:
  const std::string& message_id() const { return message_id_; }

 private:
  friend class content::WebContentsUserData<XenonCardMessageIdUserData>;
  XenonCardMessageIdUserData(content::WebContents* contents,
                             const std::string& message_id)
      : content::WebContentsUserData<XenonCardMessageIdUserData>(*contents),
        message_id_(message_id) {}
  std::string message_id_;
  WEB_CONTENTS_USER_DATA_KEY_DECL();
};
WEB_CONTENTS_USER_DATA_KEY_IMPL(XenonCardMessageIdUserData);

}  // namespace

XenonReminderCardWebView::XenonReminderCardWebView(
    Profile* profile,
    const ReminderMessage& message)
    : views::WebView(profile),
      message_id_(message.id),
      message_(message) {
  web_contents_ =
      content::WebContents::Create(content::WebContents::CreateParams(profile));
  web_contents_->SetDelegate(this);
  XenonCardMessageIdUserData::CreateForWebContents(web_contents_.get(),
                                                   message_id_);

  web_contents_->SetPageBaseBackgroundColor(SK_ColorTRANSPARENT);
  SetWebContents(web_contents_.get());

  if (!message_.title.empty()) {
    SetAccessibleName(base::UTF8ToUTF16(message_.title));
  } else {
    SetAccessibleName(u"Reminder");
  }

  LoadCardContent();
}

XenonReminderCardWebView::~XenonReminderCardWebView() {
  SetWebContents(nullptr);
  web_contents_.reset();
}

void XenonReminderCardWebView::SetDismissCallback(DismissCallback callback) {
  dismiss_callback_ = std::move(callback);
}

void XenonReminderCardWebView::ReleaseWebContents() {
  if (web_contents_) {
    SetWebContents(nullptr);
    web_contents_.reset();
  }
}

gfx::Size XenonReminderCardWebView::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  int width = content_width_ > 0 ? content_width_ : kDefaultCardWidth;
  int height =
      (is_loaded_ && content_height_ > 0) ? content_height_
                                          : kMinCardHeightBeforeLoad;
  return gfx::Size(width, height);
}

void XenonReminderCardWebView::LoadCardContent() {
  if (!web_contents_) {
    return;
  }
  std::string html = BuildCardHTML(message_);
  std::string data_url =
      "data:text/html;charset=utf-8," + base::EscapeUrlEncodedData(html, false);
  web_contents_->GetController().LoadURL(
      GURL(data_url), content::Referrer(),
      ui::PAGE_TRANSITION_AUTO_TOPLEVEL, std::string());
}

void XenonReminderCardWebView::CloseContents(content::WebContents* source) {
  if (dismiss_callback_) {
    dismiss_callback_.Run(message_id_);
  }
}

content::WebContents* XenonReminderCardWebView::AddNewContents(
    content::WebContents* source,
    std::unique_ptr<content::WebContents> new_contents,
    const GURL& target_url,
    WindowOpenDisposition disposition,
    const blink::mojom::WindowFeatures& window_features,
    bool user_gesture,
    bool* was_blocked) {
  BrowserWindowInterface* browser_window =
      GlobalBrowserCollection::GetInstance()->GetLastActiveBrowser();
  Browser* browser =
      browser_window ? browser_window->GetBrowserForMigrationOnly() : nullptr;
  if (!browser) {
    if (was_blocked) {
      *was_blocked = true;
    }
    return nullptr;
  }
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (browser_view && browser_view->IsMinimized()) {
    browser_view->Restore();
  }
  content::WebContents* result = chrome::AddWebContents(
      browser, source, std::move(new_contents), target_url, disposition,
      window_features, NavigateParams::WindowAction::kShowWindow, user_gesture);
  if (was_blocked) {
    *was_blocked = (result == nullptr);
  }
  return result;
}

content::WebContents* XenonReminderCardWebView::OpenURLFromTab(
    content::WebContents* source,
    const content::OpenURLParams& params,
    base::OnceCallback<void(content::NavigationHandle&)>
        navigation_handle_callback) {
  BrowserWindowInterface* browser_window =
      GlobalBrowserCollection::GetInstance()->GetLastActiveBrowser();
  Browser* browser =
      browser_window ? browser_window->GetBrowserForMigrationOnly() : nullptr;
  if (!browser) {
    return nullptr;
  }
  BrowserView* browser_view = BrowserView::GetBrowserViewForBrowser(browser);
  if (browser_view && browser_view->IsMinimized()) {
    browser_view->Restore();
  }
  NavigateParams navigate_params(browser, params.url, params.transition);
  navigate_params.disposition = params.disposition;
  navigate_params.referrer = params.referrer;
  navigate_params.source_contents = source;
  if (navigate_params.disposition == WindowOpenDisposition::CURRENT_TAB) {
    navigate_params.disposition = WindowOpenDisposition::NEW_FOREGROUND_TAB;
  }
  Navigate(&navigate_params);
  return navigate_params.navigated_or_inserted_contents;
}

void XenonReminderCardWebView::SetContentsBounds(content::WebContents* source,
                                                 const gfx::Rect& bounds) {
  int new_width = bounds.width() > 0 ? bounds.width() : content_width_;
  int new_height = bounds.height() > 0 ? bounds.height() : content_height_;
  bool changed = false;
  if (new_width > 0 && new_width != content_width_) {
    content_width_ = new_width;
    changed = true;
  }
  if (new_height > 0 && new_height != content_height_) {
    content_height_ = new_height;
    changed = true;
  }
  if (changed) {
    View::InvalidateLayout();
    views::View* parent_view = View::parent();
    if (parent_view) {
      parent_view->InvalidateLayout();
      views::View* grandparent = parent_view->parent();
      if (grandparent) {
        grandparent->InvalidateLayout();
      }
    }
  }
}

void XenonReminderCardWebView::DocumentOnLoadCompletedInPrimaryMainFrame() {
  if (!web_contents_) {
    return;
  }
  // 页面加载完成后获取 body 的 scroll 尺寸（宽高），用于卡片大小
  web_contents_->GetPrimaryMainFrame()->ExecuteJavaScriptForTests(
      u"(function() { return { w: document.body.scrollWidth, h: "
      u"document.body.scrollHeight }; })()",
      base::BindOnce(&XenonReminderCardWebView::OnContentHeightReceived,
                     weak_factory_.GetWeakPtr()),
      /*world_id=*/0);
}

void XenonReminderCardWebView::TitleWasSet(content::NavigationEntry* entry) {
  if (!web_contents_) {
    return;
  }
  std::u16string title = web_contents_->GetTitle();
  static constexpr char16_t kCmdPrefix[] = u"__XENON_CMD__:";
  if (!title.starts_with(kCmdPrefix)) {
    return;
  }
  std::u16string cmd =
      title.substr(std::char_traits<char16_t>::length(kCmdPrefix));
  if (cmd == u"expandGroup") {
    auto* group = static_cast<XenonReminderNotificationGroup*>(parent());
    if (group) {
      group->SetCollapsed(false);
    }
  }
}

void XenonReminderCardWebView::OnContentHeightReceived(base::Value result) {
  // 支持 { w, h } 或兼容仅返回 height 的 int
  if (result.is_dict()) {
    const base::DictValue& d = result.GetDict();
    if (std::optional<int> w = d.FindInt("w"); w && *w > 0 && *w != content_width_) {
      content_width_ = *w;
    }
    if (std::optional<int> h = d.FindInt("h"); h && *h > 0 && *h != content_height_) {
      content_height_ = *h;
    }
  } else if (result.is_int()) {
    int h = result.GetInt();
    if (h > 0 && h != content_height_) {
      content_height_ = h;
    }
  }
  if (!is_loaded_) {
    is_loaded_ = true;
    View::InvalidateLayout();
    views::View* parent_view = View::parent();
    if (parent_view) {
      parent_view->InvalidateLayout();
    }
    if (!message_.expire_time.is_null() &&
        message_.expire_time > base::Time::Now()) {
      base::TimeDelta delay = message_.expire_time - base::Time::Now();
      expire_timer_.Start(
          FROM_HERE, delay,
          base::BindOnce(
              [](base::WeakPtr<XenonReminderCardWebView> self) {
                if (self && self->dismiss_callback_) {
                  self->dismiss_callback_.Run(self->message_id_);
                }
              },
              weak_factory_.GetWeakPtr()));
    }
    if (message_.display_on_desktop) {
      constexpr base::TimeDelta kDesktopAutoCloseDelay = base::Seconds(10);
      base::TimeDelta current_delay =
          expire_timer_.IsRunning()
              ? expire_timer_.desired_run_time() - base::TimeTicks::Now()
              : base::TimeDelta::Max();
      if (kDesktopAutoCloseDelay < current_delay) {
        expire_timer_.Start(
            FROM_HERE, kDesktopAutoCloseDelay,
            base::BindOnce(
                [](base::WeakPtr<XenonReminderCardWebView> self) {
                  if (self && self->dismiss_callback_) {
                    self->dismiss_callback_.Run(self->message_id_);
                  }
                },
                weak_factory_.GetWeakPtr()));
      }
    }
  }
}

void XenonReminderCardWebView::UpdateContentHeight(int height) {
  if (height > 0 && height != content_height_) {
    content_height_ = height;
    View::InvalidateLayout();
    views::View* parent_view = View::parent();
    while (parent_view) {
      parent_view->InvalidateLayout();
      parent_view = parent_view->parent();
    }
  }
}

BEGIN_METADATA(XenonReminderCardWebView)
END_METADATA

}  // namespace xenon
