# Views 布局：用例与完备配置（第一步）

本文覆盖 **`ui/views/layout`** 常用布局器；**`ui/views/view_class_properties.h` 中声明的全部 `*Key` 已在 §0.1 单列成表**（含是否与几何布局相关），**§0.2 对逐项 `SetProperty` 用法与用途作了说明**。Chrome 专用的 `BrowserViewLayout` / `ContentsLayoutManager` 见 [reference.md](reference.md)。

**边界**：其它组件还可注册**自定义** `ui::ClassProperty`（不在该头文件）；那种键需各模块文档或 `GetProperty` 搜索。本文仅保证 **Views 公共头文件**里与 `SetProperty` 相关的标准键无遗漏。

**源码锚点**：`ui/views/layout/flex_layout.h`、`flex_layout_types.h`、`box_layout.h`、`fill_layout.h`、`table_layout.h`、`layout_types.h`、`view_class_properties.h`、`view.h`（`SetLayoutManager` / `SetUseDefaultFillLayout`）。

---

## 0. 全局约定（所有布局共用）

| 项 | 说明 |
|----|------|
| 坐标系 | 左上角为原点，x 向右、y 向下（与 Windows/GTK 一致；与 Cocoa 不同）。见 `docs/ui/views/overview.md`。 |
| 子 View bounds | **应由父级的 `Layout()` / `LayoutManager` 计算**，避免业务代码里随意 `SetBounds`（特殊绘制子 View 除外）。 |
| Border / 内容区 | 带 border 时 **content bounds** 在边框内侧；子 View 布局在 content 区域内。 |
| `InvalidateLayout()` | 改子树结构或影响测量/布局的属性后，依赖链上会失效缓存；必要时对 host `InvalidateLayout()`。 |
| `kViewIgnoredByLayoutKey` | 设为 `true` 时 **FlexLayout、BoxLayout、`LayoutManagerBase` 子类、View 的 default FillLayout** 会忽略该子 View（不参与测量与摆放）。用于「绝对定位」或父级手管的 overlay。 |
| `kMarginsKey` | `gfx::Insets*`，**外边距**（在子 View 的 bounds 之外）。FlexLayout / BoxLayout 使用；TableLayout 以列/行与 span 为主，边距仍可按 View 自身 border 处理。 |
| `SetLayoutManager(nullptr)` | 析构或换根布局前清空，避免悬空指针（如 `BrowserView` 析构）。 |
| `SetUseDefaultFillLayout(true)` | 在未设置 `LayoutManager` 时，使用 **默认 FillLayout**；一旦 `SetLayoutManager(...)` 会关闭。默认全局常量见 `view.h` 内 `kUseDefaultFillLayout`（当前多为 `false`）。 |
| `SetLayoutManagerUseConstrainedSpace(bool)` | 是否让布局尊重 **可用空间**；头文件注明后续可能弱化，新代码优先用对齐与 `CalculatePreferredSize(SizeBounds)` 路径。 |

### 0.1 `SetProperty` / `ClearProperty` 与 `view_class_properties.h` **全部**键

`View` 的 `SetProperty` / `GetProperty` / `ClearProperty` 由 `ui::PropertyHandler` 实现（见 `ui/base/class_property.h`）。

**实现侧宏（以 `ui/views/view_class_properties.cc` 为准）**：

| 宏 | 含义 |
|----|------|
| `DEFINE_OWNED_UI_CLASS_PROPERTY_KEY(...)` | View **拥有**堆上对象；`ClearProperty` 会析构；`SetProperty(key, 临时量)` 安全。头文件中类型常写作 `T*`。 |
| `DEFINE_UI_CLASS_PROPERTY_KEY(...)` | **不取得所有权**；指针/对象由调用方保证生命周期；默认值在宏第三参数。 |

下列 **`views::` 命名空间** 键全部来自 `ui/views/view_class_properties.h` / `.cc`（共 **15** 个 `DEFINE_*_KEY`，**已列全**）：

| `views::*Key` | 所有权 | `SetProperty` 典型实参 | `GetProperty` 侧类型（头文件） | 读取方（几何布局） | 其它用途 |
|---------------|--------|-------------------------|--------------------------------|-------------------|----------|
| `kHitTestComponentKey` | 按值 | `int`（如 `HTCLIENT`） | `int` | — | 非 Client 区 hit-test |
| `kMarginsKey` | **OWNED** | `gfx::Insets(...)`、`gfx::Insets::TLBR(...)` | `gfx::Insets*` | **FlexLayout**、**BoxLayout** | |
| `kInternalPaddingKey` | **OWNED** | `gfx::Insets(...)` | `gfx::Insets*` | **FlexLayout** | 从 margin 中扣减 |
| `kAnchoredDialogKey` | 非 owned | `DialogDelegate*` | `DialogDelegate*` | — | Bubble 焦点顺序 |
| `kWidgetForAnchoringKey` | 非 owned | `Widget*` | `Widget*` | — | 锚定到指定 Widget |
| `kBoxLayoutFlexKey` | **OWNED** | `BoxLayoutFlexSpecification().WithWeight(n)` | `BoxLayoutFlexSpecification*` | **BoxLayout**（优先于 `SetFlexForView`） | |
| `kHighlightPathGeneratorKey` | **OWNED** | `std::make_unique<...>()` 等 | `HighlightPathGenerator*` | — | 焦点环 / ink-drop |
| `kFlexBehaviorKey` | **OWNED** | `FlexSpecification(...)` | `FlexSpecification*` | **FlexLayout**、`AnimatingLayoutManager` 等 | |
| `kCrossAxisAlignmentKey` | **OWNED** | `LayoutAlignment::kStart` 等 | `LayoutAlignment*` | **FlexLayout** | 可作 `FlexLayout::SetDefault` |
| `kTableColAndRowSpanKey` | **OWNED** | `gfx::Size(col_span, row_span)` | `gfx::Size*` | **TableLayout** | 跨度含 padding 列/行 |
| `kTableHorizAlignKey` | **OWNED** | `LayoutAlignment::...` | `LayoutAlignment*` | **TableLayout** | |
| `kTableVertAlignKey` | **OWNED** | `LayoutAlignment::...` | `LayoutAlignment*` | **TableLayout** | |
| `kViewIgnoredByLayoutKey` | 按值 | `true` / `false` | `bool` | **LayoutManagerBase** 系、**默认 FillLayout** | overlay / 手摆子 View |
| `kElementIdentifierKey` | 按值 | `ui::ElementIdentifier(...)` | 同左 | — | DevTools / Tracker |
| `kDetachedViewFocusManagerKey` | 非 owned | `FocusManager*` | `FocusManager*` | — | 未挂 Widget 的焦点 |

**与 FlexLayout::SetDefault 的关系**：`FlexLayout::SetDefault(const ClassProperty<T>*, U&&)` 使用的是同一套 **`kCrossAxisAlignmentKey`、`kMarginsKey`、`kInternalPaddingKey`、`kFlexBehaviorKey`**，作用在 **未在子 View 上 SetProperty** 时的默认值（构造里已对 `kCrossAxisAlignmentKey` 设过默认）。

**FillLayout**：不读 `kMarginsKey` / flex 等；仅通过 **`LayoutManagerBase` → `IncludeInLayout`** 尊重 **`kViewIgnoredByLayoutKey`**。

### 0.2 各键用法与用途（逐项）

与 §0.1 表一一对应。**OWNED** 键由 View 在清除属性时释放堆对象，`SetProperty(key, 临时量)` 安全。**非 owned** 指针须在 View 仍可能 `GetProperty` 的整段生命周期内有效。

#### `kHitTestComponentKey`

- **典型写法**：`view->SetProperty(kHitTestComponentKey, HTCLIENT);`，或 `views::SetHitTestComponent(view, hit_test_id);`（`ui/views/window/hit_test_utils.h` / `GetHitTestComponent`）。
- **用途**：窗口**非客户区** View 层次上的 Win32 风格 hit-test 分量（如 `HTCLIENT`）；默认视为 `HTNOWHERE`。`GetHitTestComponent` 从 `point_in_widget` 向下取**最内层**非 `HTNOWHERE` 的值。
- **与布局**：不参与 FlexLayout / BoxLayout 等几何计算。

#### `kMarginsKey`（OWNED）

- **典型写法**：`child->SetProperty(kMarginsKey, gfx::Insets(...));` 或 `gfx::Insets::TLBR(...)` / `gfx::Insets::VH(...)`。
- **用途**：子 View **bounds 外侧**的外边距；**FlexLayout**、**BoxLayout** 用来计算子项间距与整体 preferred 尺寸。与 border 不同：border 在 bounds 内侧，margins 在 bounds 外侧由布局器解释。

#### `kInternalPaddingKey`（OWNED）

- **典型写法**：`view->SetProperty(kInternalPaddingKey, gfx::Insets(...));`
- **用途**：仅 **FlexLayout** 读取。表示「画在控件轮廓内、但布局上应从外边距里扣掉」的留白，使触控热区、拖动手柄等与**视觉间距**一致而不额外撑开与相邻控件的间隙（从有效 margin 扣到 ≥0）。与 `kMarginsKey` 配合使用。

#### `kAnchoredDialogKey`（非 owned）

- **典型写法**：一般由 **Bubble** 框架在锚点 View 上 `SetProperty` / `ClearProperty`（见 `ui/views/bubble/bubble_dialog_delegate_view.cc`）；业务侧很少手写。
- **用途**：把锚点 View 与当前 `DialogDelegate*` 关联，使 **FocusSearch**（`ui/views/focus/focus_search.cc`）把 Bubble 内容纳入 **Tab 焦点顺序**，焦点能进入 Bubble 再按顺序返回。
- **注意**：不取得所有权；必须在对话框存活期内保持指针有效。

#### `kWidgetForAnchoringKey`（非 owned）

- **典型写法**：`anchor_view->SetProperty(kWidgetForAnchoringKey, some_widget);`（例：`chrome/browser/ui/views/frame/browser_view.cc` 对内容容器设置）。
- **用途**：Bubble 锚定时使用**指定 `Widget*`**，而不是锚点 `GetWidget()`。典型场景是 **macOS 全屏**下子树迁到更高 z-order 的 overlay `Widget`，避免 Bubble 仍锚在原窗口而被遮挡（头文件注释与 `bubble_dialog_delegate_view.cc` 读取逻辑一致）。
- **注意**：不取得所有权。

#### `kBoxLayoutFlexKey`（OWNED）

- **典型写法**：`child->SetProperty(kBoxLayoutFlexKey, BoxLayoutFlexSpecification().WithWeight(n));`
- **用途**：仅 **BoxLayout**：该子项在主轴上如何参与**剩余空间分配**（权重等）。§0.1 表注：优先于仅针对 BoxLayout 的 `SetFlexForView` 类 API 路径。
- **与 `kFlexBehaviorKey`**：后者服务 **FlexLayout**，二者对应不同布局器，勿混用。

#### `kHighlightPathGeneratorKey`（OWNED）

- **典型写法**：`view->SetProperty(kHighlightPathGeneratorKey, std::make_unique<…HighlightPathGenerator>(…));`（具体生成器类型按控件形状选择）。
- **用途**：自定义焦点环 / ink-drop 的高亮路径（圆角、异形按钮等）。不参与测量与摆放。

#### `kFlexBehaviorKey`（OWNED）

- **典型写法**：`child->SetProperty(kFlexBehaviorKey, FlexSpecification(...));`
- **用途**：**FlexLayout**（及文档提到的 `AnimatingLayoutManager` 等）中子项的 flex 规则（伸缩、basis 等，见 `flex_layout_types.h`）。也可用 `FlexLayout::SetDefault(kFlexBehaviorKey, …)` 作为未单独 `SetProperty` 的子 View 的默认行为（与 §0.1 末段「SetDefault」说明一致）。

#### `kCrossAxisAlignmentKey`（OWNED）

- **典型写法**：`child->SetProperty(kCrossAxisAlignmentKey, LayoutAlignment::kCenter);` 等（枚举见 `layout_types.h`）。
- **用途**：**FlexLayout** 下该子项在**交叉轴**上的对齐，可覆盖父级默认。`FlexLayout::SetDefault(kCrossAxisAlignmentKey, …)` 设置未显式设置子项时的默认交叉轴对齐。

#### `kTableColAndRowSpanKey`（OWNED）

- **典型写法**：`cell->SetProperty(kTableColAndRowSpanKey, gfx::Size(col_span, row_span));`（`width` = 列跨度，`height` = 行跨度）。
- **用途**：**TableLayout** 指定单元格跨越的列数/行数。头文件约定：**padding 列计入跨度**——跨越「数据列 + padding 列 + 数据列」时列跨度为 **3** 而非 2。

#### `kTableHorizAlignKey` / `kTableVertAlignKey`（OWNED）

- **典型写法**：`cell->SetProperty(kTableHorizAlignKey, LayoutAlignment::kStart);`、`SetProperty(kTableVertAlignKey, …);`（`ui/views/layout/table_layout.h` 中有示例注释）。
- **用途**：**TableLayout** 单元格内容在格子内的**水平 / 垂直**对齐（`LayoutAlignment` 枚举）。

#### `kViewIgnoredByLayoutKey`

- **典型写法**：`overlay->SetProperty(kViewIgnoredByLayoutKey, true);`
- **用途**：**LayoutManagerBase** 派生布局（含 **FlexLayout**、**BoxLayout**）及 View **默认 FillLayout** 在测量与 `Layout` 时**跳过**该子 View。用于 overlay、父级手调 `SetBounds` 的层等不参与流式布局的节点。`FillLayout` 不读 margins / flex，但仍通过 `IncludeInLayout` 尊重此项。

#### `kElementIdentifierKey`

- **典型写法**：`view->SetProperty(kElementIdentifierKey, kSomeId);`（`kSomeId` 为 `DEFINE_ELEMENT_IDENTIFIER_VALUE` 等定义的 `ui::ElementIdentifier`）。
- **用途**：为 View 打**稳定逻辑标签**，供 **ElementTracker**、交互测试、DevTools 等按 id 观察或查找；不改变布局几何。

#### `kDetachedViewFocusManagerKey`（非 owned）

- **典型写法**：在 View **尚未**附加到任何 `Widget`（`GetWidget()` 为 `nullptr`）时：`view->SetProperty(kDetachedViewFocusManagerKey, focus_manager);`
- **用途**：让 `View::GetFocusManager()`（`ui/views/view.cc`）在未进窗口树时仍能返回该 `FocusManager`，从而在未挂 Widget 的场景下仍可 `RequestFocus()` 等；实现中有 `CHECK(!widget)` 分支，与「仅 detached 使用」的语义一致。
- **注意**：不取得所有权；View 进入正常 Widget 层次后应依赖 `widget->GetFocusManager()`，并视情况清除该属性。

---

## 1. `LayoutAlignment` / `LayoutOrientation`（枚举全集）

定义见 `ui/views/layout/layout_types.h`。

### `LayoutOrientation`

| 值 | 含义 |
|----|------|
| `kHorizontal` | 主轴为水平（「一行」）。 |
| `kVertical` | 主轴为垂直（「一列」）。 |

### `LayoutAlignment`

| 值 | 含义 | 注意 |
|----|------|------|
| `kStart` | 主轴或交叉轴起始侧对齐。 | 与 locale 书写方向无关时即「左/上」语义由布局定义。 |
| `kCenter` | 居中。 | |
| `kEnd` | 结束侧对齐。 | |
| `kStretch` | 在可用空间内拉伸该维尺寸。 | 交叉轴常用；子 View 会收到大于 preferred 的 bounds。 |
| `kBaseline` | 按文字基线对齐。 | **多数布局不支持**；注释写明仅垂直方向有意义。 |

---

## 2. FlexLayout（优先用于新的一维/可伸缩排列）

类：`views::FlexLayout`（`flex_layout.h`），基类 `LayoutManagerBase`。

### 2.1 Host（布局器自身）配置项 — 须全部知晓

链式调用，均返回 `FlexLayout&`。**默认值**摘自 `flex_layout.h` / `flex_layout.cc`。

| 方法 | 类型 / 默认值 | 语义 |
|------|----------------|------|
| `SetOrientation` | `LayoutOrientation`，默认 `kHorizontal` | 主轴方向。 |
| `SetMainAxisAlignment` | `LayoutAlignment`，默认 `kStart` | 主轴上多余空间如何分配（start/center/end/stretch）。 |
| `SetCrossAxisAlignment` | `LayoutAlignment`，默认 `kStretch` | 默认交叉轴对齐；**子 View 可用 `kCrossAxisAlignmentKey` 覆盖**。 |
| `SetInteriorMargin` | `gfx::Insets`，默认空 | Host **内边距**（子 View 与 host 内容边之间的空白）。 |
| `SetMinimumCrossAxisSize` | `int`，默认 `0` | Host 在交叉轴上的最小尺寸约束。 |
| `SetCollapseMargins` | `bool`，默认 `false` | `true` 时相邻子 View 的 margin 与间距取 **较大者合并**，而非相加。 |
| `SetIncludeHostInsetsInLayout` | `bool`，默认 `false` | `true` 时把 **host 的 border/insets** 视作内部留白的一部分（常与 `collapse_margins` 联用以对齐触摸留白）。 |
| `SetIgnoreDefaultMainAxisMargins` | `bool`，默认 `false` | `true` 时 **主轴首尾** 不重复套用 `SetDefault(kMarginsKey)` 的边距，仅保留 `interior_margin`；见 `flex_layout.h` 内 ASCII 图例。 |
| `SetFlexAllocationOrder` | `FlexAllocationOrder`，默认 `kNormal` | `kReverse` 时 flex 分配顺序反转（空间不足时「从哪一侧先被挤压」）。 |
| `SetDefault(const ui::ClassProperty<T>*, U&&)` | 模板 | 为**未在子 View 上设置**的属性提供默认值。FlexLayout 读取的默认键见下表。 |

**构造行为**：`FlexLayout()` 内会 `SetDefault(kCrossAxisAlignmentKey, kStretch)`，保证该属性始终有非空默认。

### 2.2 `SetDefault` 可绑定的属性键（子 View 未设置时使用）

| 属性键 | 类型 | 作用 |
|--------|------|------|
| `views::kCrossAxisAlignmentKey` | `LayoutAlignment*` | 单个子在交叉轴上对齐，覆盖 layout 的 `SetCrossAxisAlignment`。 |
| `views::kMarginsKey` | `gfx::Insets*` | 子 View 外 margin；与 `interior_margin`、`collapse_margins` 组合规则见 `flex_layout.h` 文件头注释。 |
| `views::kInternalPaddingKey` | `gfx::Insets*` | **从 margin 中扣除**的「内部垫量」（如触摸扩展、隐形拖拽区），减少有效间距，最小到 0。 |
| `views::kFlexBehaviorKey` | `FlexSpecification*` | 子 View 在主轴上的 **伸缩规则**；未设则用默认 flex 规则（见 `GetDefaultFlexRule()`）。 |

### 2.3 子 View：`FlexSpecification` 完备字段

类：`views::FlexSpecification`（`flex_layout_types.h`）。

**构造重载**：

1. `FlexSpecification()` — 默认规则：**不 flex**，尺寸为 preferred。
2. `explicit FlexSpecification(FlexRule rule)` — 自定义 `FlexRule` 回调（高级）。
3. `FlexSpecification(MinimumFlexSizeRule min_rule, MaximumFlexSizeRule max_rule = kPreferred, bool adjust_height_for_width = false)` — 两轴共用同一套 min/max 规则；`adjust_height_for_width` 用于 **变窄变高** 的多行文本。
4. `FlexSpecification(LayoutOrientation, MinimumFlexSizeRule min_main, MaximumFlexSizeRule max_main = kPreferred, bool adjust_height_for_width = false, MinimumFlexSizeRule min_cross = kPreferred)` — **仅主轴**用 min/max flex；交叉轴 min 可单独指定。

**`MinimumFlexSizeRule`（缩小顺序语义）** — 枚举全集：

- `kScaleToZero` — 可压到 0。
- `kScaleToMinimumSnapToZero` — 先压到 minimum 再可收为 0。
- `kPreferredSnapToZero` — 先 preferred 再可收为 0。
- `kScaleToMinimum` — 压到 minimum 为止。
- `kPreferredSnapToMinimum` — preferred 后可再压到 minimum。
- `kPreferred` — 不低于 preferred。

**`MaximumFlexSizeRule`（放大语义）** — 枚举全集：

- `kPreferred` — 不超过 preferred。
- `kScaleToMaximum` — 可到子 View 的 `GetMaximumSize()`。
- `kUnbounded` — 可任意变大（受父 bounds 限制）。

**链式修饰**：

| 方法 | 含义 |
|------|------|
| `WithWeight(int)` | `0` = 占满自身所需；`>0` 时与兄弟按权重分配多余/不足空间（与 BoxLayout 权重语义一致方向）。 |
| `WithOrder(int)` | **较小 order 优先**分配空间；同 order 内再按权重。 |
| `WithAlignment(LayoutAlignment)` | 在 **分配给该子的 cell** 内对齐；默认 `kStretch`（铺满 cell）；`kStart`/`kCenter`/`kEnd` 为在 cell 内「浮动」到一侧，最大不超过 preferred。 |

成员访问：`rule()`、`weight()`、`order()`、`alignment()`。

### 2.4 FlexLayout 典型用例（配置清单）

| 用例 | Host 配置 | 子 View 配置 |
|------|------------|----------------|
| 工具条：左固定 + 右可长可短 | `SetOrientation(kHorizontal)`，`SetMainAxisAlignment(kStart)`，`SetCrossAxisAlignment(kCenter)` | 左侧不设 `kFlexBehaviorKey`；右侧 `FlexSpecification(kScaleToZero, kUnbounded).WithWeight(1)`。 |
| 垂直表单：多行固定高 + 底部按钮贴底 | `SetOrientation(kVertical)`，`SetMainAxisAlignment(kStart)`；中间用 **空白 spacer View**（或 `kFlexBehaviorKey` 占满中间）把按钮顶到底部 | 可伸展区 `WithWeight(1)` + `MinimumFlexSizeRule::kScaleToZero` + `MaximumFlexSizeRule::kUnbounded`。 |
| 触摸友好间距 | `SetCollapseMargins(true)`，`SetInteriorMargin(...)` | 各子 `kMarginsKey`；按钮可用 `kInternalPaddingKey` 吃 margin。 |
| 空间不足时从左侧先「缩没」 | `SetFlexAllocationOrder(FlexAllocationOrder::kReverse)` | 配合 `WithOrder` / `WithWeight`。 |
| 某子不参与 flex、固定 preferred | 无 | `ClearProperty(kFlexBehaviorKey)` 或使用默认「零权重不伸缩」的 spec（见 `flex_layout_example.cc` 的 `GetFlexSpecification`）。 |
| 子 View 在交叉轴顶对齐而整体 stretch | `SetCrossAxisAlignment(kStretch)` | 需顶对齐的子设置 `kCrossAxisAlignmentKey = kStart`。 |
| 排除在布局外 | 无 | `SetProperty(kViewIgnoredByLayoutKey, true)`。 |

**可见性**：在 **FlexLayout 外**对子 View `SetVisible(false)` 会保持隐藏，直到再次 `SetVisible(true)`（与 layout 内部因 flex 规则隐藏区分开）。见 `flex_layout.h` 类注释。

---

## 3. BoxLayout（遗留一维；子 View 恒按 preferred 基准再分配 flex）

类：`views::BoxLayout`（`box_layout.h`），`LayoutManagerBase` 子类。

### 3.1 构造函数参数（一次性）

```cpp
explicit BoxLayout(
    Orientation orientation = kHorizontal,
    const gfx::Insets& inside_border_insets = gfx::Insets(),
    int between_child_spacing = 0,
    bool collapse_margins_spacing = false);
```

| 参数 | 含义 |
|------|------|
| `orientation` | `kHorizontal` / `kVertical`。 |
| `inside_border_insets` | Host 内容与子区域之间的 **内边距**（类注释称 inside border）。 |
| `between_child_spacing` | **相邻子 View** 之间额外间距（与 margin 叠加规则见类注释 ASCII）。 |
| `collapse_margins_spacing` | 是否与 Flex 类似 **折叠** margin / inside / between 中的较大者。 |

### 3.2 Host 可变配置（setter）

| 方法 | 说明 |
|------|------|
| `SetOrientation` | 改变主轴。 |
| `set_main_axis_alignment` / `set_cross_axis_alignment` | 类型为 `LayoutAlignment`（同 Flex 的 main/cross 语义）。 |
| `set_inside_border_insets` | 同构造 `inside_border_insets`。 |
| `set_between_child_spacing` | 子间距。 |
| `SetCollapseMarginsSpacing` / `GetCollapseMarginsSpacing` | 折叠策略开关。 |
| `set_minimum_cross_axis_size` | 交叉轴最小尺寸。 |
| `SetFlexForView(view, flex, use_min_size)` | **按 View 指针** 设置 flex 权重；`flex==0` 不缩放；`use_min_size==true` 时主轴最小为 `View::GetMinimumSize()`。 |
| `ClearFlexForView` | 恢复使用 `SetDefaultFlex`。 |
| `SetDefaultFlex` / `GetDefaultFlex` | 未单独指定 flex 的子 View 的默认权重（默认 **0**）。 |

### 3.3 子 View 属性

| 键 / API | 说明 |
|----------|------|
| `kMarginsKey` | 外边距；与 `inside_border_insets`、`between_child_spacing`、`collapse_margins_spacing` 共同决定间距（见头文件图示）。 |
| `kBoxLayoutFlexKey` | `BoxLayoutFlexSpecification*`；**优先级高于** `SetFlexForView` 注册的 flex（见 `box_layout_unittest.cc` 注释）。`BoxLayoutFlexSpecification::WithWeight` / `UseMinSize`。 |
| `kViewIgnoredByLayoutKey` | 忽略该子 View。 |

### 3.4 FlexLayout vs BoxLayout（选型）

| 维度 | FlexLayout | BoxLayout |
|------|------------|-----------|
| 伸缩模型 | `FlexSpecification` + min/max 规则 + order | 主要 **按比例权重** 分配主轴剩余/不足 |
| 溢出 | 规则驱动，可细控「先缩谁」 | 子可被 clamp，**不分配多余空间**给非 flex（见类注释） |
| 新代码 | **优先** | 维护旧 UI 或简单工具条仍可沿用 |

---

## 4. FillLayout

类：`views::FillLayout`（`fill_layout.h`）。

| 配置 | 默认值 | 含义 |
|------|--------|------|
| `SetMinimumSizeEnabled(bool)` | `false` | `true` 时 `GetMinimumSize` 取子 View **minimum** 的最大值；`false` 时与 preferred 行为兼容旧代码。 |
| `SetIncludeInsets(bool)` | `true` | preferred 计算是否包含 **host insets**；与 `View` 默认 fill 行为历史兼容有关。 |

**行为**：每个**可见**子 View 的 bounds 与 **父 content 区域**一致（多子则同区重叠，由 z-order 决定谁在上）。**首选尺寸**为所有子 preferred 的 **最大值**（各向）。

**典型用例**：单 child 铺满容器（网页区、Lens overlay 容器）；多 child 叠层绘制。

---

## 5. TableLayout

类：`views::TableLayout`（`table_layout.h`）。**先声明列/行结构，再按添加子 View 顺序依次填入单元格**；**padding 列/行**占列号/行号，但自动跳过放置子 View。

### 5.1 列：`AddColumn` / `AddPaddingColumn`

`AddColumn(h_align, v_align, horizontal_resize, size_type, fixed_width, min_width)`：

| 参数 | 含义 |
|------|------|
| `h_align` / `v_align` | 该列起始的 View 默认水平/垂直对齐（`LayoutAlignment`）。 |
| `horizontal_resize` | `>0` 表示该列在 **宽于 preferred** 时可按比例吃多余宽度；`TableLayout::kFixedSize`（0）表示不可横向伸缩。 |
| `size_type` | `ColumnSize::kFixed` 使用 `fixed_width`；`kUsePreferred` 由子 preferred 决定。 |
| `fixed_width` / `min_width` | 固定宽或最小宽（语义见注释）。 |

`AddPaddingColumn(horizontal_resize, width)`：纯留白列；**计入** col span 计数。

### 5.2 行：`AddRows` / `AddPaddingRow`

`AddRows(n, vertical_resize, height = 0)`：`vertical_resize` 类似列的 resize 权重；`height` 可为 0 表示未指定。

`AddPaddingRow(vertical_resize, height)`：垂直留白，计入 row span。

### 5.3 其它 Host API

| 方法 | 作用 |
|------|------|
| `RemoveColumns(n)` / `RemoveRows(n)` | 删除末尾 n 列/行。 |
| `LinkColumnSizes({col...})` | 强制列宽一致，取最大需求。 |
| `SetLinkedColumnSizeLimit` | 链接列宽时忽略过宽的列。 |
| `SetMinimumSize` | Host 最小尺寸。 |

### 5.4 子 View 属性（Table 专用）

| 键 | 类型 | 含义 |
|----|------|------|
| `kTableColAndRowSpanKey` | `gfx::Size*` | `width()` = **列跨度**，`height()` = **行跨度**。**注意**：跨度包含 **padding 列/行**（头文件注释：两列夹一 padding 时 span 为 3 而非 2）。 |
| `kTableHorizAlignKey` | `LayoutAlignment*` | 覆盖列默认水平对齐。 |
| `kTableVertAlignKey` | `LayoutAlignment*` | 覆盖列默认垂直对齐。 |

**伸缩规则摘要**（见类注释）：只有 **`horizontal_resize` / `vertical_resize` > 0** 的列/行会把多出来的空间分给子 View；且子 View 在该维对齐须为 **`kStretch`** 才会被拉大，否则只在 cell 内对齐。

---

## 6. 与 `views_examples` 的对应（动手对照）

| 布局器 | 示例类 / 文件 |
|--------|----------------|
| FlexLayout | `FlexLayoutExample` — `ui/views/examples/flex_layout_example.cc` |
| BoxLayout | `BoxLayoutExample` — `box_layout_example.cc` |
| FillLayout | 多处嵌套使用；浏览器侧见 `browser_view.cc` Lens overlay |
| TableLayout | `TableExample` — `table_example.cc` |

注册表：`ui/views/examples/create_examples.cc`。

---

## 7. 检查清单（写布局代码前自审）

- [ ] 主轴/交叉轴是否与 `LayoutOrientation`、对齐枚举一致？
- [ ] 子 View 是否需要 **`kMarginsKey` / `kInternalPaddingKey`**，以及 Host 的 **`collapse`** 系列是否已理解？
- [ ] Flex 子项是否明确 **`kFlexBehaviorKey`**（或 BoxLayout 的 `SetFlexForView` / `kBoxLayoutFlexKey`）？
- [ ] Table 是否漏算 **padding 列/行** 的 span？
- [ ] 是否需要 **`kViewIgnoredByLayoutKey`** 排除 overlay？
- [ ] `FillLayout` 多子重叠是否符合预期？单 child 是否更简单？
