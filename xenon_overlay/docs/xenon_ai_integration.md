# Xenon AI 集成参考（独立文档）

**端到端流程、Webium 时序、Brave Leo/Local AI 逐步对照**（细粒度、按调用链展开）见同目录 [**`xenon_ai_brave_components_reference.md`**](./xenon_ai_brave_components_reference.md)。  
**KeyedService / ProfileKeyedServiceFactory 详解与 Xenon AI 示例** 见 [**`keyed_service_guide.md`**](./keyed_service_guide.md)。

本文 **仅讨论 AI / 侧栏 / 对话与本地嵌入**，与 Xenon 其它能力（Utility Mojo、`XenonWebDialog`、提醒等）解耦。实现时请优先遵守文中的 **分层边界**，避免把重逻辑塞进 `window.xenon` 或扩展 API。

**路径约定**

- **Xenon 工程**：下文 `//xenon_overlay/...` 相对 **`chromium/src`**（你当前的 `h:\chromium_142\src`）。
- **Brave 参考**：下文 `brave/...` 相对 **`F:\brave_browser\src`**（与 `//brave/...` 同义）。

---

## 目录

0. [**流程导向：Brave 组件与 Xenon 端到端对照**](./xenon_ai_brave_components_reference.md)（独立长文，按调用链展开）
1. [Xenon 侧推荐分层](#1-xenon-侧推荐分层)
2. [与 `window.xenon`、`chrome.xenonPrivate` 的边界](#2-与-windowxenonchromexenonprivate-的边界)
3. [Chromium 上游：Side Panel](#3-chromium-上游side-panel)
4. [Brave 参考：Leo 如何挂到 Chrome 侧栏](#4-brave-参考leo-如何挂到-chrome-侧栏)
5. [Brave 两条能力线（总览）](#5-brave-两条能力线总览)
6. [附录 A：Brave Local AI（本地嵌入）详解](#附录-abrave-local-ai本地嵌入详解)
7. [附录 B：Brave AI Chat（Leo）详解](#附录-bbrave-ai-chatleo-详解)
8. [附录 C：文件索引与排查](#附录-c文件索引与排查)

---

## 1. Xenon 侧推荐分层

依赖方向 **自上而下**，上层只依赖下层 **Mojo / 窄 C++ 接口**，避免环依赖。

| 层 | 职责 | 建议落点（示例） |
|----|------|------------------|
| **表现层** | 对话 UI、设置、流式展示 | `SidePanelWebUIView` + WebUI（如 `chrome-untrusted://xenon-ai/`）或 TS 资源 |
| **浏览器 · 领域服务** | 会话、模型路由、Prefs、与 Profile 绑定策略 | `ProfileKeyedService`（如 `XenonAIService`）+ `//xenon_overlay/chrome/browser/xenon_ai/` |
| **浏览器 · 基础设施** | HTTP/SSE、鉴权、限流、日志 | 独立子目录 / target，供 Service 注入 |
| **页面集成（薄）** | 从网页唤起侧栏、提交「可审计」的页面上下文 | 现有 **`XenonPageHost` / `window.xenon`** 上 **少量方法**；内部转调 `SidePanelUI` + `XenonAIService` |
| **扩展（可选）** | 不经网页的入口 | **`chrome.xenonPrivate`**；**业务逻辑仍应复用同一 Service** |

**不要做**：在 `XenonPageHost` 上承载完整 SSE/对话状态；在 Renderer 持有机密 token。

---

## 2. 与 `window.xenon`、`chrome.xenonPrivate` 的边界

| 机制 | 适用 | 不适用 |
|------|------|--------|
| **`window.xenon`**（帧级 Mojo） | 打开/关闭 AI 侧栏、传递经 Browser 裁剪的页面上下文、轻量探测 | 长连接对话协议、模型列表主数据 |
| **`chrome.xenonPrivate`**（组件扩展） | 工具栏/扩展上下文调 Browser | 与 Service 重复实现两套业务 |

详细注入与 Broker 原理见 [`xenon_page_api_browser_interface_broker.md`](./xenon_page_api_browser_interface_broker.md)。扩展权限见 [`chrome_xenon_private_api.md`](./chrome_xenon_private_api.md)。

---

## 3. Chromium 上游：Side Panel

Google Chrome / Chromium **桌面 Views** 中，侧栏实现集中在：

- `chrome/browser/ui/views/side_panel/`：`SidePanel`、`SidePanelCoordinator`、`SidePanelRegistry`、`SidePanelEntry`、`SidePanelWebUIView` 等。
- `BrowserView` 挂载 **内容区高度** 与 **工具栏高度** 两类 `SidePanel`；**显示/切换** 应走 **`SidePanelCoordinator` / `SidePanelUI`**，不要直接操作裸 View。

Xenon 增加 AI 入口时：在 **全局或 Tab 级 `SidePanelRegistry`** 上 **`Register(std::make_unique<SidePanelEntry>(...))`**，内容工厂返回 **`SidePanelWebUIViewT<YourWebUI>`** 或等价物。新增 `SidePanelEntryId` 通常需 **扩展** `chrome/browser/ui/views/side_panel/side_panel_entry_id.h`（或 overlay 等价补丁），并配置 **action / 工具栏**（视产品需求）。

---

## 4. Brave 参考：Leo 如何挂到 Chrome 侧栏

Brave **复用 Chromium `SidePanel`**，通过 **`chromium_src` 补丁** 注册 **`SidePanelEntry::Id::kChatUI`**（在 Brave 树里扩展上游枚举）。

### 4.1 全局 registry（窗口级）

**文件：** `F:\brave_browser\src\brave\chromium_src\chrome\browser\ui\views\side_panel\side_panel_util.cc`

- 用宏 **`PopulateGlobalEntries_ChromiumImpl`** 包裹包含上游 `side_panel_util.cc`，再实现自有 **`SidePanelUtil::PopulateGlobalEntries`**。
- 当 **`ai_chat::AIChatServiceFactory::GetForBrowserContext(browser->profile())`** 非空且 **`ai_chat::ShouldSidePanelBeGlobal(profile)`** 时：

```cpp
global_registry->Register(std::make_unique<SidePanelEntry>(
    SidePanelEntry::Key(SidePanelEntry::Id::kChatUI),
    base::BindRepeating(&AIChatSidePanelWebView::CreateView,
                        browser->profile(),
                        /*is_tab_associated=*/false),
    base::NullCallback()));
```

### 4.2 Tab 级 contextual registry

**文件：** `F:\brave_browser\src\brave\browser\ui\views\side_panel\brave_side_panel_utils.cc` → **`brave::RegisterContextualSidePanel`**

- 当 Service 存在且 **`!ShouldSidePanelBeGlobal`** 时，对当前 `registry` 注册 **同一 `kChatUI`**，但 **`CreateView(..., /*is_tab_associated=*/true)`**。

### 4.3 侧栏内 WebUI 视图

**文件：** `F:\brave_browser\src\brave\browser\ui\views\side_panel\ai_chat\ai_chat_side_panel_web_view.{h,cc}`

- 继承 **`SidePanelWebUIViewT<AIChatUI>`**，即侧栏内容为主 **`AIChatUI` WebUI**。

### 4.4 工具与 WebUI 配置

- **侧栏辅助：** `F:\brave_browser\src\brave\browser\ui\side_panel\ai_chat\ai_chat_side_panel_utils.{h,cc}`（如 `ClosePanel`、`ShouldSidePanelBeGlobal`）。
- **WebUI 注册：** `F:\brave_browser\src\brave\chromium_src\chrome\browser\ui\webui\chrome_web_ui_configs.cc` 中的 **`AIChatUIConfig`**。
- **页面 Handler：** `F:\brave_browser\src\brave\browser\ui\webui\ai_chat\`（如 `ai_chat_ui.cc`、`ai_chat_ui_page_handler.cc`、`ai_chat_untrusted_conversation_ui.cc`）。

### 4.5 Brave 竖条 Sidebar 与 `kChatUI`

Brave **自有竖条**（`brave/components/sidebar/`、`brave/browser/ui/views/sidebar/`）内置项 **`BuiltInItemType::kChatUI`** 与 **`SidePanelEntryId::kChatUI`** 的映射在 **`brave/browser/ui/sidebar/sidebar_utils.cc`**。产品上是 **「竖条入口 → 同一 Chrome Side Panel 条目」**。

### 4.6 Renderer / Omnibox 等其它触点（速查）

- **页面内容抽取：** `brave/renderer/brave_content_renderer_client.cc` → `ai_chat::PageContentExtractor`。
- **Omnibox：** `brave/components/omnibox/browser/leo_provider.cc`。

---

## 5. Brave 两条能力线（总览）

| 维度 | **AI Chat（Leo）** | **Local AI（本地嵌入）** |
|------|---------------------|---------------------------|
| 主要目的 | 侧栏/全页对话、工具、远端模型 + 可选 Ollama | Renderer 隐藏页跑 **WASM**，提供 **`PassageEmbedder`**，服务语义检索等 |
| 核心 KeyedService | `AIChatService`（及 `ModelService`、`OllamaService`、`TabTrackerService`） | `LocalAIService` |
| 核心工厂（desktop） | `brave/browser/ai_chat/ai_chat_service_factory.*` | `brave/browser/local_ai/local_ai_service_factory.*` |
| 功能开关 | `BUILDFLAG(ENABLE_AI_CHAT)` + `ai_chat::features::IsAIChatEnabled()` 等 | 上游 **`history_embeddings::kHistoryEmbeddings`**（`LocalAIService` 构造里 `CHECK`） |
| 典型 WebUI | `chrome-untrusted://ai-chat` 等 | `chrome-untrusted://local-ai/` |

**与 Xenon 的对应关系**：仿 Leo → **Side Panel + WebUI + Profile KeyedService**；若将来做本地小模型 → **单独一条 Local 线**，勿与对话 UI 混在同一个巨型 `mojom` 里。

---

## 附录 A：Brave Local AI（本地嵌入）详解

### A.1 设计要点

1. **Browser 不转发每次推理**：消费者通过 **`GetPassageEmbedder()`** 拿到 **直连 renderer worker** 的 `PassageEmbedder`。
2. **模型与运行时隔离**：权重等经 **`mojo_base.mojom.BigBuffer`** 进 Renderer，**`PassageEmbedderFactory::Init`** 后再 **`Bind`** 出 `PassageEmbedder`。
3. **生命周期**：**隐藏 `WebContents`**（`BackgroundWebContentsImpl`）导航到 **`chrome-untrusted://local-ai/`**，加载 WASM（如 `candle_embedding_gemma`）并注册 Mojo 工厂。

### A.2 `LocalAIServiceFactory`

**文件：** `brave/browser/local_ai/local_ai_service_factory.h/.cc`

| 项目 | 内容 |
|------|------|
| 基类 | `ProfileKeyedServiceFactory` |
| Profile 范围 | 通常 **Regular + OriginalOnly**（与 Brave 敏感服务一致） |
| 创建 | `std::make_unique<LocalAIService>(factory, LocalModelsUpdaterState::GetInstance())` |
| BackgroundWebContents | `base::BindRepeating` 闭包注入 `BrowserContext*`，`LocalAIService` 作 `BackgroundWebContents::Delegate`；可打 **`WebContentsTags`** 便于任务管理器展示 |

### A.3 `LocalAIService`（组件层）

**文件：** `brave/components/local_ai/core/local_ai_service.h/.cc`

- **角色：** `KeyedService` + `mojom::LocalAIService` + `BackgroundWebContents::Delegate` + `LocalModelsUpdaterState::Observer`。
- **成员（摘）：** `background_web_contents_`；`receivers_`：`mojo::ReceiverSet<mojom::LocalAIService>`；`factory_`：`mojo::Remote<mojom::PassageEmbedderFactory>`；**`model_load_barrier_`**（组件就绪 + WASM 工厂注册）。
- **Feature：** 构造函数 `CHECK(history_embeddings::kHistoryEmbeddings)`。

### A.4 Mojo

**文件：** `brave/components/local_ai/core/local_ai.mojom`

- `ModelFiles`（`BigBuffer`）、`PassageEmbedder::GenerateEmbeddings`、`PassageEmbedderFactory::Init` / `Bind`、`LocalAIService::RegisterPassageEmbedderFactory` / `GetPassageEmbedder`。

### A.5 组件与 WebUI

- **组件：** `brave/components/local_ai/core/local_models_updater.*`；注册见 `brave/chromium_src/chrome/browser/component_updater/registration.cc`。
- **WebUI：** `brave/browser/ui/webui/local_ai/local_ai_ui.cc`（CSP 含 **`wasm-unsafe-eval`** 等）。
- **绑定：** `brave/browser/brave_content_browser_client.cc` 中 **`RegisterWebUIControllerInterfaceBinder<local_ai::mojom::LocalAIService, ...>`**（受 feature 保护）。
- **Untrusted config：** `brave/chromium_src/chrome/browser/ui/webui/chrome_untrusted_web_ui_configs.cc`。
- **资源：** `brave/components/local_ai/resources/`（含 `candle_embedding_gemma`）。

### A.6 工厂启动

**文件：** `brave/browser/browser_context_keyed_service_factories.cc`

```cpp
if (base::FeatureList::IsEnabled(history_embeddings::kHistoryEmbeddings)) {
  local_ai::LocalAIServiceFactory::GetInstance();
}
```

### A.7 面试追问（Local AI）

1. **`ReceiverSet`？** 多消费者可同时 `Bind` 同一 `LocalAIService`。  
2. **`GetPassageEmbedder` 挂起？** 等 **组件 + renderer 工厂** 两道 barrier。  
3. **BigBuffer？** 大文件走共享内存，减少拷贝。  
4. **与 AI Chat 关系？** 路径基本独立；Local AI 与 **History Embeddings** 强相关。

---

## 附录 B：Brave AI Chat（Leo）详解

### B.1 `AIChatService`

**文件：** `brave/components/ai_chat/core/browser/ai_chat_service.h`

- `KeyedService` + 多 Mojo 接口 + `ConversationHandler::Observer` 等。
- 依赖 **`ModelService`**、**`TabTrackerService`**、**`AIChatCredentialManager`（SKUs）**、**`SharedURLLoaderFactory`**、**`ToolProviderFactory` 列表** 等。

### B.2 `AIChatServiceFactory`

**文件：** `brave/browser/ai_chat/ai_chat_service_factory.cc`

- **`DependsOn`：** `SkusServiceFactory`、`ModelServiceFactory`、`TabTrackerServiceFactory`、`ProfileMiscMetricsServiceFactory`；条件编译下 **`ActorKeyedServiceFactory`**。
- **`GetForBrowserContext` 失败则 nullptr**（区域/SKU/ModelService 等）——**侧栏注册常以「服务存在」为前提**（见 §4.1）。

### B.3 其它工厂

- **`ModelServiceFactory`：** `brave/components/ai_chat/content/browser/model_service_factory.*`；`!IsAIChatEnabled()` → nullptr。  
- **`OllamaServiceFactory`：** `brave/browser/ai_chat/ollama/ollama_service_factory.cc`。  
- **`TabTrackerServiceFactory`：** `brave/browser/ai_chat/tab_tracker_service_factory.*`。

### B.4 工厂注册

**文件：** `brave/browser/browser_context_keyed_service_factories.cc`

```cpp
#if BUILDFLAG(ENABLE_AI_CHAT)
  if (ai_chat::features::IsAIChatEnabled()) {
    ai_chat::AIChatServiceFactory::GetInstance();
    ai_chat::ModelServiceFactory::GetInstance();
    ai_chat::OllamaServiceFactory::GetInstance();
    ai_chat::TabTrackerServiceFactory::GetInstance();
  }
#endif
```

### B.5 Browser 工具

**目录：** `brave/browser/ai_chat/tools/` — `BrowserToolProviderFactory`、各类 **Agent 工具**（点击、滚动、标签管理等）；条件编译 **`ContentAgentToolProviderFactory`**。

### B.6 WebUI 与 Mojo

- **目录：** `brave/browser/ui/webui/ai_chat/`。  
- **Mojom：** `brave/components/ai_chat/core/common/mojom/`（`ai_chat.mojom`、`untrusted_frame.mojom`、`ollama.mojom` 等）。  
- **引擎 / API：** `brave/components/ai_chat/core/browser/engine/`（SSE、对话客户端等）。

### B.7 GN 与 Feature

- **`brave/components/ai_chat/core/common/buildflags/buildflags.gni`**：`enable_ai_chat` 等。  
- **`brave/components/ai_chat/core/common/features.cc`**：`kAIChat`、`kAIChatSSE` 等。

### B.8 iOS

**目录：** `brave/ios/browser/ai_chat/`、`brave/ios/browser/ui/webui/ai_chat/`。

---

## 附录 C：文件索引与排查

### C.1 Brave 速查表

| 用途 | 路径（均相对于 `F:\brave_browser\src\brave`） |
|------|-----------------------------------------------|
| 全局侧栏注册 | `chromium_src/chrome/browser/ui/views/side_panel/side_panel_util.cc` |
| Tab 侧栏注册 | `browser/ui/views/side_panel/brave_side_panel_utils.cc` |
| 侧栏 WebView | `browser/ui/views/side_panel/ai_chat/ai_chat_side_panel_web_view.*` |
| 侧栏工具 | `browser/ui/side_panel/ai_chat/ai_chat_side_panel_utils.*` |
| Leo 工厂 | `browser/ai_chat/ai_chat_service_factory.*` |
| Leo 核心 | `components/ai_chat/core/browser/ai_chat_service.*` |
| Local 工厂 | `browser/local_ai/local_ai_service_factory.*` |
| Local 核心 | `components/local_ai/core/local_ai_service.*` |
| Local Mojom | `components/local_ai/core/local_ai.mojom` |

### C.2 排查步骤（Brave）

1. **Leo 不出现：** `IsAIChatEnabled()`、`AIChatServiceFactory::GetForBrowserContext` 是否为 nullptr（SKU/区域/ModelService）。  
2. **侧栏不注册：** `ShouldSidePanelBeGlobal` 与 **global vs contextual** 两条注册路径是否命中。  
3. **Local AI：** 组件是否就绪、untrusted 页是否加载、barrier 是否完成。

### C.3 Xenon 文档交叉引用

| 主题 | 文档 |
|------|------|
| 总架构（非 AI） | [`xenon_overlay_architecture.md`](./xenon_overlay_architecture.md) |
| `window.xenon` / Broker | [`xenon_page_api_browser_interface_broker.md`](./xenon_page_api_browser_interface_broker.md) |
| `chrome.xenonPrivate` | [`chrome_xenon_private_api.md`](./chrome_xenon_private_api.md) |

### C.4 Xenon AI 骨架实现路径（Chromium 树 + overlay）

| 分层 | 路径（相对 `src`） |
|------|---------------------|
| GN / buildflag | `xenon_overlay/buildflags/features.gni`（`enable_xenon_ai`）、`xenon_overlay/buildflags/BUILD.gn`（`ENABLE_XENON_AI`） |
| 领域服务（KeyedService） | `xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service.*`、`xenon_ai_service_factory.*`、`BUILD.gn` |
| WebUI 控制器 | `xenon_overlay/chrome/browser/ui/webui/xenon_ai/xenon_ai_side_panel_ui.*` |
| WebUI 注册 | `xenon_overlay/chrome/browser/xenon_browser_main_extra_parts.cc`（`XenonAiSidePanelUIConfig`） |
| 静态页 | `xenon_overlay/resources/webui/xenon_ai_side_panel/xenon_ai_side_panel.html`、`xenon_overlay/resources/xenon_resources.grd` |
| URL 常量 | `chrome/common/webui_url_constants.h`（`kChromeUIXenonAISidePanelHost` / `kChromeUIXenonAISidePanelURL`） |
| 侧栏视图与注册（实现） | `xenon_overlay/chrome/browser/ui/views/side_panel/xenon_ai_side_panel_web_view.*`、`xenon_ai_side_panel_registration.*`（由 `chrome/.../side_panel/BUILD.gn` 以 `//xenon_overlay/...` 源文件编入 `side_panel`） |
| 侧栏挂钩（Chrome 一行） | `chrome/browser/ui/views/side_panel/side_panel_util.cc`（`xenon::RegisterXenonAiGlobalSidePanelEntry`） |
| 自定义工具栏 | `customize_toolbar_handler.cc` 内与其它条目一致的 `switch` + `#if BUILDFLAG(ENABLE_XENON_AI)`；`xenon_ai_customize_toolbar_bridges.h` 仅提供 `AppendXenonAiCustomizeToolbarListActions`（列表追加） |
| 入口 ID / 动作 | `chrome/browser/ui/views/side_panel/side_panel_entry_id.h`（`kXenonAI`）、`chrome/browser/ui/actions/chrome_action_id.h`（`kActionSidePanelShowXenonAI`）、`chrome/browser/ui/browser_actions.cc` |
| KeyedService 工厂登记（实现） | `xenon_overlay/chrome/browser/xenon_ai/browser_context_keyed_service_factories.*` |
| 工厂挂钩（Chrome 一行） | `chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc`（`xenon::EnsureXenonBrowserContextKeyedServiceFactoriesBuilt`） |
| 指标 | `tools/metrics/actions/actions.xml`；`tools/metrics/histograms/metadata/browser/histograms.xml` 等（SidePanelEntry `XenonAI`、WebUI `XenonAiSidePanel`） |

侧栏全局注册与工具栏动作均在 **`#if BUILDFLAG(ENABLE_XENON_AI)`** 下；`chrome/browser/ui/BUILD.gn` 在 `enable_xenon_ai` 时增加对 **`//xenon_overlay/resources:resources_grit`** 的依赖，以解析 **`IDS_XENON_AI_SIDE_PANEL_TASK_MANAGER_TITLE`**。

---

*Brave 路径已在 `F:\brave_browser\src` 检出下核对；行号与符号随上游提交可能变化，以本地 `git grep` 为准。*
