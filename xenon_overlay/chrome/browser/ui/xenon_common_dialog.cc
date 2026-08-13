// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_common_dialog.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "cc/paint/paint_flags.h"
#include "chrome/browser/ui/browser_window.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/global_browser_collection.h"
#include "components/constrained_window/constrained_window_views.h"
#include "components/strings/grit/components_strings.h"
#include "components/vector_icons/vector_icons.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/compositor/layer.h"
#include "ui/compositor_extra/shadow.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/shadow_value.h"
#include "ui/views/animation/animation_builder.h"
#include "ui/views/animation/animation_sequence_block.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/button/checkbox.h"
#include "ui/views/controls/button/image_button.h"
#include "ui/views/controls/button/image_button_factory.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/highlight_path_generator.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/scroll_view.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/style/typography.h"
#include "ui/views/view_class_properties.h"
#include "ui/views/widget/widget.h"
#include "ui/views/window/dialog_delegate.h"

#if BUILDFLAG(IS_WIN)
#include <windows.h>

#include <dwmapi.h>

#include "base/win/windows_version.h"
#include "ui/views/win/hwnd_util.h"
#endif

namespace xenon {

namespace {

constexpr int kDialogCornerRadius = 24;
constexpr int kDialogShadowElevation = 8;
constexpr int kCloseButtonSize = 32;
constexpr int kCloseButtonCornerRadius = 8;

int GetDialogShadowMargin() {
  static const int margin = [] {
    const gfx::ShadowValues values =
        gfx::ShadowValue::MakeMdShadowValues(kDialogShadowElevation);
    const gfx::Insets insets = gfx::ShadowValue::GetMargin(values);
    return std::max(
        {-insets.left(), -insets.top(), -insets.right(), -insets.bottom()});
  }();
  return margin;
}

#if BUILDFLAG(IS_WIN)
// Disable DWM round/shadow/border. Dialog uses compositor 24px corner +
// shadow.
void DisableNativeWindowChrome(views::Widget* widget) {
  if (!widget) {
    return;
  }
  HWND hwnd = views::HWNDForNativeWindow(widget->GetNativeWindow());
  if (!hwnd) {
    return;
  }
  DWM_WINDOW_CORNER_PREFERENCE corner_pref = DWMWCP_DONOTROUND;
  ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_pref,
                          sizeof(corner_pref));

  if (base::win::GetVersion() >= base::win::Version::WIN11) {
    constexpr DWORD kDwmwaBorderColor = 34;
    constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;
    COLORREF border_color = kDwmColorNone;
    ::DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border_color,
                            sizeof(border_color));
  }
}
#endif

// Paints the dialog's white rounded body only inside the content area (i.e.
// excluding the shadow margin), so the ui::Shadow below stays visible.
class DialogBodyBackground : public views::Background {
 public:
  DialogBodyBackground(SkColor color, int radius)
      : color_(color), radius_(radius) {}
  ~DialogBodyBackground() override = default;

  void Paint(gfx::Canvas* canvas, views::View* view) const override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setStyle(cc::PaintFlags::kFill_Style);
    flags.setColor(color_);
    canvas->DrawRoundRect(gfx::RectF(view->GetContentsBounds()), radius_,
                          flags);
  }

 private:
  const SkColor color_;
  const int radius_;
};

// Custom Dialog Button component matching visual guidelines.
class XenonDialogButton : public views::LabelButton {
 public:
  METADATA_HEADER(XenonDialogButton, views::LabelButton)

 public:
  XenonDialogButton(PressedCallback callback,
                    const std::u16string& text,
                    bool is_confirm)
      : views::LabelButton(std::move(callback), text), is_confirm_(is_confirm) {
    SetHorizontalAlignment(gfx::ALIGN_CENTER);
    SetElideBehavior(gfx::ELIDE_TAIL);

    // Set custom font list weight to Medium
    label()->SetFontList(views::Label::GetDefaultFontList().Derive(
        0, gfx::Font::NORMAL, gfx::Font::Weight::MEDIUM));

    // Set horizontal-only padding. Height is explicitly constrained to 32px
    SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(0, 12)));

    UpdateStyle();
  }

  ~XenonDialogButton() override = default;

  void StateChanged(ButtonState old_state) override {
    views::LabelButton::StateChanged(old_state);
    UpdateStyle();
  }

  void OnThemeChanged() override {
    views::LabelButton::OnThemeChanged();
    UpdateStyle();
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    gfx::Size size = views::LabelButton::CalculatePreferredSize(available_size);
    size.set_height(32);
    int width = size.width();
    if (width < 78) {
      width = 78;
    }
    if (width > 180) {
      width = 180;
    }
    size.set_width(width);
    return size;
  }

 private:
  void UpdateStyle() {
    SkColor bg_color;
    SkColor text_color;

    if (is_confirm_) {
      // Confirm button: Dark slate blue background, white text.
      if (GetState() == ButtonState::STATE_HOVERED) {
        bg_color = SkColorSetRGB(0x3E, 0x44, 0x52);
      } else if (GetState() == ButtonState::STATE_PRESSED) {
        bg_color = SkColorSetRGB(0x1D, 0x21, 0x2A);
      } else {
        bg_color = SkColorSetRGB(0x2D, 0x32, 0x3F);
      }
      text_color = SK_ColorWHITE;
    } else {
      // Cancel button: Light grey background, dark text.
      if (GetState() == ButtonState::STATE_HOVERED) {
        bg_color = SkColorSetRGB(0xE5, 0xE6, 0xEB);
      } else if (GetState() == ButtonState::STATE_PRESSED) {
        bg_color = SkColorSetRGB(0xD9, 0xD9, 0xD9);
      } else {
        bg_color = SkColorSetRGB(0xF2, 0xF3, 0xF5);
      }
      text_color = SkColorSetRGB(0x1D, 0x21, 0x29);
    }

    SetBackground(views::CreateRoundedRectBackground(bg_color, 16));
    SetEnabledTextColors(text_color);
  }

  const bool is_confirm_;
};

BEGIN_METADATA(XenonDialogButton)
END_METADATA

class XenonCommonDialogView : public views::View {
 public:
  METADATA_HEADER(XenonCommonDialogView, views::View)

 public:
  XenonCommonDialogView(XenonCommonDialog::Style style,
                        const std::u16string& title,
                        const std::u16string& body_text,
                        const std::u16string& checkbox_text,
                        bool checkbox_checked,
                        const std::u16string& cancel_text,
                        const std::u16string& confirm_text,
                        XenonCommonDialog::Callback callback)
      : style_(style), callback_(std::move(callback)) {
    // Use a vertical BoxLayout with custom padding/insets.
    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical,
        gfx::Insets::TLBR(16, 24, 20, 16), 0));

    // 1. Header: Title and Close Button
    auto* header = AddChildView(std::make_unique<views::View>());
    auto* header_layout =
        header->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal));

    auto* title_label = header->AddChildView(std::make_unique<views::Label>(
        title, views::style::CONTEXT_DIALOG_TITLE,
        views::style::STYLE_PRIMARY));
    title_label->SetFontList(views::Label::GetDefaultFontList().Derive(
        2, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
    title_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    title_label->SetEnabledColor(SkColorSetRGB(0x1D, 0x21, 0x29));
    header_layout->SetFlexForView(title_label, 1);

    auto* close_button =
        header->AddChildView(views::CreateVectorImageButtonWithNativeTheme(
            base::BindRepeating(&XenonCommonDialogView::OnCloseButtonClicked,
                                base::Unretained(this)),
            vector_icons::kCloseChromeRefreshOldIcon, 24));
    close_button->SetBorder(nullptr);
    close_button->SetPreferredSize(
        gfx::Size(kCloseButtonSize, kCloseButtonSize));
    close_button->SetAccessibleName(l10n_util::GetStringUTF16(IDS_CLOSE));
    views::InstallRoundRectHighlightPathGenerator(close_button, gfx::Insets(),
                                                  kCloseButtonCornerRadius);

    // Spacing between header and content: 12px
    auto* body_spacer = AddChildView(std::make_unique<views::View>());
    body_spacer->SetPreferredSize(gfx::Size(1, 12));

    // 2. Body Text (contained in a ScrollView)
    auto* scroll_view = AddChildView(std::make_unique<views::ScrollView>());
    scroll_view->SetHorizontalScrollBarMode(
        views::ScrollView::ScrollBarMode::kDisabled);
    scroll_view->SetBorder(nullptr);

    auto* body_label = scroll_view->SetContents(std::make_unique<views::Label>(
        body_text, views::style::CONTEXT_DIALOG_BODY_TEXT,
        views::style::STYLE_SECONDARY));
    body_label->SetMultiLine(true);
    body_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    body_label->SetMaximumWidth(
        style_ == XenonCommonDialog::Style::kSmall ? (340 - 48) : (450 - 48));
    body_label->SetFontList(views::Label::GetDefaultFontList().Derive(
        0, gfx::Font::NORMAL, gfx::Font::Weight::NORMAL));
    body_label->SetEnabledColor(SkColorSetRGB(0x4E, 0x59, 0x69));

    // Allow ScrollView to calculate height based on content height.
    scroll_view->ClipHeightTo(0, 9999);

    layout->SetFlexForView(scroll_view, 1);

    // 3. Optional Checkbox
    if (!checkbox_text.empty()) {
      // Spacing between body and checkbox: 12px
      auto* checkbox_spacer = AddChildView(std::make_unique<views::View>());
      checkbox_spacer->SetPreferredSize(gfx::Size(1, 12));

      checkbox_ =
          AddChildView(std::make_unique<views::Checkbox>(checkbox_text));
      checkbox_->SetChecked(checkbox_checked);
      checkbox_->SetElideBehavior(gfx::ELIDE_TAIL);
      checkbox_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
      checkbox_->SetBorder(views::CreateEmptyBorder(gfx::Insets::VH(4, 0)));
      checkbox_->SetEnabledTextColors(SkColorSetRGB(0x4E, 0x59, 0x69));
    }

    // Spacing between content/checkbox and buttons: 10px
    auto* footer_spacer = AddChildView(std::make_unique<views::View>());
    footer_spacer->SetPreferredSize(gfx::Size(1, 10));

    // 4. Footer Row: Action Buttons
    auto* footer = AddChildView(std::make_unique<views::View>());
    auto* footer_layout =
        footer->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    footer_layout->set_main_axis_alignment(
        views::BoxLayout::MainAxisAlignment::kEnd);

    if (!cancel_text.empty()) {
      footer->AddChildView(std::make_unique<XenonDialogButton>(
          base::BindRepeating(&XenonCommonDialogView::OnCancelButtonClicked,
                              base::Unretained(this)),
          cancel_text, false));
    }

    if (!confirm_text.empty()) {
      footer->AddChildView(std::make_unique<XenonDialogButton>(
          base::BindRepeating(&XenonCommonDialogView::OnConfirmButtonClicked,
                              base::Unretained(this)),
          confirm_text, true));
    }

    SetBackground(nullptr);
  }

  ~XenonCommonDialogView() override {
    if (!resolved_ && callback_) {
      std::move(callback_).Run(
          {.accepted = false,
           .checkbox_checked = checkbox_ ? checkbox_->GetChecked() : false});
    }
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    int width = (style_ == XenonCommonDialog::Style::kSmall) ? 340 : 450;
    views::SizeBounds bounds(width, {});
    gfx::Size size = views::View::CalculatePreferredSize(bounds);
    size.set_width(width);
    if (style_ == XenonCommonDialog::Style::kMedium) {
      if (size.height() > 300) {
        size.set_height(300);
      }
    }
    return size;
  }

  void OnBoundsChanged(const gfx::Rect& previous_bounds) override {
    views::View::OnBoundsChanged(previous_bounds);
    UpdateCompositorShadowBounds();
  }

 private:
  void SetupCompositorShadow() {
    const int margin = GetDialogShadowMargin();
    if (margin <= 0) {
      return;
    }

    SetBorder(views::CreateEmptyBorder(gfx::Insets(margin)));
    compositor_shadow_ = std::make_unique<ui::Shadow>();
    compositor_shadow_->Init(kDialogShadowElevation);
    compositor_shadow_->SetRoundedCornerRadius(kDialogCornerRadius);
    AddLayerToRegion(compositor_shadow_->layer(), views::LayerRegion::kBelow);
    UpdateCompositorShadowBounds();
  }

  void UpdateCompositorShadowBounds() {
    if (!compositor_shadow_) {
      return;
    }
    const int margin = GetDialogShadowMargin();
    compositor_shadow_->SetContentBounds(
        gfx::Rect(margin, margin, std::max(0, width() - 2 * margin),
                  std::max(0, height() - 2 * margin)));
  }

  void OnConfirmButtonClicked() {
    resolved_ = true;
    if (callback_) {
      std::move(callback_).Run(
          {.accepted = true,
           .checkbox_checked = checkbox_ ? checkbox_->GetChecked() : false});
    }
    GetWidget()->Close();
  }

  void OnCancelButtonClicked() {
    resolved_ = true;
    if (callback_) {
      std::move(callback_).Run(
          {.accepted = false,
           .checkbox_checked = checkbox_ ? checkbox_->GetChecked() : false});
    }
    GetWidget()->Close();
  }

  void OnCloseButtonClicked() {
    resolved_ = true;
    if (callback_) {
      std::move(callback_).Run(
          {.accepted = false,
           .checkbox_checked = checkbox_ ? checkbox_->GetChecked() : false});
    }
    GetWidget()->Close();
  }

  // --- Animation ---
 public:
  void AddedToWidget() override {
    // White body is painted only inside the content area (see
    // DialogBodyBackground); the surrounding margin stays transparent so the
    // ui::Shadow layer below remains visible.
    SetBackground(std::make_unique<DialogBodyBackground>(SK_ColorWHITE,
                                                         kDialogCornerRadius));
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    SetupCompositorShadow();

    auto* widget_layer = GetWidget()->GetLayer();
    if (!widget_layer) {
      return;
    }
    widget_layer->SetOpacity(0.0f);
    gfx::Transform transform;
    transform.Translate(0, -10);
    widget_layer->SetTransform(transform);

    views::AnimationBuilder()
        .Once()
        .SetDuration(base::Milliseconds(150))
        .SetOpacity(widget_layer, 1.0f, gfx::Tween::EASE_OUT)
        .SetTransform(widget_layer, gfx::Transform(), gfx::Tween::EASE_OUT);
  }

  void StartExitAnimation(base::OnceClosure on_completed) {
    auto* widget_layer = GetWidget()->GetLayer();
    if (!widget_layer) {
      std::move(on_completed).Run();
      return;
    }
    widget_layer->SetTransform(gfx::Transform());

    views::AnimationBuilder()
        .OnEnded(std::move(on_completed))
        .Once()
        .SetDuration(base::Milliseconds(120))
        .SetOpacity(widget_layer, 0.0f, gfx::Tween::EASE_IN);
  }

  const XenonCommonDialog::Style style_;
  XenonCommonDialog::Callback callback_;
  bool resolved_ = false;
  raw_ptr<views::Checkbox> checkbox_ = nullptr;
  std::unique_ptr<ui::Shadow> compositor_shadow_;
};

class XenonCommonDialogDelegate : public views::DialogDelegate {
 public:
  XenonCommonDialogDelegate(
      std::unique_ptr<XenonCommonDialogView> contents_view) {
    contents_view_ = SetContentsView(std::move(contents_view));
  }
  ~XenonCommonDialogDelegate() override = default;

  // Self-drawn shell: transparent frame, 24px mask for HWND clip.
  std::unique_ptr<views::FrameView> CreateFrameView(
      views::Widget* widget) override {
    auto frame =
        std::make_unique<views::BubbleFrameView>(gfx::Insets(), gfx::Insets());
    auto border = std::make_unique<views::BubbleBorder>(
        views::BubbleBorder::FLOAT, views::BubbleBorder::NO_SHADOW);
    // Square HWND mask. Visual 24px round + shadow come from content layer.
    border->set_draw_border_stroke(false);
    border->SetColor(SK_ColorTRANSPARENT);
    frame->SetBubbleBorder(std::move(border));
    return frame;
  }

  bool OnCloseRequested(views::Widget::ClosedReason close_reason) override {
    if (allow_close_) {
      return true;
    }
    if (contents_view_) {
      contents_view_->StartExitAnimation(
          base::BindOnce(&XenonCommonDialogDelegate::CloseDialog,
                         weak_ptr_factory_.GetWeakPtr()));
    } else {
      return true;
    }
    return false;
  }

 private:
  void CloseDialog() {
    allow_close_ = true;
    if (GetWidget()) {
      GetWidget()->Close();
    }
  }

  raw_ptr<XenonCommonDialogView> contents_view_ = nullptr;
  bool allow_close_ = false;
  base::WeakPtrFactory<XenonCommonDialogDelegate> weak_ptr_factory_{this};
};

BEGIN_METADATA(XenonCommonDialogView)
END_METADATA

}  // namespace

// static
void XenonCommonDialog::Show(gfx::NativeWindow parent,
                             Style style,
                             const std::u16string& title,
                             const std::u16string& body_text,
                             const std::u16string& checkbox_text,
                             bool checkbox_checked,
                             const std::u16string& cancel_text,
                             const std::u16string& confirm_text,
                             Callback callback,
                             bool show_mask) {
  // If parent is null or not found in browser list, fallback to active browser
  // window.
  GlobalBrowserCollection* browsers = GlobalBrowserCollection::GetInstance();
  if (!parent || !browsers->FindBrowserWithWindow(parent)) {
    BrowserWindowInterface* active_window = browsers->GetLastActiveBrowser();
    Browser* active_browser = active_window
                                  ? active_window->GetBrowserForMigrationOnly()
                                  : nullptr;
    if (active_browser && active_browser->GetWindow()) {
      parent = active_browser->GetWindow()->GetNativeWindow();
    }
  }

  auto delegate = std::make_unique<XenonCommonDialogDelegate>(
      std::make_unique<XenonCommonDialogView>(
          style, title, body_text, checkbox_text, checkbox_checked, cancel_text,
          confirm_text, std::move(callback)));

  // Center on parent window.
  delegate->SetModalType(show_mask ? ui::mojom::ModalType::kWindow
                                   : ui::mojom::ModalType::kNone);

  // Configure delegate settings to hide default chrome.
  delegate->SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
  delegate->set_margins(gfx::Insets());
  delegate->SetShowCloseButton(false);
  delegate->set_use_custom_frame(true);
  delegate->set_corner_radius(kDialogCornerRadius);

  views::Widget* widget = nullptr;
  if (show_mask) {
    widget = constrained_window::CreateBrowserModalDialogViews(
        std::move(delegate), parent);
  } else {
    widget = views::DialogDelegate::CreateDialogWidget(
        std::move(delegate), gfx::NativeWindow(), parent);
  }

  // Preferred size already includes the shadow margin (the content view carries
  // an empty border of that size), so do not enlarge again.
  widget->CenterWindow(widget->GetRootView()->GetPreferredSize({}));
#if BUILDFLAG(IS_WIN)
  DisableNativeWindowChrome(widget);
#endif
  widget->Show();
#if BUILDFLAG(IS_WIN)
  DisableNativeWindowChrome(widget);
#endif
}

}  // namespace xenon
