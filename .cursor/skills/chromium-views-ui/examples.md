# 具体代码示例（Views / BrowserView）

**Flex / Box / Fill / Table 的完整属性表、枚举与选型**以 [layout-use-cases.md](layout-use-cases.md) 为准；本文只保留 **仓库行引用** 与 **最小手写片段**。

下列片段可直接对照仓库源码；**新功能 UI** 优先在 `views_examples` 里改一版，再迁入 `chrome/`。

---

## 1. 在容器上挂 FlexLayout，并给子 View 设置 flex

`views_examples` 里在 `CreateAdditionalControls()` 末尾创建 `FlexLayout` 并交给 `layout_panel()`：

```91:112:ui/views/examples/flex_layout_example.cc
  layout_ = layout_panel()->SetLayoutManager(std::make_unique<FlexLayout>());
}

void FlexLayoutExample::UpdateLayoutManager() {
  for (View* child : layout_panel()->children()) {
    const int flex = static_cast<ChildPanel*>(child)->GetFlex();
    if (flex < 0) {
      child->ClearProperty(views::kFlexBehaviorKey);
    } else {
      child->SetProperty(views::kFlexBehaviorKey, GetFlexSpecification(flex));
    }
  }
}

FlexSpecification FlexLayoutExample::GetFlexSpecification(int weight) const {
  return weight > 0
             ? FlexSpecification(MinimumFlexSizeRule::kScaleToZero,
                                 MaximumFlexSizeRule::kUnbounded)
                   .WithWeight(weight)
             : FlexSpecification(MinimumFlexSizeRule::kPreferredSnapToZero,
                                 MaximumFlexSizeRule::kPreferred)
                   .WithWeight(0);
}
```

**在你自己的 View 里最小化写法（示意）**：横向一排，右侧一块吃掉剩余宽度。

```cpp
#include "ui/views/layout/flex_layout.h"
#include "ui/views/layout/flex_layout_types.h"
#include "ui/views/view_class_properties.h"

// host 为某 View*
host->SetLayoutManager(std::make_unique<views::FlexLayout>())
    .SetOrientation(views::LayoutOrientation::kHorizontal)
    .SetMainAxisAlignment(views::LayoutAlignment::kStart)
    .SetCrossAxisAlignment(views::LayoutAlignment::kCenter);

views::Label* fixed = host->AddChildView(std::make_unique<views::Label>(u"Fixed"));
// 不设置 kFlexBehaviorKey → 默认用首选宽度

views::View* growing = host->AddChildView(std::make_unique<views::View>());
growing->SetProperty(
    views::kFlexBehaviorKey,
    views::FlexSpecification(views::MinimumFlexSizeRule::kScaleToZero,
                              views::MaximumFlexSizeRule::kUnbounded)
        .WithWeight(1));
```

---

## 2. FillLayout：单个子 View 铺满父区域

`BrowserView` 里 Lens  overlay 容器用法：透明容器 + 子 WebView 填满。

```905:915:chrome/browser/ui/views/frame/browser_view.cc
  auto lens_overlay_view = std::make_unique<views::View>();
  lens_overlay_view->SetID(VIEW_ID_LENS_OVERLAY);
  lens_overlay_view->SetProperty(views::kElementIdentifierKey,
                                 kLensOverlayViewElementId);
  lens_overlay_view->SetVisible(false);
  lens_overlay_view->SetLayoutManager(std::make_unique<views::FillLayout>());
  lens_overlay_view_ =
      contents_container->AddChildView(std::move(lens_overlay_view));

  contents_container->SetLayoutManager(std::make_unique<ContentsLayoutManager>(
      contents_view, lens_overlay_view_));
```

---

## 3. 专用布局：ContentsLayoutManager（内容区 + 叠层）

上例同一处：`contents_container_` 不用 Flex，而用 **`ContentsLayoutManager`** 协调 **主内容** 与 **叠在之上的 overlay**。改分屏/多内容区时读 `ContentsLayoutManager` 实现，而不是往 `BrowserView` 里硬编码 bounds。

---

## 4. BrowserView 顶层：BrowserViewLayout + LayoutViews 表

安装时机在 `InitViews` 路径中：先填 `BrowserViewLayoutViews`，再 `SetLayoutManager`。

```5237:5261:chrome/browser/ui/views/frame/browser_view.cc
  BrowserViewLayoutViews layout_views;

  // LINT.IfChange(BrowserViewLayoutViews)
  layout_views.browser_view = this;
  layout_views.window_scrim = window_scrim_view_;
  layout_views.main_shadow_overlay = main_shadow_overlay_;
  layout_views.main_background_region = main_background_region_;
  layout_views.top_container = top_container_;
  layout_views.web_app_frame_toolbar = web_app_frame_toolbar_;
  layout_views.web_app_window_title = web_app_window_title_;
  layout_views.tab_strip_region_view = tab_strip_region_view_;
  layout_views.vertical_tab_strip_container = vertical_tab_strip_container_;
  layout_views.projects_panel_container = projects_panel_container_;
  layout_views.toolbar = toolbar_;
  layout_views.infobar_container = infobar_container_;
  layout_views.contents_container = contents_container_;
  layout_views.multi_contents_view = multi_contents_view_;
  layout_views.toolbar_height_side_panel = toolbar_height_side_panel_;
  layout_views.contents_height_side_panel = contents_height_side_panel_;
  layout_views.top_container_separator = top_container_separator_;
  // LINT.ThenChange(//chrome/browser/ui/views/frame/layout/browser_view_layout.h:BrowserViewLayoutViews)

  SetLayoutManager(BrowserViewLayout::CreateLayout(
      std::make_unique<BrowserViewLayoutDelegateImpl>(*this), browser(),
      std::move(layout_views)));
```

**要点**：改窗口几何时，通常要同时改 **`browser_view_layout*.cc`** 里对应该指针的布局分支，并把新 View 加进 **`BrowserViewLayoutViews`**（与上段 LINT 块同步）。

---

## 5. 注册一个 views_examples 条目（对照「完整小功能」长什么样）

新示例类继承 `ExampleBase`，实现 `CreateExampleView(View* parent)`；在 `create_examples.cc` 里 `push_back`：

```57:76:ui/views/examples/create_examples.cc
ExampleVector CreateExamples(ExampleVector extra_examples) {
  ExampleVector examples = std::move(extra_examples);
  examples.push_back(std::make_unique<ActionsExample>());
  examples.push_back(std::make_unique<AnimatedImageViewExample>());
  examples.push_back(std::make_unique<AnimationExample>());
  examples.push_back(std::make_unique<AxExample>());
  examples.push_back(std::make_unique<BadgeExample>());
  examples.push_back(std::make_unique<BoxLayoutExample>());
  examples.push_back(std::make_unique<BubbleExample>());
  examples.push_back(std::make_unique<ButtonExample>());
  examples.push_back(std::make_unique<ButtonStickerSheet>());
  examples.push_back(std::make_unique<CheckboxExample>());
  examples.push_back(std::make_unique<ColoredDialogExample>());
  examples.push_back(std::make_unique<ColorsExample>());
  examples.push_back(std::make_unique<ComboboxExample>());
  examples.push_back(std::make_unique<DesignerExample>());
  examples.push_back(std::make_unique<DialogExample>());
  examples.push_back(std::make_unique<DialogModelExample>());
  examples.push_back(std::make_unique<FadeAnimationExample>());
  examples.push_back(std::make_unique<FlexLayoutExample>());
```

本地验证：`out/Default/views_examples --enable-examples="Flex Layout"`。

---

## 6. Skill 是否「够用」？

- **概念 + 路径**：`SKILL.md` + `reference.md` 负责让模型快速定位该读哪里。  
- **写法与 API 细节**：以 **`examples.md`（本文）+ 仓库内对应 `*_example.cc`** 为准；Chromium API 变动大，技能里保留「模式」比抄长文件更安全。

若你还希望增加 **BubbleDialogDelegateView** 或 **Widget::InitParams** 最小窗口示例，可以说明目标平台（Win/Mac/Linux），可再补一节到本文。
