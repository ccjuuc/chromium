// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_tip_bar_widget.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/compositor/layer.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "xenon_overlay/chrome/browser/reminder/xenon_reminder_notification_manager.h"

namespace xenon {

namespace {

constexpr int kTipBarHeight = 40;
constexpr int kTipBarMaxWidth = 400;
constexpr int kTipBarMinWidth = 200;
constexpr int kTipBarCornerRadius = 8;
constexpr int kTipBarPaddingH = 16;
constexpr int kTipBarPaddingV = 8;
constexpr int kTipBarMarginRight = 15;
constexpr int kTipBarMarginTop = 45;

constexpr SkColor kTipBarBackground =
    SkColorSetARGB(0xE6, 0xFF, 0xFF, 0xFF);
constexpr SkColor kTipBarTextColor = SkColorSetRGB(0x33, 0x33, 0x33);
constexpr SkColor kTipBarBorderColor =
    SkColorSetARGB(0x99, 0xCC, 0xCC, 0xCC);
constexpr int kTipBarBorderThickness = 1;

}  // namespace

std::unique_ptr<XenonTipBarWidget> XenonTipBarWidget::CreateForBrowser(
    Browser* browser,
    BrowserView* browser_view) {
  if (!browser || !browser->GetProfile()) {
    return nullptr;
  }
  std::unique_ptr<XenonTipBarWidget> tip_bar(new XenonTipBarWidget(browser));
  tip_bar->CreateWidget(browser_view);
  return tip_bar;
}

XenonTipBarWidget::XenonTipBarWidget(Browser* browser) : browser_(browser) {
  XenonReminderNotificationManager::GetInstance()->AddObserver(this);
}

XenonTipBarWidget::~XenonTipBarWidget() {
  auto_close_timer_.Stop();
  if (observed_parent_) {
    observed_parent_->RemoveObserver(this);
    observed_parent_ = nullptr;
  }
  XenonReminderNotificationManager::GetInstance()->RemoveObserver(this);
  // Drop view pointers before tearing down the Widget that owns them.
  label_ = nullptr;
  close_btn_ = nullptr;
  browser_view_ = nullptr;
  widget_.reset();
  browser_ = nullptr;
}

void XenonTipBarWidget::OnTipMessageReceived(const std::string& tip_text) {
  ShowTip(tip_text);
}

void XenonTipBarWidget::OnWidgetBoundsChanged(views::Widget* widget,
                                              const gfx::Rect& new_bounds) {
  if (widget == observed_parent_) {
    UpdateWidgetPosition();
  }
}

void XenonTipBarWidget::OnWidgetDestroying(views::Widget* widget) {
  if (widget == observed_parent_) {
    observed_parent_->RemoveObserver(this);
    observed_parent_ = nullptr;
  }
}

void XenonTipBarWidget::ShowTip(const std::string& tip_text) {
  if (!widget_ || !label_) {
    return;
  }

  label_->SetText(base::UTF8ToUTF16(tip_text));
  UpdateWidgetPosition();
  widget_->Show();

  views::Widget* browser_widget = GetBrowserWidget();
  if (browser_widget) {
    widget_->StackAboveWidget(browser_widget);
  }

  auto_close_timer_.Stop();
  if (kDefaultAutoCloseMs > 0) {
    auto_close_timer_.Start(
        FROM_HERE, base::Milliseconds(kDefaultAutoCloseMs),
        base::BindOnce(&XenonTipBarWidget::OnAutoCloseTimer,
                      base::Unretained(this)));
  }
}

void XenonTipBarWidget::Hide() {
  auto_close_timer_.Stop();
  if (widget_) {
    widget_->Hide();
  }
}

bool XenonTipBarWidget::IsVisible() const {
  return widget_ && widget_->IsVisible();
}

void XenonTipBarWidget::CreateWidget(BrowserView* browser_view) {
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
  params.accept_events = true;
  params.name = "XenonTipBarWidget";

  widget_ = std::make_unique<views::Widget>();
  widget_->Init(std::move(params));

  if (widget_->GetLayer()) {
    widget_->GetLayer()->SetFillsBoundsOpaquely(false);
  }

  auto content = std::make_unique<views::View>();
  auto* box_layout =
      content->SetLayoutManager(std::make_unique<views::BoxLayout>(
          views::BoxLayout::Orientation::kHorizontal,
          gfx::Insets::VH(kTipBarPaddingV, kTipBarPaddingH),
          /*between_child_spacing=*/8));
  box_layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  content->SetBackground(views::CreateRoundedRectBackground(
      kTipBarBackground, kTipBarCornerRadius));
  content->SetBorder(views::CreateRoundedRectBorder(
      kTipBarBorderThickness, kTipBarCornerRadius, kTipBarBorderColor));

  auto label = std::make_unique<views::Label>(std::u16string());
  label->SetEnabledColor(kTipBarTextColor);
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  label->SetMultiLine(false);
  label->SetElideBehavior(gfx::ELIDE_TAIL);
  label_ = content->AddChildView(std::move(label));
  box_layout->SetFlexForView(label_, 1);

  auto close_btn = std::make_unique<views::LabelButton>(
      base::BindRepeating(&XenonTipBarWidget::OnCloseButtonPressed,
                          base::Unretained(this)),
      u"\u00D7");
  close_btn->SetEnabledTextColors(kTipBarTextColor);
  close_btn->SetAccessibleName(u"Close");
  close_btn->SetBorder(nullptr);
  close_btn_ = content->AddChildView(std::move(close_btn));

  widget_->SetContentsView(std::move(content));

  observed_parent_ = parent;
  observed_parent_->AddObserver(this);

  widget_->Hide();
}

void XenonTipBarWidget::UpdateWidgetPosition() {
  if (!widget_ || !browser_) {
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

  int width = kTipBarMinWidth;
  if (label_) {
    int text_width = label_->GetPreferredSize().width() + kTipBarPaddingH * 2 +
                     40;
    width = std::clamp(text_width, kTipBarMinWidth, kTipBarMaxWidth);
  }

  int right_edge = parent_bounds.right();
  int x = right_edge - width - kTipBarMarginRight;
  int y = parent_bounds.y() + kTipBarMarginTop;

  widget_->SetBounds(gfx::Rect(x, y, width, kTipBarHeight));
}

void XenonTipBarWidget::OnCloseButtonPressed() {
  Hide();
}

void XenonTipBarWidget::OnAutoCloseTimer() {
  Hide();
}

views::Widget* XenonTipBarWidget::GetBrowserWidget() const {
  if (!browser_) {
    return nullptr;
  }
  BrowserView* view = BrowserView::GetBrowserViewForBrowser(browser_);
  return view ? view->GetWidget() : nullptr;
}

}  // namespace xenon
