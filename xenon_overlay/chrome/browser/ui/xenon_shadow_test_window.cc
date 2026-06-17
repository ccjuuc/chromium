// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/xenon_shadow_test_window.h"

#include <memory>
#include <string>
#include <utility>

#include "xenon_overlay/chrome/browser/ui/xenon_frameless_shadow_view.h"

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/compositor/layer.h"
#include "ui/compositor_extra/shadow.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/gfx/font.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/rounded_corners_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/views/background.h"
#include "ui/views/border.h"
#include "ui/views/bubble/bubble_border.h"
#include "ui/views/controls/button/label_button.h"
#include "ui/views/controls/label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/view_shadow.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_delegate.h"

namespace xenon {
namespace {

constexpr int kWidgetTestWidth = 760;
constexpr int kWidgetTestHeight = 430;
constexpr int kViewTestWidth = 820;
constexpr int kViewTestHeight = 560;
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
  label->SetFontList(label->font_list().Derive(
      2, gfx::Font::NORMAL, gfx::Font::Weight::BOLD));
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

  display::Display display =
      parent ? screen->GetDisplayNearestView(parent)
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
    SetBackground(rounded_window
                      ? views::CreateRoundedRectBackground(kWindowBg,
                                                           kCardRadius)
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
  delegate->SetContentsView(
      std::unique_ptr<views::View>(new ShadowBackdropContentsView(
          backdrop_bounds.size())));

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
          ? u"这个窗口使用 TYPE_WINDOW_FRAMELESS，并设置 remove_standard_frame=true "
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

void ShowWidgetShadowSample(
    gfx::NativeView parent,
    views::Widget::InitParams::ShadowType shadow_type,
    const std::u16string& title,
    const std::u16string& body,
    int offset,
    bool borderless,
    bool show_backdrop) {
  gfx::Rect sample_bounds = GetInitialBounds(parent, kSampleWindowWidth,
                                             kSampleWindowHeight, offset);
  views::Widget* backdrop = nullptr;
  if (show_backdrop) {
    backdrop = ShowShadowBackdrop(parent, sample_bounds);
  }

  auto* delegate = new WidgetShadowSampleDelegate(
      backdrop ? backdrop->GetWeakPtr() : base::WeakPtr<views::Widget>());
  delegate->SetTitle(title);
  delegate->SetCanResize(!borderless);
  delegate->SetCanMaximize(false);
  delegate->SetContentsView(CreateWidgetShadowSampleContents(
      title, body, borderless, shadow_type));

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
            u"覆盖 views::Widget::InitParams::ShadowType 的全部选项。每行都可以打开标准窗口或无边框窗口。",
            gfx::Size(kWidgetTestWidth, kWidgetTestHeight)),
        parent_(parent) {
    AddChildView(MakeSectionLabel(u"views::Widget::InitParams::ShadowType"));
    AddShadowTypeRow(views::Widget::InitParams::ShadowType::kDefault,
                     u"kDefault",
                     u"使用当前 Widget 类型和平台 native widget 的默认阴影策略。",
                     -48);
    AddShadowTypeRow(views::Widget::InitParams::ShadowType::kNone,
                     u"kNone",
                     u"请求不绘制 Widget/native window 阴影，用来验证无阴影窗口。",
                     0);
    AddShadowTypeRow(views::Widget::InitParams::ShadowType::kDrop,
                     u"kDrop",
                     u"请求绘制强调 Z-order 的 drop shadow。",
                     48);
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

    auto* row_layout =
        row->SetLayoutManager(std::make_unique<views::BoxLayout>(
            views::BoxLayout::Orientation::kHorizontal,
            gfx::Insets::VH(12, 16), 12));
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
    panel_->AddChildView(MakeTextBlock(
        u"ui::Shadow",
        u"手动创建 compositor nine-patch shadow layer，并在 Layout 中同步 content bounds。"));
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
    panel_->AddChildView(MakeTextBlock(
        u"views::ViewShadow",
        u"给普通 View 包装 shadow。ViewShadow 会让目标 View paint-to-layer，并跟随 bounds。"));

    view_shadow_ = std::make_unique<views::ViewShadow>(panel_, kShadowElevation);
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
  panel->SetBackground(views::CreateRoundedRectBackground(kCardBg,
                                                          kCardRadius));

  panel->SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::Orientation::kVertical, gfx::Insets::VH(18, 22), 6));
  panel->AddChildView(MakeTextBlock(
      u"views::BubbleBorder",
      u"使用 BubbleBorder 的 STANDARD_SHADOW 和自定义 elevation，适合气泡/浮层类 UI。"));
  return panel;
}

class ViewShadowTestContentsView : public CloseableContentsView {
 public:
  ViewShadowTestContentsView()
      : CloseableContentsView(
            u"View 阴影测试",
            u"这个窗口自身使用 Widget::ShadowType::kNone，便于观察内部 View 阴影。",
            gfx::Size(kViewTestWidth, kViewTestHeight)) {
    AddChildView(MakeSectionLabel(u"View 阴影"));
    AddChildView(std::unique_ptr<views::View>(
        new ManualCompositorShadowDemo()));
    AddChildView(std::unique_ptr<views::View>(new ViewShadowDemo()));
    AddChildView(MakeBubbleBorderDemo());
  }

  ViewShadowTestContentsView(const ViewShadowTestContentsView&) = delete;
  ViewShadowTestContentsView& operator=(const ViewShadowTestContentsView&) =
      delete;
  ~ViewShadowTestContentsView() override = default;
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
  ShowTestWindow(
      u"Xenon Widget 阴影测试",
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
  ShowTestWindow(
      u"Xenon View 阴影测试",
      std::unique_ptr<views::View>(new ViewShadowTestContentsView()),
      parent_view, views::Widget::InitParams::ShadowType::kNone,
      kViewTestWidth, kViewTestHeight);
}

}  // namespace xenon
