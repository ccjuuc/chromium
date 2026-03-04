// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_CARD_WEBVIEW_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_CARD_WEBVIEW_H_

#include <memory>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/timer/timer.h"
#include "base/values.h"
#include "content/public/browser/web_contents_delegate.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/views/controls/webview/webview.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_types.h"

namespace content {
class WebContents;
}

class Profile;

namespace xenon {

// 单张提醒卡片：WebView 容器，持有 WebContents，加载 data URL 或外部 JS 组件。
// 对应 joyme 的 JoymeReminderCardWebView，供 xenon 端显示 web 卡片使用。
class XenonReminderCardWebView : public views::WebView {
  METADATA_HEADER(XenonReminderCardWebView, views::WebView)

 public:
  using DismissCallback = base::RepeatingCallback<void(const std::string&)>;

  XenonReminderCardWebView(Profile* profile, const ReminderMessage& message);
  XenonReminderCardWebView(const XenonReminderCardWebView&) = delete;
  XenonReminderCardWebView& operator=(const XenonReminderCardWebView&) = delete;
  ~XenonReminderCardWebView() override;

  const std::string& message_id() const { return message_id_; }
  bool is_loaded() const { return is_loaded_; }
  void SetDismissCallback(DismissCallback callback);
  void ReleaseWebContents();

  // views::View
  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override;

  // content::WebContentsDelegate
  void CloseContents(content::WebContents* source) override;
  void SetContentsBounds(content::WebContents* source,
                         const gfx::Rect& bounds) override;
  content::WebContents* AddNewContents(
      content::WebContents* source,
      std::unique_ptr<content::WebContents> new_contents,
      const GURL& target_url,
      WindowOpenDisposition disposition,
      const blink::mojom::WindowFeatures& window_features,
      bool user_gesture,
      bool* was_blocked) override;
  content::WebContents* OpenURLFromTab(
      content::WebContents* source,
      const content::OpenURLParams& params,
      base::OnceCallback<void(content::NavigationHandle&)>
          navigation_handle_callback) override;

  // content::WebContentsObserver
  void DocumentOnLoadCompletedInPrimaryMainFrame() override;
  void TitleWasSet(content::NavigationEntry* entry) override;

 private:
  void LoadCardContent();
  void UpdateContentHeight(int height);
  void OnContentHeightReceived(base::Value result);

  std::string message_id_;
  ReminderMessage message_;
  std::unique_ptr<content::WebContents> web_contents_;
  DismissCallback dismiss_callback_;
  int content_width_ = 0;
  int content_height_ = 0;
  bool is_loaded_ = false;

  base::OneShotTimer expire_timer_;
  base::WeakPtrFactory<XenonReminderCardWebView> weak_factory_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_CARD_WEBVIEW_H_
