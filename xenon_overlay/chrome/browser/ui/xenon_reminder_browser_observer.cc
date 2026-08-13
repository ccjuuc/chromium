// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_reminder_browser_observer.h"

#include "base/logging.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "ui/base/accelerators/accelerator.h"
#include "ui/events/keycodes/keyboard_codes.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/widget/widget.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_manager.h"
#include "xenon_overlay/chrome/browser/ui/xenon_reminder_notification_widget.h"
#include "xenon_overlay/chrome/browser/ui/xenon_tip_bar_widget.h"

namespace xenon {

namespace {

// 测试用快捷键：Ctrl+Shift+R，触发一条 iframe 提醒
constexpr ui::KeyboardCode kTestAcceleratorKey = ui::VKEY_R;
constexpr int kTestAcceleratorModifiers = ui::EF_CONTROL_DOWN | ui::EF_SHIFT_DOWN;

// 测试 iframe 使用的 URL（可改为任意可访问页面）
constexpr char kTestReminderUrl[] = "https://www.example.com";

}  // namespace

XenonReminderBrowserObserver::XenonReminderBrowserObserver() {
  browser_collection_observation_.Observe(
      GlobalBrowserCollection::GetInstance());
}

XenonReminderBrowserObserver::~XenonReminderBrowserObserver() {
  browser_collection_observation_.Reset();
  // 不在此处对 focus_managers_ 做 Unregister：进程退出时 Browser/Widget 可能已销毁，
  // 各窗口关闭时已在 OnBrowserRemoved 中 Unregister 并 erase。
  reminder_widgets_.clear();
  tip_bar_widgets_.clear();
  focus_managers_.clear();
}

void XenonReminderBrowserObserver::OnBrowserCreated(
    BrowserWindowInterface* browser_window) {
  AttachToBrowser(browser_window->GetBrowserForMigrationOnly());
}

void XenonReminderBrowserObserver::AttachToBrowser(Browser* browser) {
  if (!browser || !browser->GetProfile()) {
    return;
  }
  BrowserView* browser_view =
      BrowserView::GetBrowserViewForBrowser(browser);
  if (!browser_view) {
    return;
  }

  auto reminder_widget =
      XenonReminderNotificationWidget::CreateForBrowser(browser, browser_view);
  if (reminder_widget) {
    reminder_widgets_[browser] = std::move(reminder_widget);
  }

  auto tip_bar =
      XenonTipBarWidget::CreateForBrowser(browser, browser_view);
  if (tip_bar) {
    tip_bar_widgets_[browser] = std::move(tip_bar);
  }

  RegisterTestAccelerator(browser);
  LOG(INFO) << "XenonReminder: attached to browser, Ctrl+Shift+R to test";
}

void XenonReminderBrowserObserver::OnBrowserClosed(
    BrowserWindowInterface* browser_window) {
  Browser* browser = browser_window->GetBrowserForMigrationOnly();
  UnregisterTestAccelerator(browser);
  reminder_widgets_.erase(browser);
  tip_bar_widgets_.erase(browser);
  focus_managers_.erase(browser);
}

bool XenonReminderBrowserObserver::AcceleratorPressed(
    const ui::Accelerator& accelerator) {
  XenonReminderNotificationManager::GetInstance()->HandlePushMessage(
      kTestReminderUrl);
  LOG(INFO) << "XenonReminder: test shortcut pressed, added iframe reminder";
  return true;
}

bool XenonReminderBrowserObserver::CanHandleAccelerators() const {
  return true;
}

void XenonReminderBrowserObserver::RegisterTestAccelerator(Browser* browser) {
  views::FocusManager* focus_manager = GetFocusManagerForBrowser(browser);
  if (!focus_manager) {
    return;
  }
  ui::Accelerator acc(kTestAcceleratorKey, kTestAcceleratorModifiers);
  focus_manager->RegisterAccelerator(
      acc, ui::AcceleratorManager::kNormalPriority, this);
  focus_managers_[browser] = focus_manager;
}

void XenonReminderBrowserObserver::UnregisterTestAccelerator(Browser* browser) {
  auto it = focus_managers_.find(browser);
  if (it == focus_managers_.end() || !it->second) {
    return;
  }
  it->second->UnregisterAccelerator(
      ui::Accelerator(kTestAcceleratorKey, kTestAcceleratorModifiers), this);
}

views::FocusManager* XenonReminderBrowserObserver::GetFocusManagerForBrowser(
    Browser* browser) {
  BrowserView* browser_view =
      BrowserView::GetBrowserViewForBrowser(browser);
  if (!browser_view) {
    return nullptr;
  }
  views::Widget* widget = browser_view->GetWidget();
  return widget ? widget->GetFocusManager() : nullptr;
}

}  // namespace xenon
