// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/xenon/chrome/browser/ui/views/xenon_toast.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/no_destructor.h"
#include "base/scoped_observation.h"
#include "base/timer/timer.h"
#include "chrome/browser/ui/views/frame/browser_view.h"
#include "third_party/skia/include/core/SkPathBuilder.h"
#include "ui/base/cursor/cursor.h"
#include "ui/base/metadata/metadata_header_macros.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "ui/compositor/layer.h"
#include "ui/events/event.h"
#include "ui/gfx/canvas.h"
#include "ui/gfx/color_palette.h"
#include "ui/gfx/font.h"
#include "ui/gfx/font_list.h"
#include "ui/gfx/geometry/rect_f.h"
#include "ui/gfx/geometry/size.h"
#include "ui/gfx/geometry/transform.h"
#include "ui/gfx/shadow_value.h"
#include "ui/gfx/skia_paint_util.h"
#include "ui/gfx/skia_util.h"
#include "ui/gfx/text_elider.h"
#include "ui/views/animation/animation_builder.h"
#include "ui/views/border.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/styled_label.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"
#include "ui/views/widget/root_view.h"
#include "ui/views/widget/widget.h"
#include "ui/views/widget/widget_observer.h"

namespace xunlei {

XenonToast::Params::Params() = default;
XenonToast::Params::~Params() = default;
XenonToast::Params::Params(Params&&) = default;
XenonToast::Params& XenonToast::Params::operator=(Params&&) = default;

namespace {

constexpr char kDdinFontFamily[] = "D-DIN";
constexpr char kShanHaiFontFamily[] = "shanhaitongmengleyuan55w";

class XenonToastView;

constexpr int kToastHeight = 33;
constexpr float kToastRadius = kToastHeight / 2.0f;
constexpr int kIconSize = 16;
constexpr int kTopOffset = 130;
constexpr int kPaddingTop = 8;
constexpr int kPaddingRight = 10;
constexpr int kPaddingBottom = 8;
constexpr int kPaddingLeft = 10;
constexpr int kSpacing = 12;
constexpr int kActionLeadingInset = 0;
constexpr int kContentLineHeight = 17;
constexpr int kMinToastWidth = 94;
constexpr int kMinTextWidth = 80;
constexpr int kMaxToastWidth = 540;
constexpr int kShadowMargin = 60;
constexpr int kAnimationOffset = 10;
constexpr auto kFadeDuration = base::Milliseconds(150);
constexpr auto kDefaultDuration = base::Milliseconds(3000);
constexpr auto kActionDuration = base::Milliseconds(4500);
constexpr auto kLoadingMinDuration = base::Milliseconds(400);
constexpr auto kLoadingRotationDuration = base::Milliseconds(400);

constexpr SkColor kBackgroundColor = SK_ColorWHITE;
constexpr SkColor kTextColor = SkColorSetRGB(0x1D, 0x21, 0x29);
constexpr SkColor kActionColor = SkColorSetRGB(0x4E, 0x5C, 0xFF);
constexpr SkColor kActionHoverColor = SkColorSetRGB(0x2F, 0x45, 0xFF);
constexpr SkColor kSuccessColor = SkColorSetRGB(0x00, 0xB4, 0x2A);
constexpr SkColor kInfoColor = SkColorSetRGB(0x4E, 0x5C, 0xFF);
constexpr SkColor kWarningColor = SkColorSetRGB(0xFF, 0x9A, 0x2E);
constexpr SkColor kErrorColor = SkColorSetRGB(0xF5, 0x3F, 0x3F);

std::map<gfx::NativeWindow, XenonToastView*>& ActiveToasts() {
  static base::NoDestructor<std::map<gfx::NativeWindow, XenonToastView*>> toasts;
  return *toasts;
}

base::TimeDelta DefaultDurationForParams(const XenonToast::Params& params) {
  if (params.duration.is_positive()) {
    return params.duration;
  }
  if (params.type == XenonToast::Type::kLoading) {
    return base::TimeDelta();
  }
  return params.action_text.empty() ? kDefaultDuration : kActionDuration;
}

SkColor IconColorForType(XenonToast::Type type) {
  switch (type) {
    case XenonToast::Type::kSuccess:
      return kSuccessColor;
    case XenonToast::Type::kError:
      return kErrorColor;
    case XenonToast::Type::kWarning:
      return kWarningColor;
    case XenonToast::Type::kLoading:
    case XenonToast::Type::kInfo:
      return kInfoColor;
  }
}

gfx::ShadowValues ToastShadowValues() {
  return {
      gfx::ShadowValue(gfx::Vector2d(0, 6), 60,
                       SkColorSetA(SkColorSetRGB(0x1F, 0x2E, 0x78), 0x29)),
      gfx::ShadowValue(gfx::Vector2d(0, 10), 80,
                       SkColorSetA(SkColorSetRGB(0x1F, 0x2E, 0x78), 0x14)),
  };
}

gfx::FontList ToastTextFontList(gfx::Font::Weight weight) {
  return views::Label::GetDefaultFontList().Derive(3, gfx::Font::NORMAL,
                                                   weight);
}

gfx::FontList ToastNumberFontList(gfx::Font::Weight weight) {
  const int font_size = ToastTextFontList(weight).GetFontSize();
  return gfx::FontList({kDdinFontFamily}, gfx::Font::NORMAL, font_size, weight);
}

gfx::FontList ToastActionFontList(gfx::Font::Weight weight) {
  const int font_size = ToastTextFontList(weight).GetFontSize();
  return gfx::FontList({kShanHaiFontFamily}, gfx::Font::NORMAL, font_size, weight);
}


class ToastIconView : public views::View {
  METADATA_HEADER(ToastIconView, views::View)

 public:
  ToastIconView() { SetPreferredSize(gfx::Size(kIconSize, kIconSize)); }

  void SetType(XenonToast::Type type) {
    type_ = type;
    SchedulePaint();
  }

  void OnPaint(gfx::Canvas* canvas) override {
    views::View::OnPaint(canvas);

    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(IconColorForType(type_));
    flags.setStyle(cc::PaintFlags::kFill_Style);

    const gfx::Rect bounds = GetContentsBounds();
    const float radius = std::min(bounds.width(), bounds.height()) / 2.0f;
    const gfx::PointF center(bounds.CenterPoint());
    canvas->DrawCircle(center, radius, flags);

    flags.setColor(SK_ColorWHITE);
    flags.setStrokeWidth(2.0f);
    flags.setStrokeCap(cc::PaintFlags::kRound_Cap);
    flags.setStrokeJoin(cc::PaintFlags::kRound_Join);
    flags.setStyle(cc::PaintFlags::kStroke_Style);

    SkPathBuilder path_builder;
    const float x = bounds.x();
    const float y = bounds.y();
    const float w = bounds.width();
    const float h = bounds.height();
    switch (type_) {
      case XenonToast::Type::kSuccess:
        path_builder.moveTo(x + w * 0.28f, y + h * 0.52f);
        path_builder.lineTo(x + w * 0.44f, y + h * 0.68f);
        path_builder.lineTo(x + w * 0.74f, y + h * 0.34f);
        canvas->DrawPath(path_builder.detach(), flags);
        break;
      case XenonToast::Type::kError:
        path_builder.moveTo(x + w * 0.36f, y + h * 0.36f);
        path_builder.lineTo(x + w * 0.64f, y + h * 0.64f);
        path_builder.moveTo(x + w * 0.64f, y + h * 0.36f);
        path_builder.lineTo(x + w * 0.36f, y + h * 0.64f);
        canvas->DrawPath(path_builder.detach(), flags);
        break;
      case XenonToast::Type::kWarning:
      case XenonToast::Type::kInfo:
        path_builder.moveTo(x + w * 0.5f, y + h * 0.28f);
        path_builder.lineTo(x + w * 0.5f, y + h * 0.56f);
        canvas->DrawPath(path_builder.detach(), flags);
        flags.setStyle(cc::PaintFlags::kFill_Style);
        canvas->DrawCircle(gfx::PointF(x + w * 0.5f, y + h * 0.72f), 1.2f,
                           flags);
        break;
      case XenonToast::Type::kLoading:
        break;
    }
  }

 private:
  XenonToast::Type type_ = XenonToast::Type::kInfo;
};

BEGIN_METADATA(ToastIconView)
END_METADATA

class ToastSpinnerView : public views::View {
  METADATA_HEADER(ToastSpinnerView, views::View)

 public:
  ToastSpinnerView() { SetPreferredSize(gfx::Size(kIconSize, kIconSize)); }

  void Start() {
    if (timer_.IsRunning()) {
      return;
    }

    start_time_ = base::TimeTicks::Now();
    timer_.Start(FROM_HERE, base::Milliseconds(16),
                 base::BindRepeating(&ToastSpinnerView::SchedulePaint,
                                     weak_ptr_factory_.GetWeakPtr()));
    SchedulePaint();
  }

  void Stop() { timer_.Stop(); }

  void OnPaint(gfx::Canvas* canvas) override {
    views::View::OnPaint(canvas);

    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(kActionColor);
    flags.setStrokeWidth(2.0f);
    flags.setStrokeCap(cc::PaintFlags::kRound_Cap);
    flags.setStyle(cc::PaintFlags::kStroke_Style);

    gfx::RectF oval(GetContentsBounds());
    oval.Inset(2.0f);

    const base::TimeDelta elapsed = base::TimeTicks::Now() - start_time_;
    const double progress = std::fmod(
        elapsed.InMillisecondsF() / kLoadingRotationDuration.InMillisecondsF(),
        1.0);
    SkPathBuilder path_builder;
    path_builder.addArc(gfx::RectFToSkRect(oval),
                        static_cast<float>(progress * 360.0 - 90.0), 270.0f);
    canvas->DrawPath(path_builder.detach(), flags);
  }

 private:
  base::RepeatingTimer timer_;
  base::TimeTicks start_time_;
  base::WeakPtrFactory<ToastSpinnerView> weak_ptr_factory_{this};
};

BEGIN_METADATA(ToastSpinnerView)
END_METADATA

class ToastActionView : public views::Label {
  METADATA_HEADER(ToastActionView, views::Label)

 public:
  ToastActionView() {
    SetAutoColorReadabilityEnabled(false);
    SetEnabledColor(kActionColor);
    SetFontList(ToastActionFontList(gfx::Font::Weight::NORMAL));
    SetLineHeight(kContentLineHeight);
    SetHorizontalAlignment(gfx::ALIGN_LEFT);
  }

  void SetCallback(base::RepeatingClosure callback) {
    callback_ = std::move(callback);
  }

  ui::Cursor GetCursor(const ui::MouseEvent& event) override {
    return ui::mojom::CursorType::kHand;
  }

  bool GetCanProcessEventsWithinSubtree() const override { return true; }

  void OnMouseEntered(const ui::MouseEvent& event) override {
    SetEnabledColor(kActionHoverColor);
  }

  void OnMouseExited(const ui::MouseEvent& event) override {
    SetEnabledColor(kActionColor);
  }

  bool OnMousePressed(const ui::MouseEvent& event) override {
    is_pressed_ = event.IsLeftMouseButton() || event.IsMiddleMouseButton();
    return is_pressed_;
  }

  void OnMouseReleased(const ui::MouseEvent& event) override {
    const bool should_activate = is_pressed_ && HitTestPoint(event.location());
    is_pressed_ = false;
    if (should_activate && callback_) {
      callback_.Run();
    }
  }

  void OnMouseCaptureLost() override { is_pressed_ = false; }

 private:
  bool is_pressed_ = false;
  base::RepeatingClosure callback_;
};

BEGIN_METADATA(ToastActionView)
END_METADATA

class XenonToastView : public views::View, public views::WidgetObserver {
  METADATA_HEADER(XenonToastView, views::View)

 public:
  XenonToastView(gfx::NativeWindow parent_window, XenonToast::Params params)
      : parent_window_(parent_window) {
    SetPaintToLayer();
    layer()->SetFillsBoundsOpaquely(false);
    layer()->SetMasksToBounds(false);
    SetBorder(views::CreateEmptyBorder(gfx::Insets::TLBR(
        kShadowMargin + kPaddingTop, kShadowMargin + kPaddingLeft,
        kShadowMargin + kPaddingBottom, kShadowMargin + kPaddingRight)));

    auto* layout = SetLayoutManager(std::make_unique<views::BoxLayout>(
        views::BoxLayout::Orientation::kHorizontal, gfx::Insets(), kSpacing));
    layout->set_cross_axis_alignment(
        views::BoxLayout::CrossAxisAlignment::kCenter);

    icon_view_ = AddChildView(std::make_unique<ToastIconView>());
    spinner_view_ = AddChildView(std::make_unique<ToastSpinnerView>());

    text_label_ = AddChildView(std::make_unique<views::Label>());
    text_label_->SetAutoColorReadabilityEnabled(false);
    text_label_->SetEnabledColor(kTextColor);
    text_label_->SetFontList(ToastNumberFontList(gfx::Font::Weight::NORMAL));
    text_label_->SetLineHeight(kContentLineHeight);
    text_label_->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    text_label_->SetElideBehavior(gfx::ELIDE_TAIL);
    text_label_->SetSkipSubpixelRenderingOpacityCheck(true);

    action_view_ = AddChildView(std::make_unique<ToastActionView>());
    action_view_->SetSkipSubpixelRenderingOpacityCheck(true);
    action_view_->SetBorder(views::CreateEmptyBorder(
        gfx::Insets::TLBR(0, kActionLeadingInset, 0, 0)));
    action_view_->SetCallback(base::BindRepeating(
        &XenonToastView::OnAction, weak_ptr_factory_.GetWeakPtr()));

    UpdateContent(std::move(params));
  }

  XenonToastView(const XenonToastView&) = delete;
  XenonToastView& operator=(const XenonToastView&) = delete;
  ~XenonToastView() override = default;

  bool is_closing() const { return is_closing_; }

  void UpdateToast(XenonToast::Params params) {
    if (last_type_ == XenonToast::Type::kLoading &&
        params.type != XenonToast::Type::kLoading &&
        base::TimeTicks::Now() - last_loading_start_ < kLoadingMinDuration) {
      pending_params_ = std::move(params);
      const base::TimeDelta delay =
          kLoadingMinDuration - (base::TimeTicks::Now() - last_loading_start_);
      loading_transition_timer_.Start(
          FROM_HERE, delay,
          base::BindOnce(&XenonToastView::ApplyPendingParams,
                         weak_ptr_factory_.GetWeakPtr()));
      return;
    }

    UpdateContent(std::move(params));
  }

  void ShowAnimated() {
    layer()->SetOpacity(0.0f);
    layer()->SetTransform(
        gfx::Transform::MakeTranslation(0, -kAnimationOffset));
    views::AnimationBuilder()
        .Once()
        .SetDuration(kFadeDuration)
        .SetOpacity(this, 1.0f)
        .SetTransform(this, gfx::Transform());
  }

  void StartClose() {
    if (is_closing_) {
      return;
    }
    is_closing_ = true;
    close_timer_.Stop();
    loading_transition_timer_.Stop();

    views::AnimationBuilder()
        .OnEnded(base::BindOnce(&XenonToastView::RemoveSelf,
                                weak_ptr_factory_.GetWeakPtr()))
        .Once()
        .SetDuration(kFadeDuration)
        .SetOpacity(this, 0.0f)
        .SetTransform(this,
                      gfx::Transform::MakeTranslation(0, -kAnimationOffset));
  }

  void UpdateBounds() {
    views::View* host = GetHostView();
    if (!host) {
      return;
    }

    const gfx::Rect anchor_bounds = host->GetLocalBounds();

    const gfx::Size size = GetPreferredSize();
    const int x =
        anchor_bounds.x() + (anchor_bounds.width() - size.width()) / 2;
    const int y = anchor_bounds.y() + kTopOffset - kShadowMargin;
    SetBoundsRect(gfx::Rect(x, y, size.width(), size.height()));
  }

  gfx::Size CalculatePreferredSize(
      const views::SizeBounds& available_size) const override {
    gfx::Size size = views::View::CalculatePreferredSize(available_size);
    size.set_height(kToastHeight + kShadowMargin * 2);
    size.set_width(std::clamp(size.width(), kMinToastWidth + kShadowMargin * 2,
                              kMaxToastWidth + kShadowMargin * 2));
    return size;
  }

  void OnPaint(gfx::Canvas* canvas) override {
    cc::PaintFlags flags;
    flags.setAntiAlias(true);
    flags.setColor(kBackgroundColor);
    flags.setStyle(cc::PaintFlags::kFill_Style);
    flags.setLooper(gfx::CreateShadowDrawLooper(ToastShadowValues()));

    gfx::RectF pill_bounds(GetLocalBounds());
    pill_bounds.Inset(kShadowMargin);
    canvas->DrawRoundRect(pill_bounds, kToastRadius, flags);
  }

  void AddedToWidget() override {
    if (views::Widget* widget = GetWidget()) {
      widget_observation_.Observe(widget);
    }
  }

  void RemovedFromWidget() override { widget_observation_.Reset(); }

  void OnWidgetBoundsChanged(views::Widget* widget,
                             const gfx::Rect& new_bounds) override {
    UpdateBounds();
  }

  void OnWidgetDestroying(views::Widget* widget) override {
    EraseActiveToast();
    close_timer_.Stop();
    loading_transition_timer_.Stop();
    widget_observation_.Reset();
  }

 private:
  void UpdateContent(XenonToast::Params params) {
    loading_transition_timer_.Stop();
    params_ = std::move(params);
    last_type_ = params_.type;

    if (params_.type == XenonToast::Type::kLoading) {
      last_loading_start_ = base::TimeTicks::Now();
    }

    icon_view_->SetType(params_.type);
    const bool is_loading = params_.type == XenonToast::Type::kLoading;
    icon_view_->SetVisible(!is_loading);
    spinner_view_->SetVisible(is_loading);
    if (is_loading) {
      spinner_view_->Start();
    } else {
      spinner_view_->Stop();
    }

    text_label_->SetText(params_.text);
    action_view_->SetText(params_.action_text);
    action_view_->SetVisible(!params_.action_text.empty());
    UpdateTextMaximumWidth();

    InvalidateLayout();
    PreferredSizeChanged();
    UpdateBounds();
    SchedulePaint();

    close_timer_.Stop();
    const base::TimeDelta duration = DefaultDurationForParams(params_);
    if (duration.is_positive()) {
      close_timer_.Start(FROM_HERE, duration,
                         base::BindOnce(&XenonToastView::StartClose,
                                        weak_ptr_factory_.GetWeakPtr()));
    }
  }

  void ApplyPendingParams() {
    if (pending_params_) {
      UpdateContent(std::move(*pending_params_));
      pending_params_.reset();
    }
  }

  void OnAction() {
    if (params_.action_callback) {
      std::move(params_.action_callback).Run();
    }
    StartClose();
  }

  void UpdateTextMaximumWidth() {
    int reserved_width = kPaddingLeft + kPaddingRight + kIconSize + kSpacing;
    if (action_view_->GetVisible()) {
      reserved_width += kSpacing + action_view_->GetPreferredSize().width();
    }
    text_label_->SetMaximumWidthSingleLine(
        std::max(kMinTextWidth, kMaxToastWidth - reserved_width));
  }

  views::View* GetHostView() {
    return parent() ? parent() : static_cast<views::View*>(nullptr);
  }

  void EraseActiveToast() {
    auto& toasts = ActiveToasts();
    const auto it = toasts.find(parent_window_);
    if (it != toasts.end() && it->second == this) {
      toasts.erase(it);
    }
  }

  void RemoveSelf() {
    EraseActiveToast();
    if (!parent()) {
      return;
    }
    parent()->RemoveChildViewT(this);
  }

  gfx::NativeWindow parent_window_ = nullptr;
  raw_ptr<ToastIconView> icon_view_ = nullptr;
  raw_ptr<ToastSpinnerView> spinner_view_ = nullptr;
  raw_ptr<views::Label> text_label_ = nullptr;
  raw_ptr<ToastActionView> action_view_ = nullptr;

  XenonToast::Params params_;
  std::optional<XenonToast::Params> pending_params_;
  XenonToast::Type last_type_ = XenonToast::Type::kInfo;
  base::TimeTicks last_loading_start_;
  bool is_closing_ = false;

  base::OneShotTimer close_timer_;
  base::OneShotTimer loading_transition_timer_;
  base::ScopedObservation<views::Widget, views::WidgetObserver>
      widget_observation_{this};
  base::WeakPtrFactory<XenonToastView> weak_ptr_factory_{this};
};

BEGIN_METADATA(XenonToastView)
END_METADATA

}  // namespace

views::Widget* XenonToast::Show(gfx::NativeWindow parent, Params params) {
  if (!parent) {
    return nullptr;
  }

  BrowserView* browser_view =
      BrowserView::GetBrowserViewForNativeWindow(parent);
  views::Widget* parent_widget =
      browser_view ? browser_view->GetWidget()
                   : views::Widget::GetWidgetForNativeWindow(parent);
  if (!parent_widget) {
    return nullptr;
  }

  views::View* host_view =
      browser_view ? static_cast<views::View*>(browser_view)
                   : static_cast<views::View*>(parent_widget->GetRootView());
  if (!host_view) {
    return parent_widget;
  }

  auto& toasts = ActiveToasts();
  auto it = toasts.find(parent);
  if (it != toasts.end()) {
    if (it->second && it->second->parent() && !it->second->is_closing()) {
      it->second->UpdateToast(std::move(params));
      return parent_widget;
    }
    toasts.erase(it);
  }

  auto toast = std::make_unique<XenonToastView>(parent, std::move(params));
  XenonToastView* toast_view = host_view->AddChildView(std::move(toast));
  toasts[parent] = toast_view;
  toast_view->UpdateBounds();
  toast_view->ShowAnimated();
  return parent_widget;
}

}  // namespace xunlei
