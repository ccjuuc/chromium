#include "xenon_overlay/chrome/browser/ui/xenon_common_bubble.h"

#include <memory>
#include <utility>

#include "base/memory/weak_ptr.h"
#include "chrome/browser/ui/views/bubble/webui_bubble_manager.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/color/color_id.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/views/background.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/layout/layout_provider.h"
#include "ui/views/metadata/view_factory.h"
#include "ui/views/style/typography.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace xenon {

namespace {

// 与 profile_menu_view_base 中 kMenuWidth 同量级；简单文本可略窄。
constexpr int kBubbleWidth = 280;

// profile_menu_view_base: kMenuEdgeMargin 16, kMenuItemLeftInternalPadding 12
constexpr int kPaddingHorizontal = 16;
constexpr int kPaddingVertical = 12;

}  // namespace

XenonCommonBubble::XenonCommonBubble(views::View* anchor_view,
                                     std::u16string text)
    // 与 ProfileMenuViewBase 相同：箭头在右上，阴影为 DIALOG_SHADOW（Win/Linux 即 STANDARD_SHADOW）
    : views::BubbleDialogDelegate(anchor_view,
                                  views::BubbleBorder::TOP_RIGHT,
                                  views::BubbleBorder::DIALOG_SHADOW) {
  set_close_on_deactivate(true);
  // 与 ProfileMenuViewBase 一致：内容区相对 client 无额外边距，由内层 BoxLayout 控制
  set_margins(gfx::Insets());
  SetShowTitle(false);
  SetShowCloseButton(false);
  SetButtons(static_cast<int>(ui::mojom::DialogButton::kNone));
  set_fixed_width(kBubbleWidth);
  set_corner_radius(kCornerRadius);

  // 使用 Xenon 统一 16px 圆角，背景沿用当前主题 Bubble 背景。
  SetBackgroundColor(ui::kColorBubbleBackground);

  if (anchor_view && anchor_view->GetWidget()) {
    set_parent_window(anchor_view->GetWidget()->GetNativeView());
  }

  auto root = views::Builder<views::View>()
                  .SetBackground(
                      views::CreateSolidBackground(ui::kColorBubbleBackground))
                  .SetLayoutManager(std::make_unique<views::BoxLayout>(
                      views::BoxLayout::Orientation::kVertical,
                      gfx::Insets::VH(kPaddingVertical, kPaddingHorizontal), 0))
                  .AddChild(
                      views::Builder<views::Label>()
                          .SetText(std::move(text))
                          .SetMultiLine(true)
                          .SetHorizontalAlignment(
                              gfx::HorizontalAlignment::ALIGN_LEFT)
                          .SetTextStyle(views::style::STYLE_BODY_2))
                  .Build();
  SetContentsView(std::move(root));
}

XenonCommonBubble::~XenonCommonBubble() = default;

// static
views::Widget* XenonCommonBubble::Show(views::View* anchor_view,
                                       std::u16string text) {
  if (!anchor_view) {
    return nullptr;
  }
  auto bubble =
      std::make_unique<XenonCommonBubble>(anchor_view, std::move(text));
  views::Widget* widget =
      views::BubbleDialogDelegate::CreateBubble(std::move(bubble));
  if (widget) {
    widget->Show();
  }
  return widget;
}

// static
void XenonCommonBubble::ConfigureBeforeWidgetInitialization(
    views::BubbleDialogDelegate* bubble_delegate) {
  if (bubble_delegate) {
    bubble_delegate->set_corner_radius(kCornerRadius);
  }
}

// static
void XenonCommonBubble::ApplyStyle(
    views::BubbleDialogDelegate* bubble_delegate) {
  if (!bubble_delegate) {
    return;
  }

  views::BubbleFrameView* frame_view = bubble_delegate->GetBubbleFrameView();
  if (!frame_view) {
    return;
  }

  frame_view->SetRoundedCorners(gfx::RoundedCornersF(kCornerRadius));
  if (frame_view->bubble_border()) {
    frame_view->bubble_border()->set_md_shadow_elevation(
        views::LayoutProvider::Get()->GetShadowElevationMetric(
            views::Emphasis::kMaximum));
  }
}

// static
void XenonCommonBubble::ConfigureWebUIBubbleManager(
    WebUIBubbleManager* bubble_manager) {
  CHECK(bubble_manager);
  // Chromium 142 WebUIBubbleManager only exposes a widget initialization
  // callback without the WebUIBubbleDialogView pointer. Apply WebUI bubble
  // styling after ShowBubble() via ApplyWebUIBubbleStyle().
}

// static
void XenonCommonBubble::ApplyWebUIBubbleStyle(
    WebUIBubbleManager* bubble_manager) {
  CHECK(bubble_manager);
  base::WeakPtr<WebUIBubbleDialogView> bubble_view =
      bubble_manager->bubble_view_for_testing();
  if (!bubble_view) {
    return;
  }
  ConfigureBeforeWidgetInitialization(bubble_view.get());
  ApplyStyle(bubble_view.get());
}

}  // namespace xenon
