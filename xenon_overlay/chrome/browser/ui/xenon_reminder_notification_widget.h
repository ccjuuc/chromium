// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_NOTIFICATION_WIDGET_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_NOTIFICATION_WIDGET_H_

#include <memory>

#include "base/memory/raw_ptr.h"
#include "ui/views/widget/widget_observer.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_observer.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_types.h"

class Browser;
class BrowserView;
class Profile;

namespace views {
class Widget;
}

namespace xenon {

class XenonReminderNotificationContainer;

// 提醒弹窗：浏览器模式 + 桌面模式两个 Widget。
class XenonReminderNotificationWidget
    : public XenonReminderNotificationObserver,
      public views::WidgetObserver {
 public:
  static std::unique_ptr<XenonReminderNotificationWidget> CreateForBrowser(
      Browser* browser,
      BrowserView* browser_view = nullptr);

  XenonReminderNotificationWidget(const XenonReminderNotificationWidget&) =
      delete;
  XenonReminderNotificationWidget& operator=(
      const XenonReminderNotificationWidget&) = delete;
  ~XenonReminderNotificationWidget() override;

  void OnReminderMessageAdded(const ReminderMessage& message) override;
  void OnReminderMessageRemoved(const std::string& message_id) override;
  void OnReminderMessageUpdated(const ReminderMessage& message) override;
  void OnReminderMessagesCleared() override;

  void OnWidgetActivationChanged(views::Widget* widget, bool active) override;
  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& new_bounds) override;
  void OnWidgetDestroying(views::Widget* widget) override;
  void OnWidgetVisibilityChanged(views::Widget* widget,
                                 bool visible) override;

  void Show();
  void ShowBrowser();
  void ShowDesktop();
  void Hide();
  bool IsVisible() const;

 private:
  explicit XenonReminderNotificationWidget(Browser* browser);

  void CreateBrowserWidget(BrowserView* browser_view);
  void CreateDesktopWidget();
  void UpdateBrowserWidgetPosition();
  void UpdateDesktopWidgetPosition();
  views::Widget* GetBrowserWidget() const;

  raw_ptr<Browser> browser_ = nullptr;
  raw_ptr<BrowserView> browser_view_ = nullptr;
  raw_ptr<Profile> profile_ = nullptr;

  raw_ptr<XenonReminderNotificationContainer> container_ = nullptr;
  std::unique_ptr<views::Widget> widget_;
  raw_ptr<views::Widget> observed_parent_ = nullptr;

  raw_ptr<XenonReminderNotificationContainer> desktop_container_ = nullptr;
  std::unique_ptr<views::Widget> desktop_widget_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_NOTIFICATION_WIDGET_H_
