// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_reminder_notification_widget.h"

#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "third_party/skia/include/core/SkPath.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/ui_base_types.h"
#include "ui/compositor/layer.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/skia_conversions.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_manager.h"
#include "xenon_overlay/chrome/browser/ui/xenon_reminder_notification_container.h"

namespace xenon {

namespace {

constexpr int kMarginRight = 5;
constexpr int kMarginTop = 30;

class XenonReminderWidgetDelegate : public views::WidgetDelegate {
 public:
  explicit XenonReminderWidgetDelegate(
      XenonReminderNotificationContainer* container)
      : container_(container) {}
  ~XenonReminderWidgetDelegate() override = default;

  bool WidgetHasHitTestMask() const override { return false; }

  void GetWidgetHitTestMask(SkPath* mask) const override {
    if (!mask || !container_) {
      return;
    }
    SkPathBuilder builder;
    std::vector<gfx::Rect> card_bounds = container_->GetCardBounds();
    for (const gfx::Rect& rect : card_bounds) {
      builder.addRect(gfx::RectToSkRect(rect));
    }
    *mask = builder.detach();
  }

 private:
  raw_ptr<XenonReminderNotificationContainer> container_;
};

bool CalculateContentSize(XenonReminderNotificationContainer* container,
                          int& out_width,
                          int& out_height) {
  constexpr int kMinPlaceholderHeight = 80;
  out_width = 360;
  out_height = 0;
  if (!container) {
    return false;
  }
  if (container->needs_layout()) {
    container->DeprecatedLayoutImmediately();
  }
  if (container->children().empty()) {
    return false;
  }
  gfx::Size preferred = container->GetPreferredSize();
  out_width = std::max(preferred.width(), 360);
  out_height = preferred.height();
  if (out_height <= 0) {
    out_height = kMinPlaceholderHeight;
  }
  return true;
}

}  // namespace

std::unique_ptr<XenonReminderNotificationWidget>
XenonReminderNotificationWidget::CreateForBrowser(Browser* browser,
                                                  BrowserView* browser_view) {
  if (!browser || !browser->GetProfile()) {
    return nullptr;
  }
  std::unique_ptr<XenonReminderNotificationWidget> widget(
      new XenonReminderNotificationWidget(browser));
  widget->CreateBrowserWidget(browser_view);
  widget->CreateDesktopWidget();
  return widget;
}

XenonReminderNotificationWidget::XenonReminderNotificationWidget(
    Browser* browser)
    : browser_(browser), profile_(browser->GetProfile()) {
  XenonReminderNotificationManager::GetInstance()->AddObserver(this);
}

XenonReminderNotificationWidget::~XenonReminderNotificationWidget() {
  if (observed_parent_) {
    observed_parent_->RemoveObserver(this);
    observed_parent_ = nullptr;
  }
  if (container_) {
    container_->ClearMessages();
  }
  if (desktop_container_) {
    desktop_container_->ClearMessages();
  }
  container_ = nullptr;
  desktop_container_ = nullptr;
  widget_.reset();
  desktop_widget_.reset();
  XenonReminderNotificationManager::GetInstance()->RemoveObserver(this);
}

void XenonReminderNotificationWidget::OnReminderMessageAdded(
    const ReminderMessage& message) {
  if (container_) {
    container_->AddMessage(message);
  }
  ShowBrowser();

  views::Widget* browser_wgt = GetBrowserWidget();
  bool browser_active = browser_wgt && browser_wgt->IsActive();
  if (message.display_on_desktop || !browser_active) {
    ReminderMessage msg = message;
    msg.display_on_desktop = true;
    if (desktop_container_) {
      desktop_container_->AddMessage(msg);
    }
    ShowDesktop();
  }
}

void XenonReminderNotificationWidget::OnReminderMessageRemoved(
    const std::string& message_id) {
  if (container_) {
    container_->RemoveMessage(message_id);
  }
  if (desktop_container_) {
    desktop_container_->RemoveMessage(message_id);
  }
  if (widget_ && container_ && container_->GetCardBounds().empty()) {
    widget_->Hide();
  }
  if (desktop_widget_ && desktop_container_ &&
      desktop_container_->GetCardBounds().empty()) {
    desktop_widget_->Hide();
  }
}

void XenonReminderNotificationWidget::OnReminderMessageUpdated(
    const ReminderMessage& message) {
  if (container_) {
    container_->UpdateMessage(message);
  }
  if (desktop_container_) {
    desktop_container_->UpdateMessage(message);
  }
}

void XenonReminderNotificationWidget::OnReminderMessagesCleared() {
  if (container_) {
    container_->ClearMessages();
  }
  if (desktop_container_) {
    desktop_container_->ClearMessages();
  }
  Hide();
}

void XenonReminderNotificationWidget::OnWidgetActivationChanged(
    views::Widget* widget,
    bool active) {
  if (widget == observed_parent_ && widget_) {
#if BUILDFLAG(IS_MAC)
    widget_->SetZOrderLevel(active ? ui::ZOrderLevel::kFloatingWindow
                                   : ui::ZOrderLevel::kNormal);
#endif
  }
}

void XenonReminderNotificationWidget::OnWidgetBoundsChanged(
    views::Widget* widget,
    const gfx::Rect& new_bounds) {
  if (widget == observed_parent_) {
    UpdateBrowserWidgetPosition();
  }
}

void XenonReminderNotificationWidget::OnWidgetDestroying(
    views::Widget* widget) {
  if (widget == observed_parent_) {
    observed_parent_->RemoveObserver(this);
    observed_parent_ = nullptr;
  }
}

void XenonReminderNotificationWidget::OnWidgetVisibilityChanged(
    views::Widget* widget,
    bool visible) {
  if (widget != observed_parent_) {
    return;
  }
  if (visible && widget_ && widget_->IsVisible()) {
    UpdateBrowserWidgetPosition();
  }
}

void XenonReminderNotificationWidget::Show() {
  ShowBrowser();
  ShowDesktop();
}

void XenonReminderNotificationWidget::ShowBrowser() {
  if (!widget_) {
    return;
  }
  int width, content_height;
  if (!CalculateContentSize(container_, width, content_height)) {
    widget_->Hide();
    return;
  }
  UpdateBrowserWidgetPosition();
  widget_->Show();
  views::Widget* browser_wgt = GetBrowserWidget();
  if (browser_wgt) {
    widget_->StackAboveWidget(browser_wgt);
  }
}

void XenonReminderNotificationWidget::ShowDesktop() {
  if (!desktop_widget_) {
    return;
  }
  int width, content_height;
  if (!CalculateContentSize(desktop_container_, width, content_height)) {
    desktop_widget_->Hide();
    return;
  }
  UpdateDesktopWidgetPosition();
  desktop_widget_->Show();
}

void XenonReminderNotificationWidget::Hide() {
  if (widget_) {
    widget_->Hide();
  }
  if (desktop_widget_) {
    desktop_widget_->Hide();
  }
}

bool XenonReminderNotificationWidget::IsVisible() const {
  return (widget_ && widget_->IsVisible()) ||
         (desktop_widget_ && desktop_widget_->IsVisible());
}

void XenonReminderNotificationWidget::CreateBrowserWidget(
    BrowserView* browser_view) {
  views::Widget* parent =
      browser_view ? browser_view->GetWidget() : GetBrowserWidget();
  if (!parent) {
    return;
  }

  browser_view_ = browser_view;

  views::Widget::InitParams params(
      views::Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET,
      views::Widget::InitParams::TYPE_POPUP);
  params.parent = parent->GetNativeView();
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
  params.activatable = views::Widget::InitParams::Activatable::kNo;
  params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
#if BUILDFLAG(IS_MAC)
  params.z_order = parent->IsActive() ? ui::ZOrderLevel::kFloatingWindow
                                      : ui::ZOrderLevel::kNormal;
#endif
  params.name = "XenonReminderNotificationWidget";
  params.accept_events = true;

  auto container =
      std::make_unique<XenonReminderNotificationContainer>(profile_);
  XenonReminderNotificationContainer* container_ptr = container.get();

  auto delegate =
      std::make_unique<XenonReminderWidgetDelegate>(container_ptr);
  params.delegate = delegate.release();

  widget_ = std::make_unique<views::Widget>();
  widget_->Init(std::move(params));

  if (widget_->GetLayer()) {
    widget_->GetLayer()->SetFillsBoundsOpaquely(false);
  }

  container_ = widget_->SetContentsView(std::move(container));
  container_->SetLayoutChangedCallback(base::BindRepeating(
      &XenonReminderNotificationWidget::UpdateBrowserWidgetPosition,
      base::Unretained(this)));

  for (const ReminderMessage& msg :
       XenonReminderNotificationManager::GetInstance()->GetAllMessages()) {
    container_->AddMessage(msg);
  }

  observed_parent_ = parent;
  observed_parent_->AddObserver(this);

  UpdateBrowserWidgetPosition();
  widget_->Hide();
}

void XenonReminderNotificationWidget::CreateDesktopWidget() {
  views::Widget::InitParams params(
      views::Widget::InitParams::WIDGET_OWNS_NATIVE_WIDGET,
      views::Widget::InitParams::TYPE_POPUP);
  params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
  params.activatable = views::Widget::InitParams::Activatable::kNo;
  params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
  params.z_order = ui::ZOrderLevel::kFloatingWindow;
  params.name = "XenonReminderDesktopWidget";
  params.accept_events = true;

  auto container = std::make_unique<XenonReminderNotificationContainer>(
      profile_, /*is_desktop=*/true);
  XenonReminderNotificationContainer* container_ptr = container.get();

  auto delegate =
      std::make_unique<XenonReminderWidgetDelegate>(container_ptr);
  params.delegate = delegate.release();

  desktop_widget_ = std::make_unique<views::Widget>();
  desktop_widget_->Init(std::move(params));

  if (desktop_widget_->GetLayer()) {
    desktop_widget_->GetLayer()->SetFillsBoundsOpaquely(false);
  }

  desktop_widget_->SetVisibleOnAllWorkspaces(true);

  desktop_container_ =
      desktop_widget_->SetContentsView(std::move(container));
  desktop_container_->SetLayoutChangedCallback(base::BindRepeating(
      &XenonReminderNotificationWidget::UpdateDesktopWidgetPosition,
      base::Unretained(this)));

  for (const ReminderMessage& msg :
       XenonReminderNotificationManager::GetInstance()->GetAllMessages()) {
    if (msg.display_on_desktop) {
      desktop_container_->AddMessage(msg);
    }
  }

  desktop_widget_->Hide();
}

void XenonReminderNotificationWidget::UpdateBrowserWidgetPosition() {
  if (!widget_ || !browser_) {
    return;
  }

  int width, content_height;
  if (!CalculateContentSize(container_, width, content_height)) {
    widget_->Hide();
    return;
  }

  BrowserView* browser_view =
      browser_view_ ? browser_view_.get()
                    : BrowserView::GetBrowserViewForBrowser(browser_);
  if (!browser_view) {
    return;
  }

  views::Widget* parent = browser_view->GetWidget();
  if (!parent) {
    return;
  }

  gfx::Rect parent_bounds = parent->GetWindowBoundsInScreen();
  int right_edge = parent_bounds.right();

  int x = right_edge - width + kMarginRight;
  int y = parent_bounds.y() + kMarginTop;
  widget_->SetBounds(gfx::Rect(x, y, width, content_height));
}

void XenonReminderNotificationWidget::UpdateDesktopWidgetPosition() {
  if (!desktop_widget_ || !browser_) {
    return;
  }

  int width, content_height;
  if (!CalculateContentSize(desktop_container_, width, content_height)) {
    desktop_widget_->Hide();
    return;
  }

  display::Screen* screen = display::Screen::Get();
  if (!screen) {
    return;
  }

  views::Widget* browser_wgt = GetBrowserWidget();
  display::Display disp;
  if (browser_wgt) {
    gfx::Rect browser_bounds = browser_wgt->GetWindowBoundsInScreen();
    disp = screen->GetDisplayMatching(browser_bounds);
  } else {
    disp = screen->GetPrimaryDisplay();
  }
  gfx::Rect work_area = disp.work_area();

  int x = work_area.right() - width - kMarginRight;
  int y = work_area.y() + kMarginTop;
  desktop_widget_->SetBounds(gfx::Rect(x, y, width, content_height));
}

views::Widget* XenonReminderNotificationWidget::GetBrowserWidget() const {
  if (!browser_) {
    return nullptr;
  }
  BrowserView* view = BrowserView::GetBrowserViewForBrowser(browser_);
  return view ? view->GetWidget() : nullptr;
}

}  // namespace xenon
