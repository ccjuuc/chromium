# Chromium / Brave 设计模式与源码示例（细粒度整理）

本文档汇总 **Chromium 主干** 与 **Brave 浏览器**（`brave/`）中常见的设计模式：每种模式给出 **定义、解决的问题、典型结构、双端示例路径、设计动机与实践注意**。便于对照读源码与做 embedder 扩展。

---

## 阅读约定

| 约定 | 含义 |
|------|------|
| **Chromium 树** | 下文示例以本仓库 `//` 根路径为准（如 `content/`, `chrome/`, `third_party/blink/`）。 |
| **Brave 树** | 独立检出时路径通常为 `F:\brave_browser\src\`；示例统一写 `//brave/...`（相对 Brave 的 `src` 根）。 |
| **GoF** | Gang of Four 经典廿三种模式；浏览器里还有不少 **Chromium 惯用法**（非严格 GoF，但团队层面等同于“固定套路”）。 |

---

## 目录

1. [Mojo 接口 + 跨进程代理](#1-mojo-接口--跨进程代理)
2. [DocumentService / Receiver 与浏览器侧实现](#2-documentservice--receiver-与浏览器侧实现)
3. [观察者（Observer）](#3-观察者observer)
4. [责任链（Chain of Responsibility）— NavigationThrottle](#4-责任链chain-of-responsibility--navigationthrottle)
5. [KeyedService + ProfileKeyedServiceFactory](#5-keyedservice--profilekeyedservicefactory)
6. [进程级门面（Facade）— BrowserProcess / BraveBrowserProcess](#6-进程级门面facade--browserprocess--bravebrowserprocess)
7. [委托（Delegate）](#7-委托delegate)
8. [异步回调与生命周期 — WeakPtrFactory + WeakPtr + Bind](#8-异步回调与生命周期--weakptrfactory--weakptr--bind)
9. [RAII 与所有权](#9-raii-与所有权)
10. [Brave 特有：`chromium_src/` 补丁式集成](#10-brave-特有chromium_src-补丁式集成)
11. [速查表](#11-速查表)

---

## 1. Mojo 接口 + 跨进程代理

### 一句话定义

用 **IDL（`.mojom`）描述接口**，由代码生成器产出 **代理（Proxy/Remote）与桩（Stub/Receiver）**，在 **不同进程** 之间以消息方式调用 C++ 虚函数语义。

### 解决的问题

- 沙箱渲染进程不能直接访问浏览器进程对象。
- 需要 **稳定、可版本演进** 的二进制协议，而不是手写 IPC 结构体。
- 统一 **能力暴露边界**（谁能通过 `BrowserInterfaceBroker` 拿到哪个接口）。

### 典型结构

```
.mojom 定义 interface
    → 生成 *.mojom.h / *.mojom.cc
Renderer: mojo::Remote<IFace>（发调用）
Browser:  mojo::Receiver<IFace> + 实现类（接调用）
```

### Chromium 示例

**文件：** `third_party/blink/public/mojom/frame/data_mask.mojom`

```mojom
interface DataMask {
  SendData(int32 request_id, DataMaskRules data);
  SendXpathData(XPathConfig data);
};

interface DataMaskToMain {
  SendDataToMain(int32 request_id, string data);
};
```

- **要点：** `DataMask` / `DataMaskToMain` 是两个方向的接口边界；embedder 可通过 `BrowserInterfaceBroker` 按帧路由，匹配与渲染可在实现层完成（与 mojom 注释一致）。

### Brave 示例

**文件：** `brave/components/playlist/core/common/mojom/playlist.mojom`（及同目录下相关 mojom）

Brave 在 `brave/components/**/*.mojom` 中为 Wallet、Rewards、Playlist、AI Chat 等定义大量接口，结构与 Chromium 相同：**组件隔离 + 跨进程契约**。

### 为何这么做

| 动机 | 说明 |
|------|------|
| 安全 | 渲染进程默认不可信；IPC 是显式边界。 |
| 可维护 | `.mojom` 即文档；变更有编译期与协议层检查。 |
| 多进程一致 | 与 Utility/GPU 等服务通信同一套绑定栈。 |

### 实践注意

- 接口粒度：**帧级** 与 **进程级** 服务要分清，避免一个巨型 interface。
- 敏感 payload（如 `SendDataToMain` 的字符串）避免直接打日志。

---

## 2. DocumentService + Receiver 与浏览器侧实现

### 一句话定义

在 **浏览器进程** 侧，将 **与某一 `RenderFrameHost`/文档 绑定的 Mojo 服务** 封装为 `content::DocumentService<Interface>`：**一个 Receiver、一条生命周期，随文档走**。

### 解决的问题

- 避免在业务类里重复写 `Receiver` 绑定、帧销毁时断开连接等样板。
- 与 **Frame 生命周期** 对齐减少 UAF 风险。

### Chromium 示例（Xenon）

**文件：** `xenon_overlay/chrome/browser/xenon_data_mask_to_main_host_impl.h`

```cpp
class XenonDataMaskToMainHostImpl
    : public content::DocumentService<blink::mojom::DataMaskToMain> {
 public:
  static void Create(content::RenderFrameHost* render_frame_host,
                     mojo::PendingReceiver<blink::mojom::DataMaskToMain> receiver);
  // ...
 private:
  void SendDataToMain(int32_t request_id, const std::string& data) override;
};
```

- **要点：** 渲染端用 `Remote` 调 `SendDataToMain`；浏览器端 `PendingReceiver` 创建实现类，基类内部持有 `Receiver`（与本项目头文件注释一致）。

### Brave 示例

Brave 大量功能走 **Profile 级 KeyedService + Mojo**，帧级服务同样存在（具体类名因功能而异）；识别方式仍是：**`*Host` / `*Handler` + `mojo::Receiver` 或 `DocumentService`**。

### 为何这么做

- **作用域正确：** 服务随 RFH/文档销毁，不泄漏到全局。
- **模式统一：** 与 Chromium content 推荐写法一致，code review 友好。

### 实践注意

- 若服务必须 **跨导航存活**，不应绑在单一 `DocumentService` 上，需重新评估 Profile 级或 WebContents 级托管。

---

## 3. 观察者（Observer）

### 一句话定义

主题（框架类）维护观察者列表，在 **生命周期或状态事件** 发生时逐个 **通知**；业务方继承 `*Observer` 或实现 `AddObserver` 接口，**不修改核心类源码**。

### 解决的问题

- 一对多扩展：广告屏蔽、统计、功能开关都可订阅同一事件。
- 降低 `content/` 与 `chrome/`、embedder 之间的编译耦合。

### Chromium 示例（Xenon）

**文件：** `xenon_overlay/chrome/renderer/xenon_render_frame_observer.cc`

```cpp
XenonRenderFrameObserver::XenonRenderFrameObserver(
    content::RenderFrame* render_frame)
    : content::RenderFrameObserver(render_frame) {}

void XenonRenderFrameObserver::OnDestruct() {
  delete this;
}

void XenonRenderFrameObserver::DidStartNavigation(
    const GURL& url,
    std::optional<blink::WebNavigationType> navigation_type) {
  last_navigation_url_ = url;
}
```

- **要点：** `RenderFrameObserver` 是典型的帧级观察者；`OnDestruct` 里 `delete this` 与 Chromium 约定一致（由框架驱动生命周期时需对照基类文档）。

### Brave 示例

**文件：** `brave/components/playlist/content/renderer/playlist_render_frame_observer.h`

```cpp
class PlaylistRenderFrameObserver final
    : public content::RenderFrameObserver,
      public content::RenderFrameObserverTracker<PlaylistRenderFrameObserver>,
      public mojom::PlaylistRenderFrameObserverConfigurator {
```

**文件：** `brave/components/psst/browser/content/psst_tab_web_contents_observer.h`（继承 `content::WebContentsObserver`）

**组件内嵌观察者：** 如 `SidebarService::Observer` : `base::CheckedObserver` — 强调 **线程与生命周期安全** 的观察者基类。

### 为何这么做

| 动机 | 说明 |
|------|------|
| 开闭原则 | 对扩展开放，对 `WebContents`/`RenderFrame` 修改闭合。 |
| 解耦 | 新功能多为「再挂一个 Observer」，而非改中央调度。 |

### 实践注意

- `CheckedObserver` 用于 **跨序列复杂度较高** 的订阅场景；注销顺序要符合 DCHECK 预期。
- 避免在回调里做 **长时间阻塞**（尤其 UI 线程）。

---

## 4. 责任链（Chain of Responsibility）— NavigationThrottle

### 一句话定义

对 **同一次导航**，按注册顺序依次调用多个 `NavigationThrottle`；每个节点返回 **PROCEED / DEFER / CANCEL / BLOCK_*** 等，形成 **可组合的决策链**。

### 解决的问题

- 企业策略、安全、Tor、扩展、Brave Shields 都要在导航管道中表态。
- 单一巨型 `if` 不可维护；**插入/排序** 比修改中央类更清晰。

### Chromium 核心类型

**文件：** `content/public/browser/navigation_throttle.h`

- `NavigationThrottle::ThrottleAction`：`PROCEED`, `DEFER`, `CANCEL`, `BLOCK_REQUEST`, `BLOCK_RESPONSE`, …
- `ThrottleCheckResult`：动作 + `net::Error` + 可选错误页 HTML。

### Brave 示例

**文件：** `brave/components/tor/tor_navigation_throttle.h`

```cpp
class TorNavigationThrottle : public content::NavigationThrottle,
                              public TorLauncherObserver {
 public:
  static void MaybeCreateAndAdd(content::NavigationThrottleRegistry& registry,
                                bool is_tor_profile);
  ThrottleCheckResult WillStartRequest() override;
  ThrottleCheckResult WillRedirectRequest() override;
  // ...
 private:
  void OnTorCircuitEstablished(bool result) override;  // TorLauncherObserver
};
```

- **要点：** Tor 在 `WillStartRequest` 等阶段 **DEFER** 直到电路就绪，再通过 `TorLauncherObserver` **Resume**；同一模式可用于任何「异步策略判定」。

### 为何这么做

- **单一职责：** 每个 throttle 只处理一类策略。
- **异步友好：** `DEFER` + `Resume` 与网络/IPC 对齐。
- **可测试：** 可单独单测每个 throttle（Brave 中见 `FRIEND_TEST_ALL_PREFIXES`）。

### 实践注意

- DEFER 拖慢导航；注释中 Chromium 官方也强调应加直方图、能异步则异步。
- 注意 **同一导航不执行 throttle** 的导航类型（如同文档、部分 BFCache/预渲染相关路径）。

---

## 5. KeyedService + ProfileKeyedServiceFactory

### 一句话定义

**按 `Profile` / `BrowserContext` 为键** 托管服务实例：工厂负责创建与依赖声明，服务实现 `KeyedService`，支持 **两阶段 Shutdown** 再析构。

### 解决的问题

- 多 Profile 下禁止「全局单例」承载用户数据。
- 统一 **依赖方向** 与 **销毁顺序**，减少 Shutdown 阶段野指针。

### Chromium 核心接口

**文件：** `components/keyed_service/core/keyed_service.h`

```cpp
// Interface for keyed services that support two-phase destruction order.
class KEYED_SERVICE_EXPORT KeyedService {
 public:
  virtual ~KeyedService() = default;
  virtual void Shutdown() {}
};
```

### Chromium / Chrome 使用方式（概念）

- `ProfileKeyedServiceFactory::GetForProfile(profile)` 懒创建。
- `BuildServiceInstanceForBrowserContext` 里组装依赖。

### Brave 示例

**文件：** `brave/browser/serp_metrics/serp_metrics_service_factory.h`

```cpp
class SerpMetricsServiceFactory : public ProfileKeyedServiceFactory {
 public:
  static SerpMetricsServiceFactory* GetInstance();
  static SerpMetricsService* GetFor(content::BrowserContext* context);
 private:
  std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
      content::BrowserContext* context) const override;
};
```

同类还有：`BraveShieldsSettingsServiceFactory`、`LocalAIServiceFactory`、`WebcompatReporterServiceFactory` 等。

### 为何这么做

| 动机 | 说明 |
|------|------|
| 隔离 | 不同 Profile 数据不串通。 |
| 生命周期 | 与 Profile 销毁绑定，先于「裸全局」更安全。 |

### 实践注意

- `Shutdown()` 阶段 **不要再从 Profile 拉其它 KeyedService**（KeyedService 头文件中的约定）。
- iOS 上有 `ProfileKeyedServiceFactoryIOS` 等平行实现，思路相同。

---

## 6. 进程级门面（Facade）— BrowserProcess / BraveBrowserProcess

### 一句话定义

**整个浏览器进程** 的顶层入口：提供懒创建的 **全局服务**（组件更新、跨 Profile 基础设施等），对外隐藏子系统初始化顺序。

### 解决的问题

- 避免 scattered statics；统一「谁在进程启动后可用」。
- 与 **Profile 级 KeyedService** 分工：进程级 vs 用户配置级。

### Brave 示例

**文件：** `brave/browser/brave_browser_process.h`

注释（节选含义）：**管理应用全局服务；每个服务首次请求时懒创建；getter 可能返回 null，调用方必须判空。**

调用点举例：`g_brave_browser_process->ad_block_service()`（出现于测试、`chromium_src` 补丁等）。

### Chromium 侧对应概念

- `chrome/browser/browser_process.h`（Chrome）；`content` 层不定义 Brave 这一层，但 embedder 模式一致。

### 为何这么做

- **认知负荷：** 「进程级找 BrowserProcess」降低搜索成本。
- **依赖注入测试：** `TestingBrowserProcess` / `TestingBraveBrowserProcess` 替换实现。

### 实践注意

- 不要把 **Profile 专属状态** 塞进 BrowserProcess，否则多用户场景必错。

---

## 7. 委托（Delegate）

### 一句话定义

框架对象（如 `WebContents`）把 **「是否允许 / 谁来处理」** 的决定交给外部 **`Delegate`**，自身只负责通用状态机。

### 解决的问题

- `content` 不依赖具体浏览器产品；Chrome / Brave / WebView 行为差异靠 Delegate 注入。

### Chromium 典型类型

- `content::WebContentsDelegate`：新窗口、下载、全屏等 **策略性回调**。

### 与 Observer 的简明区分

| | Observer | Delegate |
|---|----------|----------|
| 侧重点 | 「发生了什么」的通知 | 「是否做 / 怎么做」的决策 |
| 典型问题 | 记录、同步 UI 状态 | 是否打开新标签、是否拦截导航展示 |

### Brave

Brave 在 `chrome/browser` 层与 delegate 链路上复用 Chromium；具体类名分散在各功能模块中，**识别特征**为 `*Delegate` 与 `WebContentsDelegate` 子类。

### 为何这么做

- **依赖倒置：** content 依赖抽象接口，不依赖 brave/chrome 具体类。

---

## 8. 异步回调与生命周期 — WeakPtrFactory + WeakPtr + Bind

### 一句话定义（Chromium 惯用法）

异步任务使用 `base::BindOnce` / `RepeatingCallback` 闭包；对象上挂 **`base::WeakPtrFactory<T>`**，通过 **`GetWeakPtr()`** 得到 **`base::WeakPtr<T>`** 并绑定到回调；**对象销毁后**，所有尚未执行的回调会因 **WeakPtr 失效** 而不会访问已释放的 `this`。

### WeakPtrFactory 与 WeakPtr 是什么关系

二者是 **工厂（签发点）** 与 **产品（可被拷贝的弱引用句柄）** 的关系，共同解决：**异步回调执行时，发起方对象可能早已析构**。

| 类型 | 归属与角色 |
|------|------------|
| `base::WeakPtrFactory<T>` | **成员变量**，挂在 `T` 实例上（常见写法 `weak_factory_{this}`）。负责 **`GetWeakPtr()` 签发** `WeakPtr<T>`；在 **`T` 析构、`WeakPtrFactory` 析构时**，将所有已签发的 weak 引用 **标记为无效**。 |
| `base::WeakPtr<T>` | 从 `GetWeakPtr()` 得到的 **弱引用**。可 **拷贝、传给 `BindOnce`**；绑定到成员函数时，调度器会在调用前检查是否仍有效，**无效则不再调用**（避免 UAF）。 |

关系可记为：**Factory 在对象内部、随对象生死；WeakPtr 是对外传递的「票据」，对象没了票据自动作废。**

### 典型代码形态

```cpp
// Foo.h
class Foo {
 public:
  void DoAsync();

 private:
  void OnDone(int result);

  base::WeakPtrFactory<Foo> weak_factory_{this};
};

// Foo.cc
void Foo::DoAsync() {
  DoSomethingAsync(base::BindOnce(&Foo::OnDone, weak_factory_.GetWeakPtr()));
}
```

- `Foo` 析构 → `weak_factory_` 析构 → 尚未运行的 `OnDone` 发现 `WeakPtr` 无效 → **不会**对已死的 `this` 做成员调用。

### 实现直觉（不必背源码）

`WeakPtrFactory` 内维护 **代数（generation）或等价状态**；每次 `GetWeakPtr()` 与当前代绑定。`T` 析构时工厂 bump 一代，旧 `WeakPtr` 与当前代不一致即视为 **invalid**。具体见 `base/memory/weak_ptr.h`。

### 与 `std::weak_ptr` 的对比（建立直觉）

| | `std::weak_ptr` + `shared_ptr` | Chromium `WeakPtr` + `WeakPtrFactory` |
|---|----------------------------------|----------------------------------------|
| 所有权模型 | 共享所有权 | **单所有者**对象 + 仅取消异步回调 |
| 典型用途 | 打破 `shared_ptr` 环、窥视对象是否还活着 | **PostTask / Mojo / 导航** 等异步：`this` 没了就别回调 |
| 引入成本 | 控制块、原子引用计数 | 无 `shared_ptr`；注意 **序列（sequence）/线程** 使用约束（以头文件与 DCHECK 为准） |

### 解决的问题

- 导航、Mojo、`PostTask` 全是异步；回调里 **裸捕 `this`** 易产生 **Use-After-Free**。

### 为何这么做

- **默认安全：** 比「在析构里逐个取消登记」更不易漏。
- **局部化：** 弱引用生命周期与 **拥有 Factory 的那个对象** 绑定，心智简单。

### 实践注意

- **成员声明顺序（惯例）：** 不少类把 **`WeakPtrFactory` 放在成员列表最后** ，以便在复合析构时，**先析构其它成员、再析构 Factory**，避免其它成员析构阶段还有「未失效」的 WeakPtr 指向半销毁对象。以具体类注释/风格指南为准。
- **序列一致性：** 在错误线程或序列上使用 WeakPtr 可能触发 DCHECK；需与 **TaskRunner / Mojo 绑定序列** 对齐。
- 示例：`brave/components/playlist/.../playlist_render_frame_observer.h` 等包含 `base/memory/weak_ptr.h`，常与异步脚本或 IPC 配合。

---

## 9. RAII 与所有权

### 一句话定义

用 **作用域**（栈对象、智能指针）管理资源： acquire 在构造，release 在析构；**唯一所有权** 优先 `std::unique_ptr`。

### Chromium 中的体现

- `std::unique_ptr` 工厂产出、`DocumentService`、Mojo 管道绑定。
- `base::ScopedClosureRunner` 等「出作用域即运行清理」。

### 为何这么做

- **异常与早退路径安全**；大规模 C++ 仓库减少资源泄漏审查成本。

---

## 10. Brave 特有：`chromium_src/` 补丁式集成

### 一句话定义（工程模式）

在 **不改变上游文件路径语义** 的前提下，用 **同名覆盖**（或 GN 重定向）把 Brave 修改「织入」Chromium 构建：**diff 集中、合并上游成本低**。

### 示例位置

- `brave/chromium_src/chrome/...`
- `brave/chromium_src/third_party/blink/...`

### 为何这么做

| 动机 | 说明 |
|------|------|
| 合并成本 | 上游更新时 diff 主要在 brave 自有目录与 `chromium_src`。 |
| 可见性 | 一眼区分「我们动过哪些上游接触点」。 |

### 实践注意

- 补丁应 **最小化**；能走组件 + 工厂 + Delegate 的，不要扩大 `chromium_src`。

---

## 11. 速查表

| 模式 / 惯用法 | 你在代码里最先看到的线索 | Chromium 示例（本仓/通用） | Brave 示例 |
|---------------|-------------------------|---------------------------|------------|
| Mojo | `.mojom`, `Remote`, `Receiver` | `data_mask.mojom` | `brave/components/**/*.mojom` |
| DocumentService | `DocumentService<.*mojom` | `XenonDataMaskToMainHostImpl` | 各 Host/Handler |
| Observer | `*Observer`, `RenderFrameObserver` | `XenonRenderFrameObserver` | `PlaylistRenderFrameObserver`, `PsstTabWebContentsObserver` |
| 责任链 | `NavigationThrottle`, `AddThrottle` | `navigation_throttle.h` | `tor_navigation_throttle.*` |
| KeyedService | `ProfileKeyedServiceFactory` | `components/keyed_service` | `SerpMetricsServiceFactory` 等 |
| 进程门面 | `g_*_browser_process`, `BrowserProcess` | `browser_process.h`（Chrome） | `brave_browser_process.h`, `g_brave_browser_process` |
| Delegate | `WebContentsDelegate` | `content/public/browser` | 各浏览器层 delegate 实现 |
| 异步安全 | `WeakPtrFactory` 签发 `WeakPtr`，与 `BindOnce` 绑定 | 遍布 `content/`, `base/` | 同左 |
| RAII | `unique_ptr`, `Scoped…` | 同左 | 同左 |
| 补丁集成 | 路径在 `chromium_src` | （Chrome 无此目） | `brave/chromium_src/**` |

---

## 修订说明

- 文档可随你们分支上的实际类名增量维护；**路径以本地检出为准**（Brave 磁盘盘符可能为 `F:\` 等）。
- 若需 **单功能纵深**（例如「Shields 从 factory 到 throttle 到网络层」），建议另开一篇按调用链拆解的文档，避免本篇篇幅失控。

---

*本文档置于 `xenon_overlay/docs/`，与 Xenon 扩展同仓，便于与 `data_mask`、Mojo、Observer 等实现对照阅读。*
