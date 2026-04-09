# Xenon AI 侧栏与右键菜单：本轮改动说明

本文记录 **Xenon AI 侧栏 WebUI** 与 **网页右键 → Xenon AI** 相关的一次工程整理：数据如何从页面传到侧栏、右键菜单如何接入 Chrome、以及侧栏内如何恢复默认右键（含开发者工具）。

---

## 1. 目标行为

1. 用户在**普通网页**中选中文本，通过右键菜单中的 **Xenon AI** 项，打开 **Xenon AI 侧栏**。
2. 选中的文本进入 **`XenonAiService` 的待处理提示**（`SetPendingPrompt`），侧栏 WebUI 通过 Mojo 拉取并展示。
3. Xenon AI 侧栏页面内：**不拦截右键**，使用 WebContents 默认右键菜单（便于 Inspect / DevTools）。

---

## 2. 主要代码路径

### 2.1 右键菜单（Browser / Chrome）

| 环节 | 位置 |
|------|------|
| 挂接 `RenderViewContextMenu` | `chrome/browser/renderer_context_menu/render_view_context_menu.{h,cc}`（`#if BUILDFLAG(ENABLE_XENON_AI)`） |
| Observer 实现 | `xenon_overlay/chrome/browser/xenon_ai/xenon_ai_context_menu_observer.{h,cc}` |
| 打开侧栏 | `chrome::FindBrowserWithTab(web_contents_)` → `SidePanelCoordinator::From(browser)` → `Show(SidePanelEntryId::kXenonAI)` |
| 写入待处理文本 | `XenonAiServiceFactory::GetForProfile(...)` → `SetPendingPrompt(text)` |

说明：`FindBrowserWithWebContents` / `SidePanelCoordinator::GetOrCreateForBrowser` 在当前 Chromium 分支中不可用，已改为上述 API。

### 2.2 侧栏 WebUI ↔ Service（Mojo）

| 环节 | 位置 |
|------|------|
| `.mojom` | `xenon_overlay/chrome/browser/ui/webui/xenon_ai.mojom`（`XenonAiPageHandler` / `XenonAiPage`） |
| WebUI 绑定与数据源 | `xenon_overlay/chrome/browser/ui/webui/xenon_ai/xenon_ai_side_panel_ui.{h,cc}` |
| 前端脚本 | `xenon_overlay/resources/webui/xenon_ai_side_panel/xenon_ai_side_panel.js` |
| 打包 | `xenon_overlay/resources/xenon_resources.grd`、`xenon_overlay/resources/webui/BUILD.gn`（含 `xenon_ai.mojom-webui.ts` 生成依赖） |

关键修复：**HTML 通过 `<script src="xenon_ai_side_panel.js">` 加载**，必须在 `WebUIDataSource` 上执行  
`AddResourcePath("xenon_ai_side_panel.js", IDR_XENON_AI_SIDE_PANEL_JS)`，否则脚本 404，Mojo 初始化不执行，表现为「侧栏能开但没有内容」。

### 2.3 侧栏内放开右键

`WebUIContentsWrapper::Host` 的默认 `HandleContextMenu` 会**吞掉**右键菜单。  
在 **`XenonAiSidePanelWebView`** 中覆写并 `return false`，放行默认行为：

- `xenon_overlay/chrome/browser/ui/views/side_panel/xenon_ai_side_panel_web_view.{h,cc}`

---

## 3. 构建与依赖（GN）

| 目标 | 变更摘要 |
|------|-----------|
| `//xenon_overlay/chrome/browser/xenon_ai:xenon_ai` | 增补 `//chrome/browser/ui/views/side_panel`、`//xenon_overlay/chrome/browser/ui/webui:mojo_bindings`、`//components/renderer_context_menu:renderer_context_menu`、`//mojo/public/cpp/bindings` 等，与 observer / Mojo / 侧栏实现一致 |
| `//xenon_overlay/chrome/browser/ui/webui:mojo_bindings` | `sources` 增加 `xenon_ai.mojom`（格式整理） |

---

## 4. 规范与清理（本轮）

- 去除未使用的 `#include`（如 `xenon_ai_context_menu_observer.cc` 中的 `base/check.h`）。
- `HandleContextMenu` 使用注释形参 `/*...*/`，避免 `(void)` 噪音。
- 修正 `xenon_ai_side_panel_ui.h` 基类列表缩进；去掉 `WebUIDataSource` 段落尾随空格。
- 整理 `InitMenu` 内逗号换行与空行，符合 Chromium 常见排版。

---

## 5. 验证建议

1. 编译通过后，在网页选中文本 → 右键 **Xenon AI** → 侧栏 `Current Prompt` 应显示选中内容。
2. 在侧栏页面内右键，应出现**默认** WebView 菜单（含开发者工具相关项，视构建/策略而定）。
3. DevTools → Network：确认 `xenon_ai_side_panel.js`、`xenon_ai.mojom-webui.js` 均为 200。

---

## 6. 相关文档

- 集成总览：`xenon_ai_integration.md`
- 流程与 Brave 对照：`xenon_ai_brave_components_reference.md`
- KeyedService：`keyed_service_guide.md`

---

## 附录 A：Brave Leo 右键「就地改写选区」的规则与流程（对照源码）

以下整理自 Brave 桌面端在 **`BraveRenderViewContextMenu::ExecuteAIChatCommand`** 中的实现（路径相对 Brave 检出根下的 `brave/`）。用于理解「为什么有的菜单项会改页面选中内容、有的会打开侧栏对话」。

### A.1 菜单是否出现（总开关）

`IsAIChatEnabled()` 为真需同时满足（节选）：

- `params.selection_text` **非空**
- `ai_chat::IsAIChatEnabled(prefs)`
- **Regular Profile**（`GetProfile()->IsRegularProfile()`）
- 用户开启 **`kBraveAIChatContextMenuEnabled`**
- 不在 **PWA / 渐进式 Web 应用** 窗口中（`!IsInProgressiveWebApp()`）

文件：`brave/chromium_src/chrome/browser/renderer_context_menu/render_view_context_menu.cc`

### A.2 两条执行路径：就地改写 vs 打开 Leo 侧栏

执行时在 `ExecuteAIChatCommand` 内计算 `rewrite_in_place`。**只有全部为真**才走「流式改写当前选区」；否则走「打开侧栏 + `SubmitSelectedText`」。

Brave 注释中列出的 **就地改写** 条件（与代码一致）：

1. **`params.is_editable`**：选区在可编辑区域（如 `contenteditable`、可编辑表单等）。
2. **`HasUserOptedIn(prefs)`**：用户已对 Leo 完成 opt-in。
3. **`features::IsContextMenuRewriteInPlaceEnabled()`**：功能开关 `kContextMenuRewriteInPlace`（默认 **ENABLED_BY_DEFAULT**，见 `brave/components/ai_chat/core/common/features.cc`）。
4. **`kAIChatSSE` 开启**：Brave 明确写了 **SSE 必须开**，否则流式更新太慢、体验不可用。
5. **`IsRewriteCommand(command)`**：命令属于「改写类」子集（Paraphrase / Improve / 各语气 / Shorten / Expand 等），**不包含** Summarize、Explain、Create 等（这些走侧栏提交）。
6. **`source_web_contents_ == embedder_web_contents_`**：排除 **MimeHandlerView**（如 PDF 内嵌）等 embedder 与 source 不一致的场景。
7. **当前没有进行中的就地改写**：`WebContents` 上尚未挂 `kAIChatRewriteDataKey` 的 `UserData`。

### A.3 就地改写时的数据流

1. `SetUserData(kAIChatRewriteDataKey, AIChatRewriteData)`：保存累加缓冲区。
2. 懒创建 **`ai_engine_`**：`AIChatServiceFactory::GetForBrowserContext(...)->GetDefaultAIEngine()`。
3. **`ai_engine_->GenerateRewriteSuggestion(selected_text, action_type, on_delta, on_complete)`**
   - **流式片段**：`OnRewriteSuggestionDataReceived`
     - 若 `accumulated_text` 非空，先 **`web_contents->Undo()`**（撤销上一轮流式展示的替换）。
     - **`base::StrAppend`** 拼接到 `accumulated_text`。
     - **`web_contents->Replace(UTF8ToUTF16(accumulated_text))`** 替换选区内容。
   - **完成**：`OnRewriteSuggestionCompleted`
     - 若 **失败** 且已有部分改写：再 **Undo** 恢复原选区；然后 **打开 Leo 侧栏**，通过 `ConversationHandler::AddSubmitSelectedTextError` 把错误展示在对话里。
     - 最后 **`RemoveUserData(kAIChatRewriteDataKey)`**。

### A.4 非就地改写时（打开侧栏）

- 通过 **`AIChatTabHelper`** 取 content id，**`GetOrCreateConversationHandlerForContent`**。
- **`conversation->MaybeUnlinkAssociatedContent()`**（与当前页关联策略有关）。
- **`OpenAIChatForTab`** 激活侧栏。
- **`conversation->SubmitSelectedText(selected_text, action_type)`** 把选中内容与动作类型交给对话管线。

### A.5 与 Xenon 当前实现的对应关系

| Brave | Xenon（当前骨架） |
|-------|-------------------|
| `rewrite_in_place` + `GenerateRewriteSuggestion` | `XenonAiContextMenuObserver` 中 `rewrite_in_place` 分支仍为 TODO，需接 **`XenonAiService` 或独立 Engine** 的流式 API |
| `OnRewriteSuggestionDataReceived` 的 Undo + Replace | 与 `xenon_ai_context_menu_observer.cc` 中 **`OnRewriteSuggestionDataReceived` / `OnRewriteSuggestionCompleted`** 结构一致，待接真实生成回调 |
| 打开侧栏 + 提交文本 | 已实现：`SetPendingPrompt` + `SidePanelCoordinator::Show(kXenonAI)` + WebUI Mojo `GetInitialPrompt` |

若要对齐 Brave，建议在 Xenon 侧显式复刻 **A.2 的七条条件**（尤其是 **editable、SSE/流式、rewrite 命令子集、MimeHandler/embedder**），避免在 PDF 或不可编辑选区上误触发 `Replace`。
