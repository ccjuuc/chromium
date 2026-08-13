#include "xenon_overlay/chrome/browser/ui/xenon_common_bubble.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/memory/weak_ptr.h"
#include "cc/paint/paint_filter.h"
#include "chrome/browser/ui/views/bubble/webui_bubble_manager.h"
#include "ui/base/mojom/dialog_button.mojom.h"
#include "ui/base/mojom/ui_base_types.mojom-shared.h"
#include "ui/color/color_id.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/geometry/insets_f.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/rrect_f.h"
#include "ui/gfx/scoped_canvas.h"
#include "ui/gfx/skia_util.h"
#include "ui/views/background.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/bubble/bubble_frame_view.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/metadata/view_factory.h"
#include "ui/views/style/typography.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"
#include "xenon_overlay/chrome/browser/ui/xenon_menu_shadow_border.h"

namespace xenon {

namespace {

// 与 profile_menu_view_base 中 kMenuWidth 同量级；简单文本可略窄。
constexpr int kBubbleWidth = 280;

// profile_menu_view_base: kMenuEdgeMargin 16, kMenuItemLeftInternalPadding 12
constexpr int kPaddingHorizontal = 16;
constexpr int kPaddingVertical = 12;

int BubbleBorderElevationForShadow(const XenonMenuShadow& shadow) {
  if (shadow.style == ShadowStyle::kNone) {
    return 0;
  }
  return std::max(0, shadow.elevation);
}

gfx::Insets CustomShadowInsetsForShadow(const XenonMenuShadow& shadow) {
  const int blur = std::max(0, shadow.elevation);
  const int spread = shadow.spread;
  return gfx::Insets::TLBR(std::max(0, blur + spread - shadow.y_offset),
                           std::max(0, blur + spread - shadow.x_offset),
                           std::max(0, blur + spread + shadow.y_offset),
                           std::max(0, blur + spread + shadow.x_offset));
}

gfx::Insets MaxInsets(const gfx::Insets& lhs, const gfx::Insets& rhs) {
  return gfx::Insets::TLBR(
      std::max(lhs.top(), rhs.top()), std::max(lhs.left(), rhs.left()),
      std::max(lhs.bottom(), rhs.bottom()), std::max(lhs.right(), rhs.right()));
}

class XenonBubbleShadowBorder : public views::BubbleBorder {
 public:
  XenonBubbleShadowBorder(Arrow arrow,
                          Shadow shadow_type,
                          const XenonMenuShadow& shadow)
      : views::BubbleBorder(arrow, shadow_type), shadow_(shadow) {
    set_md_shadow_elevation(shadow.style == ShadowStyle::kBubbleBorder
                                ? BubbleBorderElevationForShadow(shadow)
                                : 0);
  }

  XenonBubbleShadowBorder(const XenonBubbleShadowBorder&) = delete;
  XenonBubbleShadowBorder& operator=(const XenonBubbleShadowBorder&) = delete;

  ~XenonBubbleShadowBorder() override = default;

  void Paint(const views::View& view, gfx::Canvas* canvas) override {
    if (UsesCustomPaintedShadow()) {
      PaintCustomShadow(view, canvas);
    }
    views::BubbleBorder::Paint(view, canvas);
  }

  gfx::Insets GetInsets() const override {
    if (!UsesCustomPaintedShadow()) {
      return views::BubbleBorder::GetInsets();
    }
    return MaxInsets(GetLayoutInsets(), CustomShadowInsetsForShadow(shadow_));
  }

  gfx::Rect GetBounds(const gfx::Rect& anchor_rect,
                      const gfx::Size& contents_size) const override {
    if (!UsesCustomPaintedShadow()) {
      return views::BubbleBorder::GetBounds(anchor_rect, contents_size);
    }

    views::BubbleBorder layout_border(arrow(), shadow());
    layout_border.set_md_shadow_elevation(
        BubbleBorderElevationForShadow(shadow_));
    layout_border.set_rounded_corners(rounded_corners());
    layout_border.set_visible_arrow(visible_arrow());
    layout_border.set_arrow_offset(arrow_offset());

    gfx::Rect bounds = layout_border.GetBounds(anchor_rect, contents_size);
    const gfx::Insets layout_insets = layout_border.GetInsets();
    const gfx::Insets paint_insets = GetInsets();
    const gfx::Insets extra_insets = paint_insets - layout_insets;
    bounds.Inset(gfx::Insets::TLBR(-extra_insets.top(), -extra_insets.left(),
                                   -extra_insets.bottom(),
                                   -extra_insets.right()));
    return bounds;
  }

 private:
  bool UsesCustomPaintedShadow() const {
    return shadow_.style == ShadowStyle::kViewShadow ||
           shadow_.style == ShadowStyle::kCompositorShadow ||
           shadow_.style == ShadowStyle::kBoxShadow;
  }

  gfx::Insets GetLayoutInsets() const {
    views::BubbleBorder layout_border(arrow(), shadow());
    layout_border.set_md_shadow_elevation(
        BubbleBorderElevationForShadow(shadow_));
    layout_border.set_rounded_corners(rounded_corners());
    layout_border.set_visible_arrow(visible_arrow());
    layout_border.set_arrow_offset(arrow_offset());
    return layout_border.GetInsets();
  }

  void PaintCustomShadow(const views::View& view, gfx::Canvas* canvas) const {
    gfx::Rect shadow_source = view.GetLocalBounds();
    shadow_source.Inset(GetInsets());
    if (shadow_source.IsEmpty()) {
      return;
    }

    shadow_source.Inset(-shadow_.spread);

    gfx::ScopedCanvas scoped(canvas);
    gfx::RectF client_bounds(view.GetLocalBounds());
    client_bounds.Inset(gfx::InsetsF(GetInsets()));
    canvas->sk_canvas()->clipRRect(
        SkRRect(gfx::RRectF(client_bounds, rounded_corners())),
        SkClipOp::kDifference, true);

    cc::PaintFlags flags;
    flags.setStyle(cc::PaintFlags::kFill_Style);
    flags.setAntiAlias(true);
    flags.setColor(SK_ColorBLACK);

    const float sigma = std::max(0, shadow_.elevation) * 0.5f;
    if (sigma > 0.0f) {
      flags.setImageFilter(sk_make_sp<cc::DropShadowPaintFilter>(
          shadow_.x_offset, shadow_.y_offset, sigma, sigma,
          SkColor4f::FromColor(
              ParseHexColor(shadow_.color_hex, shadow_.opacity)),
          cc::DropShadowPaintFilter::ShadowMode::kDrawShadowOnly, nullptr));
    }

    canvas->DrawRoundRect(shadow_source,
                          std::max(0, kBubbleShadowCornerRadius()), flags);
  }

  static int kBubbleShadowCornerRadius() {
    return XenonCommonBubble::kCornerRadius;
  }

  const XenonMenuShadow shadow_;
};

std::unique_ptr<views::BubbleBorder> CreateXenonBubbleBorder(
    views::BubbleBorder::Arrow arrow,
    views::BubbleBorder::Shadow shadow_type,
    const XenonMenuShadow& shadow) {
  auto border =
      std::make_unique<XenonBubbleShadowBorder>(arrow, shadow_type, shadow);
  border->set_rounded_corners(
      gfx::RoundedCornersF(XenonCommonBubble::kCornerRadius));
  return border;
}

}  // namespace

XenonCommonBubble::XenonCommonBubble(views::View* anchor_view,
                                     std::u16string text,
                                     const XenonMenuShadow& shadow,
                                     views::BubbleBorder::Arrow arrow)
    // 与 ProfileMenuViewBase 相同：箭头在右上，阴影为 DIALOG_SHADOW（Win/Linux
    // 即 STANDARD_SHADOW）
    : views::BubbleDialogDelegate(anchor_view,
                                  arrow,
                                  views::BubbleBorder::DIALOG_SHADOW),
      shadow_(shadow) {
  set_close_on_deactivate(true);
  // 与 ProfileMenuViewBase 一致：内容区相对 client 无额外边距，由内层 BoxLayout
  // 控制
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
                  .AddChild(views::Builder<views::Label>()
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
                                       std::u16string text,
                                       const XenonMenuShadow& shadow) {
  if (!anchor_view) {
    return nullptr;
  }
  auto bubble = std::make_unique<XenonCommonBubble>(
      anchor_view, std::move(text), shadow, views::BubbleBorder::TOP_RIGHT);
  views::Widget* widget =
      views::BubbleDialogDelegate::CreateBubbleDeprecated(
          std::move(bubble),
          views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
  if (widget) {
    widget->Show();
  }
  return widget;
}

// static
views::Widget* XenonCommonBubble::ShowAt(views::View* parent_view,
                                         const gfx::Rect& anchor_rect,
                                         views::BubbleBorder::Arrow arrow,
                                         std::u16string text,
                                         const XenonMenuShadow& shadow) {
  if (!parent_view) {
    return nullptr;
  }
  auto bubble = std::make_unique<XenonCommonBubble>(
      parent_view, std::move(text), shadow, arrow);
  bubble->SetAnchorView(nullptr);
  if (parent_view->GetWidget()) {
    bubble->SetAnchorWidget(parent_view->GetWidget());
  }
  bubble->SetAnchorRect(anchor_rect);
  views::Widget* widget =
      views::BubbleDialogDelegate::CreateBubbleDeprecated(
          std::move(bubble),
          views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
  if (widget) {
    widget->Show();
  }
  return widget;
}

std::unique_ptr<views::FrameView> XenonCommonBubble::CreateFrameView(
    views::Widget* widget) {
  const FrameMargins& margin = frame_margins();
  auto frame =
      std::make_unique<views::BubbleFrameView>(margin.title, gfx::Insets());
  frame->SetFootnoteMargins(margin.footnote);
  frame->SetFootnoteView(DisownFootnoteView());

  auto border = CreateXenonBubbleBorder(arrow(), GetShadow(), shadow_);
  border->SetColor(background_color());

  if (GetParams().round_corners) {
    border->set_rounded_corners(gfx::RoundedCornersF(GetCornerRadius()));
  }

  frame->SetBubbleBorder(std::move(border));
  return frame;
}

// static
void XenonCommonBubble::ConfigureBeforeWidgetInitialization(
    views::BubbleDialogDelegate* bubble_delegate) {
  if (bubble_delegate) {
    bubble_delegate->set_corner_radius(kCornerRadius);
  }
}

// static
void XenonCommonBubble::ApplyStyle(views::BubbleDialogDelegate* bubble_delegate,
                                   const XenonMenuShadow& shadow) {
  if (!bubble_delegate) {
    return;
  }

  views::BubbleFrameView* frame_view = bubble_delegate->GetBubbleFrameView();
  if (!frame_view) {
    return;
  }

  auto border = CreateXenonBubbleBorder(frame_view->GetArrow(),
                                        bubble_delegate->GetShadow(), shadow);
  border->SetColor(bubble_delegate->background_color());
  border->set_rounded_corners(gfx::RoundedCornersF(kCornerRadius));
  if (frame_view->bubble_border()) {
    border->set_visible_arrow(frame_view->bubble_border()->visible_arrow());
  }
  frame_view->SetBubbleBorder(std::move(border));
  bubble_delegate->SizeToContents();
  frame_view->SchedulePaint();
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
    WebUIBubbleManager* bubble_manager,
    const XenonMenuShadow& shadow) {
  CHECK(bubble_manager);
  base::WeakPtr<WebUIBubbleDialogView> bubble_view =
      bubble_manager->bubble_view_for_testing();
  if (!bubble_view) {
    return;
  }
  ConfigureBeforeWidgetInitialization(bubble_view.get());
  ApplyStyle(bubble_view.get(), shadow);
}

}  // namespace xenon
