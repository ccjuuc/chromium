// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_shadow_test_window.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/time/time.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/compositor/layer.h"
#include "ui/compositor/layer_animator.h"
#include "ui/compositor_extra/shadow.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/animation/animation.h"
#include "ui/gfx/animation/animation_delegate.h"
#include "ui/gfx/animation/linear_animation.h"
#include "ui/gfx/animation/multi_animation.h"
#include "ui/gfx/animation/slide_animation.h"
#include "ui/gfx/animation/throb_animation.h"
#include "ui/gfx/animation/tween.h"
#include "ui/gfx/font.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/compositor/scoped_layer_animation_settings.h"
#include "ui/views/animation/animation_builder.h"
#include "ui/views/animation/bounds_animator.h"
#include "ui/views/animation/widget_fade_animator.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/focusable_border.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/round_rect_painter.h"
#include "ui/views/view.h"
#include "ui/views/view_shadow.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"
#include "xenon_overlay/chrome/browser/ui/xenon_frameless_shadow_view.h"

namespace xenon {
namespace {

constexpr int kWidgetTestWidth = 760;
constexpr int kWidgetTestHeight = 430;
constexpr int kViewTestWidth = 820;
constexpr int kViewTestHeight = 560;
constexpr int kBorderTestWidth = 860;
constexpr int kBorderTestHeight = 720;
constexpr int kAnimationTestWidth = 860;
constexpr int kAnimationTestHeight = 920;
constexpr int kSampleWindowWidth = 440;
constexpr int kSampleWindowHeight = 250;
constexpr int kBackdropHorizontalPadding = 96;
constexpr int kBackdropVerticalPadding = 72;
constexpr int kBorderlessShadowMargin = 28;
constexpr int kBorderlessDefaultShadowElevation = 12;
constexpr int kBorderlessDropShadowElevation = 24;
constexpr int kCardRadius = 16;
constexpr int kShadowElevation = 12;

constexpr SkColor kWindowBg = SkColorSetRGB(0xF6, 0xF7, 0xFB);
constexpr SkColor kCardBg = SK_ColorWHITE;
constexpr SkColor kShadowBackdropBg = SkColorSetRGB(0xB7, 0xF2, 0xB0);
constexpr SkColor kTextPrimary = SkColorSetRGB(0x1D, 0x21, 0x29);
constexpr SkColor kTextSecondary = SkColorSetRGB(0x5F, 0x66, 0x73);
constexpr SkColor kBorder = SkColorSetARGB(0x24, 0x00, 0x00, 0x00);
constexpr SkColor kAnimationBlue = SkColorSetRGB(0x2F, 0x6F, 0xD6);
constexpr SkColor kAnimationGreen = SkColorSetRGB(0x18, 0x8A, 0x5A);
constexpr SkColor kAnimationPurple = SkColorSetRGB(0x7C, 0x4D, 0xB5);

std::unique_ptr<views::Label> MakeLabel(const std::u16string& text,
                                        SkColor color) {
  auto label = std::make_unique<views::Label>(text);
  label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  label->SetEnabledColor(color);
  label->SetMultiLine(true);
  return label;
}

std::unique_ptr<views::View> MakeTextBlock(const std::u16string& title,
                                           const std::u16string& body) {
  auto block = std::make_unique<views::View>();
  auto* layout = block->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(), 4));
  layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kStretch);

  auto title_label = MakeLabel(title, kTextPrimary);
  title_label->SetFontList(title_label->font_list().Derive(
      1, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
  block->AddChildView(std::move(title_label));
  block->AddChildView(MakeLabel(body, kTextSecondary));
  return block;
}

std::unique_ptr<views::Label> MakeSectionLabel(const std::u16string& text) {
  auto label = MakeLabel(text, kTextPrimary);
  label->SetFontList(
      label->font_list().Derive(2, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
  return label;
}

std::unique_ptr<views::View> MakeRoundedPanel(SkColor color,
                                              const gfx::Insets& insets) {
  auto panel = std::make_unique<views::View>();
  panel->SetBackground(views::CreateRoundedRectBackground(color, kCardRadius));
  panel->SetBorder(views::CreateRoundedRectBorder(1, kCardRadius, kBorder));
  panel->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, insets, 6));
  return panel;
}

void StyleSmallButton(views::LabelButton* button) {
  button->SetBorder(views::CreateRoundedRectBorder(1, 8, kBorder));
}

gfx::Rect GetInitialBounds(gfx::NativeView parent,
                           int width,
                           int height,
                           int offset = 0) {
  display::Screen* screen = display::Screen::Get();
  if (!screen) {
    return gfx::Rect(80 + offset, 80 + offset, width, height);
  }

  display::Display display = parent ? screen->GetDisplayNearestView(parent)
                                    : screen->GetPrimaryDisplay();
  gfx::Rect work_area = display.work_area();
  return gfx::Rect(work_area.x() + (work_area.width() - width) / 2 + offset,
                   work_area.y() + (work_area.height() - height) / 2 + offset,
                   width, height);
}

gfx::Rect GetShadowBackdropBounds(const gfx::Rect& sample_bounds) {
  return gfx::Rect(sample_bounds.x() - kBackdropHorizontalPadding,
                   sample_bounds.y() - kBackdropVerticalPadding,
                   sample_bounds.width() + kBackdropHorizontalPadding * 2,
                   sample_bounds.height() + kBackdropVerticalPadding * 2);
}

class CloseableContentsView : public views::View {
 public:
  CloseableContentsView(const std::u16string& title,
                        const std::u16string& subtitle,
                        gfx::Size preferred_size,
                        bool rounded_window = false)
      : preferred_size_(preferred_size) {
    SetBackground(rounded_window ? views::CreateRoundedRectBackground(
                                       kWindowBg, kCardRadius)
                                 : views::CreateSolidBackground(kWindowBg));
    SetBorder(views::CreateEmptyBorder(gfx::Insets(24)));

    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets(), 16));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);

    auto header = std::make_unique<views::View>();
    auto* header_layout =
        header->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    header_layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    auto title_label = MakeLabel(title, kTextPrimary);
    title_label->SetFontList(title_label->font_list().Derive(
        6, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
    views::Label* title_ptr = header->AddChildView(std::move(title_label));
    header_layout->SetFlexForView(title_ptr, 1);

    auto close_button = std::make_unique<views::LabelButton>(
        base::BindRepeating(&CloseableContentsView::CloseWindow,
                            base::Unretained(this)),
        u"关闭");
    close_button->SetAccessibleName(u"关闭阴影测试窗口");
    StyleSmallButton(close_button.get());
    header->AddChildView(std::move(close_button));
    AddChildView(std::move(header));

    AddChildView(MakeLabel(subtitle, kTextSecondary));
  }

  CloseableContentsView(const CloseableContentsView&) = delete;
  CloseableContentsView& operator=(const CloseableContentsView&) = delete;
  ~CloseableContentsView() override = default;

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return preferred_size_;
  }

 private:
  void CloseWindow() {
    if (auto* widget = GetWidget()) {
      widget->CloseNow();
    }
  }

  gfx::Size preferred_size_;
};

class ShadowBackdropContentsView : public views::View {
 public:
  explicit ShadowBackdropContentsView(const gfx::Size& preferred_size)
      : preferred_size_(preferred_size) {
    SetBackground(views::CreateSolidBackground(kShadowBackdropBg));
  }

  ShadowBackdropContentsView(const ShadowBackdropContentsView&) = delete;
  ShadowBackdropContentsView& operator=(const ShadowBackdropContentsView&) =
      delete;
  ~ShadowBackdropContentsView() override = default;

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    return preferred_size_;
  }

 private:
  gfx::Size preferred_size_;
};

views::Widget* ShowShadowBackdrop(gfx::NativeView parent,
                                  const gfx::Rect& sample_bounds) {
  gfx::Rect backdrop_bounds = GetShadowBackdropBounds(sample_bounds);

  auto* delegate = new views::WidgetDelegate();
  delegate->SetContentsView(std::unique_ptr<views::View>(
      new ShadowBackdropContentsView(backdrop_bounds.size())));

  views::Widget::InitParams params(
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET,
      views::Widget::InitParams::TYPE_WINDOW_FRAMELESS);
  params.delegate = delegate;
  params.parent = parent;
  params.name = "XenonShadowBackdropWindow";
  params.activatable = views::Widget::InitParams::Activatable::kNo;
  params.remove_standard_frame = true;
  params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
  params.bounds = backdrop_bounds;

  views::Widget* widget = new views::Widget();
  widget->Init(std::move(params));
  widget->Show();
  return widget;
}

class WidgetShadowSampleDelegate : public views::WidgetDelegate {
 public:
  explicit WidgetShadowSampleDelegate(base::WeakPtr<views::Widget> backdrop)
      : backdrop_(std::move(backdrop)) {}

  WidgetShadowSampleDelegate(const WidgetShadowSampleDelegate&) = delete;
  WidgetShadowSampleDelegate& operator=(const WidgetShadowSampleDelegate&) =
      delete;
  ~WidgetShadowSampleDelegate() override = default;

  void WindowClosing() override {
    if (backdrop_ && !backdrop_->IsClosed()) {
      backdrop_->CloseNow();
    }
    views::WidgetDelegate::WindowClosing();
  }

 private:
  base::WeakPtr<views::Widget> backdrop_;
};

int GetBorderlessShadowElevation(
    views::Widget::InitParams::ShadowType shadow_type) {
  return shadow_type == views::Widget::InitParams::ShadowType::kDrop
             ? kBorderlessDropShadowElevation
             : kBorderlessDefaultShadowElevation;
}

std::unique_ptr<views::View> CreateWidgetShadowSampleContents(
    const std::u16string& title,
    const std::u16string& body,
    bool borderless,
    views::Widget::InitParams::ShadowType shadow_type) {
  auto contents = std::make_unique<CloseableContentsView>(
      title, body, gfx::Size(kSampleWindowWidth, kSampleWindowHeight),
      borderless);

  auto panel = MakeRoundedPanel(kCardBg, gfx::Insets::VH(18, 22));
  panel->AddChildView(MakeTextBlock(
      borderless ? u"无边框 Widget 示例" : u"普通 Widget 示例",
      borderless
          ? u"这个窗口使用 TYPE_WINDOW_FRAMELESS，并设置 "
            u"remove_standard_frame=true "
            u"和透明窗口背景；XenonFramelessShadowView 负责阴影外框。"
          : u"这个窗口只使用 Widget::InitParams::shadow_type；没有额外添加 "
            u"ui::Shadow 或 views::ViewShadow，阴影落在 #B7F2B0 纯色蒙版上。"));
  contents->AddChildView(std::move(panel));

  if (!borderless) {
    return contents;
  }

  XenonFramelessShadowView::Params shadow_params;
  shadow_params.shadow_insets = gfx::Insets(kBorderlessShadowMargin);
  shadow_params.corner_radius = kCardRadius;
  if (shadow_type != views::Widget::InitParams::ShadowType::kNone) {
    shadow_params.shadow_elevation = GetBorderlessShadowElevation(shadow_type);
  }

  return std::make_unique<XenonFramelessShadowView>(std::move(contents),
                                                    std::move(shadow_params));
}

void ShowWidgetShadowSample(gfx::NativeView parent,
                            views::Widget::InitParams::ShadowType shadow_type,
                            const std::u16string& title,
                            const std::u16string& body,
                            int offset,
                            bool borderless,
                            bool show_backdrop) {
  gfx::Rect sample_bounds =
      GetInitialBounds(parent, kSampleWindowWidth, kSampleWindowHeight, offset);
  views::Widget* backdrop = nullptr;
  if (show_backdrop) {
    backdrop = ShowShadowBackdrop(parent, sample_bounds);
  }

  auto* delegate = new WidgetShadowSampleDelegate(
      backdrop ? backdrop->GetWeakPtr() : base::WeakPtr<views::Widget>());
  delegate->SetTitle(title);
  delegate->SetCanResize(!borderless);
  delegate->SetCanMaximize(false);
  delegate->SetContentsView(
      CreateWidgetShadowSampleContents(title, body, borderless, shadow_type));

  views::Widget::InitParams params(
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET,
      borderless ? views::Widget::InitParams::TYPE_WINDOW_FRAMELESS
                 : views::Widget::InitParams::TYPE_WINDOW);
  params.delegate = delegate;
  params.parent = parent;
  params.name = borderless ? "XenonBorderlessWidgetShadowSampleWindow"
                           : "XenonWidgetShadowSampleWindow";
  params.remove_standard_frame = borderless;
  if (borderless) {
    params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
  }
  params.shadow_type =
      borderless ? views::Widget::InitParams::ShadowType::kNone : shadow_type;
  if (borderless) {
    gfx::Size shadow_size =
        XenonFramelessShadowView::GetPreferredSizeForContent(
            sample_bounds.size(), gfx::Insets(kBorderlessShadowMargin));
    params.bounds = gfx::Rect(sample_bounds.x() - kBorderlessShadowMargin,
                              sample_bounds.y() - kBorderlessShadowMargin,
                              shadow_size.width(), shadow_size.height());
  } else {
    params.bounds = sample_bounds;
  }

  views::Widget* widget = new views::Widget();
  widget->Init(std::move(params));
  widget->Show();
  if (backdrop) {
    widget->StackAboveWidget(backdrop);
  }
}

class WidgetShadowTestContentsView : public CloseableContentsView {
 public:
  explicit WidgetShadowTestContentsView(gfx::NativeView parent)
      : CloseableContentsView(
            u"Widget 阴影测试",
            u"覆盖 views::Widget::InitParams::ShadowType "
            u"的全部选项。每行都可以打开标准窗口或无边框窗口。",
            gfx::Size(kWidgetTestWidth, kWidgetTestHeight)),
        parent_(parent) {
    AddChildView(MakeSectionLabel(u"views::Widget::InitParams::ShadowType"));
    AddShadowTypeRow(
        views::Widget::InitParams::ShadowType::kDefault, u"kDefault",
        u"使用当前 Widget 类型和平台 native widget 的默认阴影策略。", -48);
    AddShadowTypeRow(
        views::Widget::InitParams::ShadowType::kNone, u"kNone",
        u"请求不绘制 Widget/native window 阴影，用来验证无阴影窗口。", 0);
    AddShadowTypeRow(views::Widget::InitParams::ShadowType::kDrop, u"kDrop",
                     u"请求绘制强调 Z-order 的 drop shadow。", 48);
  }

  WidgetShadowTestContentsView(const WidgetShadowTestContentsView&) = delete;
  WidgetShadowTestContentsView& operator=(const WidgetShadowTestContentsView&) =
      delete;
  ~WidgetShadowTestContentsView() override = default;

 private:
  void AddShadowTypeRow(views::Widget::InitParams::ShadowType shadow_type,
                        const std::u16string& title,
                        const std::u16string& body,
                        int offset) {
    auto row = std::make_unique<views::View>();
    row->SetPreferredSize(gfx::Size(1, 78));
    row->SetBackground(views::CreateRoundedRectBackground(kCardBg, 12));
    row->SetBorder(views::CreateRoundedRectBorder(1, 12, kBorder));

    auto* row_layout = row->SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(12, 16),
        12));
    row_layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    views::View* text = row->AddChildView(MakeTextBlock(title, body));
    row_layout->SetFlexForView(text, 1);

    auto buttons = std::make_unique<views::View>();
    auto* buttons_layout =
        buttons->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 8));
    buttons_layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    auto standard_button = std::make_unique<views::LabelButton>(
        base::BindRepeating(&WidgetShadowTestContentsView::OpenSample,
                            base::Unretained(this), shadow_type, title, body,
                            offset, false),
        u"标准窗口");
    standard_button->SetAccessibleName(title + u" 标准窗口示例");
    StyleSmallButton(standard_button.get());
    buttons->AddChildView(std::move(standard_button));

    auto borderless_button = std::make_unique<views::LabelButton>(
        base::BindRepeating(&WidgetShadowTestContentsView::OpenSample,
                            base::Unretained(this), shadow_type, title, body,
                            offset + 24, true),
        u"无边框");
    borderless_button->SetAccessibleName(title + u" 无边框窗口示例");
    StyleSmallButton(borderless_button.get());
    buttons->AddChildView(std::move(borderless_button));

    row->AddChildView(std::move(buttons));

    AddChildView(std::move(row));
  }

  void OpenSample(views::Widget::InitParams::ShadowType shadow_type,
                  std::u16string title,
                  std::u16string body,
                  int offset,
                  bool borderless) {
    ShowWidgetShadowSample(parent_, shadow_type, title, body, offset,
                           borderless, !borderless);
  }

  gfx::NativeView parent_;
};

class ManualCompositorShadowDemo : public views::View {
 public:
  ManualCompositorShadowDemo() {
    SetPreferredSize(gfx::Size(1, 108));
    SetPaintToLayer(ui::LAYER_NOT_DRAWN);
    layer()->SetFillsBoundsOpaquely(false);

    shadow_ = std::make_unique<ui::Shadow>();
    shadow_->Init(kShadowElevation);
    shadow_->SetRoundedCornerRadius(kCardRadius);
    AddLayerToRegion(shadow_->layer(), views::LayerRegion::kBelow);

    auto panel = MakeRoundedPanel(SK_ColorWHITE, gfx::Insets::VH(14, 18));
    panel_ = AddChildView(std::move(panel));
    panel_->AddChildView(
        MakeTextBlock(u"ui::Shadow",
                      u"手动创建 compositor nine-patch shadow layer，并在 "
                      u"Layout 中同步 content bounds。"));
  }

  ManualCompositorShadowDemo(const ManualCompositorShadowDemo&) = delete;
  ManualCompositorShadowDemo& operator=(const ManualCompositorShadowDemo&) =
      delete;
  ~ManualCompositorShadowDemo() override = default;

 private:
  void Layout(PassKey) override {
    gfx::Rect content_bounds = GetLocalBounds();
    content_bounds.Inset(gfx::Insets::VH(14, 20));
    panel_->SetBoundsRect(content_bounds);
    shadow_->SetContentBounds(content_bounds);
  }

  raw_ptr<views::View> panel_ = nullptr;
  std::unique_ptr<ui::Shadow> shadow_;
};

class ViewShadowDemo : public views::View {
 public:
  ViewShadowDemo() {
    SetPreferredSize(gfx::Size(1, 108));
    SetPaintToLayer(ui::LAYER_NOT_DRAWN);
    layer()->SetFillsBoundsOpaquely(false);

    auto panel = MakeRoundedPanel(SK_ColorWHITE, gfx::Insets::VH(14, 18));
    panel_ = AddChildView(std::move(panel));
    panel_->AddChildView(
        MakeTextBlock(u"views::ViewShadow",
                      u"给普通 View 包装 shadow。ViewShadow 会让目标 View "
                      u"paint-to-layer，并跟随 bounds。"));

    view_shadow_ =
        std::make_unique<views::ViewShadow>(panel_, kShadowElevation);
    view_shadow_->SetRoundedCornerRadius(kCardRadius);
  }

  ViewShadowDemo(const ViewShadowDemo&) = delete;
  ViewShadowDemo& operator=(const ViewShadowDemo&) = delete;
  ~ViewShadowDemo() override = default;

 private:
  void Layout(PassKey) override {
    gfx::Rect content_bounds = GetLocalBounds();
    content_bounds.Inset(gfx::Insets::VH(14, 20));
    panel_->SetBoundsRect(content_bounds);
  }

  raw_ptr<views::View> panel_ = nullptr;
  std::unique_ptr<views::ViewShadow> view_shadow_;
};

std::unique_ptr<views::View> MakeBubbleBorderDemo() {
  auto panel = std::make_unique<views::View>();
  panel->SetPreferredSize(gfx::Size(1, 116));

  auto border = std::make_unique<views::BubbleBorder>(
      views::BubbleBorder::NONE, views::BubbleBorder::STANDARD_SHADOW);
  border->set_rounded_corners(gfx::RoundedCornersF(kCardRadius));
  border->set_md_shadow_elevation(16);
  panel->SetBorder(std::move(border));
  panel->SetBackground(
      views::CreateRoundedRectBackground(kCardBg, kCardRadius));

  panel->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(18, 22), 6));
  panel->AddChildView(
      MakeTextBlock(u"views::BubbleBorder",
                    u"使用 BubbleBorder 的 STANDARD_SHADOW 和自定义 "
                    u"elevation，适合气泡/浮层类 UI。"));
  return panel;
}

std::unique_ptr<views::View> MakeBorderDemoPanel(
    const std::u16string& title,
    const std::u16string& body,
    std::unique_ptr<views::Border> border) {
  auto panel = std::make_unique<views::View>();
  panel->SetPreferredSize(gfx::Size(1, 76));
  panel->SetBackground(views::CreateRoundedRectBackground(kCardBg, 12));
  panel->SetBorder(std::move(border));
  panel->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(12, 16), 4));
  panel->AddChildView(MakeTextBlock(title, body));
  return panel;
}

std::unique_ptr<views::View> MakeBubbleBorderReferenceDemo() {
  auto panel = std::make_unique<views::View>();
  panel->SetPreferredSize(gfx::Size(1, 86));

  auto border = std::make_unique<views::BubbleBorder>(
      views::BubbleBorder::NONE, views::BubbleBorder::STANDARD_SHADOW);
  border->set_rounded_corners(gfx::RoundedCornersF(kCardRadius));
  border->set_md_shadow_elevation(16);

  panel->SetBackground(std::make_unique<views::BubbleBackground>(border.get()));
  panel->SetBorder(std::move(border));
  panel->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(14, 18), 4));
  panel->AddChildView(
      MakeTextBlock(u"BubbleBorder + BubbleBackground",
                    u"BubbleBorder 的 insets 包含 shadow，BubbleBackground "
                    u"复用同一个 border 的颜色、圆角与内容区域。"));
  return panel;
}

std::unique_ptr<views::View> MakeFocusableBorderDemo() {
  auto border = std::make_unique<views::FocusableBorder>();
  border->SetInsets(gfx::Insets(2));
  border->SetCornerRadius(12);
  return MakeBorderDemoPanel(u"FocusableBorder",
                             u"输入框等可聚焦控件常用的描边；颜色随控件 "
                             u"enabled/focus 状态和主题色变化。",
                             std::move(border));
}

std::unique_ptr<views::View> MakePainterBorderDemo() {
  return MakeBorderDemoPanel(
      u"CreateBorderPainter",
      u"使用 Painter 绘制边框，insets 由调用者传入；Menu "
      u"默认圆角边框也使用类似路径。",
      views::CreateBorderPainter(
          std::make_unique<views::RoundRectPainter>(
              SkColorSetARGB(0xB0, 0x2D, 0x32, 0x3F), 12),
          gfx::Insets(8)));
}

std::unique_ptr<views::View> MakeAnimationTile(const std::u16string& text,
                                               SkColor color,
                                               const gfx::Size& size) {
  auto tile = std::make_unique<views::View>();
  tile->SetPreferredSize(size);
  tile->SetPaintToLayer();
  tile->layer()->SetFillsBoundsOpaquely(false);
  tile->layer()->SetRoundedCornerRadius(gfx::RoundedCornersF(12));
  tile->SetBackground(views::CreateRoundedRectBackground(color, 12));
  tile->SetBorder(views::CreateRoundedRectBorder(
      1, 12, SkColorSetARGB(0x28, 0x00, 0x00, 0x00)));

  auto* layout = tile->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets(), 0));
  layout->set_main_axis_alignment(
      views::BoxLayout::MainAxisAlignment::kCenter);
  layout->set_cross_axis_alignment(
      views::BoxLayout::CrossAxisAlignment::kCenter);

  auto label = MakeLabel(text, SK_ColorWHITE);
  label->SetHorizontalAlignment(gfx::ALIGN_CENTER);
  label->SetFontList(label->font_list().Derive(
      1, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
  tile->AddChildView(std::move(label));
  return tile;
}

class ProgressTrackView : public views::View {
 public:
  ProgressTrackView() {
    SetPreferredSize(gfx::Size(170, 22));
    SetBackground(views::CreateRoundedRectBackground(
        SkColorSetRGB(0xE6, 0xEA, 0xF0), 11));
    fill_ = AddChildView(std::make_unique<views::View>());
    fill_->SetBackground(views::CreateRoundedRectBackground(kAnimationBlue, 9));
  }

  ProgressTrackView(const ProgressTrackView&) = delete;
  ProgressTrackView& operator=(const ProgressTrackView&) = delete;
  ~ProgressTrackView() override = default;

  void SetProgress(double progress) {
    progress_ = std::clamp(progress, 0.0, 1.0);
    UpdateFillBounds();
    SchedulePaint();
  }

 private:
  void Layout(PassKey) override {
    UpdateFillBounds();
  }

  void UpdateFillBounds() {
    const gfx::Rect bounds = GetLocalBounds();
    const int horizontal_inset = 3;
    const int fill_height = std::max(1, bounds.height() - 6);
    const int max_width = std::max(0, bounds.width() - horizontal_inset * 2);
    const int fill_width = std::max(1, static_cast<int>(max_width * progress_));
    fill_->SetBounds(horizontal_inset, (bounds.height() - fill_height) / 2,
                     fill_width, fill_height);
  }

  raw_ptr<views::View> fill_ = nullptr;
  double progress_ = 0.0;
};

class GfxAnimationRow : public views::View, public gfx::AnimationDelegate {
 public:
  enum class Type {
    kLinear,
    kSlide,
    kThrob,
    kMulti,
  };

  GfxAnimationRow(Type type,
                  const std::u16string& title,
                  const std::u16string& body)
      : type_(type) {
    SetPreferredSize(gfx::Size(1, 62));
    SetBackground(views::CreateRoundedRectBackground(kCardBg, 10));
    SetBorder(views::CreateRoundedRectBorder(1, 10, kBorder));

    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(10, 14),
        12));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    views::View* text = AddChildView(MakeTextBlock(title, body));
    layout->SetFlexForView(text, 1);

    auto track = std::unique_ptr<views::View>(new ProgressTrackView());
    track_ = static_cast<ProgressTrackView*>(track.get());
    AddChildView(std::move(track));

    auto play_button = std::make_unique<views::LabelButton>(
        base::BindRepeating(&GfxAnimationRow::Play,
                            base::Unretained(this)),
        type == Type::kSlide ? u"切换" : u"播放");
    play_button->SetAccessibleName(title + u" 动画");
    StyleSmallButton(play_button.get());
    AddChildView(std::move(play_button));

    CreateAnimation();
  }

  GfxAnimationRow(const GfxAnimationRow&) = delete;
  GfxAnimationRow& operator=(const GfxAnimationRow&) = delete;
  ~GfxAnimationRow() override {
    if (animation_) {
      animation_->set_delegate(nullptr);
    }
  }

 private:
  void CreateAnimation() {
    switch (type_) {
      case Type::kLinear:
        animation_ = std::make_unique<gfx::LinearAnimation>(
            base::Milliseconds(650), gfx::LinearAnimation::kDefaultFrameRate,
            this);
        break;
      case Type::kSlide: {
        auto animation = std::make_unique<gfx::SlideAnimation>(this);
        animation->SetSlideDuration(base::Milliseconds(420));
        animation->SetTweenType(gfx::Tween::FAST_OUT_SLOW_IN);
        animation_ = std::move(animation);
        break;
      }
      case Type::kThrob: {
        auto animation = std::make_unique<gfx::ThrobAnimation>(this);
        animation->SetThrobDuration(base::Milliseconds(260));
        animation->SetTweenType(gfx::Tween::EASE_IN_OUT);
        animation_ = std::move(animation);
        break;
      }
      case Type::kMulti: {
        gfx::MultiAnimation::Parts parts = {
            {base::Milliseconds(220), gfx::Tween::LINEAR_OUT_SLOW_IN, 0.0,
             1.0},
            {base::Milliseconds(180), gfx::Tween::LINEAR, 1.0, 1.0},
            {base::Milliseconds(260), gfx::Tween::FAST_OUT_LINEAR_IN, 1.0,
             0.0},
        };
        auto animation = std::make_unique<gfx::MultiAnimation>(parts);
        animation->set_delegate(this);
        animation->set_continuous(false);
        animation_ = std::move(animation);
        break;
      }
    }
  }

  void Play() {
    switch (type_) {
      case Type::kLinear:
      case Type::kMulti:
        animation_->Stop();
        track_->SetProgress(0.0);
        animation_->Start();
        break;
      case Type::kSlide:
        slide_showing_ = !slide_showing_;
        if (slide_showing_) {
          static_cast<gfx::SlideAnimation*>(animation_.get())->Show();
        } else {
          static_cast<gfx::SlideAnimation*>(animation_.get())->Hide();
        }
        break;
      case Type::kThrob: {
        auto* throb_animation =
            static_cast<gfx::ThrobAnimation*>(animation_.get());
        throb_animation->Reset(0.0);
        track_->SetProgress(0.0);
        throb_animation->StartThrobbing(3);
        break;
      }
    }
  }

  void AnimationProgressed(const gfx::Animation* animation) override {
    track_->SetProgress(animation->GetCurrentValue());
  }

  void AnimationEnded(const gfx::Animation* animation) override {
    track_->SetProgress(animation->GetCurrentValue());
  }

  void AnimationCanceled(const gfx::Animation* animation) override {
    track_->SetProgress(animation->GetCurrentValue());
  }

  Type type_;
  bool slide_showing_ = false;
  raw_ptr<ProgressTrackView> track_ = nullptr;
  std::unique_ptr<gfx::Animation> animation_;
};

class GfxAnimationFamilyDemo : public views::View {
 public:
  GfxAnimationFamilyDemo() {
    SetPreferredSize(gfx::Size(1, 300));
    SetBackground(views::CreateRoundedRectBackground(
        SkColorSetRGB(0xF2, 0xF4, 0xF7), 12));
    SetBorder(views::CreateRoundedRectBorder(1, 12, kBorder));

    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(12, 14), 8));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);

    AddChildView(MakeTextBlock(
        u"gfx::Animation 基础类族",
        u"这组动画不直接改变 layer；它们提供进度值，业务代码在 delegate "
        u"里决定如何绘制或更新状态。"));
    AddChildView(std::unique_ptr<views::View>(new GfxAnimationRow(
        GfxAnimationRow::Type::kLinear, u"LinearAnimation",
        u"固定时长 0→1 进度，适合自定义绘制或任意状态插值。")));
    AddChildView(std::unique_ptr<views::View>(new GfxAnimationRow(
        GfxAnimationRow::Type::kSlide, u"SlideAnimation",
        u"可中途反向的 Show/Hide 状态，适合 hover、展开/收起。")));
    AddChildView(std::unique_ptr<views::View>(new GfxAnimationRow(
        GfxAnimationRow::Type::kThrob, u"ThrobAnimation",
        u"在 0 和 1 之间循环，适合提示性闪烁或呼吸动效。")));
    AddChildView(std::unique_ptr<views::View>(new GfxAnimationRow(
        GfxAnimationRow::Type::kMulti, u"MultiAnimation",
        u"多段 tween 串联，这里演示 fade-in / hold / fade-out。")));
  }

  GfxAnimationFamilyDemo(const GfxAnimationFamilyDemo&) = delete;
  GfxAnimationFamilyDemo& operator=(const GfxAnimationFamilyDemo&) = delete;
  ~GfxAnimationFamilyDemo() override = default;
};

class AnimationBuilderDemo : public views::View {
 public:
  AnimationBuilderDemo() {
    SetPreferredSize(gfx::Size(1, 132));
    SetBackground(views::CreateRoundedRectBackground(kCardBg, 12));
    SetBorder(views::CreateRoundedRectBorder(1, 12, kBorder));

    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(14, 18),
        16));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    views::View* text = AddChildView(MakeTextBlock(
        u"views::AnimationBuilder",
        u"显式创建 layer animation sequence；这里同时动画 opacity 和 "
        u"transform，适合 Toast、Dialog、Bubble entrance/exit。"));
    layout->SetFlexForView(text, 1);

    animated_card_ = AddChildView(MakeAnimationTile(
        u"Layer\n动画", kAnimationBlue, gfx::Size(128, 72)));

    auto replay_button = std::make_unique<views::LabelButton>(
        base::BindRepeating(&AnimationBuilderDemo::PlayAnimation,
                            base::Unretained(this)),
        u"播放");
    replay_button->SetAccessibleName(u"播放 AnimationBuilder 动画");
    StyleSmallButton(replay_button.get());
    AddChildView(std::move(replay_button));
  }

  AnimationBuilderDemo(const AnimationBuilderDemo&) = delete;
  AnimationBuilderDemo& operator=(const AnimationBuilderDemo&) = delete;
  ~AnimationBuilderDemo() override = default;

 private:
  void PlayAnimation() {
    if (!animated_card_ || !animated_card_->layer()) {
      return;
    }

    ui::Layer* layer = animated_card_->layer();
    layer->GetAnimator()->AbortAllAnimations();
    layer->SetOpacity(0.25f);
    layer->SetTransform(gfx::Transform::MakeTranslation(-28.0f, 0.0f) *
                        gfx::Transform::MakeScale(0.92f));

    views::AnimationBuilder()
        .SetPreemptionStrategy(
            ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET)
        .Once()
        .SetDuration(base::Milliseconds(180))
        .SetOpacity(animated_card_, 1.0f, gfx::Tween::LINEAR_OUT_SLOW_IN)
        .SetTransform(animated_card_, gfx::Transform(),
                      gfx::Tween::FAST_OUT_SLOW_IN)
        .Then()
        .SetDuration(base::Milliseconds(110))
        .SetTransform(animated_card_,
                      gfx::Transform::MakeTranslation(10.0f, 0.0f),
                      gfx::Tween::EASE_OUT)
        .Then()
        .SetDuration(base::Milliseconds(120))
        .SetTransform(animated_card_, gfx::Transform(),
                      gfx::Tween::FAST_OUT_SLOW_IN);
  }

  raw_ptr<views::View> animated_card_ = nullptr;
};

class ScopedLayerAnimationSettingsDemo : public views::View {
 public:
  ScopedLayerAnimationSettingsDemo() {
    SetPreferredSize(gfx::Size(1, 132));
    SetBackground(views::CreateRoundedRectBackground(kCardBg, 12));
    SetBorder(views::CreateRoundedRectBorder(1, 12, kBorder));

    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(14, 18),
        16));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    views::View* text = AddChildView(MakeTextBlock(
        u"ui::ScopedLayerAnimationSettings",
        u"临时设置 layer animator，随后普通 SetOpacity / SetTransform / "
        u"SetRoundedCornerRadius 会变成隐式动画。"));
    layout->SetFlexForView(text, 1);

    animated_card_ = AddChildView(MakeAnimationTile(
        u"隐式\n动画", kAnimationGreen, gfx::Size(128, 72)));

    auto toggle_button = std::make_unique<views::LabelButton>(
        base::BindRepeating(
            &ScopedLayerAnimationSettingsDemo::ToggleAnimation,
            base::Unretained(this)),
        u"切换");
    toggle_button->SetAccessibleName(u"切换 ScopedLayerAnimationSettings 动画");
    StyleSmallButton(toggle_button.get());
    AddChildView(std::move(toggle_button));
  }

  ScopedLayerAnimationSettingsDemo(const ScopedLayerAnimationSettingsDemo&) =
      delete;
  ScopedLayerAnimationSettingsDemo& operator=(
      const ScopedLayerAnimationSettingsDemo&) = delete;
  ~ScopedLayerAnimationSettingsDemo() override = default;

 private:
  void ToggleAnimation() {
    if (!animated_card_ || !animated_card_->layer()) {
      return;
    }

    expanded_ = !expanded_;
    ui::ScopedLayerAnimationSettings settings(
        animated_card_->layer()->GetAnimator());
    settings.SetTransitionDuration(base::Milliseconds(260));
    settings.SetTweenType(gfx::Tween::FAST_OUT_SLOW_IN);
    settings.SetPreemptionStrategy(
        ui::LayerAnimator::IMMEDIATELY_ANIMATE_TO_NEW_TARGET);

    animated_card_->layer()->SetOpacity(expanded_ ? 0.62f : 1.0f);
    animated_card_->SetTransform(
        expanded_ ? gfx::Transform::MakeTranslation(26.0f, 0.0f)
                  : gfx::Transform());
    animated_card_->layer()->SetRoundedCornerRadius(
        gfx::RoundedCornersF(expanded_ ? 26.0f : 12.0f));
  }

  bool expanded_ = false;
  raw_ptr<views::View> animated_card_ = nullptr;
};

class BoundsAnimatorPlayground : public views::View {
 public:
  BoundsAnimatorPlayground() : bounds_animator_(this, true) {
    SetPreferredSize(gfx::Size(1, 122));
    SetBackground(views::CreateRoundedRectBackground(
        SkColorSetRGB(0xF2, 0xF4, 0xF7), 12));
    SetBorder(views::CreateRoundedRectBorder(1, 12, kBorder));

    first_tile_ = AddChildView(
        MakeAnimationTile(u"A", kAnimationGreen, gfx::Size(54, 54)));
    second_tile_ = AddChildView(
        MakeAnimationTile(u"B", kAnimationBlue, gfx::Size(54, 54)));
    third_tile_ = AddChildView(
        MakeAnimationTile(u"C", kAnimationPurple, gfx::Size(54, 54)));

    bounds_animator_.SetAnimationDuration(base::Milliseconds(280));
    bounds_animator_.set_tween_type(gfx::Tween::FAST_OUT_SLOW_IN);
  }

  BoundsAnimatorPlayground(const BoundsAnimatorPlayground&) = delete;
  BoundsAnimatorPlayground& operator=(const BoundsAnimatorPlayground&) = delete;
  ~BoundsAnimatorPlayground() override = default;

  void ToggleLayout() {
    expanded_ = !expanded_;
    AnimateToTargets();
  }

 private:
  void Layout(PassKey) override {
    if (bounds_animator_.IsAnimating()) {
      return;
    }

    SetTilesToTargets();
  }

  gfx::Rect TargetForIndex(int index) const {
    constexpr int kTileSize = 54;
    constexpr int kLeft = 22;

    if (expanded_) {
      const int gap =
          std::max(12, (width() - kLeft * 2 - kTileSize * 3) / 2);
      return gfx::Rect(kLeft + index * (kTileSize + gap), 34, kTileSize,
                       kTileSize);
    }

    return gfx::Rect(kLeft + index * 18, 24 + index * 12, kTileSize,
                     kTileSize);
  }

  void SetTilesToTargets() {
    first_tile_->SetBoundsRect(TargetForIndex(0));
    second_tile_->SetBoundsRect(TargetForIndex(1));
    third_tile_->SetBoundsRect(TargetForIndex(2));
  }

  void AnimateToTargets() {
    if (width() == 0) {
      SetTilesToTargets();
      return;
    }

    bounds_animator_.AnimateViewTo(first_tile_, TargetForIndex(0));
    bounds_animator_.AnimateViewTo(second_tile_, TargetForIndex(1));
    bounds_animator_.AnimateViewTo(third_tile_, TargetForIndex(2));
  }

  bool expanded_ = false;
  raw_ptr<views::View> first_tile_ = nullptr;
  raw_ptr<views::View> second_tile_ = nullptr;
  raw_ptr<views::View> third_tile_ = nullptr;
  views::BoundsAnimator bounds_animator_;
};

class BoundsAnimatorDemo : public views::View {
 public:
  BoundsAnimatorDemo() {
    SetPreferredSize(gfx::Size(1, 214));
    SetBackground(views::CreateRoundedRectBackground(kCardBg, 12));
    SetBorder(views::CreateRoundedRectBorder(1, 12, kBorder));

    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(14, 18),
        12));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);

    auto header = std::make_unique<views::View>();
    auto* header_layout =
        header->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), 12));
    header_layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    views::View* text = header->AddChildView(MakeTextBlock(
        u"views::BoundsAnimator",
        u"use_transforms=true 时，尺寸不变的移动用 transform 过渡，结束后再落回 "
        u"真实 bounds。"));
    header_layout->SetFlexForView(text, 1);

    auto toggle_button = std::make_unique<views::LabelButton>(
        base::BindRepeating(&BoundsAnimatorDemo::ToggleLayout,
                            base::Unretained(this)),
        u"切换布局");
    toggle_button->SetAccessibleName(u"切换 BoundsAnimator 示例布局");
    StyleSmallButton(toggle_button.get());
    header->AddChildView(std::move(toggle_button));
    AddChildView(std::move(header));

    auto playground =
        std::unique_ptr<views::View>(new BoundsAnimatorPlayground());
    playground_ = static_cast<BoundsAnimatorPlayground*>(playground.get());
    AddChildView(std::move(playground));
  }

  BoundsAnimatorDemo(const BoundsAnimatorDemo&) = delete;
  BoundsAnimatorDemo& operator=(const BoundsAnimatorDemo&) = delete;
  ~BoundsAnimatorDemo() override = default;

 private:
  void ToggleLayout() {
    if (playground_) {
      playground_->ToggleLayout();
    }
  }

  raw_ptr<BoundsAnimatorPlayground> playground_ = nullptr;
};

class WidgetFadeSampleDelegate : public views::WidgetDelegate {
 public:
  WidgetFadeSampleDelegate() {
    SetTitle(u"WidgetFadeAnimator 示例");
    SetCanResize(false);
    SetCanMaximize(false);
    SetContentsView(CreateContentsView());
  }

  WidgetFadeSampleDelegate(const WidgetFadeSampleDelegate&) = delete;
  WidgetFadeSampleDelegate& operator=(const WidgetFadeSampleDelegate&) = delete;
  ~WidgetFadeSampleDelegate() override = default;

  void StartFade(views::Widget* widget) {
    fade_animator_ = std::make_unique<views::WidgetFadeAnimator>(widget);
    fade_animator_->set_show_type(
        views::WidgetFadeAnimator::WidgetShowType::kShowInactive);
    fade_animator_->set_close_on_hide(true);
    fade_animator_->FadeIn();
  }

 private:
  std::unique_ptr<views::View> CreateContentsView() {
    auto contents = std::make_unique<views::View>();
    contents->SetPreferredSize(gfx::Size(360, 180));
    contents->SetBackground(views::CreateRoundedRectBackground(kWindowBg, 16));
    contents->SetBorder(views::CreateEmptyBorder(gfx::Insets(22)));

    auto* layout = contents->SetLayoutManager(
        std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kVertical, gfx::Insets(), 14));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);

    contents->AddChildView(MakeTextBlock(
        u"WidgetFadeAnimator",
        u"这个窗口由 WidgetFadeAnimator FadeIn 显示，点击按钮后 FadeOut，"
        u"动画结束时 close_on_hide=true 会关闭 Widget。"));

    auto close_button = std::make_unique<views::LabelButton>(
        base::BindRepeating(&WidgetFadeSampleDelegate::FadeOut,
                            base::Unretained(this)),
        u"FadeOut 并关闭");
    close_button->SetAccessibleName(u"淡出并关闭 WidgetFadeAnimator 示例窗口");
    StyleSmallButton(close_button.get());
    contents->AddChildView(std::move(close_button));
    return contents;
  }

  void FadeOut() {
    if (fade_animator_) {
      fade_animator_->FadeOut();
    }
  }

  std::unique_ptr<views::WidgetFadeAnimator> fade_animator_;
};

void ShowWidgetFadeSample(gfx::NativeView parent) {
  auto* delegate = new WidgetFadeSampleDelegate();

  views::Widget::InitParams params(
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
  params.type = views::Widget::InitParams::TYPE_WINDOW;
  params.delegate = delegate;
  params.parent = parent;
  params.name = "XenonWidgetFadeAnimationSampleWindow";
  params.shadow_type = views::Widget::InitParams::ShadowType::kDefault;
  params.bounds = GetInitialBounds(parent, 360, 180, 36);

  views::Widget* widget = new views::Widget();
  widget->Init(std::move(params));
  delegate->StartFade(widget);
}

class HighLevelAnimationDemo : public views::View {
 public:
  HighLevelAnimationDemo() {
    SetPreferredSize(gfx::Size(1, 126));
    SetBackground(views::CreateRoundedRectBackground(kCardBg, 12));
    SetBorder(views::CreateRoundedRectBorder(1, 12, kBorder));

    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets::VH(14, 18),
        12));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    views::View* text = AddChildView(MakeTextBlock(
        u"Views 高层动画入口",
        u"WidgetFadeAnimator 管 Widget 淡入淡出；InkDrop 是 Button 等控件的 "
        u"hover/press/focus 反馈；BubbleSlideAnimator 用于真实 Bubble anchor "
        u"迁移。"));
    layout->SetFlexForView(text, 1);

    auto buttons = std::make_unique<views::View>();
    auto* buttons_layout =
        buttons->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kVertical, gfx::Insets(), 8));
    buttons_layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kStretch);

    auto fade_button = std::make_unique<views::LabelButton>(
        base::BindRepeating(&HighLevelAnimationDemo::OpenWidgetFadeSample,
                            base::Unretained(this)),
        u"WidgetFade");
    fade_button->SetAccessibleName(u"打开 WidgetFadeAnimator 示例");
    StyleSmallButton(fade_button.get());
    buttons->AddChildView(std::move(fade_button));

    auto ink_button = std::make_unique<views::LabelButton>(
        base::BindRepeating([]() {}), u"InkDrop 按钮");
    ink_button->SetAccessibleName(u"InkDrop hover press focus 示例按钮");
    StyleSmallButton(ink_button.get());
    buttons->AddChildView(std::move(ink_button));

    AddChildView(std::move(buttons));
  }

  HighLevelAnimationDemo(const HighLevelAnimationDemo&) = delete;
  HighLevelAnimationDemo& operator=(const HighLevelAnimationDemo&) = delete;
  ~HighLevelAnimationDemo() override = default;

 private:
  void OpenWidgetFadeSample() {
    ShowWidgetFadeSample(GetWidget() ? GetWidget()->GetNativeView() : nullptr);
  }
};

class ViewBorderTestContentsView : public CloseableContentsView {
 public:
  ViewBorderTestContentsView()
      : CloseableContentsView(u"View 边框测试",
                              u"这个窗口展示常见 views::Border 类型，重点观察 "
                              u"Paint、Insets 与内容布局的关系。",
                              gfx::Size(kBorderTestWidth, kBorderTestHeight)) {
    AddChildView(MakeSectionLabel(u"基础 Border"));
    AddChildView(MakeBorderDemoPanel(
        u"CreateEmptyBorder",
        u"只提供 insets，不绘制任何像素；常用于 padding 或给自绘 shadow "
        u"预留透明区域。",
        views::CreateEmptyBorder(gfx::Insets::TLBR(10, 18, 10, 18))));
    AddChildView(MakeBorderDemoPanel(
        u"CreateSolidBorder",
        u"四边同厚度实线边框；GetInsets 返回统一 thickness。",
        views::CreateSolidBorder(2, SkColorSetARGB(0xA0, 0x2D, 0x32, 0x3F))));
    AddChildView(MakeBorderDemoPanel(
        u"CreateSolidSidedBorder",
        u"四边可设置不同厚度；适合只画某几边或模拟非对称内容内边距。",
        views::CreateSolidSidedBorder(gfx::Insets::TLBR(1, 6, 3, 12),
                                      SkColorSetARGB(0x90, 0x0F, 0x52, 0x2E))));
    AddChildView(MakeBorderDemoPanel(
        u"CreateRoundedRectBorder",
        u"圆角描边；corner radius 是外边缘半径，搭配 rounded background "
        u"时要注意 border thickness。",
        views::CreateRoundedRectBorder(
            2, 14, SkColorSetARGB(0xB0, 0x2D, 0x32, 0x3F))));
    AddChildView(MakeBorderDemoPanel(
        u"CreatePaddedBorder",
        u"内部 border 的画法不变，但额外增加未绘制 padding，把内容继续向内推。",
        views::CreatePaddedBorder(
            views::CreateRoundedRectBorder(
                1, 12, SkColorSetARGB(0x90, 0x2D, 0x32, 0x3F)),
            gfx::Insets::VH(8, 14))));

    AddChildView(MakeSectionLabel(u"特殊 Border"));
    AddChildView(MakeBubbleBorderReferenceDemo());
    AddChildView(MakeFocusableBorderDemo());
    AddChildView(MakePainterBorderDemo());
  }

  ViewBorderTestContentsView(const ViewBorderTestContentsView&) = delete;
  ViewBorderTestContentsView& operator=(const ViewBorderTestContentsView&) =
      delete;
  ~ViewBorderTestContentsView() override = default;
};

class ViewShadowTestContentsView : public CloseableContentsView {
 public:
  ViewShadowTestContentsView()
      : CloseableContentsView(
            u"View 阴影测试",
            u"这个窗口自身使用 Widget::ShadowType::kNone，便于观察内部 View "
            u"阴影。",
            gfx::Size(kViewTestWidth, kViewTestHeight)) {
    AddChildView(MakeSectionLabel(u"View 阴影"));
    AddChildView(
        std::unique_ptr<views::View>(new ManualCompositorShadowDemo()));
    AddChildView(std::unique_ptr<views::View>(new ViewShadowDemo()));
    AddChildView(MakeBubbleBorderDemo());
  }

  ViewShadowTestContentsView(const ViewShadowTestContentsView&) = delete;
  ViewShadowTestContentsView& operator=(const ViewShadowTestContentsView&) =
      delete;
  ~ViewShadowTestContentsView() override = default;
};

class ViewAnimationTestContentsView : public CloseableContentsView {
 public:
  ViewAnimationTestContentsView()
      : CloseableContentsView(
            u"View 动画测试",
            u"这个窗口展示 Chromium Views 常用动画入口，重点对比 layer "
            u"动画和 bounds/layout 动画的行为差异。",
            gfx::Size(kAnimationTestWidth, kAnimationTestHeight)) {
    AddChildView(MakeSectionLabel(u"gfx::Animation"));
    AddChildView(std::unique_ptr<views::View>(new GfxAnimationFamilyDemo()));

    AddChildView(MakeSectionLabel(u"Layer 动画"));
    AddChildView(std::unique_ptr<views::View>(new AnimationBuilderDemo()));
    AddChildView(
        std::unique_ptr<views::View>(new ScopedLayerAnimationSettingsDemo()));

    AddChildView(MakeSectionLabel(u"Bounds / Layout 动画"));
    AddChildView(std::unique_ptr<views::View>(new BoundsAnimatorDemo()));

    AddChildView(MakeSectionLabel(u"Views 高层封装"));
    AddChildView(std::unique_ptr<views::View>(new HighLevelAnimationDemo()));
  }

  ViewAnimationTestContentsView(const ViewAnimationTestContentsView&) = delete;
  ViewAnimationTestContentsView& operator=(
      const ViewAnimationTestContentsView&) = delete;
  ~ViewAnimationTestContentsView() override = default;
};

void ShowTestWindow(const std::u16string& title,
                    std::unique_ptr<views::View> contents,
                    gfx::NativeView parent,
                    views::Widget::InitParams::ShadowType shadow_type,
                    int width,
                    int height) {
  auto* delegate = new views::WidgetDelegate();
  delegate->SetTitle(title);
  delegate->SetCanResize(true);
  delegate->SetCanMaximize(true);
  delegate->SetContentsView(std::move(contents));

  views::Widget::InitParams params(
      views::Widget::InitParams::NATIVE_WIDGET_OWNS_WIDGET);
  params.type = views::Widget::InitParams::TYPE_WINDOW;
  params.delegate = delegate;
  params.parent = parent;
  params.name = "XenonShadowTestWindow";
  params.shadow_type = shadow_type;
  params.bounds = GetInitialBounds(parent, width, height);

  views::Widget* widget = new views::Widget();
  widget->Init(std::move(params));
  widget->Show();
}

}  // namespace

void XenonShadowTestWindow::ShowWidgetShadowTestWindow(
    gfx::NativeView parent_view) {
  ShowTestWindow(u"Xenon Widget 阴影测试",
                 std::unique_ptr<views::View>(
                     new WidgetShadowTestContentsView(parent_view)),
                 parent_view, views::Widget::InitParams::ShadowType::kDefault,
                 kWidgetTestWidth, kWidgetTestHeight);
}

void XenonShadowTestWindow::ShowWidgetShadowSample(
    gfx::NativeView parent_view,
    const std::string& shadow_type_str,
    bool borderless,
    bool show_backdrop) {
  views::Widget::InitParams::ShadowType shadow_type =
      views::Widget::InitParams::ShadowType::kDefault;
  std::u16string title;
  std::u16string body;
  int offset = 0;

  if (shadow_type_str == "kDefault") {
    shadow_type = views::Widget::InitParams::ShadowType::kDefault;
    title = u"kDefault";
    body = u"使用当前 Widget 类型 and 平台 native widget 的默认阴影策略。";
    offset = -48;
  } else if (shadow_type_str == "kNone") {
    shadow_type = views::Widget::InitParams::ShadowType::kNone;
    title = u"kNone";
    body = u"请求不绘制 Widget/native window 阴影，用来验证无阴影窗口。";
    offset = 0;
  } else if (shadow_type_str == "kDrop") {
    shadow_type = views::Widget::InitParams::ShadowType::kDrop;
    title = u"kDrop";
    body = u"请求绘制强调 Z-order 的 drop shadow。";
    offset = 48;
  } else {
    return;
  }

  ::xenon::ShowWidgetShadowSample(parent_view, shadow_type, title, body, offset,
                                  borderless, show_backdrop);
}

void XenonShadowTestWindow::ShowViewShadowTestWindow(
    gfx::NativeView parent_view) {
  ShowTestWindow(u"Xenon View 阴影测试",
                 std::unique_ptr<views::View>(new ViewShadowTestContentsView()),
                 parent_view, views::Widget::InitParams::ShadowType::kNone,
                 kViewTestWidth, kViewTestHeight);
}

void XenonShadowTestWindow::ShowViewBorderTestWindow(
    gfx::NativeView parent_view) {
  ShowTestWindow(u"Xenon View 边框测试",
                 std::unique_ptr<views::View>(new ViewBorderTestContentsView()),
                 parent_view, views::Widget::InitParams::ShadowType::kDefault,
                 kBorderTestWidth, kBorderTestHeight);
}

void XenonShadowTestWindow::ShowViewAnimationTestWindow(
    gfx::NativeView parent_view) {
  ShowTestWindow(
      u"Xenon View 动画测试",
      std::unique_ptr<views::View>(new ViewAnimationTestContentsView()),
      parent_view, views::Widget::InitParams::ShadowType::kDefault,
      kAnimationTestWidth, kAnimationTestHeight);
}

}  // namespace xenon
