# Chromium / Brave 性能相关提交精读笔记

> 基于本地检出：`H:\chromium_142\src`（为主）、`F:\brave_browser\src`（部分提交与上游 Chromium 同源）。  
> 用途：理解**修改目的**、**涉及模块**与**可复用的优化思路**；具体实现请以对应 `git show <hash>` 为准。

---

## 目录

1. [Chromium：渲染 / CSS / 调度](#1-chromium渲染--css--调度)
2. [Chromium：指标与正确性（LCP / PaintTiming / API）](#2-chromium指标与正确性lcp--painttiming--api)
3. [Chromium：数据结构微优化](#3-chromium数据结构微优化)
4. [Chromium / 共用：浏览器 UI 与进程调度](#4-chromium--共用浏览器-ui-与进程调度)
5. [Brave 检出中的代表性提交（多为 Chromium 上游）](#5-brave-检出中的代表性提交多为-chromium-上游)
6. [学习时建议配合的命令](#6-学习时建议配合的命令)
7. [附录 B：实现级深入展开](#7-附录-b实现级深入展开具体怎么做)

---

## 1. Chromium：渲染 / CSS / 调度

### 1.1 `scroll-target-group` 用 OrderedScopeTree 取代全量遍历

| 项目 | 内容 |
|------|------|
| **仓库** | `H:\chromium_142\src` |
| **提交** | `8228728f96d14055cffed90012367445c14a0f7a` |
| **标题** | Optimize scroll-target-group using OrderedScopeTree |

**修改目的**

- 降低 **`scroll-target-group` 与 anchor 变更**时的开销：旧逻辑在属性变化时倾向于**全局重算**；新逻辑只在受影响范围内增量维护。

**技术细节（模块）**

- CSS 引擎与 OrderedScope 基础设施：`ordered_scope.h`、`ordered_scope_tree.h`
- 新增 `scroll_target_group_scope.{cc,h}`，并接入 `style_engine.{cc,h}`、`build.gni`

**修改步骤（抽象层面）**

1. 将 scroll-target-group 纳入 **OrderedScope** 的树形范围管理，使节点增删改可**局部更新**。
2. 扩展 **OrderedScope**：支持条目存放在**调用方控制的存储**（此处为 `ScrollMarkerGroupData::focus_group_`），树只负责 scope 与批量更新协调。
3. 用集成测试 / 现有样式测试覆盖行为等价性（见评审 CL）。

**可迁移的学习点**

- “**全局重算 → 范围树 + 局部失效**” 是布局/样式/滚动类性能优化的核心模式。
- 通用基础设施要支持 **“索引结构 vs. 数据所有权”** 分离，才能被多个特性复用。

---

### 1.2 Blink 主线程调度：渲染被 defer 时不应再因输入而错误 defer 任务

| 项目 | 内容 |
|------|------|
| **仓库** | `H:\chromium_142\src` |
| **提交** | `4d7420f1fde217b1a7bc6b2bb9f5f85a36a599b6` |
| **标题** | [blink scheduler] Don't defer tasks after input if rendering is deferred |

**修改目的**

- 修复 **早期加载阶段主线程更新被暂停/延期** 时，离散输入仍会触发 “等下一帧再跑” 的策略；但此时 **下一帧根本不会来**，导致任务（含加载相关）被长时间拖住。
- 典型劣化场景：**按住键盘重复输入**（重复间隔可小于 50ms），加载线程上的工作会被拖到松键。

**技术细节（模块）**

- `main_thread_scheduler_impl.{cc,h}`、`widget_scheduler_impl.cc`、`widget.{cc,h}`
- 新增调度集成测试 `scheduler_policy_test.cc` 等（约 10 个文件，见 `git show --stat`）

**修改步骤（抽象层面）**

1. 在调度策略中引入对 **main frame updates 是否 paused / deferred** 的判断（含 early loading、`IsRenderingPaused()`、视图过渡等现状）。
2. 当渲染已延期时，**不要**再按 “已请求帧→等呈现后再继续” 的路径去 defer 任务。
3. 用集成测试固定策略：避免加载期键盘重复导致任务饥饿。

**可迁移的学习点**

- 节流/合并输入时，必须区分 **“等待 VSync 合理”** 与 **“当前根本没有 VSync 管线”**。
- 性能问题往往不是 “少做了一点动画”，而是 **错误的不变性** 导致 **优先级反转 / 饥饿**。

---

### 1.3 Tab 集合动画布局：拖拽场景下的性能修补

| 项目 | 内容 |
|------|------|
| **仓库** | `H:\chromium_142\src` |
| **提交** | `c675ccc0b87416baecd002441ef01ebab73bcfaf` |
| **标题** | Optimizations to TabCollectionAnimatingLayoutManager for dragging |

**修改目的**

- 在 **TabCollectionAnimatingLayoutManager** 支持拖拽的调查过程中，对齐 **AnimatingLayoutManager** 上已有的一些策略，**在不改成多动画模型** 的前提下减轻卡顿。

**技术细节（模块）**

- `tab_collection_animating_layout_manager.{cc,h}`

**修改步骤（抽象层面）**

1. 复用已在其他 AnimatingLayoutManager 中验证过的布局/动画更新模式。
2. 针对拖拽路径减少重复测量、布局或无效动画触发的热点（以 diff 为准）。

**可迁移的学习点**

- UI 性能：**同一产品内“已证明更快的模式”横向复用**，比每处自创一套动画状态机更稳。

---

## 2. Chromium：指标与正确性（LCP / PaintTiming / API）

> 本节多条来自 **Paint Timing / Soft Nav** 方向：多数是 **指标正确性、与规范对齐**，对“页面变快”的直接加速有限，但对 **衡量与回归检测** 至关重要。

### 2.1 `PerformanceNavigationTiming` confidence 默认开启

| 项目 | 内容 |
|------|------|
| **仓库** | `H:\chromium_142\src` |
| **提交** | `6ba554f0152d147ae7d37cec8a53f606fee91e4e` |
| **标题** | Enable PerformanceNavigationTiming confidence by default |

**修改目的**

- 实现规范中的 **timing confidence**（导航计时可信度）字段默认暴露给 Web，便于站点区分 **冷启动/系统调度** 等导致的计时不确定性。

**技术细节**

- `runtime_enabled_features.json5`、Web 暴露列表期望文件等。

**修改步骤**

1. 去掉/调整特性开关默认值，使 API 在生产可用。
2. 更新 **global-interface-listing** 等基线，反映 IDL/Web 暴露面。

**学习点**

- “性能”不仅是帧时间，还包括 **可观测性**：让站点能解释异常慢的 `navigation timing`。

---

### 2.2 LCP：`largest-ignored-text` 在 HTML opacity 场景只应候选一次

| 项目 | 内容 |
|------|------|
| **提交** | `fe0b851a767befa60939632094201b4118baedc8` |
| **标题** | LCP: Only consider largest-ignored-text as an LCP candidate once |

**修改目的**

- 文档 **opacity 从 0 → 非 0** 时上报 `largest-ignored-text` 的路径上，此前未像其它文本绘制一样写入 `recorded_set_`，导致内容变化时 **重复考虑候选**，与规范及其它分支不一致。

**技术细节**

- `text_paint_timing_detector.{cc,_test.cc}`

**修改步骤**

1. 在对应分支把该文本纳入与其它 paint 一致的 **去重/记录集合**。
2. 增加单测覆盖：opacity 切换后 innerText 变化等组合场景。

**学习点**

- 性能指标管线里，**状态机遗漏**会造成额外工作与错误指标；修复往往是一行集合维护 + 针对性测试。

---

### 2.3 PaintTiming：队列中节点已 detach 仍记录绘制/呈现时间（软导航）

| 项目 | 内容 |
|------|------|
| **提交** | `b967fb8ce7fde4202959ab603bde78742088748e` |
| **标题** | PaintTiming: Record paint time for queued but detached elements |

**修改目的**

- **Soft Navigation Heuristics** 会持有首次绘制记录，等到 **presentation time** 再发性能条目；若 DOM 在回调前被移除，则 **永远不发射**，真实 SPA 上会丢软导航/ICP。
- 修复：**在 feature flag 后** 仍为 queue 中已移除节点记录 paint/presentation 时间；**不**把这些算作硬 **LCP 候选**。

**技术细节**

- `image_paint_timing_detector.{cc,h}`、`text_paint_timing_detector.cc`、`paint_timing_record.h`、`runtime_enabled_features.json5`、新 WPT 风格 fixture、histogram enum 等。

**修改步骤**

1. 给 `PaintTimingRecord` 增加 **节点已移除** 的布尔标记（因其它 detector 的 `recorded_set_` 不足以判断）。
2. 图像/文本 detector 在 presentation 回调路径上处理 detach。
3. 加用例复现 “先绘制、后移除、再生软导航”。

**学习点**

- 异步呈现回调与 DOM 生命周期交错时，指标系统要有 **明确的终止条件**，否则表现为 **无限期等待**。

---

### 2.4 图像：文档 opacity 0→非 0 时 “largest ignored image” 与文本对齐

| 项目 | 内容 |
|------|------|
| **提交** | `b61cecde114985d19a79a06664b33f592d8ce163` |
| **标题** | PaintTiming: Make reporting largest ignored image consistent with text |

**修改目的**

- 同上特殊场景下：
  1. **无论是否因输入已停止 LCP 记录**，都应 **标记 FCP**（与 `TextPaintTimingDetector` 对齐；FCP 不因交互停止）。
  2. 同时标记 **first image paint**，与所记录内容一致；可能轻微影响 `NavigationToFirstImagePaint`。

**技术细节**

- `image_paint_timing_detector.{cc,h,_test.cc}`、`paint_timing_detector.cc`、`paint_timing.h`、`text_paint_timing_detector_test.cc`

**学习点**

- **FCP vs LCP** 在交互边界上语义不同；图像与文本 detector 必须 **共享同一套产品决策**，否则指标漂移。

---

### 2.5 重命名：`EmitPerformanceEntry` → `EmitLcpPerformanceEntry`

| 项目 | 内容 |
|------|------|
| **提交** | `3d2579166e9b003fef346da5465ac0fe85750f0d` |
| **标题** | PaintTiming: Rename EmitPerformanceEntry to EmitLcpPerformanceEntry |

**修改目的**

- 可读性：delegate 可能发射多类 performance entry，原名易误导；**无行为变更**。

**技术细节**

- `largest_contentful_paint_calculator.{cc,h}`、`paint_timing_detector.{cc,h}`、`soft_navigation_context.{cc,h}`

---

## 3. Chromium：数据结构微优化

### 3.1 事件分发回归修复（PGO 与分支不对称）

| 项目 | 内容 |
|------|------|
| **提交** | `be5957a7c49617bef77012e17c2b14c4e66514ef` |
| **标题** | Fix performance regression for EventsDispatching |

**修改目的**

- 先前某补丁在 **仅 flag 为 false** 时使用 `target_node` 的分支不对称，可能影响 **PGO（按配置优化）** 效果，实测 microbenchmark 从约 **340 runs/s → 371 runs/s**。

**技术细节**

- `js_based_event_listener.cc`、`runtime_enabled_features.json5`（小范围改动）

**修改步骤**

1. 重构分支使 **`target_node` 使用对称**，编译器/PGO 更易优化热路径。
2. 用既有性能用例复测。

**学习点**

- **微结构、热路径上的分支形态** 会改变编译器与 PGO 结果；回归要先看 **是否改变了“可预测性”**。

---

### 3.2 Extensions Toolbar：`std::map` → `base::flat_map`

| 项目 | 内容 |
|------|------|
| **提交** | `7fad18f74fb541d2c4fa0012556ae232482c7917` |
| **标题** | actions revamp: Use base::flat_map for ETVM |

**修改目的**

- 小规模、缓存友好的关联容器：`flat_map` 减少指针追逐；**无功能变化**。

**技术细节**

- `extensions_toolbar_view_model.{cc,h}`

---

### 3.3 文本编码名：`CaseFoldingHashTraits` → `IgnoringAsciiCaseHashTraits`

| 项目 | 内容 |
|------|------|
| **提交** | `392263ba4c35c5fa9a468e7db00f57742e572c47` |
| **标题** | WTF: Use IgnoringAsciiCaseHashTraits for TextEncodingNameMap |

**修改目的**

- 从别名查 canonical encoding 名：**避免完整 case folding**，仅 **ASCII 忽略大小写**，加快查找。

**技术细节**

- 新增 `ignoring_ascii_case_hash.{h,_test.cc}`，`string_view.{h,cc}` 增加 `ContainsOnlyLatin1OrEmpty()`，`text_encoding_registry.cc` 接入。

**修改步骤**

1. 为 `StringView` 查找实现 **IgnoringAsciiCaseHashTranslator**。
2. 编码表哈希策略切换并加单元测试。

**学习点**

- 优化 = **削弱不必要的 Unicode 语义**（在安全前提下），常用别名表几乎总是 ASCII。

---

## 4. Chromium / 共用：浏览器 UI 与进程调度

### 4.1 MultiContentsView：布局时缓存 pref

| 项目 | 内容 |
|------|------|
| **提交** | `274c0e777d6ca9eb00c6f62a3602cc294c4d659d`（在 Brave 检出中验证） |
| **标题** | [SxS] Optimize reading prefs during MultiContentsView layout |

**修改目的**

- `IsDragAndDropEnabled()` 在布局路径上每次读 `PrefService`；改为 **构造时读一次** + **`PrefChangeRegistrar` 监听变更** 并 `InvalidateLayout()`。

**技术细节**

- `multi_contents_view.{cc,h}`

**学习点**

- UI layout：**读 prefs/profile 属于慢路径**，应收敛到 **缓存 + 变更回调**。

---

### 4.2 Performance Manager：关闭中的标签页提升渲染优先级

| 项目 | 内容 |
|------|------|
| **提交** | `2c7e26cf69ff4e93371819ccea71eb6e30b19799` |
| **标题** | performance_manager: Boost priority of closing tabs |

**修改目的**

- 后台页渲染进程可能已被降到低优先级，用户点关闭时 **unload 变慢 → UI “拖泥带水”**。新增 `ClosingPageVoter`，在 `PageNode` 进入 closing 时对主帧投 **USER_BLOCKING** 票。

**技术细节**

- `components/performance_manager/...`（含 `closing_page_voter.cc` 等），默认由 **`kBoostClosingTabs`** 特性开关控制（提交说明：**默认关闭**）。

**学习点**

- **关闭路径** 也是 UX 性能；进程优先级要与 **用户意图（正在关闭）** 对齐。

---

## 5. Brave 检出中的代表性提交（多为 Chromium 上游）

以下哈希来自 `F:\brave_browser\src`；说明 Brave 树是 **带 Cherry-pick 分支的 Chromium**，内容仍以 Chromium CL 为主。

### 5.1 `blink::CharacterAttributes`：按机器字扫描

| **提交** | `ff316c0ba453157a797a0f3f056f6a6299b54347` |
| **目的** | `String::FromUTF8` 热路径上，`CharacterAttributes` 仍偏慢；改为 **按 word 批量** 而非逐字符。 |
| **效果（提交内自述）** | 加载 CNN 时渲染进程 CPU 约 **-0.69%**。 |
| **模块** | `ascii_fast_path.{cc,h}`、测试、`BUILD.gn` |

**步骤摘要**：实现 word-at-a-time 扫描 + 大量单测保证边界与 ASCII fast path 等价。

---

### 5.2 `StyleFromMatchedRulesForElement`：减合并次数与 `Name()` 调用

| **提交** | `52aaff07c9a41b01aab583b0e9995b3802b134c9` |
| **场景** | 富文本跨行删除时，样式合并使用 `MergeAndOverrideOnConflict`，**O(N²)** 且大量临时 `CSSPropertyName`。 |
| **手段** | 减少多余 `CSSPropertyValue::Name()`；用 **第一条 matched rule 初始化** 样式以减少 merge 次数。 |
| **模块** | `css_property_value_set.cc`、`editing_style.cc` |

---

### 5.3 Win 无障碍：`OnAtomicUpdateFinished` 从 N 次分配 → 1 次

| **提交** | `56811e7299423c31c3e68f454e3773ed32ecd19b` |
| **效果** | Pinpoint：**blink_perf.accessibility** VERY_MANY_NODES 故事 **+2.8%**。 |
| **手段** | 树更新期间旧/新状态需要暂存：从 **每节点一分配** 改为 **更新结束单次分配容纳全部节点**。 |

---

### 5.4 Android：`CCTEarlyNav` / `CCTFixWarmup` 默认开启

| **提交** | `42fb2d0675a089edb64a8a11b2c5e766b91a4118` |
| **目的** | **Custom Tab** 场景导航/预热路径优化。 |
| **自述收益** | **CCT 的 LCP/FCP 约提升 30–40ms**。 |
| **模块** | `chrome_feature_list.cc`、`ChromeFeatureList.java`、fieldtrial 配置清理 |

---

### 5.5 Android：视口预测预加载默认参数

| **提交** | `5c0b180f21a951ec8490ff08769f53e4cc515133` |
| **标题** | Enable PreloadingDeterministicViewportBasedPredictorAndroid by default |
| **手段** | 默认参数对齐实验 “aggressive” 臂；**仅 Android**；`intersection_observation_after_fcp_only` 在部分测试中显式关掉以保测试意义。 |
| **模块** | `preloading_decider.cc`、`features.cc`、anchor metrics / interaction tracker 等 |

---

### 5.6 触摸滚动：scroll 开始后立即异步发送 touch move

| **提交** | `d70f05809ce213a47c71da17f88e810434f0321e`（M141 Reland） |
| **问题** | 先前仅当 **GestureScrollUpdate 的 ack 为 kConsumed** 才把 touch move 标为 async；若 **GSU ack 前** 又到来 touch move，会 **阻塞发往 Renderer**，延后 GSU 生成。 |
| **手段** | **scroll 已开始** 即把后续 touch moves 发异步；符合 touch 规范（仅在 touchstart/first touchmove 可 cancel 默认行为）。 |
| **收益方向** | 输入管线吞吐、滚动跟手性（见 Bug 433304196）。 |

---

### 5.7 Scroll Jank V4 指标（测量，非直接加速）

| **提交** | `5f0dd1e6bfbb73161ddb0b02fdacedd25730c4ca` |
| **目的** | 新 UMA：**输入→帧一致性**、快滑连续、fling 过渡期 missed vsync 等；用于 **评估滚动卡顿**，驱动后续优化。 |

---

### 5.8 Sync：`CountDuplicateClientTags` 用 `absl::flat_hash_set`

| **提交** | `fbef426334fb1a61a6f8f551053130006db00ca3` |
| **手段** | 用集合大小与时间复杂度更优的哈希表；逻辑改为 **总数 - 唯一数 = 重复数**。 |
| **模块** | `client_tag_based_data_type_processor.cc` |

---

## 6. 学习时建议配合的命令

在对应仓库下：

```bash
# 完整差异
git show <full-or-abbrev-hash>

# 只看文件列表与行数
git show --stat <hash>

# 结合父提交看演进
git log -1 -p <hash>

# 搜索同一文件近期相关修改
git log -n 20 -- path/to/file
```

---

## 7. 附录 B：实现级深入展开（具体怎么做）

以下按提交说明**调用链、条件分支、数据结构**和**与优化前差异**，便于对照 `git show <hash>` 读源码。路径均相对于 `src/`。

### B.1 `8228728f96d14` — `scroll-target-group` + OrderedScopeTree

**优化前在干什么（概念上）**  
`Document` 里大量与 scroll marker 相关的逻辑在 anchor / `scroll-target-group` 变化时做 **整文档或大范围遍历** 来重建「哪些 anchor 属于哪个 group」。任意局部 CSS 变动都容易触发这种 **全局重算**。

**优化后怎么做**

1. **泛型脚手架**  
   `ordered_scope.h` 给 `OrderedScope<T>` 增加 `StoresItemsInScope()`（默认 `true`）。为 `false` 时：`AttachItem` / `DetachItem` **不再**向 scope 内部的 `items_` 插删，只调用特化出来的 `OnItemAttached` / `OnItemDetached`，由特性自己决定存哪儿。

2. **scroll-target-group 特化**（`scroll_target_group_scope.cc`）  
   - `StoresItemsInScope()` 返回 `false`，真实列表在 **`ScrollMarkerGroupData::focus_group_`**（scope 根元素上的用户数据）。  
   - `OnItemAttached`：`EnsureScrollTargetGroupDataForScope`，`AddToFocusGroup(*anchor)`，`SetNeedsScrollersMapUpdate`。  
   - `OnItemDetached`：从容器的 `GetScrollTargetGroupContainerData` 里 `RemoveFromFocusGroup`。  
   - `OnScopeCreated`：仅在「有 `scope_root` 且父 scope 没有非根 root」时，用 **`LayoutTreeBuilderTraversal`** 在 flat 树序下遍历 `scope_root` 的子树；遇到 **`CreatesScope`** 的子元素则 **整棵子树跳过**（`NextSkippingChildren`），只对带 `ScrollTargetElement()` 的 `HTMLAnchorElement` 调用 `AttachItem`。这样收集 anchor 是 **O(子树规模)** 且尊重嵌套 scope 边界，而不是全文档。  
   - `OnScopeMoveItemsFromParent` / `OnScopeReattachItems`：在父/子 scope 之间 **批量搬** `focus_group_` 里的 anchor，并触发 detach/attach 钩子以更新订阅。  
   - `UpdateItemsInScope`：`UpdateScrollableAreaSubscriptions`、`UpdateSelectedScrollMarker`，再递归子 scope。

3. **从 Document 迁出**  
   提交从 `document.{cc,h}` 删掉一大块旧逻辑，改由 **StyleEngine + OrderedScopeTree** 在样式/ DOM 变化时驱动上述钩子（见 `style_engine.cc` 的接线）。

**你怎么读代码**  
先读 `OrderedScope::AttachItem` / `DetachItem` 与 `StoresItemsInScope` 的分支，再读 `OnScopeCreated` 的遍历循环，对照 CSS `scroll-target-group` 规范里「scope 与嵌套」的语义。

---

### B.2 `4d7420f1fde21` — 主线程调度：`frame_requested` →「是否真会有一帧」

**问题机制**  
离散输入（键盘/点击等）处理完后，调度器会记 `is_frame_requested_after_discrete_input`，之后 **把部分队列推迟到「响应这一输入的帧」之后**，避免抢输入的响应。但在 **主帧被 defer / pause**（早期加载、view transition 等）时，**那一帧可能永远不会 commit**，推迟就演变成 **长时间饥饿**；键盘 repeat 高频时尤其明显。

**实现步骤**

1. **`WidgetBase::AreMainFramesPausedOrDeferred()`**（`widget_base.cc`）  
   委托给 `cc::LayerTreeHost`：`MainFrameUpdatesAreDeferred() || IsRenderingPaused()`。

2. **`WidgetScheduler::Delegate` 接口**（`widget_scheduler.h`）由 `WidgetBase` 实现，供 `WidgetSchedulerImpl` 查询。

3. **`WidgetSchedulerImpl::DidHandleInputEventOnMainThread`**（`widget_scheduler_impl.cc`）  
   原样收到 `frame_requested`，若 feature `kDeferTasksAfterInputOnlyWhenRenderingUnpaused` 开启且 `delegate_` 存在：  
   `is_frame_expected = frame_requested && !delegate_->AreMainFramesPausedOrDeferred()`。  
   把 **`is_frame_expected`** 传给 `MainThreadSchedulerImpl::DidHandleInputEventOnMainThread`。

4. **`MainThreadSchedulerImpl`**（`main_thread_scheduler_impl.cc`）  
   将成员 **`is_frame_requested_after_discrete_input` 重命名为 `is_frame_expected_after_discrete_input`**，语义变为：**不仅「请求了帧」，还要「渲染管线真的会走主帧」**。  
   `MaybeUpdatePolicyOnTaskCompleted` 里对 input queue 的判断改用新 flag。

5. **测试**  
   `scheduler_policy_test.cc`、对 `widget_scheduler` 的 mock `AreMainFramesPausedOrDeferred`。

**要点**  
这是 **策略条件里补了一层「渲染是否冻结」**，属于典型的调度 bugfix，而不是简单「加并行」。

---

### B.3 `c675ccc0b8741` — `TabCollectionAnimatingLayoutManager` 拖拽

**改了什么**

1. **`AnimationProgressed`**  
   若 `current_offset_ == animation->GetCurrentValue()` **直接 return**，避免动画值未变却反复 `InvalidateHost`。

2. **动画进度归一化**  
   新增 `starting_offset_`、`current_offset_`。`LayoutImpl` 里不用 raw `GetCurrentValue()` 插值，而用  
   `percent = clamp((current_offset_ - starting_offset_) / (1.0 - starting_offset_), 0, 1)` 再 `InterpolateLayout(percent)`，这样 **中途改目标** 时可以从「当前视觉位置」重新斜率插值，而不是从头 `Reset(0)`。

3. **`RecalculateTarget`**  
   - 无动画且目标没变：重置 offset 为「已完成」状态。  
   - 动画进行中但若 **`current_offset_ == starting_offset_` 且尚未真正出一帧**：不重启，避免闪一下。  
   - 若已越过阈值 `kResetAnimationThreshold`（0.8）：从当前 `current_layout_` **新开一段 0→1**，避免曲线末端「蹭很久」。  
   - 否则：**更新 `starting_offset_` 为当前 offset**，计时器不停，只换斜率（见注释 *new slope*）。

4. **`InterpolateLayout` 复杂度**  
   以前是两层循环：对每个 `target_child` 在 `starting_layout_.child_layouts` 里 **线性查找** 同 `View*`。  
   现在先建 **`base::flat_map<const views::View*, gfx::Rect>`**，再 O(1) 查起点 bounds，降低拖拽时频繁 layout 的成本。

---

### B.4 `6ba554f0152d1` — `PerformanceNavigationTiming.confidence`

**几乎全是特性与暴露面**  
- `runtime_enabled_features.json5`：`PerformanceNavigationTimingConfidence` 从 `experimental` 改为 **`stable`**，去掉 `origin_trial_feature_name`。  
- 更新 WebView / Blink **global-interface-listing-expected.txt**：为 `PerformanceNavigationTiming` 增加 **getter `confidence`**，并列出 **`PerformanceTimingConfidence`** 接口（`value`、`randomizedTriggerRate`、`toJSON`）。  
实现计算逻辑在更早的 CL 里；本提交 **不负责算法**，只负责 **默认开放 API**。

---

### B.5 `fe0b851a767be` — `largest-ignored-text` 只认一次

**代码**（`text_paint_timing_detector.cc` 的 `ReportLargestIgnoredText`）  
在 `MarkFirstContentfulPaint()` 之后、给 record 设置 frame index 之前，增加：

```cpp
recorded_set_.insert(record->GetNode()->GetLayoutObject(), TextPaintStatus::kPainted);
```

**含义**  
其它文本 paint 路径会把 `(layout_object, 状态)` 放进 `recorded_set_`，用于 **已记录 / 不再作为候选**。opacity 0→非 0 这条「最大被忽略文本」路径之前 **没 insert**，导致后续改 `innerText` 仍会被当作新候选。insert 后与规范及其它路径一致。

**测试** `OpacityZeroHTMLTextRecordedOnce`：先 `opacity:0`，再改 documentElement 为 `opacity:1`，再 `setInnerText`，断言 **队列里不再排队新 timing**。

---

### B.6 `b967fb8ce7fde` — detach 后仍要写 paint time（软导航）

**数据**  
`PaintTimingRecord` 增加 `was_image_or_text_removed_while_pending_` 与访问器；用于 **无法靠 `recorded_set_` 判断** 的场景（提交里点名 largest_ignored_text 与 bug 459517297）。

**文本**（`TextPaintTimingDetector::LayoutObjectWillBeDestroyed`）  
原：`texts_queued_for_paint_time_.erase(&object)`。  
现：若启用 `PaintTimingRecordTimingForDetachedPaintedElementsEnabled()`，找到 queue 中记录并调用 `OnImageOrTextRemovedWhilePending()`，**保留在 map 里** 等 presentation；否则保持旧行为 erase。

**文本 `AssignPaintTimeToQueuedRecords`**（节选逻辑）  
对每个仍缺 paint time 的 record 先 `SetPaintTime`；若 `WasImageOrTextRemovedWhilePending()`，则 **不当作 LCP/ element timing 候选**（注释说明与硬 LCP 行为对齐），并可能跟踪「removed 里最大文本」用于 use counter。

**图像**（`ImageRecordsManager::AssignPaintTimeToRegisteredQueuedRecords`）  
若 `pending_images_` 里已没有对应 record（因 DOM 移除），旧逻辑 `continue` 跳过；新逻辑在 **flag + WasImageOrTextRemovedWhilePending** 时仍 **`SetPaintTime`**。对硬 LCP：`pending` 缺失时在 `is_recording_lcp` 下单独跟踪 `largest_removed_image`，**不**并入正常 `largest_painted_image_`，并可能打 `WebFeature::kLcpCandidateRemovedWhilePaintTimePending`。

**学习点**  
这是 **状态机 + 异步 presentation 回调** 类 bug：必须明确 **「结构没了，但计时还要闭合」** 的路径。

---

### B.7 `b61cecde11498` — largest ignored image 与文本对齐

**两处联动**

1. **`PaintTimingDetector::ReportIgnoredContent`**（`paint_timing_detector.cc`）  
   - 旧：`ReportLargestIgnoredImage` 仅在 `image_paint_timing_detector_->IsRecordingLargestImagePaint()` 为真时调用。  
   - 新：**总是调用** `ReportLargestIgnoredImage()`，与 **`ReportLargestIgnoredText()` 对称**。这样即使用户交互已停止 LCP 记录，**图像这条「被忽略内容」仍会走一遍报告逻辑**（具体是否推进 LCP 在内部再判断）。

2. **`ImageRecordsManager::ReportLargestIgnoredImage`**  
   - 返回类型改为 **`bool`**（是否在本帧 `added_entry`）。  
   - 原：`MarkFirstContentfulPaint()`。  
   - 现：先 **`MarkFirstImagePaint()`**；若 **`!is_recording_lcp` 则 `return false`**，不再 `AddPendingImage`。  
   - 否则：`recorded_images_` insert、`AddPendingImage`、`OnImageLoadedInternal`，`return true`。  
   - `ImagePaintTimingDetector::ReportLargestIgnoredImage` 里用 **`added_entry_in_latest_frame_ |= records_manager_.ReportLargestIgnoredImage(...)`**。

**与提交说明的关系**  
说明里写「Mark FCP…」而 diff 体现为 **`MarkFirstImagePaint` + 外层不再用 `IsRecordingLargestImagePaint` 短路**：产品意图是 **与文本侧一致地处理「opacity 翻转后的最大被忽略内容」**；**以 `git show` 为准** 理解实际打的 timing 点是 **FirstImagePaint** 与 **是否继续参与 LCP 记录** 的组合。

---

### B.8 `3d2579166e9b0` — 纯重命名

`EmitPerformanceEntry` → `EmitLcpPerformanceEntry` 贯穿 `largest_contentful_paint_calculator`、`paint_timing_detector`、`soft_navigation_context`。无逻辑变化，学习时 **可跳过diff**，只跟符号引用。

---

### B.9 `be5957a7c4961` — `JSBasedEventListener::Invoke` 与 PGO

**唯一逻辑 diff**（`js_based_event_listener.cc`）  
- 删：`Node* target_node = event->RawTarget()->ToNode();`  
- 原 shadow 检测：feature 分支外使用 `(target_node && target_node->IsInShadowTree())`。  
- 现：**内联**为 `(event->RawTarget()->ToNode() && event->RawTarget()->ToNode()->IsInShadowTree())`，**两次 `ToNode()`**。

**为何反而更快**  
编译器/PGO 下，原先 **只在 else 分支使用 `target_node`** 的形态可能导致 **寄存器分配与热点块划分不理想**；内联后分支预测与指令布局更均匀。这不是「少了一次调用」——`ToNode()` 仍可能执行两次——而是 **微结构对齐优化器期望**。

另：`runtime_enabled_features.json5` 里注释把可移除版本从 M144 推到 **M147**（与特性生命周期有关）。

---

### B.10 `7fad18f74fb54` — `ExtensionsToolbarViewModel` 用 `flat_map`

- 头文件：`std::map<..., std::unique_ptr<...>> actions_` → **`base::flat_map`**（连续存储 + 少分配）。  
- `OnToolbarModelInitialized`：不再循环 `AttachActionModel` 逐个插入；先 **`vector<pair>` reserve**，循环 `emplace_back(action_id, delegate_->CreateActionViewModel(action_id))`，再 **`actions_ = base::flat_map(..., std::move(initial_actions))`** 一次性构造。  
减少 **红黑树反复插入** 与指针跳跃；对扩展数量中等时主线程更友好。

---

### B.11 `392263ba4c35c` — 编码别名表哈希

- `TextEncodingNameMap` 的 `HashTraits`：**`CaseFoldingHashTraits`** → **`IgnoringAsciiCaseHashTraits`**（全 Unicode case fold → 仅 ASCII 大小写无关）。  
- 新增 **`IgnoringAsciiCaseHashTranslator`**：支持 **`HashMap::Find<..., StringView>`** 而不先 `ToString()`；8bit 与「仅 Latin1 的 16 位」走专用 `StringHasher::ComputeHashAndMaskTop8Bits`，否则 fallback `LowerASCII`。  
- `StringView::ContainsOnlyLatin1OrEmpty()` 为 translator 前提。  
效果：解析 `meta charset`、encoding alias **常见路径** 更便宜。

---

### B.12 `274c0e777d6ca` — `MultiContentsView` 缓存 pref

- 构造：读一次 `prefs::kSplitViewDragAndDropEnabled` → **`is_drag_drop_pref_enabled_`**；初始化 **`PrefChangeRegistrar`**，订阅同一 pref，回调 **`OnDragAndDropPrefStateChange`**。  
- `IsDragAndDropEnabled`：由 **`GetBoolean` 每次读** 改为读 **`is_drag_drop_pref_enabled_`**。  
- 回调里：更新缓存并 **`InvalidateLayout()`**。  
典型 **「热路径读 pref → 缓存 + 订阅」** 模式。

---

### B.13 `2c7e26cf69ff4` — `ClosingPageVoter`

- 新类实现 **`PageNodeObserver`**：`OnIsClosingChanged`。  
- `PageNode::IsClosing()` 为真：通过 main frame 找到 **`ExecutionContext`**，`SubmitVote(ec, Vote(USER_BLOCKING, kPageIsClosingReason))`，`active_votes_[page_node]=ec`。  
- 从 closing 恢复或页移除：`InvalidateVote` 并擦除 map。  
- **`kBoostClosingTabs`** feature：提交说明 **默认关**，需在 flags 里开才有行为。  
学习点：**进程优先级投票** 是抽象通道，voter 只负责在图里 **响应状态位**。

---

### B.14 `ff316c0ba4531` — `CharacterAttributes` 字级扫描（Brave 树 / 上游）

新文件 **`ascii_fast_path.cc`** 显式实例化 `CharacterAttributes(base::span<const LChar>)`：

1. **前缀**：直到指针 **对齐到 `MachineWord`**，逐字节 OR 到 `all_char_bits_word`，并更新大写检测。  
2. **主循环**：每次读一个 **`MachineWord`**，一次 OR 进 `all_char_bits_word`（用于末尾判断是否有 ≥128 的字节）。  
3. **大写 ASCII**：对 word 做 **SIMD 式位技巧**——strip MSB、加常数、与区间掩码，把「是否在 A–Z」折叠进每字节的 MSB（提交内常量 `kLowerAddend`、`kUpperMinuend` 等）。  
4. **尾部**：不足一字节数的残余再逐字符处理。  
最后在 **`all_char_bits_word`** 上检查高位判断非 ASCII。  
学习点：**热路径先对齐 + 按字读 + 无分支位运算**，经典 UTF-8/拉丁检测优化。

---

### B.15 `52aaff07c9a41` — `StyleFromMatchedRulesForElement` + 自定义属性匹配

**`editing_style.cc`**  
- 旧：`style` 空集合，**对每个** `matched_rules->at(i)` 调 `MergeAndOverrideOnConflict`。  
- 新：若规则非空，**`style = 第一条规则的 Properties()` 拷贝构造**，**从 i=1** 开始 merge。少一次「空与第一条合并」的等价工作，常数项更小；在 **N 大** 时仍受 `MergeAndOverrideOnConflict` 代价主导，但 **少一轮**。

**`css_property_value_set.cc`**  
- `IsPropertyMatch`（variable id）：不再 `property.Name() == CSSPropertyName(custom_property_name)`（会构造临时 **`CSSPropertyName`**）；改为 **`property.PropertyID() == kVariable` 时用 `CustomPropertyName()`** 与 `AtomicString` 比。  
- `SetLonghandProperty`：`FindPropertyPointer(property.Name().ToAtomicString())` → **`FindPropertyPointer(property.CustomPropertyName())`**。  
减少临时对象与字符串包装，直击中 **O(N²) merge 里频繁 `Name()`** 的热点。

---

### B.16 `56811e7299423` — Win A11y 单次分配（概念）

提交把 **`BrowserAccessibilityComWin`** 在原子更新过程中 **每节点临时 holder** 改为在 **`OnAtomicUpdateFinished` 一次性** 为本次更新涉及的所有节点分配/管理容器；火焰图上减少 allocator churn，Pinpoint 在 **very-many-nodes** 场景 **+2.8%**。细节见 `browser_accessibility_com_win.{cc,h}` 的 diff。

---

### B.17 产品侧默认开启（实现多为改 Feature 默认值）

| 提交 | 做法摘要 |
|------|-----------|
| **`42fb2d0675a08`** | `chrome_feature_list.cc` / Java：打开 **`CCTEarlyNav`、`CCTFixWarmup`**；删 fieldtrial 里冗余实验配置；指标 claiming **CCT LCP/FCP −30–40ms**。 |
| **`5c0b180f21a95`** | `third_party/blink/common/features.cc`、`preloading_decider.cc` 等：Android **`PreloadingDeterministicViewportBasedPredictorAndroid`** 默认参数对齐 aggressive；测试里对部分用例把 **`intersection_observation_after_fcp_only`** 设回 `false` 以免假阴性。 |
| **`d70f05809ce21`** | Browser 输入侧：一旦认定 **scroll 已开始**，后续 **touch move 改异步投递**，不必再等 **GestureScrollUpdate blocked ack**；减少 **Browser↔Renderer 管道阻塞**，利于 GSU 及时生成。需结合 `SyntheticGestureController` / scroll 控制器代码读。 |
| **`5f0dd1e6bfbb7`** | 新增 Scroll Jank **V4** UMA：固定窗口/每次滚动比例、missed vsync 分解（输入节拍、快滑、fling 起点等）；实现是大段 **metrics + compositor 钩子等**，用于 **衡量** 而非单点算法加速。 |

---

### B.18 `fbef426334fb1` — `CountDuplicateClientTags`

- 容器：`std::set` → **`absl::flat_hash_set`**（平均 O(1) 插入）。  
- 逻辑：不再边扫边 `if (already) count++`；改为 **全部 `insert`（flat_hash_set 保留唯一键）**，返回 **`metadata_map.size() - client_tag_hashes.size()`**。  
数学上重复条目数 = 总条目 − 唯一 tag 数，**结果与旧实现一致**，少了树结构开销。

---

## 附：如何读这份笔记

- **概要**：第 1–5 节；**具体怎么写代码**：本章 **第 7 节附录 B**。  
- **直接加速页面**：B.1、B.2、B.3、B.11、B.12、B.14、B.15、B.17（CCT/预加载/触摸）等。  
- **指标与正确性**：B.4–B.8、B.17（Scroll Jank V4）。  
- **系统与 UX**：B.13、B.16。
