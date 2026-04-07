# Chromium：Renderer 进程内存回收与多标签场景

本文说明 **已打开页面（主要在 Renderer 进程）** 在内存压力下的典型处理路径，以及与 **`NetworkService::OnMemoryPressure`** 的边界。

- 配套（NetworkService API）：`docs/network_service_mojom_browser_reference.zh-CN.md`

---

## 1. 进程分工（为何不能混淆）

| 进程（常见） | 与「10 个标签」相关的内存 |
|--------------|---------------------------|
| **Renderer** | 每个或每组站点通常一个进程：Blink DOM/CSS、V8 JS 堆、部分解码缓存、**内存型 Resource/MemoryCache**、Skia/合成相关对象等。**标签内存大头多在这里。** |
| **Browser** | 标签壳、`WebContents`、策略、**Performance Manager**（见 §1.1）、向子进程发 IPC 等。 |
| **Network Service** | HTTP 栈、socket 池、（取决于配置的）HTTP 缓存后端、Shared Dictionary 等。**不持有页面 DOM。** |
| **GPU** | 纹理、栅格化结果等，另有一套路。 |

### 1.1 Performance Manager 是什么、起什么作用

**Performance Manager** 指 Chrome **Browser 进程**里的子系统（代码主要在 `chrome/browser/performance_manager/`），核心是维护一张 **`Graph`（对象关系图）**：

- **节点（Node）** 类型包括 **页（Page）**、**帧（Frame）**、**进程（Process）**、**Worker** 等，表示「当前浏览器里有哪些可调度单元、谁属于谁」。
- **策略（Policy）** 以 **`GraphOwned`** 等形式挂在图上，在**专用序列**上读图、做决策。

**作用**：把分散的 UI/标签/进程信息收成**统一的数据结构**，便于做 **内存与性能相关的自动化决策**，例如：

- **标签丢弃（discarding）**、**可否丢弃的资格判定**、紧急内存压力时丢哪些页（如 `UrgentPageDiscardingPolicy`、`PageDiscardingHelper`）；
- **页面/进程冻结与排序**、部分平台的 **进程优先级/置换** 策略；
- 以及 **User Performance Tuning**、CPU 健康等与「整机体验」相关的跟踪与策略。

它 **不是**单独一个进程；**不替代** Renderer 里的 Blink/V8，而是 **Browser 里「根据全局状态指挥怎么省内存/保体验」的框架**。文档里写「Performance Manager 图」即指这张供策略使用的 **Graph**。

结论：**网络服务里的 `OnMemoryPressure` 不负责「减掉 Renderer 里 10 页的 V8」**；那是 **Renderer 自身 + Browser 策略（常经 Performance Manager 挂载的策略）** 的事。

---

## 2. 压力信号如何进入 Renderer

### 2.1 Renderer 进程内的 `MemoryPressureListener`

Renderer 在主线程上可注册 **`base::MemoryPressureListenerRegistration`**。进程内存在 **`MemoryPressureListenerRegistry`**，与 Browser/Network 类似：**同一级别广播给所有订阅者**，由各模块自己「瘦身」。

`RenderThreadImpl` 会注册 `kRenderThreadImpl` 监听器；其 `OnMemoryPressure` 主要打 trace，**具体回收分散在 Blink / V8 / Skia 等其它监听者**上。

### 2.2 Android：Browser 主动通知 Renderer

Android 上可通过子进程接口把压力级别打进 Renderer，再在子进程里 **`NotifyMemoryPressure`**：

```919:929:h:\chromium_142\src\content\child\child_thread_impl.cc
void ChildThreadImpl::OnMemoryPressureFromBrowserReceived(
    base::MemoryPressureLevel level) {
  if (IsInBrowserProcess()) {
    return;
  }
  base::MemoryPressureListener::NotifyMemoryPressure(level);
}
```

另有 **`RenderThreadImpl::OnMemoryPressureFromBrowserReceived`**（Android）调用 **`blink::RequestUserLevelMemoryPressureSignal`**，用于 **User-level** 压力路径（与系统监测并行的一种调度）。

### 2.3 桌面等平台

桌面 Renderer 通常仍有 **本进程**的内存压力监测与 `MemoryPressureListener` 订阅；是否与 Browser「共用同一事件源」因平台与分支而异，但**模型一致**：**谁在 Renderer 里接到 `NotifyMemoryPressure`，谁就按监听表执行**，与 `NetworkService::OnMemoryPressure` **无直接调用关系**。

---

## 3. Blink / Renderer 内常见「回收」行为（举例）

### 3.1 `MemoryPurgeManager`（后台 / 冻结时主动 purge）

`third_party/blink/renderer/platform/scheduler/main_thread/memory_purge_manager.cc`：在 **Renderer 进入后台**、**页面冻结**等条件下，可能延迟触发 **`NotifyMemoryPressureFromAnyThread(CRITICAL)`**，驱动一轮进程内监听者；若 **所有页都冻结**，还可能 **`SetNotificationsSuppressed(true)`** 减少后续噪声。这与「用户仍在前台疯狂切 10 个标签」的路径不同，更偏向 **后台内存收缩**。

### 3.2 `MemoryCache`（资源内存缓存）

`MemoryCache::OnMemoryPressure`：在非 `NONE` 级别下，若开启 feature，可 **`ClearStrongReferences()`**，松掉对资源的强引用，便于 GC / 压缩缓存 footprint（不改变已显示像素的唯一方式，但可减轻内存峰值）。

```556:565:h:\chromium_142\src\third_party\blink\renderer\platform\loader\fetch\memory_cache.cc
void MemoryCache::OnMemoryPressure(base::MemoryPressureLevel level) {
  if (level == base::MEMORY_PRESSURE_LEVEL_NONE) {
    return;
  }
  if (base::FeatureList::IsEnabled(
          features::kReleaseResourceStrongReferencesOnMemoryPressure)) {
    ClearStrongReferences();
  }
}
```

### 3.3 `Resource`、媒体 `UrlIndex`、Skia 等

- `Resource::OnMemoryPressure`：可在 feature 控制下释放 **decoded** 数据等（节省内存，再次显示可能需重解码）。
- `UrlIndex::OnMemoryPressure` 等：媒体侧索引/缓存裁剪。
- `SkiaGraphicsPressureListener`：图形相关压力响应。

（具体到哪些 level 触发、feature 是否默认开启，以当前分支与 FieldTrial 为准。）

### 3.4 V8

`base::MemoryPressureLevel` 与 `v8::MemoryPressureLevel` 枚举值对齐（见 `render_thread_impl.cc` 中 `static_assert`）。V8 会在压力通知下更积极 **`MemoryPressureNotification`** / GC 策略调整；细节在 V8 集成层，此处只记 **「Renderer 内压力会传导到 JS 引擎」**。

### 3.5 `DiscardableMemory`

`RenderThreadImpl` 在前后台切换时会通知 **discardable** 分配器；与「内存压力监听」配合，用于 **可丢弃** 类缓冲。

---

## 4. Browser / Chrome 层：直接动「标签」的策略

即使 Renderer 内部只做「软回收」，**Chrome 仍可在极压下把整页从内存里拿掉**：

### 4.1 紧急页丢弃（Urgent discarding）

`chrome/browser/performance_manager/policies/urgent_page_discarding_policy.cc`：在 **`MEMORY_PRESSURE_LEVEL_CRITICAL`**（或持续压力实验路径）下 **`HandleMemoryPressureEvent`**，通过 **`PageDiscardingHelper`** **丢弃（discard）页面**——效果接近 **关掉标签的内存占用**，用户回来时往往要 **重新加载**（除非 BFCache 等另作恢复）。

```103:112:h:\chromium_142\src\chrome\browser\performance_manager\policies\urgent_page_discarding_policy.cc
void UrgentPageDiscardingPolicy::OnMemoryPressure(
    base::MemoryPressureLevel new_level) {
  if (new_level != base::MEMORY_PRESSURE_LEVEL_CRITICAL) {
    return;
  }
  HandleMemoryPressureEvent();
}
```

这是 **Browser 侧**对「多标签占内存」最「硬」的手段之一，与 **NetworkService::OnMemoryPressure** 是 **并行**关系（同源压力事件，不同监听者）。

### 4.2 标签冻结（Tab / Page freeze）

**含义**：后台、非 `VISIBLE` 的标签可被标记为 **frozen**：Browser 通过 Mojo 更新 `blink::PageLifecycleState`，**冻结 Blink 侧调度**（计时器、部分任务等受约束），从而减少后台 CPU；常与 **Performance Manager 的 `LifecycleState::kFrozen`** 对齐（Chrome `TabLifecycleUnit` 在「已 discard」与「活跃」之间还有 **`FROZEN`** 状态，见 `tab_lifecycle_unit.cc` 中 `RecomputeLifecycleUnitState`）。

**代码入口（Content）**：`WebContentsImpl::SetPageFrozen` 要求页面 **不可见**，并对每个 `RenderViewHostImpl` 调用 **`SetIsFrozen`**：

```4107:4114:h:\chromium_142\src\content\browser\web_contents\web_contents_impl.cc
void WebContentsImpl::SetPageFrozen(bool frozen) {
  TRACE_EVENT1("content", "WebContentsImpl::SetPageFrozen", "frozen", frozen);

  // A visible page is never frozen.
  DCHECK_NE(Visibility::VISIBLE, GetVisibility());

  primary_frame_tree_.ForEachRenderViewHost(
      [&frozen](RenderViewHostImpl* rvh) { rvh->SetIsFrozen(frozen); });
}
```

`RenderViewHostImpl::SetIsFrozen` 委托给 **`PageLifecycleStateManager::SetIsFrozen`**，再由 **`SendUpdatesToRendererIfNeeded`** 把新生命周期状态发到 Renderer（`page_lifecycle_state_manager.cc`）。

**代码入口（Performance Manager 组件）**：`components/performance_manager/freezing/freezer.cc` 在 **`Freezer::MaybeFreezePageNode`** 里对非可见 `WebContents` 调用 **`SetPageFrozen(true)`**（解冻对称 **`UnfreezePageNode`**）：

```16:27:h:\chromium_142\src\components\performance_manager\freezing\freezer.cc
void Freezer::MaybeFreezePageNode(const PageNode* page_node) {
  DCHECK(page_node);

  base::WeakPtr<content::WebContents> contents = page_node->GetWebContents();
  CHECK(contents);

  // A visible page should not be frozen.
  if (contents->GetVisibility() == content::Visibility::VISIBLE) {
    return;
  }

  contents->SetPageFrozen(true);
}
```

**何时触发**：Performance Manager 的 **`FreezingPolicy`**（`components/performance_manager/freezing/freezing_policy.cc`）在图满足条件时会调用 **`freezer_->MaybeFreezePageNode`**；此外 **`chrome://discards` 等工具**也可直接 **`SetPageFrozen`**（如 `chrome/browser/ui/webui/discards/discards_ui.cc`）。

**与 `MemoryPurgeManager` 的关系**：页面进入 **冻结** 等路径时，Blink 侧 **`MemoryPurgeManager::OnPageFrozen`** 可能安排延迟 **purge**（或对全部页冻结后抑制后续压力通知），见 `third_party/blink/renderer/platform/scheduler/main_thread/memory_purge_manager.cc`（正文 §3.1）。

---

### 4.3 前进/后退缓存（BFCache, BackForwardCache）

**含义**：用户 **离开**某一页但 **可能通过「后退」恢复** 时，Browser 可将整页 **保留在内存**（含 Renderer 中的文档与受限 IPC 行为），避免完整重载；与 **freeze** 不同：BFCache 针对 **历史会话恢复**，有一整套 **可否进入缓存** 的判定与 **超时驱逐**。

**Browser 侧状态机**：`PageLifecycleStateManager::SetIsInBackForwardCache` 设置 **`eviction_enabled_`**、**`pagehide_dispatch_`** 等，并 **`SendUpdatesToRendererIfNeeded`** 通知进入/离开 BFCache（`page_lifecycle_state_manager.cc`）。**`RendererExpectedToSendChannelAssociatedIpcs()`** 等在缓存内会收紧 IPC 期望（见 `page_lifecycle_state_manager.h` 注释）。

**Renderer / 框架回调**：主文档进入缓存时 **`RenderFrameHostImpl::DidEnterBackForwardCache`** 会通知 View、取消部分统计、调用 **`DidEnterBackForwardCacheInternal()`**（将 **`lifecycle_state()`** 设为 **`kInBackForwardCache`**），并对子帧递归；同时 **启动 BFCache 驱逐定时器**：

```3006:3042:h:\chromium_142\src\content\browser\renderer_host\render_frame_host_impl.cc
// The current frame went into the BackForwardCache.
void RenderFrameHostImpl::DidEnterBackForwardCache() {
  TRACE_EVENT0("navigation", "RenderFrameHostImpl::EnterBackForwardCache");
  DCHECK(IsBackForwardCacheEnabled());
  DCHECK(IsInPrimaryMainFrame());

  // Notifies the View that the page is stored in the `BackForwardCache`.
  //
  // We shouldn't BFCache a renderer without a View.
  CHECK(GetView());
  static_cast<RenderWidgetHostViewBase*>(GetView())->DidEnterBackForwardCache();

  // Cancel loading memory tracker if it hasn't already recorded loading
  // memory stats, as we would now be including stats from the navigation
  // navigating away from the page.
  GetPage().CancelLoadingMemoryTracker();

  CHECK(GetRenderWidgetHost());
  CHECK(GetRenderWidgetHost()->view_is_frame_sink_id_owner());

  DidEnterBackForwardCacheInternal();
  // Pages in the back-forward cache are automatically evicted after a certain
  // time.
  StartBackForwardCacheEvictionTimer();

  for (FrameTreeNode* node : FrameTree::SubtreeAndInnerTreeNodes(
           this,
           /*include_delegate_nodes_for_inner_frame_trees=*/true)) {
    RenderFrameHostImpl* subframe = node->current_frame_host();
    ...
    if (subframe && !subframe->IsPendingDeletion()) {
      subframe->DidEnterBackForwardCacheInternal();
    }
  }
}
```

**实现中枢**：`content/browser/renderer_host/back_forward_cache_impl.cc` / **`back_forward_cache_impl.h`**（如 **`kBackForwardCacheSize`** 等 Feature 参数限制条目数）。**驱逐**可走 **`RenderFrameHostImpl::EvictFromBackForwardCacheWithReason`** 等 API（内存压、超时、不兼容特性）。

**与 freeze / discard 的区分**：BFCache **保留文档与 Renderer 工作单元** 以便瞬时恢复；**freeze** 主要是后台 **减活**；**discard** 通常 **释放 WebContents 对应标签内容** 需 **重新加载**。

---

### 4.4 合成层「帧」驱逐（Frame eviction，`viz::FrameEvictionManager`）

**`viz` 指什么**：Chromium 里 **`viz`** 是 **visuals / 可视合成（compositing）** 相关代码的命名空间与组件目录（主要在 **`components/viz/`**）。它把各 Renderer 提交的 **CompositorFrame** 交给显示管线（常与 **GPU 进程**、**Display Compositor** 协作），和 **网络（NetworkService）**、**页面文档（Blink DOM）** 不在同一层。因此本节讲的是 **合成纹理/帧缓存** 的回收，不是整页卸载。

**含义**：**不可见标签**仍会暂时 **保留少量已提交的合成帧（compositor frame）** 作为 LRU，以加快来回切换；超出上限或 **内存压力** 时，**丢弃帧的纹理/资源**，但 **不卸载 DOM**。可见标签可通过 **lock** 避免被驱逐（见 `frame_eviction_manager.h` 类注释）。

**核心类**：`components/viz/client/frame_eviction_manager.h` 中的 **`FrameEvictionManager`**（`MemoryPressureListener`）：**`MODERATE`** 时 **`PurgeMemory(50)`**（按比例减少保留的未锁定帧数），**`CRITICAL`** 时 **`PurgeAllUnlockedFrames()`**：

```207:234:h:\chromium_142\src\components\viz\client\frame_eviction_manager.cc
void FrameEvictionManager::OnMemoryPressure(
    base::MemoryPressureLevel memory_pressure_level) {
  switch (memory_pressure_level) {
    case base::MEMORY_PRESSURE_LEVEL_MODERATE:
      PurgeMemory(kModeratePressurePercentage);
      break;
    case base::MEMORY_PRESSURE_LEVEL_CRITICAL:
      PurgeAllUnlockedFrames();
      break;
    case base::MEMORY_PRESSURE_LEVEL_NONE:
      // No need to change anything when there is no pressure.
      return;
  }
}

void FrameEvictionManager::PurgeMemory(int percentage) {
  int saved_frame_limit = max_number_of_saved_frames_;
  int remaining_frames = std::max(1, (saved_frame_limit * percentage) / 100);

  if (saved_frame_limit <= 1)
    return;

  CullUnlockedFrames(remaining_frames);
}

void FrameEvictionManager::PurgeAllUnlockedFrames() {
  CullUnlockedFrames(0);
}
```

**触发 eviction 的路径**：`CullUnlockedFrames` 对最老的 **未锁定** client 调用 **`EvictCurrentFrame()`**（`frame_eviction_manager.cc`）。典型 client 与 **DelegatedFrameHost** / **离屏标签** 测试相关（如 `render_widget_host_view_*` 中的用法）。

**注意**：这是 **GPU/合成相关显存与缓冲**，与 **NetworkService** 或 **HTTP 磁盘缓存** 无关；切回标签时可能 **重绘/重新提交帧**。

---

### 4.5 小节对照（便于记忆）

| 机制 | 主要目的 | 典型代码锚点 |
|------|----------|--------------|
| **Page freeze** | 后台页 **减活**（调度/定时器等） | `freezer.cc`、`web_contents_impl.cc` `SetPageFrozen` |
| **BFCache** | **后退/前进** 快速恢复，限条目的内存驻留 | `back_forward_cache_impl.*`、`render_frame_host_impl.cc` `DidEnterBackForwardCache` |
| **Frame eviction** | 不可见标签 **少囤合成帧**、压力下 **扔帧** | `components/viz/client/frame_eviction_manager.*` |
| **Tab discard** | **回收标签内存**，常需 **重新加载** | `urgent_page_discarding_policy.cc`、`tab_lifecycle_unit.cc` |

---

## 5. 和 `NetworkService::OnMemoryPressure` 对照

| 维度 | NetworkService 路径 | Renderer / Browser 路径 |
|------|---------------------|-------------------------|
| **主要释放对象** | 网络进程：socket 池、网络侧缓存、字典等 | Renderer：Blink 缓存、解码缓冲、V8；Browser：可 **丢弃整页** |
| **10 个前台标签的 DOM** | 基本 **不动** | **可能**通过丢弃/冻结 + Renderer 内 purge **动** |
| **用户主观感受** | 可能变慢、多费流量（重连/重拉） | 可能标签变灰/重载、后台页更激进被收 |

---

## 6. 维护说明

- 行为高度依赖 **feature flag / 平台 / 是否进程外**，升级 Chromium 后请以代码与 `base::FeatureList` 为准。
- 相关入口关键词：**`MemoryPressureListener`**、**`MemoryPurgeManager`**、**`Freezer::MaybeFreezePageNode`**、**`WebContentsImpl::SetPageFrozen`**、**`BackForwardCacheImpl`**、**`RenderFrameHostImpl::DidEnterBackForwardCache`**、**`viz::FrameEvictionManager`**、**`UrgentPageDiscardingPolicy`**、**`PageDiscardingHelper`** 等（以仓库符号为准）。
