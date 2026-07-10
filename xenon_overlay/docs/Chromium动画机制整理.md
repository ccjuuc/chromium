# Chromium Animation 机制整理

本文基于当前 Chromium 142 源码，整理 Views / UI 体系里的动画类，并结合 Xenon 现有用法说明后续应如何选择和扩展。

Chromium 的动画不是单一体系，而是三层能力叠加：

| 层级 | 代表类 | 适用场景 |
|---|---|---|
| `gfx::Animation` | `LinearAnimation`、`SlideAnimation`、`ThrobAnimation`、`MultiAnimation` | 需要每帧拿到 0..1 进度，然后自己更新 View、Widget 或绘制状态 |
| `ui::LayerAnimator` | `LayerAnimationElement`、`LayerAnimationSequence`、`ScopedLayerAnimationSettings` | 动画目标是 layer 属性，例如 opacity、transform、bounds、rounded corners |
| `views` 便捷封装 | `AnimationBuilder`、`BoundsAnimator`、`WidgetFadeAnimator`、`BubbleSlideAnimator`、`InkDrop` | Views UI 的常见动画：淡入淡出、移动、布局 bounds、气泡切换、按钮水波纹 |

核心源码：

| 文件 | 作用 |
|---|---|
| `ui/gfx/animation/animation.{h,cc}` | `gfx::Animation` 基类，负责 start/stop、delegate、container |
| `ui/gfx/animation/animation_container.{h,cc}` | 管理一组 `Animation`，保证同一批动画共用 tick |
| `ui/gfx/animation/animation_runner.{h,cc}` | 动画 tick 来源，默认用 `base::Timer`，Views 可切到 compositor runner |
| `ui/gfx/animation/linear_animation.{h,cc}` | 固定时长 0..1 线性进度动画 |
| `ui/gfx/animation/slide_animation.{h,cc}` | 可反向的 0..1 show/hide 动画 |
| `ui/gfx/animation/throb_animation.{h,cc}` | 在 show/hide 间循环的提示动画 |
| `ui/gfx/animation/multi_animation.{h,cc}` | 多段 tween 动画，适合 fade-in/hold/fade-out |
| `ui/gfx/animation/tween.{h,cc}` | easing 曲线与数值/颜色/矩形/transform 插值 |
| `ui/compositor/layer_animator.{h,cc}` | layer 属性动画调度器 |
| `ui/compositor/layer_animation_element.{h,cc}` | 单个 layer 属性动画片段 |
| `ui/compositor/layer_animation_sequence.{h,cc}` | 串联多个 element 的动画序列 |
| `ui/compositor/scoped_layer_animation_settings.{h,cc}` | 临时设置 layer animator，使后续属性变更变成隐式动画 |
| `ui/compositor/layer_animation_observer.h` | layer 动画 observer 与 implicit animation observer |
| `ui/views/animation/animation_builder.{h,cc}` | Views 层链式 layer 动画 DSL |
| `ui/views/animation/animation_sequence_block.{h,cc}` | `AnimationBuilder` 的 sequence/block 实现 |
| `ui/views/animation/bounds_animator.{h,cc}` | 多 View bounds 动画 |
| `ui/views/animation/animation_delegate_views.{h,cc}` | 把 `gfx::Animation` 接到 widget compositor tick |
| `ui/views/animation/widget_fade_animator.{h,cc}` | Widget opacity 淡入淡出 |
| `ui/views/animation/bubble_slide_animator.{h,cc}` | Bubble 在不同 anchor view 之间滑动 |
| `ui/views/animation/ink_drop.h` | Button/控件 ink drop 状态动画入口 |

## 1. 选型表

| 需求 | 推荐类 | 原因 |
|---|---|---|
| 每帧根据进度自定义绘制或改任意状态 | `gfx::LinearAnimation` | 最直接，delegate 里拿 `GetCurrentValue()` |
| hover、展开/收起、可中途反向的 0..1 状态 | `gfx::SlideAnimation` | `Show()` / `Hide()` 会从当前值继续或反向 |
| 提示性闪烁、呼吸、有限/无限循环 | `gfx::ThrobAnimation` | 内建 show/hide 循环 |
| 多段进度，例如淡入、停留、淡出 | `gfx::MultiAnimation` | 每段可设 duration、tween、start/end value |
| 动画 opacity / transform / bounds / rounded corners | `views::AnimationBuilder` | 直接生成 layer animation sequence，写法短 |
| 临时让 `layer->SetOpacity()` 或 `view->SetBounds()` 隐式动画 | `ui::ScopedLayerAnimationSettings` | RAII 修改 `LayerAnimator` 的 duration/tween/preemption |
| 多个子 View 从当前 bounds 移到目标 bounds | `views::BoundsAnimator` | 统一管理多 View，完成后通知 observer |
| Widget 淡入淡出并在结束时 hide/close | `views::WidgetFadeAnimator` | 已处理 show/hide/close 和 opacity |
| Bubble 切换 anchor view | `views::BubbleSlideAnimator` | 使用 `BubbleDialogDelegateView` 的 anchor/bounds 体系 |
| Button ripple、hover、focus highlight | `views::InkDrop` | Chromium 控件标准反馈动画 |
| 需要直接控制 compositor layer 序列和抢占策略 | `ui::LayerAnimator` | 底层能力最全，但使用成本最高 |

## 2. `gfx::Animation` 基类

`gfx::Animation` 是 UI 线程侧的进度动画基类。它本身不规定动画效果，只负责生命周期和 tick 调度。

关键职责：

| 职责 | 说明 |
|---|---|
| 生命周期 | `Start()`、`Stop()`、析构时停止 container |
| 进度推进 | 子类实现 `Step(base::TimeTicks time_now)` |
| 当前值 | 子类实现 `GetCurrentValue()`，通常返回 0..1 |
| 通知 | 通过 `gfx::AnimationDelegate` 发 `AnimationProgressed`、`AnimationEnded`、`AnimationCanceled` |
| 容器 | 通过 `AnimationContainer` 让多动画共享 tick |
| 减少动态效果 | `ShouldRenderRichAnimation()`、`RichAnimationDuration()`、`PrefersReducedMotion()` |

`Start()` 流程：

1. 如果没有 container，创建 `AnimationContainer`。
2. 通知 delegate：`AnimationContainerWasSet(container)`。
3. 记录 start time。
4. 调用 `container_->Start(this)`。
5. 调子类钩子 `AnimationStarted()`。

`Stop()` 流程：

1. 标记不再 animating。
2. 先从 container 停止，因为 delegate 回调可能删除动画对象。
3. 调 `AnimationStopped()`。
4. 根据 `ShouldSendCanceledFromStop()` 发 ended 或 canceled。

注意点：

- `Stop()` 不一定等于 cancel；例如 `LinearAnimation::End()` 会把状态推进到 1，再触发 ended。
- 析构时如果动画仍在跑，会从 container 移除，但不会发 delegate 通知。
- `CurrentValueBetween()` 是便捷插值入口，内部使用 `gfx::Tween`。
- `RichAnimationDuration(duration)` 在减少动态效果时可返回 0，适合业务代码统一降级。

## 3. `AnimationContainer` 与 `AnimationRunner`

`AnimationContainer` 管理一组 `AnimationContainerElement`，核心价值是让同组动画使用同一个 tick 时间，避免多个动画略微错帧。

行为要点：

| 方法 | 说明 |
|---|---|
| `Start(element)` | 加入集合，更新最小 tick interval，启动 runner |
| `Stop(element)` | 从集合移除，空集合时停止 runner |
| `Run(time)` | 拷贝当前 elements 后逐个 `Step(time)`，避免回调修改集合导致迭代失效 |
| `SetAnimationRunner()` | 替换 tick 来源，可用默认 timer 或 compositor runner |

`AnimationRunner` 是 tick 抽象。默认实现基于 `base::OneShotTimer`，但 Chromium 注释里明确更偏向 compositor-based runner。Views 侧通过 `AnimationDelegateViews` 自动把 container 换成 `CompositorAnimationRunner`，让动画跟随 widget compositor step。

## 4. `LinearAnimation`

`LinearAnimation` 是固定时长的 0..1 进度动画。

核心字段：

| 字段 | 作用 |
|---|---|
| `duration_` | 动画时长，至少为 timer interval |
| `state_` | 当前线性进度，范围 0..1 |
| `in_end_` | 标记 `End()` 触发的停止流程 |

关键行为：

- 默认帧率是 60Hz，最小 tick interval 不小于 10ms。
- `Step()` 根据 `(time_now - start_time) / duration` 计算 `state_`。
- 每帧先调用 `AnimateToState(state_)`，再通知 delegate `AnimationProgressed()`。
- 当 `state_ == 1.0` 时自动 `Stop()`。
- `End()` 会触发最终帧，确保 delegate 收到 ended 而不是 canceled。

适用场景：

- 自定义绘制进度。
- Widget opacity 之类不是 layer 的状态更新。
- 需要自己在 delegate 里组合多个属性。

## 5. `SlideAnimation`

`SlideAnimation` 继承 `LinearAnimation`，但它的当前值不是底层 `state_`，而是独立维护的 `value_current_`。

核心能力：

| 方法 | 说明 |
|---|---|
| `Show()` | 朝 1.0 动画 |
| `Hide()` | 朝 0.0 动画 |
| `Reset(value)` | 停止并设置当前值 |
| `SetSlideDuration()` | 设置完整 0->1 或 1->0 的理论时长 |
| `SetTweenType()` | 设置 easing，默认 `gfx::Tween::EASE_OUT` |
| `SetDampeningValue()` | 控制从中间状态开始时 duration 缩短的程度 |

关键细节：

- `Show()` / `Hide()` 可以中途反向，新的起点是当前值。
- 实际 duration 会按当前进度缩短，不一定等于 `slide_duration_`。
- `slide_duration_ == 0` 时会直接跳到终点，并手动发 progressed/ended。
- `IsShowing()` 在已经显示完成后仍可能返回 true，这一点源码里有 TODO。

适用场景：

- hover highlight。
- 抽屉展开/收起。
- 自定义 View 的 show/hide progress。
- `BoundsAnimator` 内部也用它给每个 View 做 0..1 progress。

## 6. `ThrobAnimation` 与 `MultiAnimation`

`ThrobAnimation` 是 `SlideAnimation` 的循环版本。

行为：

- `StartThrobbing(cycles)` 在 0 和 1 之间循环。
- `cycles < 0` 表示无限循环。
- 循环结束时会停在隐藏状态。
- 普通 `Show()`、`Hide()`、`Reset()` 会停止 throbbing，回到普通 slide 行为。

`MultiAnimation` 是多段动画。

每个 `Part` 包含：

| 字段 | 说明 |
|---|---|
| `length` | 本段持续时间 |
| `type` | 本段 tween |
| `start_value` | 本段起始值 |
| `end_value` | 本段结束值 |

默认 `continuous_ = true`，表示跑到最后会继续循环；如需一次性完成，需要调用 `set_continuous(false)`。

典型场景：

- 0->1 fade in。
- 1->1 hold。
- 1->0 fade out。
- 多段 loading / attention animation。

## 7. `gfx::Tween`

`gfx::Tween` 负责 easing 和插值。它不是动画调度器，只是数学工具。

常用曲线：

| 曲线 | 说明 |
|---|---|
| `LINEAR` | 匀速 |
| `EASE_OUT` | 快进慢出，很多旧 Views 动画默认值 |
| `EASE_IN` | 慢进快出 |
| `EASE_IN_OUT` | 两端慢，中间快 |
| `FAST_OUT_SLOW_IN` | Chromium/Material 常用曲线，适合多数可见 UI 位移动画 |
| `LINEAR_OUT_SLOW_IN` | 进入场景、fade in |
| `FAST_OUT_LINEAR_IN` | 离开场景、fade out |
| `ZERO` | 永远返回 0 |
| `ACCEL_*_DECEL_*` | 新命名体系，对应具体 cubic bezier 参数 |

插值工具：

| 方法 | 插值对象 |
|---|---|
| `DoubleValueBetween()`、`FloatValueBetween()` | 数值 |
| `IntValueBetween()`、`LinearIntValueBetween()` | 整数 |
| `ColorValueBetween()` | 颜色 |
| `RectValueBetween()`、`RectFValueBetween()` | 矩形 |
| `SizeValueBetween()`、`SizeFValueBetween()` | 尺寸 |
| `TransformValueBetween()` | transform |
| `TransformOperationsValueBetween()` | transform operations |

注意：`AnimationBuilder` 默认很多 setter 的 tween 是 `LINEAR`，如果要符合常见 Chromium UI 观感，通常应显式传 `FAST_OUT_SLOW_IN`、`EASE_OUT` 或 `EASE_IN`。

## 8. `LayerAnimator` 体系

`ui::LayerAnimator` 是 layer 属性动画的底层调度器。`ui::Layer` 的很多属性变更最终都会通过它生效。

可动画属性定义在 `LayerAnimationElement::AnimatableProperty`：

| 属性 | 说明 |
|---|---|
| `TRANSFORM` | layer transform |
| `BOUNDS` | layer bounds |
| `OPACITY` | layer opacity |
| `VISIBILITY` | visibility |
| `BRIGHTNESS` | brightness filter |
| `GRAYSCALE` | grayscale filter |
| `COLOR` | solid color layer color |
| `CLIP` | clip rect |
| `ROUNDED_CORNERS` | layer rounded corners |
| `GRADIENT_MASK` | gradient mask |

核心类分工：

| 类 | 作用 |
|---|---|
| `LayerAnimationElement` | 一个属性片段，知道如何从当前值走到目标值 |
| `LayerAnimationSequence` | 串联多个 element，支持重复、pause、observer |
| `LayerAnimator` | 持有 queue/running animations，处理 start、schedule、preemption |
| `LayerAnimationObserver` | 监听 scheduled/started/ended/aborted |
| `ImplicitAnimationObserver` | 配合 `ScopedLayerAnimationSettings` 等隐式动画完成 |

`LayerAnimator` 抢占策略：

| 策略 | 行为 |
|---|---|
| `IMMEDIATELY_SET_NEW_TARGET` | 默认；冲突时直接把旧动画推进到目标，再设置新目标 |
| `IMMEDIATELY_ANIMATE_TO_NEW_TARGET` | abort 旧动画，然后从当前状态动画到新目标 |
| `ENQUEUE_NEW_ANIMATION` | 新动画排队等待 |
| `REPLACE_QUEUED_ANIMATIONS` | 清掉未开始的队列动画，再加入新动画 |

关键注意：

- 同一 layer 同一属性同一时间只能有一个有效动画。
- `StartTogether()` 可让多个属性或多个 sequence 尽量同起点启动，但不能动画相同属性。
- duration 为 0 时，属性会立即设置，并清理对应动画。
- 部分 transform/opacity 动画可走 compositor thread；bounds、rounded corners 等不一定有同样性能特征。

## 9. `ScopedLayerAnimationSettings`

`ScopedLayerAnimationSettings` 是 RAII 工具，用于临时修改某个 layer animator 的设置。对象析构时恢复旧设置。

可设置：

| 方法 | 说明 |
|---|---|
| `SetTransitionDuration()` | 设置隐式动画时长 |
| `SetTweenType()` | 设置隐式动画曲线 |
| `SetPreemptionStrategy()` | 设置抢占策略 |
| `AddObserver()` | 添加 `ImplicitAnimationObserver` |
| `CacheRenderSurface()` | 动画期间请求 render surface cache |
| `DeferPaint()` | 动画期间延迟 paint |
| `TrilinearFiltering()` | 动画期间请求 trilinear filtering |

典型用法：

```cpp
{
  ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
  settings.SetTransitionDuration(base::Milliseconds(200));
  settings.SetTweenType(gfx::Tween::FAST_OUT_SLOW_IN);
  layer->SetOpacity(1.0f);
  layer->SetTransform(gfx::Transform());
}
```

注意：

- 构造 `ScopedLayerAnimationSettings` 时会应用默认 200ms transition duration。
- 设置只在对象生命周期内生效，因此属性变更必须写在同一个作用域内。
- 如果只是一次显式动画，`views::AnimationBuilder` 通常更清楚。

## 10. `views::AnimationBuilder`

`AnimationBuilder` 是 Views 层最常用的 layer 动画 DSL。

示例：

```cpp
views::AnimationBuilder()
    .OnEnded(base::BindOnce(&MyView::OnAnimationDone,
                            weak_ptr_factory_.GetWeakPtr()))
    .Once()
    .SetDuration(base::Milliseconds(150))
    .SetOpacity(this, 1.0f, gfx::Tween::EASE_OUT)
    .SetTransform(this, gfx::Transform(), gfx::Tween::EASE_OUT);
```

重要生命周期：

1. `Once()` 或 `Repeatedly()` 创建 sequence block。
2. `SetDuration()` 和 `Set*()` 只是在 builder 内记录动画元素。
3. 调用 `Then()`、`At()`、`Offset()` 会结束当前 block，并创建新 block。
4. `AnimationBuilder` 析构时，才把所有 sequence 交给目标 layer 的 `LayerAnimator::StartTogether()`。

这意味着临时对象写法是有意设计：

```cpp
views::AnimationBuilder().Once().SetDuration(...).SetOpacity(...);
```

语句结束时临时 builder 析构，动画开始。

可动画属性：

| 方法 | 目标 |
|---|---|
| `SetBounds()` | layer bounds |
| `SetOpacity()` | layer opacity |
| `SetTransform()` | layer transform |
| `SetInterpolatedTransform()` | 自定义 interpolated transform |
| `SetVisibility()` | visibility |
| `SetBrightness()` | brightness |
| `SetGrayscale()` | grayscale |
| `SetColor()` | solid color |
| `SetClipRect()` | clip rect |
| `SetRoundedCorners()` | rounded corners |
| `SetGradientMask()` | gradient mask |

回调：

| 回调 | 时机 |
|---|---|
| `OnScheduled()` | animation sequence 被 scheduled |
| `OnStarted()` | 第一个 sequence started |
| `OnEnded()` | builder 创建的所有 sequence 都正常结束 |
| `OnWillRepeat()` | repeating sequence 完成一轮并即将重复 |
| `OnAborted()` | 任一 sequence 被 abort |

关键约束：

- 回调必须在 `Once()` / `Repeatedly()` 之前设置。
- callback 可能比 builder 活得更久；被捕获对象要用 weak pointer 或保证生命周期。
- `OnAborted()` 不应再启动新动画。
- 同一 block 内同一 layer 的同一属性只能设置一次。
- 同一 sequence 内同一属性动画不能互相重叠，否则 DCHECK。
- `GetAbortHandle()` 可让 handle 析构时 abort 相关动画，但会 abort layer 上全部属性动画，包括不是该 builder 创建的动画。

## 11. `BoundsAnimator`

`views::BoundsAnimator` 用于把一个父 View 下的多个子 View 从当前 bounds 动画到目标 bounds。

核心能力：

| 方法 | 说明 |
|---|---|
| `AnimateViewTo(view, target)` | 从当前 bounds 动到目标 bounds |
| `SetTargetBounds(view, target)` | 不重启动画，只更新目标 bounds |
| `StopAnimatingView(view)` | 停止单个 View 动画 |
| `Complete()` | 全部跳到目标 bounds，发完成通知 |
| `Cancel()` | 停在当前位置，发取消通知 |
| `SetAnimationDuration()` | 设置新动画时长 |
| `set_tween_type()` | 设置新动画曲线，默认 `EASE_OUT` |

内部机制：

- 每个 View 创建一个 `gfx::SlideAnimation`。
- 所有子动画共用一个 `AnimationContainer`。
- 普通模式每帧计算 `Tween::RectValueBetween()`，然后 `view->SetBoundsRect(new_bounds)`。
- repaint bounds 会先合并，等 container 本轮 progressed 后统一 `SchedulePaintInRect()`。

`use_transforms=true` 模式：

- 尺寸不变、起始 bounds 非空、View 当前 transform 是 identity 时，使用 transform 做移动。
- 动画中只改 `view->SetTransform()`，不每帧 relayout/repaint。
- 动画结束时再 `SetBoundsRect(target_bounds)` 并清空 transform。
- 如果动画取消，会把当前 transform 映射回真实 bounds，再清空 transform。

适用建议：

- 多个 View 重排位置，优先考虑 `BoundsAnimator`。
- 只做 layer opacity/transform，优先 `AnimationBuilder`。
- `use_transforms=true` 只适合尺寸不变的移动；尺寸变化会退回 bounds 动画。

## 12. Views 高层动画类

### `AnimationDelegateViews`

`AnimationDelegateViews` 同时实现：

- `gfx::AnimationDelegate`
- `views::ViewObserver`
- `gfx::AnimationContainerObserver`

它的关键作用是：当 `gfx::Animation` 的 container 设置后，如果目标 View 已经在 Widget 中，就把 container 的 runner 换成 `CompositorAnimationRunner`。

这能让 `gfx::Animation` 跟 compositor step 对齐。View 移出 widget 或 widget/compositor 不可用时，会清掉 custom runner，回退默认 runner。

### `WidgetFadeAnimator`

用于 Widget 淡入淡出。

特点：

- 内部是 `gfx::LinearAnimation fade_animation_`。
- fade in 前先把 widget opacity 设为 `0.01f`，避免 visible 且完全透明的窗口 show 问题。
- fade out 结束时根据 `close_on_hide_` 决定 `Hide()` 或 `Close()`。
- 默认 fade in 200ms，fade out 150ms，tween 为 `FAST_OUT_SLOW_IN`。

### `BubbleSlideAnimator`

用于 `BubbleDialogDelegateView` 在不同 anchor view 之间滑动。

行为：

- `AnimateToAnchorView()` 记录当前 bubble window bounds 和目标 anchor 对应 bounds，然后启动 `LinearAnimation`。
- 每帧用 `Tween::RectValueBetween()` 设置 widget bounds。
- 当前 bounds 到达 target 时设置新的 anchor view。
- `SnapToAnchorView()` 直接跳到目标位置并发 complete callback。

注意：

- 依赖 `BubbleDialogDelegateView` 的 anchor 和 `BubbleFrameView::GetUpdatedWindowBounds()`。
- `UpdateTargetBounds()` 在动画中只更新 target，源码注释说明可能产生 mid-animation pop。

### `InkDrop`

`InkDrop` 是控件状态反馈动画入口。

常用能力：

- `AnimateToState(InkDropState state)` 切换 ripple 状态。
- `SetHovered()` / `SetFocused()` 控制 hover/focus highlight。
- `SnapToActivated()` / `SnapToHidden()` 跳过动画。
- `UseInkDropForSquareRipple()`、`UseInkDropForFloodFillRipple()`、`UseInkDropWithoutAutoHighlight()` 是常用配置入口。

适用场景：

- Button ripple。
- Hover/focus highlight。
- 需要跟 Chromium 原生控件风格一致的交互反馈。

## 13. Xenon 现有动画用法

当前 Xenon 已经有几类动画实践。

| 文件 | 用法 |
|---|---|
| `xenon_overlay/xenon/chrome/browser/ui/views/xenon_toast.cc` | `AnimationBuilder` 做 toast show/close：opacity + y transform |
| `xenon_overlay/chrome/browser/ui/xenon_common_dialog.cc` | `AnimationBuilder` 做 dialog enter/exit：widget layer opacity + transform |
| `xenon_overlay/chrome/browser/ui/xenon_web_dialog.cc` | `AnimationBuilder` 做 web dialog enter：widget layer opacity + transform |
| `xenon_overlay/chrome/browser/ui/xenon_reminder_notification_group.cc` | `ScopedLayerAnimationSettings` 配合 `SetBounds()` 做卡片重排动画 |

### Toast

`XenonToastView::ShowAnimated()`：

- 初始 opacity 0。
- 初始 transform 为向上偏移。
- `AnimationBuilder` 在 150ms 左右恢复 opacity 1 和 identity transform。

`XenonToastView::StartClose()`：

- 设置 `is_closing_`。
- 停止 timer。
- `AnimationBuilder::OnEnded()` 里通过 weak pointer 删除自身。
- opacity 动到 0，transform 回到上偏移。

这个模式适合普通 View 浮层，因为动画目标是 View 自己的 layer。

### CommonDialog / WebDialog

dialog 使用 widget layer 动画：

- `GetWidget()->GetLayer()` 作为动画目标。
- 进入动画：opacity 0 -> 1，transform y -10 -> identity。
- 退出动画：opacity 1 -> 0，完成后执行 close callback。

这个模式适合 Widget 级浮层，因为目标是整个窗口 layer，不是内部某个 child view。

### Reminder Notification Group

`XenonReminderNotificationGroup::BringCardToFront()` 中：

- 确保 child paint-to-layer。
- 对每个 child layer 创建 `ScopedLayerAnimationSettings`。
- 设置 duration 300ms、tween `EASE_IN_OUT`。
- 随后调用 `child->SetBounds(...)`。

这里依赖 layer animator 的隐式 bounds 动画。需要注意 `settings` 必须覆盖 `SetBounds()` 调用所在作用域；如果 settings 先析构，后续属性设置就不会动画。

## 14. 实战注意事项

### 优先动画 transform 和 opacity

transform / opacity 通常比每帧改 bounds 更便宜，也更容易走 compositor。对浮层 show/hide、toast、dialog entrance，优先使用：

- opacity；
- transform translate/scale；
- 必要时 rounded corners。

### bounds 动画会影响 layout

`View::SetBounds()` 可能触发布局、paint、子 View 更新。只有确实需要真实布局变化时才用 bounds 动画。移动而不改尺寸时，可以考虑：

- `AnimationBuilder::SetTransform()`；
- `BoundsAnimator(use_transforms=true)`；
- 动画结束后再落真实 bounds。

### `AnimationBuilder` 的回调生命周期要保守

builder 的 observer 会挂到 layer animation sequence 上，可能在 builder 析构后继续存活。回调里不要裸捕获 View/Widget，除非能保证对象比 layer 动画活得久。

推荐：

```cpp
.OnEnded(base::BindOnce(&MyView::OnDone, weak_ptr_factory_.GetWeakPtr()))
```

### 设置初始状态要在动画前完成

常见写法：

```cpp
layer()->SetOpacity(0.0f);
layer()->SetTransform(gfx::Transform::MakeTranslation(0, -10));

views::AnimationBuilder()
    .Once()
    .SetDuration(base::Milliseconds(150))
    .SetOpacity(this, 1.0f)
    .SetTransform(this, gfx::Transform());
```

如果先启动动画再改初始状态，会触发抢占或瞬间跳变。

### View 必须有 layer

`AnimationBuilder` 的 View/LayerOwner overload 最终需要目标有 layer。对自定义 View，通常先：

```cpp
SetPaintToLayer();
layer()->SetFillsBoundsOpaquely(false);
```

如果动画的是 widget layer，则先确认 `GetWidget()` 和 `GetWidget()->GetLayer()` 非空。

### 注意 preemption

默认抢占策略是 `IMMEDIATELY_SET_NEW_TARGET`，这会让旧动画直接到终点。对于 hover 或频繁切换的 UI，有时更适合：

- `IMMEDIATELY_ANIMATE_TO_NEW_TARGET`：从当前状态平滑到新目标；
- `REPLACE_QUEUED_ANIMATIONS`：避免队列堆积；
- `ENQUEUE_NEW_ANIMATION`：明确需要顺序播放时才用。

### 减少动态效果

如果动画是装饰性、丰富动效，时长应考虑：

```cpp
gfx::Animation::RichAnimationDuration(base::Milliseconds(150))
```

系统或命令行要求 reduced motion 时，这类动画可以自动变成 0 duration。

### `ScopedLayerAnimationSettings` 的作用域必须紧贴属性变更

正确：

```cpp
{
  ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
  settings.SetTransitionDuration(base::Milliseconds(150));
  layer->SetOpacity(0.0f);
}
```

错误思路：

```cpp
ui::ScopedLayerAnimationSettings settings(layer->GetAnimator());
// settings 提前离开作用域后再 SetOpacity，不会保留设置。
```

### 不要在 `OnAborted()` 里启动新动画

`AnimationBuilder` 和 `LayerAnimationObserver` 的注释都强调 abort 回调不应启动新动画。abort 可能发生在 layer/widget 析构过程中，此时再启动动画容易访问半销毁对象。

## 15. 后续 Xenon 测试入口建议

如果继续在 `chrome://xenon-ui` 添加动画测试入口，建议分成四类：

| 分类 | Demo |
|---|---|
| 基础进度动画 | `LinearAnimation` 数值条、`SlideAnimation` hover progress、`ThrobAnimation` pulse |
| Layer 动画 | opacity、translate、scale、rounded corners、clip rect |
| Layout 动画 | `BoundsAnimator` 普通模式和 `use_transforms=true` 对比 |
| 浮层动画 | Toast enter/exit、Widget fade、Bubble slide |

建议默认先展示 `AnimationBuilder` 和 `BoundsAnimator`，因为它们最贴近 Xenon 浮层、菜单、Bubble、dialog 的后续需求。
