# Chromium Views Border 机制整理

本文基于当前 Chromium 142 源码，整理 Views UI 体系里的 border 相关机制，并结合 Xenon 最近的 menu / bubble / shadow 改动说明如何正确使用和扩展。

核心源码：

| 文件 | 作用 |
|---|---|
| `ui/views/border.{h,cc}` | `views::Border` 基类与基础工厂方法 |
| `ui/views/view.{h,cc}` | `View::SetBorder()`、`GetInsets()`、paint 顺序 |
| `ui/views/background.{h,cc}` | background 与 border 的绘制配合 |
| `ui/views/bubble/bubble_border.{h,cc}` | Bubble 的 border、shadow、arrow、bounds 计算 |
| `ui/views/bubble/bubble_frame_view.{h,cc}` | Bubble window non-client frame，持有 `BubbleBorder` |
| `ui/views/controls/menu/menu_scroll_view_container.{h,cc}` | Menu 使用默认 border 或 bubble border 的入口 |
| `ui/views/controls/menu/menu_host.cc` | MenuHost 根据 bubble border 决定 native shadow / opacity |
| `ui/views/highlight_border.{h,cc}` | ChromeOS/浮层类高亮双线 border，当前 `ui/views/BUILD.gn` 仅在 `is_chromeos` 下编译 |
| `ui/views/controls/focusable_border.{h,cc}` | Textfield 等控件焦点 border |
| `ui/views/controls/button/label_button_border.{h,cc}` | Button padding / asset border |

## 1. 总览

Chromium Views 里的 border 不是 CSS border 的简单等价物。它同时承担三个职责：

| 职责 | 说明 |
|---|---|
| 绘制 | `Border::Paint()` 在 View paint 时绘制外框、阴影、图片边框等 |
| 布局占位 | `Border::GetInsets()` 决定 `View::GetContentsBounds()` 向内收缩多少 |
| 最小尺寸 | `Border::GetMinimumSize()` 表示 border 自身绘制不被裁剪所需的最小尺寸 |

这意味着修改 border 不只是视觉变化，也会影响 layout、preferred size、widget bounds。

## 2. `views::Border` 基类

定义在 `ui/views/border.h`：

```cpp
class Border {
 public:
  virtual void Paint(const View& view, gfx::Canvas* canvas) = 0;
  virtual gfx::Insets GetInsets() const = 0;
  virtual gfx::Size GetMinimumSize() const = 0;
  virtual void OnViewThemeChanged(View* view);
  virtual void SetColor(ui::ColorVariant color);
};
```

关键点：

- Border 由 View 拥有，调用 `view->SetBorder(std::unique_ptr<Border>)` 后转移所有权。
- 不是所有 View 都天然适配 border。自定义 View 在 layout 和 paint 时要使用 `GetContentsBounds()` 或 `GetLocalBoundsExcludingBorder()` 这一类排除 border 的 bounds。
- `ui::ColorVariant` 支持语义颜色，例如 `ui::kColorMenuBorder`。语义颜色依赖 `ColorProvider`，theme 改变时要触发 repaint。
- `OnViewThemeChanged()` 是给依赖主题色的 border 使用的回调。

## 3. View 如何集成 Border

`View::GetInsets()`：

```cpp
gfx::Insets View::GetInsets() const {
  return border_ ? border_->GetInsets() : gfx::Insets();
}
```

`View::GetContentsBounds()`：

```cpp
gfx::Rect View::GetContentsBounds() const {
  gfx::Rect contents_bounds(GetLocalBounds());
  contents_bounds.Inset(GetInsets());
  return contents_bounds;
}
```

`View::SetBorder()`：

- 保存旧的 contents bounds。
- 替换 `border_`。
- 如果 View 已经在 Widget 中，调用 `border_->OnViewThemeChanged(this)`。
- 如果新旧 contents bounds 不同，调用 `InvalidateLayout()`。
- 最后 `SchedulePaint()`。

这解释了一个常见现象：单纯换 border 也可能触发 layout，因为 border insets 改变了内容区。

paint 顺序：

```cpp
void View::OnPaint(gfx::Canvas* canvas) {
  OnPaintBackground(canvas);
  OnPaintBorder(canvas);
}
```

顺序是 background 先画，border 后画。自定义 border 如果要画 shadow，需要自己考虑和 background 的关系，避免背景覆盖阴影。

## 4. 基础 Border 工厂

`ui/views/border.h` 暴露了几类工厂。

### `NullBorder()`

返回 `nullptr`，等价于没有 border。

常见用途：

```cpp
button->SetBorder(nullptr);
```

### `CreateEmptyBorder()`

只占位，不绘制。

用途：

- 做 padding。
- 给 shadow 预留透明区域。
- 在内容里额外留出菜单或 dialog 的内边距。

示例：

```cpp
SetBorder(views::CreateEmptyBorder(gfx::Insets(margin)));
```

Xenon 里的使用：

- `XenonCommonDialogView::SetupCompositorShadow()` 用 empty border 预留 shadow margin。
- `XenonWebDialogView::SetupFramelessCompositorShadow()` 用 empty border 预留 frameless compositor shadow margin。
- `XenonDialogButton` 用 empty border 实现水平 padding。

### `CreateSolidBorder()`

创建四边同厚度纯色 border。

底层实现是 `SolidSidedBorder(gfx::Insets(thickness), color)`。

绘制方式：

- 先按 insets 得到内部 clip rect。
- 对内部区域做 difference clip。
- 对剩余外圈 `DrawColor()`。

特点：

- 对不同 device scale factor 做了像素对齐处理。
- 适合简单实线边框。

### `CreateSolidSidedBorder()`

创建四边厚度不同的纯色 border。

用途：

- 只画某几边。
- 菜单在自定义背景色时，用它保证 inset 区域也是背景/边框色。

### `CreateRoundedRectBorder()`

创建圆角矩形描边。

注意：

- `corner_radius` 是 outer edge radius，不是 stroke radius。
- Paint 时会用 `thickness / 2` 内缩，实际绘制半径为 `corner_radius - half_thickness`。
- `GetInsets()` 返回 `gfx::Insets(thickness) + paint_insets`。

搭配 background 时，推荐让 background 传入同样 radius 和 border thickness：

```cpp
view->SetBackground(
    views::CreateRoundedRectBackground(color, radius, border_thickness));
view->SetBorder(
    views::CreateRoundedRectBorder(border_thickness, radius, border_color));
```

这样 background 会向内避开 border，避免背景颜色从描边下露出来。

### `CreatePaddedBorder()`

包装一个已有 border，并额外增加 unpainted padding。

语义：

- `Paint()` 仍然调用内部 border 的 paint。
- `GetInsets()` 返回内部 border insets 加 extra insets。
- `GetMinimumSize()` 也加 extra insets。

适合“外观不变，只想把内容再向内推”的场景。

### `CreateBorderPainter()`

用 `views::Painter` 做 border。

特点：

- `Paint()` 调 `Painter::PaintPainterAt()`。
- insets 由调用者传入。
- `SetColor()` 对 painter border 不适用。

菜单默认圆角外框使用了 `RoundRectPainter` + `CreateBorderPainter()`。

## 5. Background 与 Border

background 和 border 是两个不同对象，但经常需要配合。

关键注意点：

1. `CreateLayerBasedSolidBackground()` 会让 View 使用 `ui::LAYER_SOLID_COLOR`，限制是这个 View 或子树不能再普通绘制其它内容，包括 border。
2. 普通 `CreateRoundedRectBackground()` 支持传入 `for_border_thickness`，让背景内缩，避免覆盖圆角描边。
3. `BubbleBackground` 特殊依赖 `BubbleBorder`，它使用 border 的 color、insets、rounded corners 来绘制气泡内容背景。

因此在 bubble/menu 里，如果换了 BubbleBorder，通常也要同步换 BubbleBackground 或 frame border，否则背景区域和 border insets 可能不一致。

## 6. `BubbleBorder`

`views::BubbleBorder` 是最复杂也最重要的 border 类型。它不是单纯描边，而是把以下能力集中到一个 Border：

- shadow；
- stroke；
- rounded corners；
- optional arrow；
- content insets；
- anchor rect 到 widget bounds 的转换；
- visible arrow 的绘制和定位。

### Arrow

`BubbleBorder::Arrow` 包含：

- `TOP_LEFT / TOP_RIGHT / BOTTOM_LEFT / BOTTOM_RIGHT`
- `LEFT_TOP / RIGHT_TOP / LEFT_BOTTOM / RIGHT_BOTTOM`
- `TOP_CENTER / BOTTOM_CENTER / LEFT_CENTER / RIGHT_CENTER`
- `NONE`
- `FLOAT`

`NONE` 表示无箭头，放在 anchor rect 下方并水平居中。

`FLOAT` 表示无箭头，bubble 中心与 anchor rect 中心对齐。

### Shadow

`BubbleBorder::Shadow` 包含：

| Shadow | 含义 |
|---|---|
| `STANDARD_SHADOW` | 标准 MD 气泡阴影 |
| `CHROMEOS_SYSTEM_UI_SHADOW` | ChromeOS system UI shadow，仅 ChromeOS |
| `NO_SHADOW` | 不画 stroke / shadow |
| `DIALOG_SHADOW` | 平台别名，Mac 为 `NO_SHADOW`，ChromeOS 为 system UI shadow，其它平台为 standard |

`set_md_shadow_elevation()` 可以设置 MD shadow elevation。空 elevation 时使用 BubbleBorder 默认 shadow values。

### Insets

`BubbleBorder::GetInsets()` 来源：

1. 如果显式 `set_insets()`，直接返回该值。
2. 否则根据 shadow 类型和 elevation 计算 border + shadow insets。
3. 如果 visible arrow 开启，还会取 shadow insets 和 arrow insets 的 max。

`GetBorderAndShadowInsets()` 通过 `gfx::ShadowValue::GetMargin()` 计算 shadow margin，再加上是否绘制 stroke 的 1dip border thickness。

这点很关键：BubbleBorder 的 insets 包含 shadow 外扩区域，不只是 stroke 厚度。

### Bounds

`BubbleBorder::GetBounds(anchor_rect, contents_size)` 会返回整个 bubble widget bounds。

流程概念：

1. 先通过 `GetSizeForContentsSize()` 把 contents size 加上 border/shadow/arrow insets。
2. 根据 arrow 类型决定 bubble 相对 anchor 的位置。
3. 如果 `avoid_shadow_overlap_` 开启，提前把 shadow insets 纳入 anchor 避让。
4. 如果有 visible arrow，还会为箭头腾出空间并计算 `visible_arrow_rect_`。
5. `arrow_offset_` 用于 bubble 贴近屏幕边缘时微调位置。

因此 BubbleBorder 的 bounds 计算和 paint 是一套闭环。自定义 Bubble 的时候，不建议只改 paint 而不改 insets/bounds。

### Paint

普通 shadow 路径：

```cpp
SkRRect r_rect = GetClientRect(view);
canvas->sk_canvas()->clipRRect(r_rect, SkClipOp::kDifference, true);
DrawBorderAndShadowImpl(...);
```

含义：

- `GetClientRect()` 是 local bounds 减去 `GetInsets()` 后的内容圆角矩形。
- 先把内容区域 clip 掉，只在外圈画 shadow/stroke。
- visible arrow 单独绘制。

`NO_SHADOW` 路径：

- `PaintNoShadow()` 对 client rect 外部做透明清理。
- 常用于无 native shadow 或平台自己提供 shadow 的场景。

### BubbleBackground

`BubbleBackground` 使用同一个 `BubbleBorder`：

- 根据 border color 填充背景。
- bounds 内缩 `border_->GetInsets()`。
- rounded corners 使用 `border_->rounded_corners()`。

这也是为什么很多代码会这样写：

```cpp
auto border = std::make_unique<views::BubbleBorder>(arrow, shadow);
view->SetBackground(std::make_unique<views::BubbleBackground>(border.get()));
view->SetBorder(std::move(border));
```

注意：`BubbleBackground` 保存的是 raw pointer，必须保证 border 生命周期覆盖 background。Views 里 `View` 成员声明顺序保证 border 在 background 之后销毁，因为 background 可能依赖 border。

## 7. `BubbleFrameView`

`BubbleFrameView` 是 bubble-styled widget 的 non-client frame。

重点：

- 它不是直接用 `View::SetBorder()`，而是提供 `SetBubbleBorder(std::unique_ptr<BubbleBorder>)`。
- `GetBoundsForClientView()` 基于 `GetContentsBounds()` 再扣 content margins。
- `GetWindowBoundsForClientBounds()` 会调用 `bubble_border_->GetBounds()`。
- `GetWindowMask()` 会根据 bubble border 的 shadow、arrow、rounded corners 返回窗口 mask。
- header、title、client view、footnote 都在 frame view 内布局。

对 Xenon 来说，`XenonCommonBubble::CreateFrameView()` 中创建 `BubbleFrameView` 并调用 `SetBubbleBorder()` 是正确路径。

## 8. Menu 的 Border 路径

菜单最相关的是 `MenuScrollViewContainer`。

### 创建时机

构造函数里会根据 menu anchor 算出 `arrow_`，然后立刻 `CreateBorder()`。

原因是菜单创建代码需要知道最终尺寸，所以 insets 必须在返回前可用。

### 是否使用 BubbleBorder

`HasBubbleBorder()` 返回 true 的条件：

- `arrow_ != BubbleBorder::NONE`；
- 或 `MenuConfig::ShouldUseBubbleBorderForMenu()` 返回 true。

### 默认 border

`CreateDefaultBorder()` 做的事：

1. 根据 `MenuConfig` 得到 vertical/horizontal border size。
2. 如果不使用 outer border，设置 `CreateEmptyBorder(insets)`。
3. 如果设置了自定义 border color，使用 `CreateSolidSidedBorder(insets, color)`。
4. 否则设置圆角背景，并用 `RoundRectPainter` 通过 `CreateBorderPainter()` 画外框。

### Bubble border

`CreateBubbleBorder()` 做的事：

1. 创建 `BubbleBorder(arrow_, shadow_type)`。
2. 设置 background color。
3. 设置 menu 或 submenu 的 shadow elevation。
4. 根据 `MenuConfig::use_outer_border` 决定是否画 stroke。
5. 设置 rounded corners。
6. 拆分两类 insets：
   - `outside_border_insets_`：border 外部区域，例如 shadow。
   - `additional_insets_`：border 内部给 menu 内容的 padding。
7. 非 Ash 路径使用 `BubbleBackground`。
8. 最后 `SetBorder(std::move(bubble_border))`。

`MenuScrollViewContainer::GetInsets()` 返回：

```cpp
return View::GetInsets() + additional_insets_;
```

所以菜单总 insets 不是单纯 BubbleBorder 的 insets，而是 shadow/stroke + menu 内部 padding 的组合。

### MenuHost 的 native 配合

`MenuHost::InitMenuHost()` 会根据 `HasBubbleBorder()` 设置 native window 参数：

| 条件 | `shadow_type` | `opacity` | 圆角 |
|---|---|---|---|
| 有 bubble border | `kNone` | `kTranslucent` | bubble border 自己画 |
| 无 bubble border 但有 corner radius | `kDrop` | `kTranslucent` | platform rounded corners |
| 无 bubble border 且无 radius | `kDrop` | `kOpaque` | 无额外圆角 |

这解释了为什么 Xenon 自定义菜单阴影需要让 menu 走 bubble-border-like 路径：只有这样 MenuHost 才会是透明窗口且没有 native shadow，custom shadow 才不会被原生阴影叠加或遮挡。

## 9. 常见派生 Border

### `HighlightBorder`

用于 ChromeOS/system UI 风格浮层。注意：当前 `ui/views/BUILD.gn` 仅在 `is_chromeos` 下把 `highlight_border.cc` 编入 `ui/views`，Windows/Linux 代码不要直接实例化它，否则可能出现头文件可见但链接不到实现的错误。

特点：

- 绘制 outer border 和 inner highlight 两层线。
- `InsetsType` 可选：
  - `kNoInsets`：不占内容空间。
  - `kHalfInsets`：1dip。
  - `kFullInsets`：2dip。
- 颜色来自 `ColorProvider`。
- `OnViewThemeChanged()` 触发 repaint。

### `FocusableBorder`

用于 textfield 等可聚焦控件。

特点：

- 默认 1dip inset。
- 默认 corner radius 来自 `FocusRing::kDefaultCornerRadiusDp`。
- 根据 View enabled 状态选择颜色。
- paint 时撤销 device scale factor，保证描边清晰。

### `LabelButtonBorder`

用于 button padding 或 asset painter。

特点：

- 基类 `LabelButtonBorder` 默认不绘制，只返回 insets。
- `LabelButtonAssetBorder` 可以按 button focus/state 使用 image grid painter。
- 当前 Chromium UI 趋势中，很多 button 已改用更现代的 background/ink drop/focus ring，但这个类仍存在。

## 10. Border 与 Shadow 的边界

Chromium Views 里，border 和 shadow 经常交织，但概念不同：

| 机制 | 属于 Border 吗 | 说明 |
|---|---|---|
| `CreateSolidBorder` | 是 | 简单描边 + insets |
| `CreateEmptyBorder` | 是 | 只占位，不绘制 |
| `BubbleBorder` | 是 | 同时画 border、shadow、arrow |
| `views::ViewShadow` | 否 | 独立对象，给 View 加 compositor shadow |
| `ui::Shadow` | 否 | compositor shadow layer |
| `Widget::InitParams::shadow_type` | 否 | native/widget 层 shadow |
| `XenonBoxShadowBorder` | 是 | Xenon 自定义 Border，模拟 CSS box-shadow |

对菜单和 bubble 来说，`BubbleBorder` 是例外，它把 shadow 当成 border 的一部分来管理，因此 `GetInsets()` 也包含 shadow 区域。

## 11. Xenon 当前用法对应关系

### `XenonBoxShadowBorder`

文件：

- `xenon_overlay/chrome/browser/ui/xenon_menu_shadow_border.{h,cc}`

符合 Chromium Border 约定：

- 继承 `views::Border`。
- `Paint()` 负责画 shadow 和 foreground round rect。
- `GetInsets()` 根据 blur/spread/offset 返回四边外扩，避免阴影裁剪。
- `GetMinimumSize()` 返回 insets 加圆角需要的最小尺寸。

需要注意：

- 它当前既画 shadow 也画背景 foreground，因此目标 View 的 background 通常应设为透明，避免重复绘制。
- 它的 insets 变化会影响 menu preferred size，调用方必须修正 widget bounds。

### `XenonBubbleShadowBorder`

文件：

- `xenon_overlay/chrome/browser/ui/xenon_common_bubble.cc`

设计点：

- 继承 `views::BubbleBorder`，保留 Bubble 的 anchor、arrow、layout 能力。
- `kBubbleBorder` 时走原生 BubbleBorder shadow。
- `kViewShadow / kCompositorShadow / kBoxShadow` 时在 `Paint()` 里自绘 shadow。
- 自定义 `GetInsets()` 返回 layout insets 和 custom shadow insets 的 max。
- 自定义 `GetBounds()` 先用普通 BubbleBorder 计算 anchor，再按额外 paint insets 外扩，防止 bubble 漂移。

这比“给 BubbleFrameView 直接挂 ViewShadow”更稳，因为 BubbleBorder 的几何闭环没有被破坏。

### `MenuShadowModifier`

文件：

- `xenon_overlay/chrome/browser/ui/xenon_menu_shadow_modifier.{h,cc}`

它修改 menu border 时要额外处理：

- `MenuHost` 异步创建后才能访问 contents view。
- `MenuScrollViewContainer::GetInsets()` 包含 BubbleBorder insets 和 additional insets。
- 替换 border 后 preferred size 可能变化。
- 修改 bounds 时要用新旧 insets 差值修正 `x/y`，保持菜单视觉锚点不变。

### `XenonCommonDialog`

当前 dialog 里也用了 border：

- button 用 `CreateEmptyBorder()` 做 padding。
- shadow wrapper 用 empty border 预留 compositor shadow margin。
- frame view 用 `BubbleBorder::FLOAT + NO_SHADOW` 做透明 frame shell。

### `XenonTipBarWidget`

Tip bar 使用普通圆角背景 + 圆角 border：

```cpp
content->SetBackground(
    views::CreateRoundedRectBackground(kTipBarBackground, kTipBarCornerRadius));
content->SetBorder(
    views::CreateRoundedRectBorder(kTipBarBorderThickness,
                                   kTipBarCornerRadius,
                                   kTipBarBorderColor));
```

这属于标准 Views 用法。

## 12. 自定义 Border 编写清单

新增一个自定义 Border 时，至少要回答这些问题：

1. `Paint()` 画什么？
   - 只画边框？
   - 画背景？
   - 画 shadow？
   - 是否需要 clip 掉内容区域？

2. `GetInsets()` 返回什么？
   - 是否只包含 stroke？
   - 是否包含 shadow blur/spread？
   - 是否包含额外 padding？
   - offset 为负数时左右/上下是否正确？

3. `GetMinimumSize()` 是否足够？
   - 图片 border 或大圆角 border 需要非零 minimum size。
   - EmptyBorder 通常可以返回空 size。

4. 是否依赖 theme color？
   - 如果使用 `ui::ColorId`，`OnViewThemeChanged()` 应触发 repaint。
   - Paint 时用 `color().ResolveToSkColor(view.GetColorProvider())` 或直接查 `ColorProvider`。

5. 是否影响 layout？
   - `SetBorder()` 会触发布局失效，但外层 Widget bounds 不一定自动修正。
   - Popup/Menu/Bubble 这类锚定 UI 要额外处理 anchor 或 bounds。

6. 是否和 background 冲突？
   - 如果 border 自己画 foreground/background，View background 应避免重复覆盖。
   - 如果 background 需要依赖 border，应确保生命周期安全。

## 13. 常见坑

### 只改 Paint，不改 Insets

如果 shadow 或 border 画到 View 外侧，但 `GetInsets()` 没有预留空间，结果通常是：

- 阴影被裁剪；
- preferred size 太小；
- popup 位置看起来不对。

### 只改 Insets，不修正 Popup Bounds

Popup/Menu/Bubble 的 widget bounds 通常已经根据旧 insets 算好。运行时替换 border 后，如果只 `SetBorder()`，内容会重新布局，但 widget 的屏幕位置不一定跟着正确移动。

Xenon 菜单修正就是为了解这个问题。

### `BubbleBackground` 指向旧 Border

`BubbleBackground` 保存 `BubbleBorder*`。如果替换 BubbleBorder，必须同步替换 background 或确保 background 指向新的 border。

### Layer-based background 会吃掉 border

`CreateLayerBasedSolidBackground()` 的注释明确说它使用 solid color layer，View 或子树不能再普通 paint，包括 border。

需要 border 的 View，不要随手换成 layer-based background。

### BubbleBorder 的 `NO_SHADOW` 不只是“不画阴影”

`NO_SHADOW` 也不画 stroke。它常用于平台自己提供 shadow，或者外层 frame/content 自己画视觉边框的情况。

### Menu border 拆成两段 insets

菜单 bubble border 场景下：

- BubbleBorder insets 主要表示 stroke/shadow/arrow 区域。
- `additional_insets_` 表示菜单内容 padding。

不要只看 `GetBorder()->GetInsets()` 就推断菜单总内容区域，应看 `MenuScrollViewContainer::GetInsets()`。

## 14. 实用选择建议

| 需求 | 推荐 |
|---|---|
| 普通 View padding | `CreateEmptyBorder()` |
| 普通实线边框 | `CreateSolidBorder()` 或 `CreateSolidSidedBorder()` |
| 圆角实线边框 | `CreateRoundedRectBorder()` + `CreateRoundedRectBackground()` |
| 图片九宫格边框 | `CreateBorderPainter()` |
| Bubble/Menu 标准阴影 | `BubbleBorder` |
| Bubble/Menu 自定义 shadow 且保持 anchor 稳定 | 继承/包装 `BubbleBorder`，同时处理 `Paint/GetInsets/GetBounds` |
| 普通 View 外阴影 | `views::ViewShadow` |
| 需要手动控制 shadow layer | `ui::Shadow` |
| CSS-like box-shadow | 自定义 `views::Border`，类似 `XenonBoxShadowBorder` |

## 15. 后续分析方向

如果继续深入，可以按这几个方向拆：

1. `BubbleBorder` 的 arrow 几何和屏幕避让逻辑。
2. `MenuConfig` 如何影响 menu border、corner radius、shadow elevation。
3. `NonClientFrameView` / `FrameView` 与 `View::Border` 的区别。
4. `ColorProvider` 和 `ui::ColorVariant` 在 border theme 更新中的路径。
5. `gfx::ShadowValue`、`ui::Shadow`、`BubbleBorder` shadow value 的关系。
