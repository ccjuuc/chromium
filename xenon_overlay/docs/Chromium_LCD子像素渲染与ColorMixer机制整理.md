# Chromium Views LCD 子像素渲染与 ColorMixer 机制整理

本文基于 Chromium 142 源码，整理 Views 文本的 LCD 子像素渲染、字体“看起来偏粗”的排查方法，以及 `ColorId`、`ColorMixer`、`ColorProvider` 的工作链路。文末结合 Xenon 当前代码给出迁移建议。

核心源码：

| 文件 | 作用 |
|---|---|
| `ui/views/controls/label.{h,cc}` | `views::Label` 字体、颜色、背景色和子像素渲染控制 |
| `ui/views/controls/button/label_button.{h,cc}` | `LabelButton` 对内部 Label 的字体和颜色封装 |
| `ui/gfx/render_text.{h,cc}` | 文本布局与最终绘制参数，包括 subpixel suppression |
| `ui/color/color_id.h` | Views/UI 层语义颜色 ID |
| `ui/color/color_mixer.{h,cc}` | 单层颜色 recipe 映射与向前级 Mixer 回退 |
| `ui/color/color_recipe.{h,cc}` | 多个 `ColorTransform` 的组合 |
| `ui/color/color_transform.{h,cc}` | alias、透明度、混色、对比度等颜色变换 |
| `ui/color/color_provider.{h,cc}` | Mixer 管线、颜色查询与结果缓存 |
| `ui/color/color_provider_key.h` | light/dark、高对比度、系统主题等 Provider 维度 |
| `ui/color/color_provider_manager.{h,cc}` | 按 `ColorProviderKey` 创建和缓存 Provider |
| `ui/color/color_mixers.cc` | UI 基础 Mixer 注册顺序 |
| `chrome/browser/ui/color/chrome_color_mixers.cc` | Chrome 业务 Mixer 注册顺序 |

## 1. 先记结论

1. 字体视觉粗细不只由 `gfx::Font::Weight` 决定。抗锯齿模式、前景色、背景透明度、合成层和缩放比例都会改变观感。
2. LCD 子像素渲染依赖稳定且不透明的背景。透明 Layer、opacity 动画或中间离屏合成通常不适合 LCD 文本。
3. `Label::SetSkipSubpixelRenderingOpacityCheck(true)` 只跳过 DCHECK，不会强制启用 LCD，也不会让透明背景变安全。
4. `ColorMixer` 是语义颜色求值管线，不是 CSS。ColorId 本身不代表 background、border 或 shadow；最终效果取决于调用方把颜色用于哪里。
5. 颜色越深、对比度越高，NORMAL 字重也可能看起来更粗。比较字体前要先保证两处文字颜色和抗锯齿方式一致。
6. 组件专属颜色应定义专属 ColorId。修改全局 ColorId 会影响所有消费者，容易出现扩展弹窗、登录框等无关区域一起变色。

## 2. LCD 子像素渲染是什么

LCD 屏幕的一个逻辑像素通常由 R、G、B 三个子像素组成。子像素文本渲染会分别控制三个颜色通道，从水平方向获得比整像素灰度抗锯齿更高的有效分辨率。

常见视觉差异：

| 模式 | 特征 | 常见观感 |
|---|---|---|
| LCD/子像素抗锯齿 | 分别使用 RGB 子像素；依赖背景颜色 | 边缘更锐利，也可能显得更黑、更粗、更紧 |
| 灰度抗锯齿 | 每个像素统一 alpha | 边缘更柔和，透明合成更稳定，常显得更细 |
| 无抗锯齿 | 像素级硬边 | 小字号锯齿明显，通常不用于普通 UI 文本 |

“LCD”描述的是渲染策略，不等于最终一定由 DirectWrite ClearType 以固定方式输出。Skia、平台字体后端、设备缩放和 Compositor 都可能继续影响最终结果。

## 3. Views 文本渲染链路

简化调用链：

```text
views::Label
  -> gfx::RenderText
     -> 字体、颜色、背景色、subpixel suppression
        -> gfx::Canvas / Skia / 平台文字后端
           -> Compositor / ui::Layer
              -> 屏幕
```

`Label` 不直接根据 `FontList` 决定所有视觉结果。它还会把背景色和子像素开关传给 `RenderText`。

### 3.1 默认开关

`views::Label` 默认允许子像素渲染：

```cpp
bool subpixel_rendering_enabled_ = true;
```

公开接口：

```cpp
label->SetSubpixelRenderingEnabled(false);
```

`LabelButton` 也提供转发接口：

```cpp
button->SetTextSubpixelRenderingEnabled(false);
```

两者最终修改的是内部 Label。

### 3.2 背景必须不透明

`Label::ApplyTextColors()` 的核心判断：

```cpp
const bool subpixel_rendering_enabled =
    subpixel_rendering_enabled_ && IsOpaque(GetBackgroundColor());
display_text_->set_subpixel_rendering_suppressed(
    !subpixel_rendering_enabled);
```

所以 LCD 生效需要同时满足：

- `subpixel_rendering_enabled_ == true`
- `Label::GetBackgroundColor()` 为不透明颜色

如果 Label 没显式设置背景色，主题刷新时默认背景通常来自 `ui::kColorDialogBackground`。这不一定等于控件实际绘制区域的背景色。自定义控件应让 Label 知道真实背景：

```cpp
label->SetBackgroundColor(kColorMyComponentBackground);
```

此调用主要告诉文字渲染器背景颜色，不等同于给 Label 安装 `views::Background`。实际背景仍要由父 View 或 Label 自身正确绘制。

### 3.3 透明 Layer 和动画

以下场景应重点检查：

- `SetPaintToLayer()` 后调用 `layer()->SetFillsBoundsOpaquely(false)`
- View 或祖先 View 做 opacity 动画
- 文字跟随缩放、旋转或非整数平移
- Bubble、Toast、阴影窗口使用透明 Widget
- 文字先绘制到离屏纹理，再与背景混合

LCD 文本的 RGB 结果建立在“文字直接画到已知不透明背景”这一前提上。先画到透明纹理再整体混合，会让 RGB 子像素参与错误的二次混色。此时应禁用子像素渲染：

```cpp
label->SetSubpixelRenderingEnabled(false);
```

不要为了通过 DCHECK 直接使用：

```cpp
label->SetSkipSubpixelRenderingOpacityCheck(true);
```

该接口只跳过 `Label::PaintText()` 的不透明性检查。它不会修改 `RenderText::subpixel_rendering_suppressed()`，也不会修复真实的透明合成问题。只有能证明文字实际画在不透明区域、但 Views 检测不到时，才适合设置该标志。

### 3.4 Debug DCHECK 做了什么

Debug 构建中，`Label::PaintText()` 会沿父 View 向上检查：

- 是否存在不透明 `views::Background`
- 遇到 Layer 时，该 Layer 是否声明 `fills_bounds_opaquely()`

这是一种近似检查，不理解所有自定义绘制路径。检查失败时先确认真实背景和 Layer 结构，不要把跳过 DCHECK 当成视觉修复。

## 4. 为什么 NORMAL 字重仍可能显粗

### 4.1 抗锯齿方式不同

两处文字即使都是：

```cpp
gfx::Font::Weight::NORMAL
```

一处使用 LCD，另一处使用灰度抗锯齿，截图中仍可能明显不同。Windows 小字号中文尤其容易出现这种观感差异。

### 4.2 前景色不同

深色文字会覆盖更多视觉重量。例如 `#1F2530` 与较浅的 secondary/subtle 文本色，即使字体完全一致，前者也更像粗体。

检查时必须同时比较：

- 最终 `SkColor`
- alpha
- 背景颜色
- 字号
- 字重
- 子像素渲染开关

### 4.3 DPI 与截图缩放

125%、150% 等设备缩放下，DIP 会映射到非整数物理像素。Canvas snapping、Layer 栅格化比例和截图软件二次缩放都可能改变笔画。

不要只凭缩放后的截图判断字体 Weight。优先在相同窗口、相同 DSF、相同背景和相同截图比例下对比。

### 4.4 文本被重复绘制

如果字体参数、颜色和抗锯齿均一致，但文字仍异常粗，还要检查：

- 是否有两个 Label 重叠
- 自定义 `OnPaint()` 是否再次绘制同一段文字
- 动画旧 Layer 是否未销毁
- 同一 View 是否同时在多个 Layer region 中绘制

## 5. 文本问题排查顺序

推荐按以下顺序排查，避免只改 FontList：

| 步骤 | 检查项 | 典型接口 |
|---|---|---|
| 1 | 最终控件是否找对 | 文案资源 ID、`SetText()`、`SetLabel()` 调用链 |
| 2 | 字号与字重 | `label->font_list().GetFontSize()`、`GetFontWeight()` |
| 3 | TextContext/TextStyle | `GetTextContext()`、`GetTextStyle()` |
| 4 | 最终文字颜色 | `GetEnabledColor()`、ColorId 的 Mixer 结果 |
| 5 | Label 背景是否不透明 | `GetBackgroundColor()`、`SetBackgroundColor()` |
| 6 | 子像素开关 | `GetSubpixelRenderingEnabled()` |
| 7 | Layer/opacity/transform | `View::layer()`、`fills_bounds_opaquely()` |
| 8 | 主题刷新覆盖 | `OnThemeChanged()` 的调用顺序 |
| 9 | 是否重叠绘制 | View 层级、Layer 树、自定义 Paint |

### 5.1 推荐诊断日志

调试阶段可以临时输出：

```cpp
const gfx::FontList& font = label->font_list();
LOG(ERROR) << "size=" << font.GetFontSize()
           << " weight=" << static_cast<int>(font.GetFontWeight())
           << " subpixel=" << label->GetSubpixelRenderingEnabled()
           << " foreground=" << SkColorName(label->GetEnabledColor())
           << " background=" << SkColorName(label->GetBackgroundColor());
```

日志仅用于定位，提交前删除。

## 6. 推荐文本配置模式

### 6.1 透明或动画控件

```cpp
label->SetTextStyle(views::style::STYLE_BODY_4);
label->SetEnabledColor(kColorMyComponentText);
label->SetSubpixelRenderingEnabled(false);
```

适合 Toast、Bubble 动画、透明窗口、opacity 动画和复杂 Layer 结构。

### 6.2 确定画在不透明背景上的控件

```cpp
label->SetTextStyle(views::style::STYLE_BODY_4);
label->SetEnabledColor(kColorMyComponentText);
label->SetBackgroundColor(kColorMyComponentBackground);
```

只有实际绘制背景确实与 `kColorMyComponentBackground` 一致时，才应保留 LCD 子像素渲染。

### 6.3 LabelButton

```cpp
button->SetEnabledTextColors(kColorMyComponentText);
button->SetTextSubpixelRenderingEnabled(false);
```

如果只想修改文字，不要使用会同时影响图标和文字的统一 foreground 接口。先确认目标类中 `GetForegroundColor()` 是否也被图标绘制路径使用。

### 6.4 主题刷新

基类 `OnThemeChanged()` 可能重建 LabelButton 的状态色。必须先调用基类，再恢复组件专属状态：

```cpp
void MyButton::OnThemeChanged() {
  BaseButton::OnThemeChanged();
  SetEnabledTextColors(kColorMyComponentText);
}
```

如果直接使用 `Label::SetEnabledColor(ColorId)`，Label 会在主题变化时重新解析该 ColorId。若基类主动覆盖 requested color，仍需在子类中重设。

## 7. ColorId、ColorMixer、ColorProvider 的关系

三者职责不同：

| 类型 | 职责 |
|---|---|
| `ColorId` | 语义名称，例如“主文字”“弹窗背景”“危险状态图标” |
| `ColorMixer` | 定义某批 ColorId 如何从常量或其他 ColorId 计算 |
| `ColorProvider` | 持有完整 Mixer 管线，按 ID 求最终颜色并缓存 |
| `ColorProviderKey` | 描述 light/dark、高对比度、系统主题等环境 |
| `ColorProviderManager` | 按 Key 创建并缓存 ColorProvider |

查询链：

```text
View::GetColorProvider()
  -> ColorProvider::GetColor(ColorId)
     -> 最后加入的 ColorMixer
        -> 当前 Mixer 有 recipe：计算结果
        -> 当前 Mixer 无 recipe：查询 previous mixer
           -> ...
              -> 无定义：gfx::kPlaceholderColor
```

## 8. Mixer 顺序与覆盖规则

`ColorProvider::AddMixer()` 添加的新 Mixer 位于普通管线末端。查询从最后加入的 Mixer 开始，因此：

```cpp
provider.AddMixer()[kColorExample] = {SK_ColorRED};
provider.AddMixer()[kColorExample] = {SK_ColorBLUE};
```

最终结果是蓝色。后加入的 Mixer 覆盖前面的定义；若后级没有该 ID，则回退到前级。

Chromium UI 基础顺序位于 `ui/color/color_mixers.cc`：

```text
Ref
-> Sys
-> CoreDefault
-> NativeCore
-> UI
-> MaterialUI
-> FluentUI（条件启用）
-> NativeUI
-> CSS System
-> Native Postprocessing
```

Chrome 业务层在此之后追加 Components 和 Chrome Mixers。`chrome_color_mixers.cc` 中 Native Chrome Mixer 明确要求靠后，以覆盖前面的 Chrome recipe；自定义 Theme 和 `app_controller` Mixer 又在其后追加。

### 8.1 Postprocessing Mixer

`AddPostprocessingMixer()` 位于整条管线最末端，适合强制对比度、系统高对比度等最终处理。

普通组件颜色不要放进 Postprocessing Mixer。否则很容易无意中改写整个 UI 的最终输出。

### 8.2 颜色缓存

`ColorProvider::GetColor()` 会把结果缓存到 `color_map_`。新增 Mixer 时缓存会清空，但 Provider 设计目标是在初始化后保持不变。

不要在 UI 已经开始查询颜色后再动态修改 Mixer recipe。`ColorMixer::operator[]` 本身不会通知 Provider 清理已有结果。主题变化应通过新的 `ColorProviderKey` 或重建 Provider 处理。

## 9. ColorRecipe 常见写法

### 9.1 固定颜色

```cpp
mixer[kColorMyText] = {SkColorSetRGB(0x1F, 0x25, 0x30)};
```

### 9.2 Alias 到已有语义色

```cpp
mixer[kColorMyText] = {ui::kColorSysOnSurface};
```

Alias 比复制固定色更容易自动支持 dark mode 和高对比度。

### 9.3 设置 alpha

```cpp
mixer[kColorMyDisabledText] =
    ui::SetAlpha(kColorMyText, 0x61);
```

### 9.4 前景叠加到背景

```cpp
mixer[kColorMyHoverBackground] = ui::AlphaBlend(
    kColorMyAccent, kColorMyBackground, 0x14);
```

### 9.5 保证最低对比度

```cpp
mixer[kColorMyReadableText] = ui::BlendForMinContrast(
    kColorMyText, kColorMyBackground);
```

不要把对比度变换和“字重”混为一谈。提高文字与背景对比度会让文字视觉变重，但不会修改 Font Weight。

## 10. Light、Dark 与高对比度

Mixer 函数通常接收：

```cpp
void AddMyColorMixer(ui::ColorProvider* provider,
                     const ui::ColorProviderKey& key);
```

可根据 Key 选择颜色：

```cpp
ui::ColorMixer& mixer = provider->AddMixer();

const bool dark =
    key.color_mode == ui::ColorProviderKey::ColorMode::kDark;
mixer[kColorMyBackground] = {
    dark ? SkColorSetRGB(0x20, 0x22, 0x26) : SK_ColorWHITE};
```

还要考虑：

- `key.contrast_mode`
- `key.forced_colors`
- `key.system_theme`
- `key.custom_theme`

如果已有合适的 `ui::kColorSys*` 颜色，优先 alias 系统语义色，减少手写分支。

## 11. 自定义颜色接入建议

推荐流程：

1. 在模块 ColorId 列表中声明组件专属语义 ID。
2. 建立 `AddXenonColorMixer()`，集中定义 light/dark recipe。
3. 在 Chrome ColorProvider 初始化阶段追加 Mixer，确保发生在首个 Provider 查询前。
4. View 中优先传递 ColorId，不要过早解析成 `SkColor`。
5. 为 light、dark、高对比度写 ColorProvider 单元测试。

示意：

```cpp
void AddXenonColorMixer(ui::ColorProvider* provider,
                        const ui::ColorProviderKey& key) {
  ui::ColorMixer& mixer = provider->AddMixer();
  const bool dark =
      key.color_mode == ui::ColorProviderKey::ColorMode::kDark;

  mixer[kColorXenonToastBackground] = {
      dark ? SkColorSetRGB(0x28, 0x2A, 0x30) : SK_ColorWHITE};
  mixer[kColorXenonToastText] = {
      dark ? SkColorSetARGB(0xE0, 0xFF, 0xFF, 0xFF)
           : SkColorSetRGB(0x1D, 0x21, 0x29)};
  mixer[kColorXenonToastAction] = {SkColorSetRGB(0x4E, 0x5C, 0xFF)};
}
```

View 使用：

```cpp
text_label_->SetEnabledColor(kColorXenonToastText);
SetBackground(views::CreateRoundedRectBackground(
    kColorXenonToastBackground, radius));
```

只有绘制 API 只接受 `SkColor` 时才解析：

```cpp
const SkColor color =
    GetColorProvider()->GetColor(kColorXenonToastAction);
flags.setColor(color);
```

## 12. ColorId 不是 CSS 属性

同一个 ColorId 可以被用于文字、背景、边框或阴影。Mixer 只负责返回颜色，不知道调用方语义。

错误示例：

```cpp
SetBackgroundColor(kColorMyShadow);
```

如果 `kColorMyShadow` 是偏红测试色，整个窗口内容区都会偏红。这不是 Mixer 把 shadow 画进了窗口，而是调用方把 shadow 色当作 background 使用。

正确做法：

```cpp
shadow_painter->SetColor(
    GetColorProvider()->GetColor(kColorMyShadow));
SetBackgroundColor(kColorMyDialogBackground);
```

同理，修改一个 Bubble 共用的全局阴影 ColorId，会影响扩展弹窗、登录弹窗等所有消费者。只想修改某一种弹窗时，应定义组件专属 ColorId 或在专属 painter 中处理。

## 13. Xenon 当前代码注意点

### 13.1 Toast 使用硬编码颜色

`xenon_overlay/xenon/chrome/browser/ui/views/xenon_toast.cc` 当前定义：

```cpp
constexpr SkColor kBackgroundColor = SK_ColorWHITE;
constexpr SkColor kTextColor = SkColorSetRGB(0x1D, 0x21, 0x29);
constexpr SkColor kActionColor = SkColorSetRGB(0x4E, 0x5C, 0xFF);
```

这种方式简单，但不会自动响应 dark mode、高对比度和自定义主题。后续建议迁移为：

- `kColorXenonToastBackground`
- `kColorXenonToastText`
- `kColorXenonToastAction`
- `kColorXenonToastActionHover`
- `kColorXenonToastSuccess/Warning/Error`

### 13.2 Toast 的 opacity check

同一文件当前对正文 Label 调用了：

```cpp
text_label_->SetSkipSubpixelRenderingOpacityCheck(true);
```

需要确认 Toast 内容区是否始终由不透明背景覆盖：

- 如果真实背景不透明，应给 Label 设置准确背景色，并保留 LCD。
- 如果 Toast 使用透明 Widget、阴影透明边距或 opacity 动画，应改为 `SetSubpixelRenderingEnabled(false)`。
- 不建议只跳过检查而不确认合成路径。

### 13.3 Common Dialog 使用硬编码文字色

`xenon_overlay/chrome/browser/ui/xenon_common_dialog.cc` 的标题和正文使用固定 `SkColorSetRGB()`。建议迁移为 Dialog 专属 ColorId，避免暗色主题下对比度失效。

### 13.4 Common Bubble 已使用语义背景

`xenon_overlay/chrome/browser/ui/xenon_common_bubble.cc` 使用：

```cpp
SetBackgroundColor(ui::kColorBubbleBackground);
```

这是推荐方向。组件若没有特殊设计要求，应优先复用已有系统语义色。

## 14. 本次“文字偏粗”案例总结

现象：

- 字号与 `gfx::Font::Weight::NORMAL` 已正确设置。
- 文字仍比相邻按钮明显更粗、更挤。
- 重复在 `SetLabel()` 或动画后设置 FontList 没有改善。

定位：

- 文案最终写入的是正确 Label。
- `SetText()` 不会改变字体和抗锯齿设置。
- 视觉差异来自子像素渲染，而非 FontList 被覆盖。

有效修复：

```cpp
label->SetTextStyle(views::style::STYLE_BODY_4);
label->SetSubpixelRenderingEnabled(false);
```

代码清理原则：

- 在 Label 初始化时设置一次字体和渲染模式。
- `SetText()` 后无需重复设置字体。
- 只有确实会覆盖字体的刷新入口才需要专门处理。
- 主题刷新后，仅恢复会被基类覆盖的状态色。
- 图标和文字需要不同颜色时，使用文字专属接口，不修改共用 foreground。

## 15. ColorMixer 故障排查

### 颜色没生效

检查：

1. ColorId 是否真的在 Mixer 中定义。
2. 自定义 Mixer 是否在 Provider 首次创建前注册。
3. 是否有更后加入的 Mixer 覆盖该 ID。
4. View 是否来自预期 Widget 的 ColorProvider。
5. 是否在构造期 `GetColorProvider() == nullptr` 时提前解析。
6. 是否把 ColorId 解析成 SkColor 后长期缓存，导致主题切换不更新。

### 改一个颜色，其他弹窗也变色

原因通常不是 Mixer 异常，而是 ColorId 复用范围过大。用 `rg` 搜索该 ColorId 的所有消费者，确认是否应拆成组件专属 ID。

### Light 正常，Dark 错误

检查 recipe 是否：

- 使用固定浅色常量
- alias 到只适用于浅色的 leaf color
- 忽略 `ColorProviderKey::color_mode`
- 在高对比度 Mixer 之后又被自定义 Mixer 覆盖

### 颜色值正确，观感仍不对

继续检查使用位置：

- 文字 alpha 是否叠加两次
- 背景是否把半透明色当作不透明色
- shadow 色是否误用于背景
- 图标是否由 `ImageModel` 自己解析 ColorId
- LCD/灰度抗锯齿是否造成文字视觉重量变化

## 16. 测试建议

### 16.1 ColorProvider 单元测试

```cpp
ui::ColorProvider provider;
ui::ColorProviderKey key;
key.color_mode = ui::ColorProviderKey::ColorMode::kLight;
AddXenonColorMixer(&provider, key);

EXPECT_EQ(provider.GetColor(kColorXenonToastText),
          SkColorSetRGB(0x1D, 0x21, 0x29));
```

至少覆盖：

- Light + Normal Contrast
- Dark + Normal Contrast
- Light + High Contrast
- Dark + High Contrast

### 16.2 View 测试

对关键 Label 验证：

```cpp
EXPECT_EQ(label->font_list().GetFontWeight(), gfx::Font::Weight::NORMAL);
EXPECT_FALSE(label->GetSubpixelRenderingEnabled());
EXPECT_EQ(label->GetEnabledColor(),
          label->GetColorProvider()->GetColor(kColorXenonToastText));
```

具体颜色比较应在 View 已进入 Widget、ColorProvider 可用后进行。

### 16.3 人工视觉检查

固定以下条件截图：

- 相同 DSF
- 相同窗口激活状态
- 相同 light/dark mode
- 相同背景色
- 相同截图缩放比例

重点观察：

- 中文横竖笔画是否一致
- opacity 动画期间文字是否跳粗细
- Theme 切换后颜色是否恢复默认
- Bubble 透明边缘是否出现彩边

## 17. 提交前检查清单

- [ ] 文案对应的最终 Label 已定位
- [ ] 字号、字重、TextStyle 已确认
- [ ] 前景色与背景色均为预期 ColorId
- [ ] 透明 Layer/opacity 动画下已禁用子像素渲染
- [ ] 未用 `SetSkipSubpixelRenderingOpacityCheck(true)` 掩盖真实问题
- [ ] ColorId 语义范围足够窄，不会污染无关组件
- [ ] 自定义 Mixer 注册顺序正确
- [ ] 未在 Provider 查询后动态修改 recipe
- [ ] Theme 切换后状态正确
- [ ] Light、Dark、高对比度均已验证

## 18. 总结

字体“看起来粗”时，不要只盯着 Font Weight。Views 的最终文本观感由字体、颜色、背景、子像素模式、Layer 和设备缩放共同决定。透明或动画 UI 中，灰度抗锯齿通常比 LCD 子像素渲染更稳定。

ColorMixer 解决的是语义颜色在不同主题环境中的求值和覆盖，不负责决定颜色如何绘制。正确做法是：用组件专属 ColorId 表达语义，用 Mixer 处理主题差异，用 View 的文字、背景、边框或阴影 API 把颜色放到正确位置。
