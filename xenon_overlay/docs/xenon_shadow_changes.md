# Xenon Shadow 提交整理

本文整理 `feature/xenon-overlay` 最近几次 shadow 相关提交的内容，并按实现层级归类说明。当前分支中，早前讨论过的 `76032aa` 已经被 amend，最终以 `836ad8d` 为准。

## 范围

纳入本文的提交：

| 提交 | 标题 | 主要内容 |
|---|---|---|
| `633751b` | 添加 `chrome://xenon-ui` 测试页面，包含阴影弹窗 | 建立 WebUI 测试入口、Widget/View 阴影测试窗口、frameless 阴影 wrapper |
| `0f5707a` | 菜单、Bubble、WebBubble 支持自定义圆角和阴影，WebDialog 支持缩放 | 将 shadow 能力接入实际 UI：菜单、CommonBubble、WebUIBubble、WebDialog |
| `836ad8d` | 菜单和 Bubble 阴影支持多样式同步 | 引入统一 shadow 参数模型，并同步到菜单、CommonBubble、WebUIBubble 测试入口 |

未纳入主线的相邻提交：

| 提交 | 原因 |
|---|---|
| `9828f3f` | 主题是内嵌字体、加载字体、toast 提示框，不属于 shadow 主线 |

## 总体目标

这几次提交围绕同一个目标展开：把 Xenon 浮层类 UI 的圆角与阴影从零散测试推进到可复用、可调试、可对齐的实现。

目标拆成四类：

| 分类 | 目标 |
|---|---|
| 测试基础设施 | 在 `chrome://xenon-ui` 中直接打开 Widget/View/Menu/Bubble/WebUIBubble 阴影样例 |
| 原生窗口阴影 | 验证 `Widget::InitParams::ShadowType`、无边框窗口、DWM 圆角、透明窗口的行为差异 |
| Views 层阴影 | 对比 `views::BubbleBorder`、`views::ViewShadow`、`ui::Shadow`、自绘 box-shadow 的能力边界 |
| 业务组件接入 | 让 `XenonMenuRunner`、`XenonCommonBubble`、`WebUIBubbleManager` 共享同一组 shadow 参数 |

## Shadow 类型分类

### Widget 原生阴影

对应 Chromium 的窗口级配置：

- `views::Widget::InitParams::ShadowType::kDefault`
- `views::Widget::InitParams::ShadowType::kNone`
- `views::Widget::InitParams::ShadowType::kDrop`

特点：

- 作用在 native Widget/window 层。
- 适合普通顶层窗口的默认平台阴影。
- 对无边框、透明、圆角窗口的可控性较弱，Windows 上尤其容易受到 DWM 和 HWND style 影响。

相关文件：

- `xenon_overlay/chrome/browser/ui/xenon_shadow_test_window.{h,cc}`
- `xenon_overlay/chrome/browser/ui/xenon_frameless_shadow_view.{h,cc}`

### BubbleBorder 阴影

对应 `views::BubbleBorder` 的标准气泡阴影。

特点：

- 与 Bubble 的箭头、anchor、bounds、insets、圆角配套。
- 适合菜单、气泡、浮层类 UI。
- 使用 `set_md_shadow_elevation()` 调整阴影高度。
- 不直接表达 CSS 式 `offset / spread / color / opacity` 全量参数。

当前统一参数中，默认类型就是 `kBubbleBorder`。

### ViewShadow 阴影

对应 `views::ViewShadow`。

特点：

- 绑定到某个 `views::View`，通常要求目标 View paint-to-layer。
- `ViewShadow` 内部持有 `ui::Shadow`，会跟随目标 View bounds。
- 适合普通 Views 内容块，不适合所有 Popup/Menu 直接套用，因为 popup 的内容结构和 insets 会影响阴影位置。

在菜单场景中，当前实现会优先找到 `MenuScrollViewContainer` 的背景子 View，将阴影挂到背景 View 上；没有背景子 View 时才退回内容 View。

### CompositorShadow 阴影

对应手动创建的 `ui::Shadow`。

特点：

- 自己创建 shadow layer，使用 `AddLayerToRegion(..., views::LayerRegion::kBelow)` 放到内容层下方。
- 必须显式调用 `SetContentBounds()`，在 View bounds 改变时同步。
- 可控制颜色、透明度、偏移，但维护成本比 `BubbleBorder` 和 `ViewShadow` 高。

相关实现：

- `XenonWebDialogView::SetupFramelessCompositorShadow()`
- `MenuShadowModifier` 的 `kCompositorShadow` 分支
- `XenonShadowTestWindow` 的 `ManualCompositorShadowDemo`

### BoxShadow 自绘阴影

对应 `XenonBoxShadowBorder`，用于模拟 CSS `box-shadow`。

特点：

- 继承 `views::Border`。
- 使用 `cc::DropShadowPaintFilter` 绘制圆角矩形阴影。
- 支持 `x_offset / y_offset / blur_radius / spread_radius / color / opacity`。
- `GetInsets()` 根据 blur、spread、offset 计算外扩，避免阴影被裁剪。
- `Paint()` 使用 `kDrawShadowAndForeground`，同时绘制阴影和圆角背景。

相关文件：

- `xenon_overlay/chrome/browser/ui/xenon_menu_shadow_border.{h,cc}`

### None

统一参数中的 `kNone` 表示禁用阴影。

当前行为：

- 菜单侧设置 `views::BubbleBorder::NO_SHADOW`。
- 保留 16px 圆角。
- 仍然需要处理透明背景和 bounds，否则菜单内容可能因原生 shadow/border 策略出现差异。

## 统一参数模型

`836ad8d` 新增 `XenonMenuShadow`，作为菜单、CommonBubble、WebUIBubble 的统一 shadow 参数。

文件：

- `xenon_overlay/chrome/browser/ui/xenon_menu_shadow.h`

结构：

```cpp
enum class ShadowStyle {
  kNone,
  kBubbleBorder,
  kViewShadow,
  kCompositorShadow,
  kBoxShadow,
};

struct XenonMenuShadow {
  ShadowStyle style = ShadowStyle::kBubbleBorder;
  int elevation = 12;
  double opacity = 0.2;
  int x_offset = 0;
  int y_offset = 0;
  int spread = 0;
  std::string color_hex = "#000000";
};
```

默认值说明：

| 字段 | 默认值 | 说明 |
|---|---:|---|
| `style` | `kBubbleBorder` | 默认使用 Chromium Bubble 标准阴影 |
| `elevation` | `12` | 原生菜单默认阴影高度 |
| `opacity` | `0.2` | 自定义阴影默认透明度 |
| `x_offset` | `0` | X 方向偏移 |
| `y_offset` | `0` | Y 方向偏移 |
| `spread` | `0` | CSS box-shadow 风格扩张半径 |
| `color_hex` | `#000000` | 阴影基底色 |

## 参数传递链路

WebUI 测试面板向 C++ 发送统一数组：

```text
[style, elevation, opacity, offsetX, offsetY, spread, colorHex]
```

C++ 侧由 `ParseXenonMenuShadow()` 转成 `XenonMenuShadow`：

- `style` 字符串映射到 `ShadowStyle`。
- `elevation` 控制 Bubble elevation、ViewShadow elevation、CompositorShadow elevation、BoxShadow blur。
- `opacity` 控制自定义阴影透明度。
- `offsetX / offsetY` 控制自定义阴影偏移。
- `spread` 主要用于 box-shadow 风格或自定义绘制阴影。
- `colorHex` 传递 `#RRGGBB`。

相关文件：

- `xenon_overlay/chrome/browser/ui/webui/xenon_ui_controller.cc`
- `xenon_overlay/resources/webui/xenon_ui/xenon_ui.{html,js}`

注意：

- JS 侧的 `parseBoxShadow()` 支持 `#RGB`、`#RGBA`、`#RRGGBB`、`#RRGGBBAA`、`rgb()`、`rgba()` 和部分命名颜色。
- C++ 侧 `ParseHexColor()` 目前主要解析 `#RRGGBB`，opacity 通过单独字段传入。

## 提交一：`633751b` 测试基础设施

该提交建立 shadow 调试入口，主要解决“能看到、能对比、能复现”的问题。

### `chrome://xenon-ui`

新增 WebUI 页面：

- `xenon_overlay/chrome/browser/ui/webui/xenon_ui_controller.{h,cc}`
- `xenon_overlay/resources/webui/xenon_ui/xenon_ui.{html,css,js}`
- `xenon_overlay/resources/xenon_resources.grd`

能力：

- 页面中提供按钮触发原生 dialog、Widget shadow test、View shadow test。
- C++ 通过 `chrome.send()` message handler 响应 WebUI 操作。
- 日志区记录 WebUI 到 C++ 的请求和回调。

### `XenonShadowTestWindow`

新增开发测试窗口：

- Widget 阴影测试：列出 `kDefault / kNone / kDrop`。
- 普通 Widget 样例：直接使用 `Widget::InitParams::shadow_type`。
- 无边框 Widget 样例：使用 `TYPE_WINDOW_FRAMELESS`、`remove_standard_frame=true`、透明窗口背景，并通过 `XenonFramelessShadowView` 负责内容外阴影。
- View 阴影测试：对比 `ui::Shadow`、`views::ViewShadow`、`views::BubbleBorder`。

### `XenonFramelessShadowView`

新增 frameless window 的 shadow wrapper。

设计点：

- 外层 View 预留 `shadow_insets`，防止阴影被 Widget bounds 裁剪。
- 内容 View 放在 inset 后的区域内。
- 可选挂 `views::ViewShadow` 到内容 View。
- 用于规避 Windows frameless/native shadow 与 Views 圆角混用时的视觉问题。

典型 Widget 配置：

```cpp
params.type = views::Widget::InitParams::TYPE_WINDOW_FRAMELESS;
params.remove_standard_frame = true;
params.opacity = views::Widget::InitParams::WindowOpacity::kTranslucent;
params.shadow_type = views::Widget::InitParams::ShadowType::kNone;
```

## 提交二：`0f5707a` 业务组件初步接入

该提交把测试阶段得到的 shadow/圆角经验接入到菜单、Bubble、WebDialog 等实际 UI。

### `XenonMenuRunner`

新增菜单封装：

- 文件：`xenon_overlay/chrome/browser/ui/xenon_menu_runner.{h,cc}`
- 统一 16px 圆角：`kCornerRadius = 16`
- 封装 `views::MenuRunner::RunMenuAt()`
- 调用 Chromium 菜单 runner 时传入 `gfx::RoundedCornersF(kCornerRadius)`

后续在 `836ad8d` 中，该类继续扩展为接收 `XenonMenuShadow`。

### `XenonCommonBubble`

将 CommonBubble 统一为接近 Chrome profile menu 的气泡风格：

- 固定宽度约 280。
- 内部 padding 对齐 profile menu 常见尺寸。
- 16px 圆角。
- `BubbleDialogDelegate` 使用 `DIALOG_SHADOW`。
- 提供 `Show()` 和 `ShowAt()` 两种入口。
- 提供 `ApplyStyle()`，用于对已创建的 Bubble 重新设置 frame border。

### WebUIBubble 接入

通过 `XenonCommonBubble::ConfigureWebUIBubbleManager()` 和 `ApplyWebUIBubbleStyle()` 适配 `WebUIBubbleManager`。

限制：

- Chromium 142 的 `WebUIBubbleManager` 只暴露 widget initialization callback，不直接把 `WebUIBubbleDialogView` 传给配置函数。
- 因此当前做法是在 `ShowBubble()` 成功之后，通过 `bubble_view_for_testing()` 取得 bubble view，再应用样式。

### `XenonWebDialog`

该提交同时让 WebDialog 支持：

- frameless/native frame 开关；
- DWM 圆角开关；
- resize 开关；
- minimize/maximize/always-on-top/taskbar 等窗口行为参数。

Windows 上关键分支：

| 场景 | 策略 |
|---|---|
| `frame=true` | 使用原生窗口 frame |
| `frame=false && dwm=true && Win11+` | 使用 DWM 圆角，保持 opaque，避免 translucent 破坏原生 resize |
| `frame=false && !UseDwmRoundedCorners()` | 使用 translucent window，自绘 compositor shadow，并由 Views 手动处理 resize |

无边框自绘阴影：

- `kFramelessCompositorShadowElevation = 8`
- 通过 `gfx::ShadowValue::MakeMdShadowValues()` 计算 shadow margin。
- `GetDialogSize()` 会把 shadow margin 加到 dialog 外层尺寸。
- `XenonWebDialogView::SetupFramelessCompositorShadow()` 创建 `ui::Shadow`。
- `OnBoundsChanged()` 中同步 shadow bounds。

手动 resize：

- 非 DWM translucent 路径下，Chromium Windows 会移除 `WS_THICKFRAME`，即使 hit-test 返回 resize 区域也无法原生 resize。
- 因此 `XenonWebDialogView` 在 resize border 内拦截鼠标事件，自己计算并设置 Widget bounds。

## 提交三：`836ad8d` 多样式同步

该提交把 shadow 配置从“组件各自处理”升级为“同一组参数驱动多个组件”。

### 新增文件

| 文件 | 作用 |
|---|---|
| `xenon_menu_shadow.h` | 统一 shadow 类型与参数 |
| `xenon_menu_shadow_border.{h,cc}` | 自绘 CSS-like box-shadow border |
| `xenon_menu_shadow_modifier.{h,cc}` | 在菜单 `MenuHost` 创建后修改菜单阴影、背景、边框、尺寸 |

### `XenonMenuRunner` 的 shadow 流程

流程：

1. `RunMenuAt()` 接收 `const XenonMenuShadow& shadow`。
2. 如果 parent widget 存在，创建 `MenuShadowModifier` 并观察 parent widget。
3. 调用 Chromium `views::MenuRunner::RunMenuAt()`。
4. `MenuShadowModifier::OnWidgetChildAdded()` 捕获名称为 `MenuHost` 的子 Widget。
5. 下一轮 task 执行 `ApplyShadow()`，避免菜单内容尚未初始化完毕。
6. `ApplyShadow()` 根据 `shadow.style` 修改 content view / background view / border / layer。
7. 重新计算 preferred size，必要时根据新旧 insets 差值移动 Widget，保持视觉位置稳定。

生命周期安全：

- 延迟任务只绑定 `WeakPtr<MenuShadowModifier>`，不再捕获 `MenuHost` 裸指针。
- `OnWidgetDestroying()` 中会 `InvalidateWeakPtrs()`，避免 Widget 销毁后 pending task 继续访问。
- modifier 在 menu widget 或 parent widget 销毁后用 `DeleteSoon()` 自删除。

anchor 处理：

- 非 `kBubbleBorder` 或 elevation 为 0 时，会把部分普通 anchor 映射为 bubble anchor。
- 目的：让 `MenuHost::InitMenuHost` 走 bubble border 路径，从而得到透明窗口和无 native shadow 的基础条件，方便自定义阴影绘制。

### 菜单各 style 行为

| Style | 菜单侧实现 |
|---|---|
| `kNone` | 设置 `BubbleBorder::NO_SHADOW`，保留 16px 圆角 |
| `kBubbleBorder` | 如果内容是 `MenuScrollViewContainer` 且已有 BubbleBorder，则只调整 `md_shadow_elevation` |
| `kViewShadow` | 优先给 `MenuScrollViewContainer` 的背景子 View 挂 `views::ViewShadow`；否则挂到 content view |
| `kCompositorShadow` | 创建 `ui::Shadow` layer，放到目标 View 下方，并在 bounds 变化时同步 content bounds |
| `kBoxShadow` | 给 content view 设置 `XenonBoxShadowBorder`，使用 `DropShadowPaintFilter` 自绘阴影和背景 |

菜单位置修正：

- 改动 border/insets 后，content preferred size 会变化。
- `MenuShadowModifier` 保存旧 insets，应用样式后计算新 preferred size。
- 如果 size 改变，使用新旧左/上 inset 差值修正 Widget 的 `x/y`，避免阴影样式切换导致菜单整体偏移。

### `XenonCommonBubble` 多样式实现

`XenonCommonBubble` 改为接受 `XenonMenuShadow`：

- constructor 保存 `shadow_`。
- `Show()` / `ShowAt()` 增加 shadow 参数，默认值为 `XenonMenuShadow()`。
- `ApplyStyle()` / `ApplyWebUIBubbleStyle()` 也接收 shadow 参数。

核心是自定义 `XenonBubbleShadowBorder`：

- 继承 `views::BubbleBorder`，保留 Bubble 原有 arrow、anchor、layout 逻辑。
- `kBubbleBorder` 继续使用 BubbleBorder 原生阴影。
- `kViewShadow / kCompositorShadow / kBoxShadow` 在 border 的 `Paint()` 中统一自绘 shadow。
- `GetInsets()` 返回 layout insets 与 custom shadow insets 的 max，避免阴影被裁剪。
- `GetBounds()` 先用普通 BubbleBorder 计算 anchor/bounds，再按额外 paint insets 外扩，避免 bubble 位置漂移。
- `PaintCustomShadow()` 先 clip 掉内容区域，再用 `DropShadowPaintFilter::kDrawShadowOnly` 绘制圆角阴影。

这一点是 CommonBubble 稳定性的关键：它没有把 `ViewShadow` 或 `ui::Shadow` 直接挂到 BubbleFrameView 上，而是在 BubbleBorder 内统一处理 shadow paint 和 geometry。

### WebUIBubble 同步

`HandleShowXenonWebUIBubble()` 与 `HandleShowXenonCommonBubble()` 使用同一个 `ParseXenonMenuShadow(args)`。

流程：

1. WebUI 按钮收集当前 shadow 参数。
2. `chrome.send('showXenonWebUIBubble', shadow.args)`。
3. C++ 创建或复用 `WebUIBubbleManager`。
4. `ShowBubble()` 成功后调用 `XenonCommonBubble::ApplyWebUIBubbleStyle(manager, shadow)`。
5. WebUIBubble 与 CommonBubble 获得相同圆角和阴影参数。

## WebUI 测试面板

相关文件：

- `xenon_overlay/resources/webui/xenon_ui/xenon_ui.html`
- `xenon_overlay/resources/webui/xenon_ui/xenon_ui.js`
- `xenon_overlay/chrome/browser/ui/webui/xenon_ui_controller.cc`

面板控件：

| 控件 | 作用 |
|---|---|
| `menu-shadow-style` | 选择 `kBubbleBorder / kViewShadow / kCompositorShadow / kBoxShadow / kNone` |
| `menu-shadow-elevation` | 选择 3、6、12、16、24 等 elevation/blur |
| `menu-shadow-opacity` | 选择自定义阴影透明度 |
| `menu-shadow-offset-x/y` | 选择阴影偏移 |
| `menu-shadow-spread` | 选择 CSS-like spread |
| `menu-shadow-color` | 选择阴影颜色 |
| `menu-shadow-css` | 输入 CSS `box-shadow` 字符串，仅在 `kBoxShadow` 下显示 |

测试按钮：

| 按钮 | C++ message | 测试对象 |
|---|---|---|
| 测试 `XenonMenuRunner` | `showXenonMenuRunner` | 原生菜单 |
| 测试 `XenonCommonBubble` | `showXenonCommonBubble` | 原生 Views Bubble |
| 测试 `WebUI Bubble` | `showXenonWebUIBubble` | Chromium `WebUIBubbleManager` 创建的 WebUI Bubble |

JS 同步逻辑：

- `getCurrentShadowArgs()` 统一收集当前参数。
- `kBoxShadow` 下会解析 CSS 字符串，并把 blur/offset/spread/color/opacity 拆成 C++ 参数。
- 修改颜色或 opacity 时，会同步更新 CSS `box-shadow` 输入框。
- 修改 CSS `box-shadow` 时，会反向同步 color picker 和 opacity。
- `kNone` 会隐藏所有 shadow 数值控件。
- `kBoxShadow` 会隐藏分散的 offset/elevation/spread 控件，显示 CSS 输入框。

## 使用建议

| 需求 | 推荐方式 | 原因 |
|---|---|---|
| 标准菜单/气泡阴影 | `kBubbleBorder` | 与 Chromium Bubble 几何和平台表现最一致 |
| 禁用阴影但保留圆角 | `kNone` | 不引入额外 layer 或自绘 |
| 验证 View 级 shadow 行为 | `kViewShadow` | 跟随 View bounds，适合普通 Views 内容 |
| 需要手动控制 layer/bounds | `kCompositorShadow` | 可直接控制 `ui::Shadow` layer |
| 对齐 CSS `box-shadow` 参数 | `kBoxShadow` | 支持 offset、blur、spread、color、opacity |
| 无边框 WebDialog | DWM 圆角或 compositor shadow 分支 | 根据 Windows/DWM/resize 约束选择 |

默认生产路径建议优先用 `kBubbleBorder`。只有在需要 CSS-like 视觉、特殊颜色/透明度/偏移时，再选择自绘或 compositor 路径。

## 关键约束与注意事项

1. 菜单的 `MenuHost` 是异步创建的，必须等 `OnWidgetChildAdded()` 捕获后再修改内容视图。
2. 延迟 `ApplyShadow()` 不能捕获 raw `views::Widget*`，否则菜单快速关闭时可能访问悬空 Widget。
3. `ViewShadow` 和 `ui::Shadow` 都要求目标 View/layer 透明，否则阴影容易被背景遮住。
4. 自定义阴影会改变 insets，需要同步 preferred size 和 Widget bounds，否则菜单或 bubble 会发生位置偏移。
5. WebUIBubble 需要在 `ShowBubble()` 后再应用样式，因为 Chromium 142 的 manager 初始化回调拿不到 bubble view。
6. DWM 圆角和 translucent window 在 Windows 上不能简单混用；非 DWM frameless 路径需要手动 resize。
7. C++ 颜色解析目前以 `#RRGGBB + opacity` 为主，WebUI 输入会尽量归一化到这个格式。

## 构建与验证

本轮 shadow 修正后已验证：

```powershell
git diff --check
node --check xenon_overlay\resources\webui\xenon_ui\xenon_ui.js
autoninja -C out\Debug_64 xenon_overlay/chrome/browser:browser
```

建议手工验证路径：

1. 打开 `chrome://xenon-ui`。
2. 在“原生 Views 组件测试”中逐个选择五种 shadow style。
3. 分别点击 `测试 XenonMenuRunner`、`测试 XenonCommonBubble`、`测试 WebUI Bubble`。
4. 重点观察：
   - 阴影是否可见；
   - bubble/menu 是否仍贴在页面左上测试区域；
   - 自定义 offset/spread 是否被裁剪；
   - `kNone` 是否无阴影但保留圆角；
   - `kBoxShadow` 的 CSS 输入是否能同步颜色和透明度。

## 维护清单

新增 shadow 类型或参数时，需要同步更新：

1. `xenon_menu_shadow.h` 的 enum/struct/default。
2. `xenon_ui_controller.cc` 的 `ShadowStyleFromString()` 和 `ParseXenonMenuShadow()`。
3. `xenon_ui.html` 的控件。
4. `xenon_ui.js` 的参数收集、显示隐藏和日志。
5. `MenuShadowModifier` 的 style 分支。
6. `XenonCommonBubble` 的 custom border paint / insets / bounds 逻辑。
7. `BUILD.gn` 中新增源文件。

