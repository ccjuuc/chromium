#include "xenon_overlay/chrome/browser/ui/xenon_common_bubble.h"

#include <memory>
#include <utility>

#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/color/color_id.h"
#include "ui/views/background.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
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

  // 使用系统对话框圆角（GetCornerRadius 默认 kDialogRadius），与多数 Chrome bubble 一致
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

}  // namespace xenon
