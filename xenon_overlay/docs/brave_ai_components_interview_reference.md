# Brave 浏览器 AI 相关源码整理（面试向）

**检出根路径：** `F:\brave_browser\src`（以下内容中 `//brave/...` 均相对该 `src`）。  
**说明：** Brave 的 AI 能力在工程上大致分为两条线——**Leo / AI Chat（对话、工具、远端模型 + 可选本地 Ollama）** 与 **Local AI（随 Chromium History Embeddings 开关启用的本地嵌入向量 Worker）**。二者目录、工厂与 Mojo 边界不同，面试官常会分开问。

---

## 一、总览：两条能力线

| 维度 | **AI Chat（Leo）** | **Local AI（本地嵌入）** |
|------|---------------------|---------------------------|
| 主要目的 | 侧栏/全页对话、工具调用、与 Brave SKUs/凭证、多模型路由 | 在 **renderer 隐藏页** 里跑 **WASM + Candle** 等，向消费者提供 **`PassageEmbedder`**（段落嵌入），服务 **历史/语义检索** 一类场景 |
| 核心 KeyedService | `AIChatService`（及 `ModelService`、`OllamaService`、`TabTrackerService`） | `LocalAIService` |
| 核心工厂（desktop） | `//brave/browser/ai_chat/ai_chat_service_factory.*` 等 | `//brave/browser/local_ai/local_ai_service_factory.*` |
| 功能开关 | `BUILDFLAG(ENABLE_AI_CHAT)` + `ai_chat::features::IsAIChatEnabled()` 等 | **上游** `history_embeddings::kHistoryEmbeddings`（`LocalAIService` 构造里 `CHECK`） |
| 典型 WebUI | `chrome-untrusted://ai-chat`、`brave://settings` 等 | `chrome-untrusted://local-ai/`（`kUntrustedLocalAIHost` = `local-ai`） |

---

## 二、构建与开关

### 2.1 `enable_ai_chat`（GN）

**文件：** `//brave/components/ai_chat/core/common/buildflags/buildflags.gni`

- `enable_ai_chat = !is_brave_origin_branded`：部分 Brave Origin 品牌构建会关掉 AI Chat。
- 派生：`enable_brave_ai_chat_agent_profile`、`enable_ai_chat_tab_management_tool`（桌面 Agent 相关工具等）。

### 2.2 AI Chat 运行时装配

- **`ModelServiceFactory::GetForBrowserContext`**：若 `features::IsAIChatEnabled()` 才创建 `ModelService`（_prefs 驱动模型列表等）。
- **`AIChatServiceFactory::GetForBrowserContext`**：前置条件包括 `IsAllowedForContext`、`ModelService`、**`SkusService`** 存在，否则会 **返回 `nullptr`**（付费/区域逻辑与 SKU 绑定）。
- **Local AI**：不依赖上述 `enable_ai_chat`；依赖 **`history_embeddings::kHistoryEmbeddings`** 与组件是否成功安装（见下文）。

---

## 三、Local AI（重点：`LocalAIServiceFactory` 与全链路）

### 3.1 实现目的（面试可答）

1. **不经过 Browser 进程转发推理**：`LocalAIService` 注释写明——消费者通过 **`GetPassageEmbedder()`** 拿到 **直连 renderer worker** 的 `PassageEmbedder`，**Browser 侧不代理每次 `GenerateEmbeddings` 调用**。
2. **模型与运行时隔离**：权重、tokenizer、config 通过 **`mojo_base.mojom.BigBuffer`** 从磁盘读入，经 Mojo 交给 **renderer 内工厂** `PassageEmbedderFactory::Init`，再在工厂上 **`Bind`** 出 `PassageEmbedder`。
3. **生命周期**：用 **隐藏 `WebContents`**（`BackgroundWebContentsImpl`）加载 `chrome-untrusted://local-ai/`，在页面里加载 **WASM**（如 `candle_embedding_gemma`）并注册 Mojo 工厂。

### 3.2 `LocalAIServiceFactory`

**文件：** `//brave/browser/local_ai/local_ai_service_factory.h/.cc`

| 项目 | 内容 |
|------|------|
| 基类 | `ProfileKeyedServiceFactory` |
| Profile 范围 | `ProfileSelections::Builder().WithRegular(ProfileSelection::kOriginalOnly).Build()`（与常见 Brave 敏感服务一致：仅常规 profile 的 original） |
| 创建内容 | `BuildServiceInstanceForBrowserContext` 里 `std::make_unique<LocalAIService>(factory, LocalModelsUpdaterState::GetInstance())` |
| `BackgroundWebContents` 注入 | 使用 **`base::BindRepeating` 闭包**，在 browser 层绑定 `BrowserContext*`，内部 `std::make_unique<BackgroundWebContentsImpl>(context, GURL(kUntrustedLocalAIURL), delegate, ...)`；第三个参数为 `LocalAIService` 作为 `BackgroundWebContents::Delegate`；第四个回调里 **`task_manager::WebContentsTags::CreateForToolContents`**，便于任务管理器展示（字符串资源 `IDS_LOCAL_AI_TASK_MANAGER_TITLE`）。 |
| Mojo 对外 | `GetForProfile` → `service->MakeRemote()`；`BindForProfile` → `service->Bind(receiver)` |

**要点：** Factory 只做 **Profile 级单例 + 依赖注入（如何造 BackgroundWebContents）**；核心逻辑在 `components/local_ai`。

### 3.3 `LocalAIService`（组件层）

**文件：** `//brave/components/local_ai/core/local_ai_service.h/.cc`

- **多重角色：** `KeyedService` + `mojom::LocalAIService` + `BackgroundWebContents::Delegate` + `LocalModelsUpdaterState::Observer`。
- **成员（摘）：**
  - `background_web_contents_`：隐藏页载体。
  - `receivers_`：`mojo::ReceiverSet<mojom::LocalAIService>`（多消费者绑定同一 Profile 服务）。
  - `factory_`：`mojo::Remote<mojom::PassageEmbedderFactory>`（**renderer 注册上来**）。
  - `model_load_barrier_`：**BarrierClosure**——需同时满足「组件模型文件就绪」与「WASM 工厂已 `RegisterPassageEmbedderFactory`」才 `LoadLocalModelFiles`。
- **磁盘读取：** `ReadFileToBigBuffer` 将 gguf / safetensors / json 读入 `BigBuffer`（>64KB 可走共享内存，减少拷贝）。
- **Feature CHECK：** 构造函数 `CHECK(base::FeatureList::IsEnabled(history_embeddings::kHistoryEmbeddings))`。

### 3.4 Mojo 契约

**文件：** `//brave/components/local_ai/core/local_ai.mojom`

- `ModelFiles`：weights、dense1、dense2、tokenizer、config 均为 `BigBuffer`。
- `PassageEmbedder::GenerateEmbeddings(string) => (array<double> embedding)`：**消费者直接调 renderer 实现**。
- `PassageEmbedderFactory::Init(ModelFiles) => (bool success)`、`Bind(pending_receiver<PassageEmbedder>)`。
- `LocalAIService::RegisterPassageEmbedderFactory(pending_remote<PassageEmbedderFactory>)`、`GetPassageEmbedder() => (pending_remote<PassageEmbedder>?)`。

### 3.5 本地模型组件（Component Updater）

**文件：** `//brave/components/local_ai/core/local_models_updater.h/.cc`

- `LocalModelsComponentInstallerPolicy`：Brave 组件 CRX 策略（`IsBraveComponent`、校验安装目录等）。
- `LocalModelsUpdaterState`：**单例**（`NoDestructor`），保存 **embeddinggemma** 各文件路径常量：`kEmbeddingGemmaModelDir` 等；`Observer::OnLocalModelsReady(install_dir)`。
- **`ManageLocalModelsComponentRegistration(cus)`**：在浏览器组件注册流程里挂上本地模型组件（见下文 `chromium_src`）。

### 3.6 隐藏 WebContents 实现

**文件：** `//brave/components/local_ai/content/background_web_contents_impl.h/.cc`

- 参考 **`BackgroundContents`** 思路：`WebContentsDelegate` + `WebContentsObserver`，**无 UI**，只负责导航到给定 untrusted URL 与生命周期/crash 通知。
- **不代理 Mojo**（头文件注释）：IPC 组合在 `LocalAIService` 层完成。

### 3.7 WebUI 与资源

**文件：** `//brave/browser/ui/webui/local_ai/local_ai_ui.cc`

- `UntrustedLocalAIUI`：`MojoWebUIController`，`WebUIDataSource` host 为 `kUntrustedLocalAIURL`。
- **CSP：** `ScriptSrc` 允许 `'wasm-unsafe-eval'` 与 `chrome-untrusted://resources`，以便 WASM。
- **`BindInterface`**：`LocalAIServiceFactory::BindForProfile(Profile::FromWebUI(web_ui), receiver)`——**同一 Profile 的 KeyedService** 绑定到 WebUI 管道。

**资源：** `//brave/components/local_ai/resources/`（`local_ai.ts/html`、**`candle_embedding_gemma`** Rust/WASM 模块，`BUILD.gn` 生成 grit）。

### 3.8 与 Chromium 的缝合点（`chromium_src`）

| 修改目的 | 文件 |
|----------|------|
| 注册 **untrusted WebUI config** | `//brave/chromium_src/chrome/browser/ui/webui/chrome_untrusted_web_ui_configs.cc`：`std::make_unique<local_ai::UntrustedLocalAIUIConfig>()` |
| 注册**本地模型组件** | `//brave/chromium_src/chrome/browser/component_updater/registration.cc`：`local_ai::ManageLocalModelsComponentRegistration(cus)` |
| **WebUI ↔ `LocalAIService` Mojo** 绑定类型 | `//brave/browser/brave_content_browser_client.cc`：`RegisterWebUIControllerInterfaceBinder<local_ai::mojom::LocalAIService, local_ai::UntrustedLocalAIUI>`，且包在 `history_embeddings::kHistoryEmbeddings` 开关内 |

### 3.9 工厂启动顺序

**文件：** `//brave/browser/browser_context_keyed_service_factories.cc`

```cpp
if (base::FeatureList::IsEnabled(history_embeddings::kHistoryEmbeddings)) {
  local_ai::LocalAIServiceFactory::GetInstance();
}
```

——确保 Profile 创建路径上会构建该 Factory，从而能 `GetServiceForBrowserContext`。

### 3.10 URL 常量

**文件：** `//brave/components/local_ai/core/url_constants.h`

- Host：`local-ai`；完整：`chrome-untrusted://local-ai/`。

### 3.11 面试高频追问（Local AI）

1. **为什么用 `ReceiverSet`？** 多个 UI 或内部消费者可同时 `Bind` 同一 `LocalAIService`。
2. **为什么 `GetPassageEmbedder` 可能挂起？** 需等 **组件下发 + renderer 工厂注册** 两道门槛（barrier）。
3. **BigBuffer 意义？** 大文件走共享内存，避免 browser 里大块堆拷贝再序列化。
4. **与 AI Chat 的关系？** 代码路径基本独立；Local AI 当前与 **History Embeddings** 特性旗强绑定。

---

## 四、AI Chat（Leo）组件与工厂

### 4.1 核心服务：`AIChatService`

**文件：** `//brave/components/ai_chat/core/browser/ai_chat_service.h`

- `KeyedService` + `mojom::Service` + `mojom::UntrustedService` + `ConversationHandler::Observer` + `mojom::TabDataObserver`。
- 构造函数依赖：`ModelService*`、`TabTrackerService*`、`AIChatCredentialManager`（**SKUs**）、`PrefService*`、`AIChatMetrics*`、`OSCryptAsync`、**`SharedURLLoaderFactory`**、渠道字符串、profile 路径、**`ToolProviderFactory` 列表**。
- 提供 `MakeRemote` / `Bind` 供 WebUI 等绑定。

### 4.2 `AIChatServiceFactory`

**文件：** `//brave/browser/ai_chat/ai_chat_service_factory.cc`

- **`DependsOn`：** `SkusServiceFactory`、`ModelServiceFactory`、`TabTrackerServiceFactory`、`ProfileMiscMetricsServiceFactory`；若 `ENABLE_BRAVE_AI_CHAT_AGENT_PROFILE` 还依赖 **`ActorKeyedServiceFactory`**（Chromium actor / agent profile 实验）。
- **`GetForBrowserContext` 前置：** `IsAllowedForContext`、`ModelService` 非空、`SkusService` 非空。
- **`BuildServiceInstanceForBrowserContext`：** 组装 `AIChatCredentialManager`、metrics、**`BrowserToolProviderFactory`**，条件编译下 **`ContentAgentToolProviderFactory`**（与 `actor_service`、Agent Profile 能力相关），最后 `std::make_unique<AIChatService>(...)`。

### 4.3 `ModelServiceFactory`（组件层）

**文件：** `//brave/components/ai_chat/content/browser/model_service_factory.h/.cc`

- **`BrowserContextKeyedServiceFactory`**（注意：不是 `ProfileKeyedServiceFactory`，但通常仍 per `BrowserContext`）。
- `GetForBrowserContext`：若 `!IsAIChatEnabled()` 则 **nullptr**。
- 构建：`std::make_unique<ModelService>(user_prefs::UserPrefs::Get(context))`。

### 4.4 `OllamaServiceFactory`（本地 Ollama 集成）

**文件：** `//brave/browser/ai_chat/ollama/ollama_service_factory.cc`

- `ProfileKeyedServiceFactory`；`CreateProfileSelections` 内若 **`!features::IsAIChatEnabled()`** 则 `BuildNoProfilesSelected()`（AI Chat 关则整个服务不挂 profile）。
- **`DependsOn(ModelServiceFactory)`**。
- `BuildServiceInstanceForBrowserContext`：取 **storage partition 的 `URLLoaderFactory`**，构造 **`OllamaModelFetcher`**（依赖 `ModelService` + prefs + 可选 delegate），再 **`OllamaService`** 接管 fetcher 并设 delegate。

### 4.5 `TabTrackerServiceFactory`

**文件：** `//brave/browser/ai_chat/tab_tracker_service_factory.h`（`BrowserContextKeyedServiceFactory`）

- 为 AI Chat 提供 **标签页上下文/聚焦信息**（与 `mojom::tab_tracker` 等配合），在 `AIChatServiceFactory` 中 `DependsOn`。

### 4.6 工厂集中注册

**文件：** `//brave/browser/browser_context_keyed_service_factories.cc`

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

### 4.7 Browser 层「工具」生态

**目录：** `//brave/browser/ai_chat/tools/`

- 大量 **`Content Agent`** 工具：`click_tool`、`scroll_tool`、`navigation_tool`、`tab_management_tool`、`code_execution_tool` 等，供模型 **function calling** 驱动浏览器自动化（能力受 feature / 平台限制）。
- **`BrowserToolProviderFactory`**：把浏览器侧工具注入 `AIChatService`。
- **`ContentAgentToolProviderFactory`**（条件编译）：依赖 **Actor** 与 Agent Profile。

其它：`upload_file_helper`、`page_content_fetcher`、`print_preview_extractor` 等支撑 **页面上下文与上传**。

### 4.8 WebUI 与 Mojo（AI Chat）

**目录：** `//brave/browser/ui/webui/ai_chat/`  
**核心：** `ai_chat_ui.cc`、`ai_chat_ui_page_handler.cc`、`ai_chat_untrusted_conversation_ui.cc`（**untrusted 对话帧**、权限挑战等）。

**组件内 Mojo：** `//brave/components/ai_chat/core/common/mojom/`（如 `ai_chat.mojom`、`common.mojom`、`untrusted_frame.mojom`、`ollama.mojom`、`settings_helper.mojom`、`history.mojom` 等）。

### 4.9 引擎与远端 API

**目录：** `//brave/components/ai_chat/core/browser/engine/`

- `engine_consumer*.h`、`conversation_api*_client`、**OAI 序列化/解析**——对接 Brave 对话后端 / 流式 SSE（feature param `kAIChatSSE` 等）。

### 4.10 iOS

**目录：** `//brave/ios/browser/ai_chat/`

- **`AIChatServiceFactory`**、`ModelServiceFactory`、`TabTrackerServiceFactory`（iOS 变体）、`ai_chat_settings_helper`、`ai_chat_distiller_*` 等。
- WebUI：`//brave/ios/browser/ui/webui/ai_chat/`。

---

## 五、开发与排查步骤建议

1. **确认构建维：** `ENABLE_AI_CHAT` / `IsAIChatEnabled()` / `enable_brave_ai_chat_agent_profile`；Local AI 另看 `kHistoryEmbeddings`。  
2. **确认服务是否为空：** `AIChatServiceFactory::GetForBrowserContext` 任一路径失败即 **nullptr**（常见于 SKU、ModelService、区域）。  
3. **Local AI：** 查组件是否 `ComponentReady`、untrusted 页是否加载、renderer 是否调用 `RegisterPassageEmbedderFactory`、barrier 是否两边都触发。  
4. **新增消费者拿 `PassageEmbedder`：** C++ 侧通过 **`LocalAIServiceFactory::GetForProfile` → `MakeRemote()`** 或直接已有管道 `GetPassageEmbedder`（需读 `local_ai_service.cc` 后半 `BindPassageEmbedder` 逻辑）。

---

## 六、文件索引（速查）

### Local AI

| 路径 | 作用 |
|------|------|
| `brave/browser/local_ai/local_ai_service_factory.*` | Profile 工厂 + BackgroundWebContents 闭包注入 |
| `brave/components/local_ai/core/local_ai_service.*` | KeyedService、Mojo、barrier、嵌入器装配 |
| `brave/components/local_ai/core/local_ai.mojom` | IPC 契约 |
| `brave/components/local_ai/core/local_models_updater.*` | 组件与路径状态机 |
| `brave/components/local_ai/content/background_web_contents_impl.*` | 隐藏 WebContents |
| `brave/browser/ui/webui/local_ai/local_ai_ui.*` | Untrusted WebUI + CSP + BindInterface |
| `brave/components/local_ai/resources/*` | 前端 + WASM 模块 |

### AI Chat

| 路径 | 作用 |
|------|------|
| `brave/browser/ai_chat/ai_chat_service_factory.*` | Leo 主服务工厂 |
| `brave/components/ai_chat/core/browser/ai_chat_service.*` | 对话、Mojo、Observer |
| `brave/components/ai_chat/content/browser/model_service_factory.*` | 模型/偏好服务 |
| `brave/browser/ai_chat/ollama/ollama_service_factory.*` | Ollama 侧 Profile 服务 |
| `brave/browser/ai_chat/tab_tracker_service_factory.*` | 标签跟踪 |
| `brave/browser/ai_chat/tools/*` | 浏览器工具集 |
| `brave/components/ai_chat/core/common/features.*` | Feature / Finch 参数 |
| `brave/components/ai_chat/core/common/buildflags/buildflags.gni` | GN 总开关 |

---

*文档基于对 `F:\brave_browser\src\brave` 目录结构与关键源文件的梳理；具体行号随上游提交可能变化，面试前建议本地再 `git grep` 核对一次。*
