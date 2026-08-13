// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_BROWSER_OBSERVER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_BROWSER_OBSERVER_H_

#include <map>
#include <memory>

#include "base/memory/raw_ptr.h"
#include "base/scoped_observation.h"
#include "chrome/browser/ui/browser_window/public/browser_collection_observer.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "ui/base/accelerators/accelerator.h"

class Browser;

namespace views {
class FocusManager;
}

namespace xenon {

class XenonReminderNotificationWidget;
class XenonTipBarWidget;

// 监听 Browser 创建/销毁：为每个 Browser 创建提醒 Widget + TipBar Widget，
// 并注册全局快捷键 Ctrl+Shift+R 用于测试 Manager（触发一条测试 iframe 提醒）。
class XenonReminderBrowserObserver : public BrowserCollectionObserver,
                                     public ui::AcceleratorTarget {
 public:
  XenonReminderBrowserObserver();
  XenonReminderBrowserObserver(const XenonReminderBrowserObserver&) = delete;
  XenonReminderBrowserObserver& operator=(const XenonReminderBrowserObserver&) =
      delete;
  ~XenonReminderBrowserObserver() override;

  // BrowserCollectionObserver:
  void OnBrowserCreated(BrowserWindowInterface* browser) override;
  void OnBrowserClosed(BrowserWindowInterface* browser) override;

  void AttachToBrowser(Browser* browser);

  // ui::AcceleratorTarget（测试用：按下快捷键时调用 Manager::HandlePushMessage）
  bool AcceleratorPressed(const ui::Accelerator& accelerator) override;
  bool CanHandleAccelerators() const override;

 private:
  void RegisterTestAccelerator(Browser* browser);
  void UnregisterTestAccelerator(Browser* browser);
  views::FocusManager* GetFocusManagerForBrowser(Browser* browser);

  std::map<Browser*, std::unique_ptr<XenonReminderNotificationWidget>>
      reminder_widgets_;
  std::map<Browser*, std::unique_ptr<XenonTipBarWidget>> tip_bar_widgets_;
  std::map<Browser*, raw_ptr<views::FocusManager>> focus_managers_;
  base::ScopedObservation<GlobalBrowserCollection, BrowserCollectionObserver>
      browser_collection_observation_{this};
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_REMINDER_BROWSER_OBSERVER_H_
