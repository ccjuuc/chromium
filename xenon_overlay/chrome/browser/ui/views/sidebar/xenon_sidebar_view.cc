// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/views/sidebar/xenon_sidebar_view.h"

#include <algorithm>
#include <memory>
#include <string>

#include "base/command_line.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/color/chrome_color_id.h"
#include "chrome/browser/ui/window_feature_controller/window_feature_controller.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/window_open_disposition.h"
#include "ui/color/color_id.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/insets_f.h"
#include "ui/gfx/geometry/point_f.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/accessibility/view_accessibility.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/layout/box_layout.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/sidebar/xenon_sidebar_service_factory.h"
#include "xenon_overlay/chrome/browser/ui/xenon_web_dialog.h"

namespace xenon {

namespace {

constexpr char kDisableXenonSidebarSwitch[] = "disable-xenon-sidebar";
constexpr char kDisableXenonSidebarAutoHideSwitch[] =
    "disable-xenon-sidebar-auto-hide";

constexpr int kSidebarWidth = 48;
constexpr int kHotCornerWidth = 7;
constexpr int kHideDelayInMS = 400;
constexpr int kButtonSize = 36;
constexpr int kSideOptionButtonHeight = 24;
constexpr int kButtonCornerRadius = 8;
constexpr int kVerticalPadding = 8;
constexpr int kHorizontalPadding = 6;
constexpr int kButtonSpacing = 6;

struct SidebarShortcut {
  const char* token;
  const char* title;
  const char* url;
};

constexpr SidebarShortcut kShortcuts[] = {
    // {"XO", "Xenon Overlay", "chrome://xenon-overlay/"},
    // {"AI", "Xenon AI", "chrome://xenon-ai-side-panel.top-chrome/"},
    // {"UI", "Xenon UI", "chrome://xenon-ui/"},
    // {"N", "Xenon Node", "chrome://xenon-node/"},
    // {"PC", "pc_addon", "chrome://xenon-node/#pc_addon"},
    // {"XP", "Xenon Player", "chrome://xenon-player/"},
    // {"XPE", "Electron Container Test", "chrome://xenon-player-by-elec/"},
    {"PLE", "Player via Electron Container",
     "chrome://xenon-player-electron/"},
    {"TH", "Thunder 2025", "chrome://thunder-2025/"},
    // {"VT", "Local Video Test", "chrome://local-video-test/"},
    // {"RDL", "Render DLL Test", "chrome://render-dll-test/"},
    // {"MI", "Media Internals", "chrome://media-internals/"},
    // {"GPU", "GPU Internals", "chrome://gpu/"},
    // {"VS", "Video Sniffer", "chrome://video-sniffer/"},
    // {"SW", "Simple WebUI", "chrome://simple-webui/"},
    // {"LG", "Xenon Login", "chrome://xenon-login/"},
    // {"VER", "Version", "chrome://version/"},
    // {"FLG", "Flags", "chrome://flags/"},
};

class XenonSidebarButton : public views::LabelButton {
  METADATA_HEADER(XenonSidebarButton, views::LabelButton)

 public:
  XenonSidebarButton(views::Button::PressedCallback callback,
                     std::u16string token,
                     std::u16string title,
                     std::u16string url,
                     gfx::Size button_size = gfx::Size(kButtonSize,
                                                       kButtonSize),
                     base::RepeatingCallback<bool()> selected_callback =
                         base::RepeatingCallback<bool()>())
      : views::LabelButton(std::move(callback), std::move(token)),
        selected_callback_(std::move(selected_callback)) {
    SetMinSize(button_size);
    SetMaxSize(button_size);
    SetHorizontalAlignment(gfx::ALIGN_CENTER);
    SetImageLabelSpacing(0);
    SetBorder(views::CreateEmptyBorder(gfx::Insets()));
    SetFocusRingCornerRadius(kButtonCornerRadius);
    std::u16string tooltip = title;
    if (!url.empty()) {
      tooltip.append(u"\n");
      tooltip.append(url);
    }
    SetTooltipText(tooltip);
    GetViewAccessibility().SetName(title);
  }

  XenonSidebarButton(const XenonSidebarButton&) = delete;
  XenonSidebarButton& operator=(const XenonSidebarButton&) = delete;
  ~XenonSidebarButton() override = default;

  void RefreshVisualState() { UpdateBackground(); }

  // views::LabelButton:
  void OnThemeChanged() override {
    views::LabelButton::OnThemeChanged();
    SetEnabledTextColors(kColorToolbarButtonText);
    UpdateBackground();
  }

  void StateChanged(ButtonState old_state) override {
    views::LabelButton::StateChanged(old_state);
    UpdateBackground();
  }

  void OnFocus() override {
    views::LabelButton::OnFocus();
    UpdateBackground();
  }

  void OnBlur() override {
    views::LabelButton::OnBlur();
    UpdateBackground();
  }

 private:
  bool IsSelected() const {
    return selected_callback_ && selected_callback_.Run();
  }

  void UpdateBackground() {
    if (IsSelected() || GetState() == STATE_HOVERED ||
        GetState() == STATE_PRESSED || HasFocus()) {
      SetBackground(views::CreateRoundedRectBackground(
          ui::kColorSysStateHoverOnSubtle, kButtonCornerRadius));
      return;
    }

    SetBackground(nullptr);
  }

  base::RepeatingCallback<bool()> selected_callback_;
};

BEGIN_METADATA(XenonSidebarButton)
END_METADATA

std::unique_ptr<XenonSidebarButton> CreateShortcutButton(
    const SidebarShortcut& shortcut,
    views::Button::PressedCallback callback) {
  return std::make_unique<XenonSidebarButton>(
      std::move(callback), base::UTF8ToUTF16(shortcut.token),
      base::UTF8ToUTF16(shortcut.title), base::UTF8ToUTF16(shortcut.url));
}

std::unique_ptr<XenonSidebarButton> CreateSideOptionButton(
    std::u16string token,
    std::u16string title,
    views::Button::PressedCallback callback,
    base::RepeatingCallback<bool()> selected_callback) {
  return std::make_unique<XenonSidebarButton>(
      std::move(callback), std::move(token), std::move(title), std::u16string(),
      gfx::Size(kButtonSize, kSideOptionButtonHeight),
      std::move(selected_callback));
}

bool IsSidebarAutoHideAllowedByCommandLine() {
  return !base::CommandLine::ForCurrentProcess()->HasSwitch(
      kDisableXenonSidebarAutoHideSwitch);
}

}  // namespace

bool IsXenonSidebarEnabledForBrowser(const Browser* browser) {
  if (!browser ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      !WindowFeatureController::From(browser)->SupportsWindowFeature(
          WindowFeatureController::WindowFeature::kFeatureTabStrip)) {
    return false;
  }

  return !base::CommandLine::ForCurrentProcess()->HasSwitch(
      kDisableXenonSidebarSwitch);
}

bool IsXenonSidebarAutoHideEnabled() {
  return IsSidebarAutoHideAllowedByCommandLine();
}

XenonSidebarView::XenonSidebarView(Browser* browser)
    : browser_(browser),
      service_(browser_ ? XenonSidebarServiceFactory::GetForProfile(
                              browser_->GetProfile())
                        : nullptr) {
  if (service_) {
    service_->AddObserver(this);
    auto_hide_enabled_ = IsSidebarAutoHideAllowedByCommandLine() &&
                         service_->IsAutoHideEnabled();
  }
  sidebar_visible_ = !auto_hide_enabled_;

  SetMirrored(false);
  SetNotifyEnterExitOnChild(true);
  SetVisible(sidebar_visible_);

  auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical,
      gfx::Insets::VH(kVerticalPadding, kHorizontalPadding), kButtonSpacing));
  layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  for (const SidebarShortcut& shortcut : kShortcuts) {
    AddChildView(CreateShortcutButton(
        shortcut, base::BindRepeating(&XenonSidebarView::OpenShortcut,
                                      weak_ptr_factory_.GetWeakPtr(),
                                      GURL(shortcut.url))));
  }

  views::View* spacer = AddChildView(std::make_unique<views::View>());
  layout->SetFlexForView(spacer, 1);

  left_align_button_ = AddChildView(CreateSideOptionButton(
      u"\u5de6", u"\u4fa7\u8fb9\u680f\u9760\u5de6",
      base::BindRepeating(&XenonSidebarView::SetSidebarRightAligned,
                          weak_ptr_factory_.GetWeakPtr(), false),
      base::BindRepeating(
          [](base::WeakPtr<XenonSidebarView> sidebar) {
            return sidebar && !sidebar->IsRightAligned();
          },
          weak_ptr_factory_.GetWeakPtr())));

  right_align_button_ = AddChildView(CreateSideOptionButton(
      u"\u53f3", u"\u4fa7\u8fb9\u680f\u9760\u53f3",
      base::BindRepeating(&XenonSidebarView::SetSidebarRightAligned,
                          weak_ptr_factory_.GetWeakPtr(), true),
      base::BindRepeating(
          [](base::WeakPtr<XenonSidebarView> sidebar) {
            return sidebar && sidebar->IsRightAligned();
          },
          weak_ptr_factory_.GetWeakPtr())));

  hide_button_ = AddChildView(CreateSideOptionButton(
      u"\u9690", u"\u81ea\u52a8\u9690\u85cf\u4fa7\u8fb9\u680f",
      base::BindRepeating(&XenonSidebarView::ToggleSidebarAutoHide,
                          weak_ptr_factory_.GetWeakPtr()),
      base::BindRepeating(
          [](base::WeakPtr<XenonSidebarView> sidebar) {
            return sidebar && sidebar->auto_hide_enabled_;
          },
          weak_ptr_factory_.GetWeakPtr())));

  if (service_) {
    OnSidebarAlignmentChanged(service_->IsRightAligned());
    OnSidebarAutoHideChanged(service_->IsAutoHideEnabled());
  }
}

XenonSidebarView::~XenonSidebarView() {
  if (service_) {
    service_->RemoveObserver(this);
  }
}

bool XenonSidebarView::IsSidebarVisible() const {
  return sidebar_visible_;
}

bool XenonSidebarView::IsRightAligned() const {
  return service_ ? service_->IsRightAligned() : false;
}

void XenonSidebarView::ShowSidebarOnMouseOver(
    const gfx::PointF& point_in_screen,
    const gfx::Rect& detect_bounds_in_screen) {
  if (!auto_hide_enabled_) {
    return;
  }

  if (sidebar_visible_) {
    if (!gfx::RectF(GetBoundsInScreen()).Contains(point_in_screen)) {
      ScheduleHideSidebar();
    }
    return;
  }

  gfx::RectF hot_zone(detect_bounds_in_screen);
  const float inset = std::max(0.0f, hot_zone.width() - kHotCornerWidth);
  hot_zone.Inset(IsRightAligned() ? gfx::InsetsF::TLBR(0, inset, 0, 0)
                                  : gfx::InsetsF::TLBR(0, 0, 0, inset));
  if (hot_zone.Contains(point_in_screen)) {
    ShowSidebar();
  }
}

gfx::Size XenonSidebarView::CalculatePreferredSize(
    const views::SizeBounds& available_size) const {
  return gfx::Size(ShouldReserveWidth() ? kSidebarWidth : 0,
                   views::View::CalculatePreferredSize(available_size)
                       .height());
}

gfx::Size XenonSidebarView::GetMinimumSize() const {
  return gfx::Size(ShouldReserveWidth() ? kSidebarWidth : 0, 0);
}

void XenonSidebarView::OnThemeChanged() {
  views::View::OnThemeChanged();
  SetBackground(views::CreateSolidBackground(kColorSidePanelBackground));
}

void XenonSidebarView::OnMouseEntered(const ui::MouseEvent& event) {
  views::View::OnMouseEntered(event);
  if (auto_hide_enabled_) {
    sidebar_hide_timer_.Stop();
  }
}

void XenonSidebarView::OnMouseExited(const ui::MouseEvent& event) {
  views::View::OnMouseExited(event);
  ScheduleHideSidebar();
}

bool XenonSidebarView::ShouldReserveWidth() const {
  return !auto_hide_enabled_ || sidebar_visible_;
}

bool XenonSidebarView::ShouldForceShowSidebar() const {
  if (IsMouseHovered()) {
    return true;
  }

  const views::FocusManager* focus_manager = GetFocusManager();
  if (!focus_manager) {
    return false;
  }

  const views::View* focused_view = focus_manager->GetFocusedView();
  return focused_view && Contains(focused_view);
}

void XenonSidebarView::ShowSidebar() {
  if (!auto_hide_enabled_ || sidebar_visible_) {
    return;
  }

  sidebar_hide_timer_.Stop();
  sidebar_visible_ = true;
  SetVisible(true);
  PreferredSizeChanged();
}

void XenonSidebarView::HideSidebar() {
  if (!auto_hide_enabled_ || !sidebar_visible_ || ShouldForceShowSidebar()) {
    return;
  }

  sidebar_visible_ = false;
  SetVisible(false);
  PreferredSizeChanged();
}

void XenonSidebarView::ToggleSidebarAutoHide() {
  if (!service_ || !IsXenonSidebarAutoHideEnabled()) {
    return;
  }

  const bool new_auto_hide = !service_->IsAutoHideEnabled();
  if (new_auto_hide) {
    if (views::FocusManager* focus_manager = GetFocusManager()) {
      focus_manager->ClearFocus();
    }
  }
  service_->SetAutoHideEnabled(new_auto_hide);
}

void XenonSidebarView::ScheduleHideSidebar() {
  if (!auto_hide_enabled_ || !sidebar_visible_) {
    return;
  }

  if (ShouldForceShowSidebar()) {
    return;
  }

  if (sidebar_hide_timer_.IsRunning()) {
    return;
  }

  sidebar_hide_timer_.Start(
      FROM_HERE, base::Milliseconds(kHideDelayInMS),
      base::BindOnce(&XenonSidebarView::HideSidebar,
                     weak_ptr_factory_.GetWeakPtr()));
}

void XenonSidebarView::SetSidebarRightAligned(bool right_aligned) {
  if (!service_) {
    return;
  }

  if (service_->IsRightAligned() == right_aligned) {
    return;
  }

  service_->SetRightAligned(right_aligned);
}

void XenonSidebarView::OnSidebarAutoHideChanged(bool auto_hide) {
  const bool auto_hide_enabled =
      IsSidebarAutoHideAllowedByCommandLine() && auto_hide;
  const bool auto_hide_changed = auto_hide_enabled_ != auto_hide_enabled;
  auto_hide_enabled_ = auto_hide_enabled;

  sidebar_hide_timer_.Stop();
  if (auto_hide_enabled_) {
    sidebar_visible_ = ShouldForceShowSidebar();
    SetVisible(sidebar_visible_);
  } else {
    sidebar_visible_ = true;
    SetVisible(true);
  }

  if (hide_button_) {
    static_cast<XenonSidebarButton*>(hide_button_.get())->RefreshVisualState();
  }

  if (auto_hide_changed) {
    PreferredSizeChanged();
  }
}

void XenonSidebarView::OnSidebarAlignmentChanged(bool /*right_aligned*/) {
  if (left_align_button_) {
    static_cast<XenonSidebarButton*>(left_align_button_.get())
        ->RefreshVisualState();
  }
  if (right_align_button_) {
    static_cast<XenonSidebarButton*>(right_align_button_.get())
        ->RefreshVisualState();
  }

  PreferredSizeChanged();
}

void XenonSidebarView::OpenShortcut(const GURL& url) {
  if (!browser_) {
    return;
  }

  // Player HWND windows are created by packaged main.js BrowserWindow.
  // Sidebar only activates those widgets — do not open a second WebDialog.
  if (url.host() == "xenon-player") {
    XenonWebDialog::ShowXenonPlayer(browser_->GetProfile());
    return;
  }
  if (url.host() == "xenon-player-by-elec") {
    XenonWebDialog::ShowXenonPlayerByElec(browser_->GetProfile());
    return;
  }
  if (url.host() == "xenon-player-electron") {
    XenonWebDialog::ShowXenonPlayerElectron(browser_->GetProfile());
    return;
  }
  if (url.host() == "thunder-2025") {
    XenonWebDialog::ShowThunder2025(browser_->GetProfile());
    return;
  }

  browser_->OpenGURL(url, WindowOpenDisposition::NEW_FOREGROUND_TAB);
}

BEGIN_METADATA(XenonSidebarView)
END_METADATA

}  // namespace xenon
