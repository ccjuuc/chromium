// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_TIP_BAR_WIDGET_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_TIP_BAR_WIDGET_H_

#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/timer/timer.h"
#include "ui/views/widget/widget_observer.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_observer.h"

class Browser;
class BrowserView;

namespace views {
class Label;
class LabelButton;
class Widget;
}  // namespace views

namespace xenon {

// 服务端 tipMessage 提示条：浮动在内容区右上角的圆角 InfoBar。
class XenonTipBarWidget : public XenonReminderNotificationObserver,
                          public views::WidgetObserver {
 public:
  static constexpr int kDefaultAutoCloseMs = 5000;

  static std::unique_ptr<XenonTipBarWidget> CreateForBrowser(
      Browser* browser,
      BrowserView* browser_view = nullptr);

  XenonTipBarWidget(const XenonTipBarWidget&) = delete;
  XenonTipBarWidget& operator=(const XenonTipBarWidget&) = delete;
  ~XenonTipBarWidget() override;

  void OnReminderMessageAdded(const ReminderMessage& message) override {}
  void OnReminderMessageRemoved(const std::string& message_id) override {}
  void OnReminderMessageUpdated(const ReminderMessage& message) override {}
  void OnReminderMessagesCleared() override {}
  void OnTipMessageReceived(const std::string& tip_text) override;

  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& new_bounds) override;
  void OnWidgetDestroying(views::Widget* widget) override;

  void ShowTip(const std::string& tip_text);
  void Hide();
  bool IsVisible() const;

 private:
  explicit XenonTipBarWidget(Browser* browser);

  void CreateWidget(BrowserView* browser_view);
  void UpdateWidgetPosition();
  void OnCloseButtonPressed();
  void OnAutoCloseTimer();
  views::Widget* GetBrowserWidget() const;

  raw_ptr<Browser> browser_ = nullptr;
  raw_ptr<BrowserView> browser_view_ = nullptr;
  // |widget_| owns the content tree that |label_| / |close_btn_| point into.
  // Declare the owning Widget first so child raw_ptrs are cleared before it
  // (destruction order is reverse of declaration); otherwise exit hits
  // dangling raw_ptr checks in ~XenonTipBarWidget.
  std::unique_ptr<views::Widget> widget_;
  raw_ptr<views::Label> label_ = nullptr;
  raw_ptr<views::LabelButton> close_btn_ = nullptr;
  raw_ptr<views::Widget> observed_parent_ = nullptr;

  base::OneShotTimer auto_close_timer_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_TIP_BAR_WIDGET_H_
