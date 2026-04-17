---
name: chromium-views-ui
description: >-
  Explains Chromium desktop UI stack (Views, Widget, layout managers), how to
  run views_examples, and how BrowserView wires chrome/browser/ui. Includes a
  full property and use-case reference for FlexLayout, BoxLayout, FillLayout,
  and TableLayout (layout-use-cases.md). Use when modifying native Chrome UI,
  BrowserView, TopContainerView, layout under chrome/browser/ui/views/frame,
  choosing layout managers or view layout properties, or when the user
  mentions Views, FlexLayout, BoxLayout, TableLayout, views_examples, or
  BrowserViewLayout.
---

# Chromium Views 与 BrowserView（技能速查）

## 必读官方 / 源码文档（按顺序）

1. [Views Overview](https://chromium.googlesource.com/chromium/src/+/main/docs/ui/views/overview.md) — 坐标系、View 树、Border/Background、布局、绘制顺序、Widget。
2. [views_examples README](https://chromium.googlesource.com/chromium/src/+/main/ui/views/examples/README.md) — 构建、参数、示例列表与源码路径。
3. [UI debugging（含 views_examples）](https://chromium.googlesource.com/chromium/src/+/main/docs/ui/learn/ui_debugging.md) — `views_examples` 命令与 UI DevTools。
4. Chromium OS 侧入门（术语与目录导航）：[Views intro](https://www.chromium.org/chromium-os/developer-library/guides/views/intro/)。
5. 浏览器窗口设计说明（历史与对象关系）：[browser-window](https://www.chromium.org/developers/design-documents/browser-window)（与 `browser_view.h` 顶部注释一致）。

**布局器属性与用例（第一步，配置表最全）**：[layout-use-cases.md](layout-use-cases.md)。BrowserView 专用布局与子树见 [reference.md](reference.md)。**仓库行引用 + 最小代码片段**见 [examples.md](examples.md)。

## 核心心智模型（写 UI 前记住）

- **View**：类似 DOM 节点；有 bounds（相对父视图）、子 View、可选 **LayoutManager**；不要手改 bounds，优先写/选布局器。
- **Widget**：连接 View 树与 **native window**；每个 Widget 有 **root view**，其唯一子节点是 **contents view**；其余 UI 挂在 contents 下。
- **典型窗口**：contents 常为 **NonClientView**，子节点为 **NonClientFrameView**（标题栏/边框）与 **ClientView**（客户区）。`BrowserView` 继承 `ClientView`，承载标签栏、工具栏、网页区域等。
- **布局**：通用场景优先 **FlexLayout**（类 CSS 主轴/交叉轴）；简单填满用 **FillLayout**；表格用 **TableLayout**；遗留一维排列仍可见 **BoxLayout**。浏览器主窗口顶层不用这些通用布局器，而用专用 **`BrowserViewLayout`**。

## views_examples（动手最快）

```bash
autoninja -C out/Default views_examples
out/Default/views_examples
# 仅开部分示例：
out/Default/views_examples --enable-examples="Flex Layout,Box Layout"
# 列出全部名称：
out/Default/views_examples --enable-examples
```

带 WebView 的变体：`views_examples_with_content`（见 README）。**注意**：README 写明在部分平台 `views_examples` 在 Mac 上不可用，以当前 tree 的 `ui/views/examples/README.md` 为准。

与布局直接相关的示例：`flex_layout_example.cc`、`box_layout_example.cc`、`table_example.cc`、`scroll_view_example.cc`；示例注册表在 `ui/views/examples/create_examples.cc`。

## BrowserView 在代码里的锚点

- 类定义：`chrome/browser/ui/views/frame/browser_view.h` — `BrowserView` = `ClientView` 子类，注释说明职责。
- 子 View 构造顺序与父子关系：`chrome/browser/ui/views/frame/browser_view.cc` 构造函数（`MainBackgroundRegionView`、`TopContainerView`、`ContentsLayoutManager`、`SidePanel` 等）。
- 顶层布局：`BrowserViewLayout::CreateLayout` + `BrowserViewLayoutViews`（`chrome/browser/ui/views/frame/layout/`）。实现侧见 `BrowserViewLayoutImpl` 与（部分配置下）`BrowserViewLayoutImplOld`。

调试：`chrome://flags` 启用 `ui-debug-tools`，或 `--enable-ui-devtools`，见 `docs/ui/learn/ui_debugging.md`。

## 修改 UI 时的检查清单

- [ ] 新控件是否应挂在 `TopContainerView`、`contents_container_` 还是独立 Widget（Bubble）？
- [ ] 若改 **几何/可见性**：是否同步更新 **`BrowserViewLayoutViews`** 与 **`BrowserViewLayout`** 的计算路径？
- [ ] 子 View 的 **z-order / AddChildViewAt** 是否影响绘制与点击（注释里常见「最后添加以盖住下层」）？
- [ ] 是否需要 **`views::kElementIdentifierKey`** / metadata 以便 UI DevTools 可 inspect？
- [ ] 能否用 **`views_examples`** 先验证控件与 Flex 行为，再迁入 chrome 外壳？
