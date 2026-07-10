# Xenon AI 与 Brave 浏览器 AI 组件参考（流程导向）

本文把 **Chromium 侧栏机制**、**Xenon AI 在 Chromium 树中的接法**，以及 **Brave Leo / Brave Local AI** 两条参考实现，按 **时间顺序与调用链** 串成可读的一条线。粒度偏细，适合对照源码走读；**分层原则与附录级索引**仍以 [`Xenon_AI集成参考.md`](./Xenon_AI集成参考.md) 为准。

---

## 0. 文档关系与路径约定

| 文档 | 侧重 |
|------|------|
| **本文** | 端到端流程、Brave 与 Xenon **逐步对照**、组件在链路上的位置 |
| [`Xenon_AI集成参考.md`](./Xenon_AI集成参考.md) | Xenon 推荐分层、`window.xenon` 边界、附录 A/B/C、**实现路径速查表** |
| [`Xenon_Overlay架构与实现参考.md`](./Xenon_Overlay架构与实现参考.md) | Xenon 总架构（Mojo、Utility、WebUI、扩展），非 AI 专论 |
| [`window_xenon与BrowserInterfaceBroker备忘.md`](./window_xenon与BrowserInterfaceBroker备忘.md) | 帧级 `XenonPageHost` / Broker |
| [`chrome_xenonPrivate扩展API.md`](./chrome_xenonPrivate扩展API.md) | `chrome.xenonPrivate` |
| [`Chromium与Brave设计模式.md`](./Chromium与Brave设计模式.md) | KeyedService、Mojo、`chromium_src` 等模式速查 |

**路径约定**

- **Xenon 工程**：`//xenon_overlay/...`、`chrome/...` 相对 **`chromium/src`**（例如 `H:\chromium_142\src`）。
- **Brave 参考**：`brave/...` 相对 **`F:\brave_browser\src\brave`**（与上游 `//brave/...` 同义）。

---

## 1. Chromium 侧栏：概念层（读后续流程前的共同语言）

### 1.1 五个名词，一条竖线串起来

1. **`SidePanelEntryId` / `SidePanelEntry::Id`**  
   逻辑上的「这是哪一种侧栏内容」。新增产品入口通常要扩展枚举，并配套 **action**、**直方图**、**URL 常量** 等。

2. **`SidePanelEntry::Key`**  
   把「全局 id」或「上下文 id」封装成 registry 里查找用的 key。

3. **`SidePanelRegistry`**  
   **窗口级或 Tab 级** 的登记表：有哪些 `SidePanelEntry`、各自如何 **创建视图**。

4. **`SidePanelEntry`**  
   三元组抽象：**Key** + **创建视图的工厂**（常为 `BindRepeating`）+ **可选回调**。`Register(std::make_unique<SidePanelEntry>(...))` 是标准写法。

5. **`SidePanelWebUIView` / `SidePanelWebUIViewT<WebUIController>`**  
   侧栏 **里** 那一块：本质是包了一层 **WebContents**，加载 **Top Chrome WebUI** 或等价配置；`SidePanelCoordinator` / `SidePanelUI` 负责显示区域与动画，**不要绕过协调器直接改裸 View**（除非你在维护协调器本身）。

### 1.2 「全局注册」通常发生在哪里

在桌面 Chromium 中，**窗口级**全局条目往往在 **`SidePanelUtil::PopulateGlobalEntries(Browser*, SidePanelRegistry*)`** 一类路径上被 **批量注册**；该函数会在 **Browser 窗口特性初始化** 过程中、针对当前 `Browser` 的 registry 调用。  
**WebUI Browser（Webium）** 路径下，该函数可能 **提前 return**，但 **仍需要在 return 之前** 把必须在 Webium 下存在的条目注册掉——这是 Xenon AI 挂钩时序的要点（见 §3）。

---

## 2. 流程 A：标准桌面 Chromium — 从「有条目」到「用户看见 WebUI」

下列步骤是 **逻辑顺序**，便于你在源码里 **顺着调用栈** 验证；行号随上游会变，以本地符号为准。

| 步骤 | 发生了什么 | 典型关注点 |
|------|------------|------------|
| **A1** | `Browser` 与 `BrowserWindowFeatures` 完成构造/初始化 | 侧栏 UI 持有者（如 `SidePanelCoordinator` 或 Webium 的 `WebUIBrowserSidePanelUI`）已挂到 window features |
| **A2** | 某处调用 `SidePanelUtil::PopulateGlobalEntries`（或产品补丁的等价入口） | 向 **窗口级** `SidePanelRegistry` 注册多个 `SidePanelEntry` |
| **A3** | 用户点击工具栏「打开某侧栏」或使用 `SidePanelUI::Show` / `Toggle` | 根据 **Key** 找到 `SidePanelEntry`，执行工厂 |
| **A4** | 工厂创建 `SidePanelWebUIViewT<YourWebUI>`（或协调器托管的等价物） | `WebUIContentsWrapper` 持有 **WebContents**，导航到 `chrome-untrusted://...` 或 `chrome://...` |
| **A5** | `WebUIController` / `WebUIConfig` 绑定数据源、Mojo | 页面可交互；侧栏内 **ShowUI** 等生命周期与 Top Chrome 约定一致 |

**连贯性小结**：**注册（A2）** 与 **展示（A3–A5）** 解耦：没有注册，Show/Toggle 找不到 entry；没有 WebUI 配置，视图创建后空白或加载失败。

---

## 3. 流程 B：Webium（WebUI Browser）与 Xenon Toggle — 时序特例

### 3.1 为何「早退」仍要注册

在 **`webui_browser::IsWebUIBrowserEnabled()`** 为真时，`PopulateGlobalEntries` 里可能对 **非 Webium 专用** 的注册 **直接 return**。  
若 Xenon AI 只写在 return **之后**，则 **Webium 窗口的 registry 永远没有 kXenonAI**，工具栏动作会 **无效**。因此 Xenon 在 **`return` 之前** 调用 `xenon::RegisterXenonAiGlobalSidePanelEntry`（见 `chrome/browser/ui/views/side_panel/side_panel_util.cc`）。

### 3.2 Toggle 为何不能保持空实现

Webium 侧 **`WebUIBrowserSidePanelUI`** 上游 **`Toggle`** 可能为空；工具栏 **`CreateToggleSidePanelActionCallback`** 仍会把 **Toggle** 指进来。Xenon 在 **`ENABLE_XENON_SERVICE`** 下通过 **`webui_browser_side_panel_toggle.inc`** 注入与 **`SidePanelCoordinator::Toggle`** 语义接近的逻辑（关闭路径走 webshell 异步通知）。这样 **不改** `browser_window_features` 里 `make_unique` 的具体类型，也能行为一致。

**连贯性小结**：Webium 路径 = **同一套 Key**，但 **注册时机** 与 **Toggle 实现** 必须与 shell 行为对齐。

---

## 4. 流程 C：Xenon AI 在 Chromium 树中的落地（按构建 → 首次使用）

### 4.1 构建与开关

| 次序 | 位置（相对 `src`） | 作用 |
|------|-------------------|------|
| C1 | `xenon_overlay/buildflags/features.gni` | `enable_xenon_ai`（默认跟随 `enable_xenon_service`） |
| C2 | `xenon_overlay/buildflags/BUILD.gn` | 生成 `ENABLE_XENON_AI` / `ENABLE_XENON_SERVICE` 等到 `buildflags.h` |
| C3 | `chrome/browser/ui/views/side_panel/BUILD.gn` 等 | `enable_xenon_ai` 时把 overlay 源文件编进 `side_panel`；`chrome/browser/ui/BUILD.gn` 增加 grit 依赖以解析字符串资源 |

### 4.2 Profile 与 KeyedService（「有没有业务对象」）

| 次序 | 位置 | 作用 |
|------|------|------|
| C4 | `xenon_overlay/chrome/browser/xenon_ai/browser_context_keyed_service_factories.*` | 集中 `GetInstance()` 触发工厂注册 |
| C5 | `chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc` | 在合适生命周期调用 `xenon::EnsureXenonBrowserContextKeyedServiceFactoriesBuilt()` |
| C6 | `xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service_factory.*`、`xenon_ai_service.*` | `ProfileKeyedServiceFactory` + 占位/后续扩展的 `XenonAIService` |

**与侧栏的关系**：侧栏 **能打开** 只要求 **registry + WebUI**；**对话/模型** 等重逻辑应落在 **Service**（见 `Xenon_AI集成参考.md` §1），避免塞进 Renderer。

### 4.3 全局 registry 与视图工厂

| 次序 | 位置 | 作用 |
|------|------|------|
| C7 | `side_panel_util.cc`（`#if BUILDFLAG(ENABLE_XENON_AI)`） | 在 Webium 早退 **前** 调用 `xenon::RegisterXenonAiGlobalSidePanelEntry` |
| C8 | `xenon_overlay/.../xenon_ai_side_panel_registration.cc` | `Register(Key(kXenonAI), BindRepeating(&XenonAiSidePanelWebView::Create, profile), ...)` |
| C9 | `xenon_overlay/.../xenon_ai_side_panel_web_view.*` | `SidePanelWebUIViewT<XenonAiSidePanelUI>` + `WebUIContentsWrapperT` 导航到 `kChromeUIXenonAISidePanelURL` |

### 4.4 WebUI 与资源

| 次序 | 位置 | 作用 |
|------|------|------|
| C10 | `chrome/common/webui_url_constants.h` | host / URL 常量 |
| C11 | `xenon_overlay/.../xenon_ai_side_panel_ui.*` | `TopChromeWebUIController` + `WebUIDataSource` + `WebUIPrimaryPageChanged` 里 `embedder()->ShowUI()` |
| C12 | `xenon_overlay/chrome/browser/xenon_browser_main_extra_parts.cc` | 注册 `XenonAiSidePanelUIConfig` |
| C13 | `xenon_overlay/resources/`、`xenon_resources.grd` | HTML/打包与 `IDR_*` |

### 4.5 工具栏、动作与指标

| 次序 | 位置 | 作用 |
|------|------|------|
| C14 | `side_panel_entry_id.h`、`chrome_action_id.h` | `kXenonAI`、`kActionSidePanelShowXenonAI` |
| C15 | `browser_actions.cc` | `SidePanelAction(..., kActionSidePanelShowXenonAI, ...)` |
| C16 | `customize_toolbar_handler.cc` | Mojo `ActionId` ↔ Chrome `ActionId` 的 `switch` + `#if BUILDFLAG(ENABLE_XENON_AI)` |
| C17 | `xenon_ai_customize_toolbar_bridges.h` | 仅负责向列表 **追加** 一项，保持 handler 以 switch 为主 |
| C18 | `tools/metrics/...` | actions / histograms 与 WebUI 名称 |

**流程 C 一句话**：**开关 → 工厂登记 → PopulateGlobalEntries 内注册 entry → WebView 创建 WebContents → WebUIConfig 与资源 → 工具栏与 Mojo 映射**；Webium 下 **C7 必须在早退前**。

---

## 5. 流程 D：Brave Leo（AI Chat）— 同一骨架，补丁式接入

Brave **不fork 一整套 Side Panel**，而是 **复用 Chromium 类型**，在 **`brave/chromium_src/...`** 与 **`brave/browser/...`** 补注册与 UI。

### 5.1 与流程 A 的映射

| 本流程步骤 | Brave 侧典型落点 | 说明 |
|------------|------------------|------|
| D1 枚举扩展 | Brave 对 `SidePanelEntry::Id` 的补丁 | 增加 **`kChatUI`**（名称以 Brave 树为准） |
| D2 全局注册 | `brave/chromium_src/chrome/browser/ui/views/side_panel/side_panel_util.cc` | 宏包裹上游实现，自定义 **`PopulateGlobalEntries`** |
| D3 注册条件 | `AIChatServiceFactory::GetForBrowserContext` 非空且 **`ShouldSidePanelBeGlobal(profile)`** | **服务不存在则不注册**（与 Xenon 当前「始终注册骨架」可不同产品策略） |
| D4 工厂 | `base::BindRepeating(&AIChatSidePanelWebView::CreateView, profile, /*is_tab_associated=*/false)` | 与 Xenon 的 `XenonAiSidePanelWebView::Create` 同角色 |
| D5 Tab 级 | `brave/browser/ui/views/side_panel/brave_side_panel_utils.cc` → `RegisterContextualSidePanel` | 非 global 时 **`CreateView(..., true)`** |
| D6 WebUI | `brave/browser/ui/views/side_panel/ai_chat/ai_chat_side_panel_web_view.*` | **`SidePanelWebUIViewT<AIChatUI>`** |
| D7 配置与页面 | `chrome_web_ui_configs.cc` 补丁、`brave/browser/ui/webui/ai_chat/` | Handler、对话页、untrusted 帧等 |
| D8 KeyedService | `AIChatServiceFactory` 等 | `browser_context_keyed_service_factories.cc` 里 **`IsAIChatEnabled()`** 闸门 |
| D9 产品竖条 | `brave/browser/ui/sidebar/sidebar_utils.cc` | **`BuiltInItemType::kChatUI`** 映射到同一 **Side Panel Key** |

### 5.2 与 Xenon 流程 C 的差异（设计层）

| 维度 | Xenon AI（当前骨架） | Brave Leo |
|------|----------------------|-----------|
| 接入 Chromium | overlay 源进 `BUILD.gn` + 少量 `chrome/` 钩子 | `chromium_src` 覆盖 + `brave/` 实现 |
| 注册闸门 | `#if BUILDFLAG(ENABLE_XENON_AI)` | `BUILDFLAG` + **运行时** `IsAIChatEnabled`、SKU、区域等 |
| Global vs Tab | 当前以 **全局注册** 为主（见 registration） | **显式分支** global / contextual |

**连贯性小结**：Leo = **Chromium Side Panel 标准模型 + Brave 补丁 + 以 AIChatService 为闸门的注册**。

---

## 6. 流程 E：Brave Local AI — 第二条能力线（非侧栏对话）

Local AI 解决的是 **Renderer 内 WASM 嵌入向量** 等能力，**主叙事不是 Side Panel**，与 Leo **并行存在**。

| 步骤 | 内容 | 典型路径（`brave/` 下） |
|------|------|-------------------------|
| E1 | Feature 闸门 | `history_embeddings::kHistoryEmbeddings`（构造里常见 `CHECK`） |
| E2 | `LocalAIServiceFactory` | `browser/local_ai/local_ai_service_factory.*` |
| E3 | `LocalAIService` | `components/local_ai/core/local_ai_service.*`；`BackgroundWebContents` 导航 **`chrome-untrusted://local-ai/`** |
| E4 | Mojo | `local_ai.mojom`：`PassageEmbedderFactory` / `GetPassageEmbedder` 等 |
| E5 | 浏览器绑定 | `brave_content_browser_client.cc` 等 `RegisterWebUIControllerInterfaceBinder` |

**与 Xenon 的启示**：若将来 Xenon 做 **本地小模型**，应 **单独一条服务线 + 单独 mojom**，不要与 **侧栏对话 WebUI** 混成单一巨型接口（`Xenon_AI集成参考.md` §5）。

---

## 7. 三向对照总表（落地时自测用）

| 维度 | Xenon AI（骨架） | Brave Leo | Brave Local AI |
|------|------------------|-----------|------------------|
| 用户可见主形态 | 侧栏 WebUI | 侧栏 + 全页对话等 | 无默认大 UI；供嵌入/历史等消费 |
| 核心 KeyedService | `XenonAIService`（扩展中） | `AIChatService` 等 | `LocalAIService` |
| 侧栏 Key | `kXenonAI` | `kChatUI` | — |
| 注册入口 | `side_panel_util`（早退前） | `side_panel_util` / `brave_side_panel_utils` | — |
| WebView 模板 | `SidePanelWebUIViewT<XenonAiSidePanelUI>` | `SidePanelWebUIViewT<AIChatUI>` | — |
| 与 Renderer | 常规 WebUI | Page 抽取、工具等 | WASM + `PassageEmbedder` |

---

## 8. 分层边界（摘要）

**只做摘要**，避免与 [`Xenon_AI集成参考.md`](./Xenon_AI集成参考.md) 重复：

- **侧栏 WebUI**：展示与轻逻辑；重状态进 **`ProfileKeyedService`**。
- **`window.xenon`**：薄；打开侧栏、裁剪后的页面上下文；详见 Broker 文档。
- **`chrome.xenonPrivate`**：扩展上下文；业务仍应复用同一 Service。

---

## 9. Brave 路径速查表（相对 `F:\brave_browser\src\brave`）

| 用途 | 路径 |
|------|------|
| 全局侧栏注册（补丁） | `chromium_src/chrome/browser/ui/views/side_panel/side_panel_util.cc` |
| Tab 级注册 | `browser/ui/views/side_panel/brave_side_panel_utils.cc` |
| Leo 侧栏 WebView | `browser/ui/views/side_panel/ai_chat/ai_chat_side_panel_web_view.*` |
| Leo 侧栏工具 | `browser/ui/side_panel/ai_chat/ai_chat_side_panel_utils.*` |
| Leo 工厂 | `browser/ai_chat/ai_chat_service_factory.*` |
| Leo 核心 | `components/ai_chat/core/browser/ai_chat_service.*` |
| Local 工厂 | `browser/local_ai/local_ai_service_factory.*` |
| Local 核心 / mojom | `components/local_ai/core/local_ai_service.*`、`local_ai.mojom` |

---

## 10. Xenon AI 实现速查（相对 `chromium/src`）

与 [`Xenon_AI集成参考.md`](./Xenon_AI集成参考.md) **附录 C.4（Xenon AI 骨架实现路径）** 一致，此处按 **流程顺序** 再列一遍，便于打印：

| 顺序 | 路径 |
|------|------|
| 1 | `xenon_overlay/buildflags/features.gni`、`BUILD.gn` |
| 2 | `xenon_overlay/chrome/browser/xenon_ai/*service*`、`browser_context_keyed_service_factories.*` |
| 3 | `chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc` |
| 4 | `chrome/browser/ui/views/side_panel/side_panel_util.cc` |
| 5 | `xenon_overlay/.../xenon_ai_side_panel_registration.*`、`xenon_ai_side_panel_web_view.*` |
| 6 | `xenon_overlay/.../xenon_ai_side_panel_ui.*`、`xenon_browser_main_extra_parts.cc` |
| 7 | `chrome/common/webui_url_constants.h`、`side_panel_entry_id.h`、`chrome_action_id.h` |
| 8 | `browser_actions.cc`、`customize_toolbar_handler.cc`、`xenon_ai_customize_toolbar_bridges.h` |
| 9 | `chrome/browser/ui/webui_browser/webui_browser_side_panel_ui.cc` + `webui_browser_side_panel_toggle.inc` |
| 10 | `tools/metrics/*`、`xenon_overlay/resources/*` |

---

## 11. 排查顺序建议（连贯）

1. **侧栏无入口**：`BUILDFLAG(ENABLE_XENON_AI)` 是否打开；`PopulateGlobalEntries` 是否在 Webium 早退 **前** 调用注册。  
2. **入口灰了但点不开**：registry 是否有对应 Key；`WebUIConfig` 是否注册；URL 常量是否与 grd 一致。  
3. **Webium 下 Toggle 无反应**：`ENABLE_XENON_SERVICE` 与 `webui_browser_side_panel_toggle.inc` 是否生效。  
4. **对照 Brave Leo**：`GetForBrowserContext` 是否为 nullptr；global vs contextual 是否走错分支。

---

*Brave 路径以本地检出为准；符号与文件名随上游提交可能变化，请以 `git grep` 核对。*
