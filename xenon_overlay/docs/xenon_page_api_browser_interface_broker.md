# Xenon `window.xenon` 与 BrowserInterfaceBroker — 面试备忘

本文整理 **feature/xenon-overlay** 中「页面 JS ↔ 浏览器进程」这条链路的实现细节，重点说明 **`BrowserInterfaceBroker` 的用法与原理**，并与 **Brave 钱包** 的同类模式对照，便于口述设计与排障。

---

## 1. 要解决什么问题

- 在 **普通网页 / WebUI** 的 **主世界（main world）** 上暴露 `window.xenon`，页面脚本可调用 `ping()`、`getApiVersion()` 等，底层与 **浏览器进程** 通信。
- 要求：**类型安全的 Mojo 接口**、符合 Chromium 多进程与安全模型，代码风格可对标 **Brave `JSEthereumProvider`**（`gin::Wrappable` + `DidClearWindowObject` + `GetBrowserInterfaceBroker().GetInterface`）。

---

## 2. 端到端数据流（一张图口述）

```text
┌─────────────────────────────────────────────────────────────────────────┐
│ renderer process                                                        │
│                                                                         │
│  XenonRenderFrameObserver::DidClearWindowObject()                       │
│       → JSXenonApi::Install(render_frame)                               │
│       → cppgc::MakeGarbageCollected<JSXenonApi>(..., render_frame)       │
│       → gin::Wrappable：window.xenon（只读全局属性）                     │
│                                                                         │
│  页面 JS： await xenon.ping()                                           │
│       → JSXenonApi::Ping(gin::Arguments*)                                │
│       → EnsureConnected()：                                             │
│             render_frame()->GetBrowserInterfaceBroker()                  │
│                 .GetInterface(xenon_host_.BindNewPipeAndPassReceiver())   │
│       → xenon_host_->Ping(Callback)  /* Mojo 异步 */                    │
│       → OnPing(...)：MicrotasksScope + Promise::Resolver::Resolve        │
└───────────────────────────────┬─────────────────────────────────────────┘
                                │ Mojo IPC（per-frame 管道，通常与帧任务队列绑定）
                                ▼
┌─────────────────────────────────────────────────────────────────────────┐
│ browser process                                                          │
│                                                                          │
│  chrome::internal::PopulateChromeFrameBinders(...)                       │
│       → xenon::PopulateXenonFrameBinders(map)                           │
│       → map->Add<xenon::mojom::XenonPageHost>(&BindXenonPageHost)       │
│                                                                          │
│  BindXenonPageHost(rfh, receiver)                                        │
│       → XenonPageHostImpl::Create(rfh, std::move(receiver))              │
│       → mojo::MakeSelfOwnedReceiver(impl, receiver)                     │
│                                                                          │
│  XenonPageHostImpl::Ping(callback) → std::move(callback).Run(ok, msg)    │
└─────────────────────────────────────────────────────────────────────────┘
```

**要点**：页面从不直接拿「浏览器对象指针」；只拿 **`mojo::Remote<XenonPageHost>`**，通过 **进程间消息** 调用实现于浏览器侧的 **`XenonPageHostImpl`**。

---

## 3. 涉及的核心类与文件（按层次）

### 3.1 Mojo（跨进程契约）

| 项 | 说明 |
|----|------|
| **`xenon_overlay/public/mojom/xenon_page_api.mojom`** | 定义 `xenon.mojom.XenonPageHost`：`Ping() => (bool ok, string message)`、`GetApiVersion() => (string version)`。 |
| **生成代码** | `xenon::mojom::XenonPageHost`（C++ 接口）、`PingCallback`、`PendingReceiver` / `Remote` 等。 |

### 3.2 渲染进程

| 类/文件 | 职责 |
|---------|------|
| **`xenon::XenonRenderFrameObserver`**（`xenon_render_frame_observer.{h,cc}`） | 继承 `content::RenderFrameObserver`；在 **`DidClearWindowObject`** 里判断是否允许注入，然后调用 `JSXenonApi::Install`。`OnDestruct` 里 `delete this`（与 Chrome 常见 observer 写法一致）。 |
| **`xenon::JSXenonApi`**（`js_xenon_api.{h,cc}`） | 继承 **`gin::Wrappable<JSXenonApi>`** + **`content::RenderFrameObserver`**；`static constexpr gin::WrapperInfo kWrapperInfo`（需在 **`gin/public/wrappable_pointer_tags.h`** 中占用独立 **`kXenonPageApi`**）；模板方法 `GetObjectTemplateBuilder` 注册 `name` / `ping` / `getApiVersion`。 |
| **`chrome::ChromeContentRendererClient::RenderFrameCreated`** | `new xenon::XenonRenderFrameObserver(render_frame)`（在 `ENABLE_XENON_SERVICE` 下）。 |

### 3.3 浏览器进程

| 类/文件 | 职责 |
|---------|------|
| **`xenon::XenonPageHostImpl`**（`xenon_page_host_impl.{h,cc}`） | 实现 `xenon::mojom::XenonPageHost`；持 `raw_ptr<content::RenderFrameHost>`；`Ping`/`GetApiVersion` 内校验 `IsRenderFrameLive()`。 |
| **`xenon::PopulateXenonFrameBinders`**（`xenon_frame_interface_binder.{h,cc}`） | 向 **`mojo::BinderMapWithContext<content::RenderFrameHost*>`** 注册 `XenonPageHost`。 |
| **`chrome::internal::PopulateChromeFrameBinders`**（`chrome_browser_interface_binders.cc`） | 在 `#if BUILDFLAG(ENABLE_XENON_SERVICE)` 下调用 `xenon::PopulateXenonFrameBinders(map)`。 |

### 3.4 WebUI 与自测页面

| 项 | 说明 |
|----|------|
| **`XenonWebDialog::GetXenonOverlayWebUIUrl()`** | `chrome://xenon-overlay/` 单一来源；`ShowXenonOverlay` 使用同一 URL。 |
| **`resources/webui/index.ts`** | 按钮调用 `window.xenon.ping()` / `getApiVersion()` 做页面侧自测。 |
| **注入白名单** | `XenonRenderFrameObserver::IsPageUrlEligibleForApi` 除 `https?`、`chrome-extension` 外，增加 **`chrome` scheme + host `xenon-overlay`**，与 WebUI host 一致。 |

---

## 4. BrowserInterfaceBroker：是什么、解决什么问题

### 4.1 问题背景

在 Chromium 中，**渲染进程不能直接持有浏览器进程 C++ 对象的指针**。跨进程协作需要：

- 明确的 **接口边界**（Mojo `.mojom`）；
- **能力最小化**（只暴露「这一帧/这一文档」需要的服务）；
- **由浏览器在合适的时机**把 **`PendingReceiver<Interface>`** 交给渲染端，渲染端 **`BindRemote`** 后得到 **`mojo::Remote<Interface>`**。

**`BrowserInterfaceBroker`** 就是「**按文档（frame）粒度**、在浏览器侧 **注册工厂**，在渲染侧 **按接口类型请求连接**」的统一入口。

### 4.2 两侧各自看到什么 API（概念）

- **浏览器（Browser 进程）**  
  - 使用 **`PopulateChromeFrameBinders`**（或等价逻辑）往 **`mojo::BinderMapWithContext<content::RenderFrameHost*>`** 里 **`Add<YourInterface>(&YourBinder)`**。  
  - `YourBinder(RenderFrameHost* rfh, PendingReceiver<YourInterface> receiver)` 里创建 **`YourImpl`**（常用 **`mojo::MakeSelfOwnedReceiver`** 或把实现了接口的对象绑到 `Receiver` 上）。  
  - **上下文是 `RenderFrameHost*`**：每个 **document / frame** 一次绑定，实现里可知「是哪个 RFH 发起的连接」，便于校验与计费等。

- **渲染（Renderer 进程）**  
  - 通过 **`content::RenderFrame::GetBrowserInterfaceBroker()`**（Blink 侧为 **`blink::BrowserInterfaceBrokerProxy`**）调用 **`GetInterface(receiver)`**。  
  - 典型写法：  
    `render_frame()->GetBrowserInterfaceBroker().GetInterface(remote.BindNewPipeAndPassReceiver());`  
  - 得到 **`mojo::Remote<xenon::mojom::XenonPageHost>`** 后，像普通异步 API 一样 `Ping(callback)`。

**原理一句话**：浏览器预先登记「某接口名 → 如何用当前 `RenderFrameHost*` 创建实现并接入管道」；渲染进程只发「我要这个接口」，内核把 **新管道的一端**交给渲染端 **`Bind`**，另一端交给浏览器端 **已注册的 Binder**，从而建立 **帧级、类型化的 Mojo 连接**。

### 4.3 与「ExposesInterfacesToRenderer」等非 Frame Binder 的区别（口述用）

- **`ExposeInterfacesToRenderer` / 进程级 BinderRegistry**：更偏 **进程级**、或 **与具体 RFH 无关** 的服务。  
- **`PopulateChromeFrameBinders` + `BinderMapWithContext<RenderFrameHost*>`**：**与每一帧（每一文档）绑定**，适合 **页面 API**（如 Xenon、支付、翻译驱动等），因为浏览器实现常需要 **`RenderFrameHost*`** 来做安全决策。

### 4.4 线程与回调（结合本次实现）

- **Frame-scoped** 的 `mojo::Remote`，Chromium 通常会把 **回复回调派发到与该 `RenderFrame` 一致的任务序列**（因此 **Brave 钱包不对 Mojo 回调再包一层 `BindPostTask`**；Xenon 与之一致时也可直接 `base::BindOnce`）。  
- **V8 `MicrotasksPolicy::kScoped`** 下，在异步路径里对 **`Promise::Resolver::Resolve/Reject`** 需在 **`MicrotasksScope`** 内执行；Brave 在 **`SendResponse`** 里使用 **`kDoNotRunMicrotasks`** + `isolate` + `context->GetMicrotaskQueue()`，Xenon 在 **`OnPing` / `OnGetApiVersion`** 对齐该写法，避免 Debug 构建下 microtask 队列 DCHECK。

---

## 5. `gin::Wrappable` + `cppgc` + `RenderFrameObserver`（口述）

- **`gin::Wrappable<JSXenonApi>`**：宿主对象与 V8 对象 **模板缓存**、**`WrapperInfo` + `WrappablePointerTag`** 防错 unwrap。实例用 **`cppgc::MakeGarbageCollected`** 分配在 **V8 cpp heap**。  
- **同时继承 `RenderFrameObserver`**：便于在 **`WillReleaseScriptContext`**（主世界）里 **`xenon_host_.reset()`**，在 **`OnDestruct`** 里再次 reset，避免帧销毁后仍向浏览器发 Mojo。  
- **`DidClearWindowObject`**：与 Brave 钱包类似，在 **window 对象被置干净的时机** 安装 **`window.xenon`**，用 **`DefineProperty`** 做 **不可配置、不可写** 的全局属性（对标 Brave `SetProviderNonWritable` 思路）。

---

## 6. 注入策略与安全（可追问点）

- **主帧**：当前实现仅在 **main frame** 安装（减少 iframe 攻击面；若要做成 Permissions-Policy 可再扩展）。  
- **安全上下文**：`blink::WebDocument::IsSecureContext()`。  
- **非 provisional frame**。  
- **URL 白名单**：`http`/`https`、`chrome-extension`、以及 **`chrome://xenon-overlay/`**（与 `ShowXenonOverlay` / WebUI host 对齐）。

---

## 7. 与 Brave `JSEthereumProvider` 对照（面试加分）

| 维度 | Brave | Xenon（本次） |
|------|--------|----------------|
| 挂对象时机 | `DidClearWindowObject` | 相同 |
| JS 宿主 | `gin::Wrappable` + cppgc | 相同 |
| 连浏览器 | `GetBrowserInterfaceBroker().GetInterface` | 相同 |
| 浏览器注册 | 在 Chrome frame binders 中 `Add` | `PopulateXenonFrameBinders` |
| Promise 回调内 V8 | `MicrotasksScope` + **`kDoNotRunMicrotasks`** | 对齐 |
| Mojo 回调线程 | 直接 `BindOnce`，无 `BindPostTask` | 对齐（frame-scoped remote） |
| 生命周期 | `WeakPtr` + `cppgc::Persistent` 等（钱包更复杂） | 当前 `Unretained`，可演进 `WeakPtr` |

---

## 8. 排障清单（dmp / 现场）

1. **`window.xenon` 为 `undefined`**：查 `DidClearWindowObject` 条件（URL scheme、安全上下文、是否主帧）、是否已 `Install`。  
2. **Mojo 连不上**：查浏览器是否 `PopulateXenonFrameBinders`、`BUILDFLAG(ENABLE_XENON_SERVICE)`、目标进程是否为带 Chrome frame binders 的 chrome（非裸 content shell 等）。  
3. **Debug 崩溃 + microtask DCHECK**：异步路径 `Resolve/Reject` 前补 **`MicrotasksScope`**（与 Brave 一致 **`kDoNotRunMicrotasks`**）。  
4. **`BrowserInterfaceBrokerProxy` incomplete type`**：cc 文件需 `#include "third_party/blink/public/platform/browser_interface_broker_proxy.h"`。

---

## 9. 一句话总结（30 秒版本）

**在渲染里用 `gin` 挂 `window.xenon`，通过 `RenderFrame::GetBrowserInterfaceBroker().GetInterface` 拿到 `Remote<XenonPageHost>`；浏览器在 `PopulateChromeFrameBinders` 里用 `BinderMapWithContext<RenderFrameHost*>` 注册实现类，建立帧级 Mojo 管道；异步返回后在 `MicrotasksScope(kDoNotRunMicrotasks)` 里解析 Promise，与 Brave 钱包模式一致。**

---

*文档对应实现树：`xenon_overlay/chrome/renderer/`、`xenon_overlay/chrome/browser/`、`xenon_overlay/public/mojom/xenon_page_api.mojom`，以及 `chrome/renderer/chrome_content_renderer_client.cc`、`chrome/browser/chrome_browser_interface_binders.cc` 中的集成。*

---

## 10. WebUI `PageHandler` vs `window.xenon`：`chrome_browser_interface_binders` 与 `WebUIBrowserInterfaceBrokerRegistry`

`chrome://xenon-overlay/` 同时可能看到 **`window.xenon`**（帧级全局 Broker）与 **`PageHandler.getRemote()`**（WebUI 专用管线）。二者都通过 Mojo 连到 Browser，但 **注册位置、Binder 粒度、JS 入口** 不同，排障与扩展接口时不要混用。

### 10.1 两条路径对照

| 维度 | `window.xenon` → `XenonPageHost` | WebUI → `PageHandler`（`xenon.mojom-webui.js`） |
|------|----------------------------------|------------------------------------------------|
| **Mojom** | `xenon_page_api.mojom`（`XenonPageHost`） | WebUI 专用 `*.mojom`（如 `PageHandler`） |
| **渲染侧入口** | `RenderFrame::GetBrowserInterfaceBroker().GetInterface`（C++ `JSXenonApi`） | TS：`PageHandler.getRemote()`（Mojo WebUI 生成物） |
| **浏览器侧注册** | `PopulateChromeFrameBinders` → `map->Add<XenonPageHost>(…)` | **`WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry().ForWebUI<…>().Add<PageHandler>()`**（Xenon 在首次建页时懒注册） |
| **Binder 所在 Map** | 与全 Chrome **共享**的 `mojo::BinderMapWithContext<RenderFrameHost*>` | **按 WebUIController 类型**一份「子 Map」，经 `PerWebUIBrowserInterfaceBroker` 挂到该 WebUI |
| **典型用途** | 普通页 / 扩展页 / WebUI 主世界注入的页面 API | WebUI 打包 JS、与 `MojoWebUIController` / `PageHandler` 配对 |

### 10.2 `chrome_browser_interface_binders`：`RegisterBrowserInterfaceBindersForFrame` 里做了什么

**入口**（Chrome）：`ChromeContentBrowserClient::RegisterBrowserInterfaceBindersForFrame` 对同一 `mojo::BinderMapWithContext<content::RenderFrameHost*>* map` 依次调用：

1. **`chrome::internal::PopulateChromeFrameBinders(map, render_frame_host)`** — 面向 **所有文档帧** 的通用接口（Xenon 的 **`PopulateXenonFrameBinders`** 在此挂载 `XenonPageHost`）。
2. **`chrome::internal::PopulateChromeWebUIFrameBinders(map, render_frame_host)`** — 仍写入 **同一个** `map`，但每条注册多通过 **`content::RegisterWebUIControllerInterfaceBinder<Interface, WebUIController…>(map)`** 完成。

**原理（帧级大图）**：

- 浏览器在**进程/帧**侧维护「接口名 → 如何用当前 `RenderFrameHost*` 绑定 `PendingReceiver`」的表。
- 渲染进程调用 **`GetBrowserInterfaceBroker().GetInterface`** 时，在内核里查这张表，把管道一端交给实现类。
- **`BinderMapWithContext<RenderFrameHost*>`** 的 **context** 让每个 Binder 回调都能拿到 **发起请求的帧**，便于做 `IsRenderFrameLive()`、origin 校验等。

**`PopulateChromeWebUIFrameBinders` 与 `PopulateChromeFrameBinders` 的差异**（易混点）：

- 名字都带 WebUI，但 **`PopulateChromeWebUIFrameBinders` 并不是 `WebUIBrowserInterfaceBrokerRegistry`**。
- 它把 **WebUI 特有的接口**注册进 **全局帧级** `BinderMap`；`RegisterWebUIControllerInterfaceBinder` 内部会 `GetWebUI()` / `GetAs<WebUIControllerSubclass>()`，**仅当当前帧确实是该类 WebUI** 时才 `BindInterface`，否则打日志并视为无效请求（见 `web_ui_controller_interface_binder.h`）。
- 因此：**内置 Omnibox、Settings 等「老」WebUI 页里用 C++/ gin 直接 `GetBrowserInterfaceBroker()` 拉接口的**，往往走这条 **RegisterWebUIControllerInterfaceBinder** 路径。

**接入步骤（帧级 Binder，与 Xenon `XenonPageHost` 同类）**：

1. 实现 `YourInterface` 的 Browser 侧实现， Binder 签名为 `(RenderFrameHost*, PendingReceiver<YourInterface>)`。
2. 在 `PopulateChromeFrameBinders`（或等价 embedder 入口）里 `map->Add<YourInterface>(&BindYourInterface)`。
3. 渲染侧在合法上下文中 `GetBrowserInterfaceBroker().GetInterface(receiver)`。

**接入步骤（WebUI 控制器分支 + 仍用全局帧 Map）**：

1. 实现 `YourWebUIController::BindInterface(PendingReceiver<…>)` 或 **`BindInterface(RenderFrameHost*, PendingReceiver<…>)`**（若需 RFH）。
2. 在 `PopulateChromeWebUIFrameBinders` 的某一 `Parts*.cc` 里调用 **`RegisterWebUIControllerInterfaceBinder<YourHandler, YourWebUIController>(map)`**。
3. 保证只在 **最外层主帧** 请求（Binder 里对 `GetParentOrOuterDocument()` 有检查）。

### 10.3 `WebUIBrowserInterfaceBrokerRegistry`：用法与原理

**用途**：为 **可信 / 不可信 WebUI** 单独建一套 **「仅含该 WebUI 允许接口」的 `BinderMap`**，与渲染侧 **Mojo WebUI / TypeScript** 生成代码配合（如 `Foo.getRemote()`），避免把所有帧级接口都暴露给 WebUI JS。

**注册入口**（Chrome）：`ChromeContentBrowserClient::RegisterTrustedWebUIInterfaceBrokers` / `RegisterUntrustedWebUIInterfaceBrokers` 调用 `chrome::internal::PopulateTrustedChromeWebUIFrameInterfaceBrokers` 等，对传入的 **`content::WebUIBrowserInterfaceBrokerRegistry& registry`** 执行大量 **`registry.ForWebUI<SomeController>().Add<SomeHandler>()`**。

**原理**（可直接对齐 `content/public/browser/web_ui_browser_interface_broker_registry.h` 头部注释）：

1. **注册阶段**：`ForWebUI<ControllerType>().Add<Interface>()` 并不立刻填全局帧 Map，而是往 **`WebUIController::Type → vector<BinderInitializer>`** 里追加一段 **可重复执行的初始化闭包**（因 `BinderMap::Add` 需模板参数，不能像普通函数指针一样扁平存储）。
2. **某 WebUI 开始加载**：按 Controller 类型取出 initializer 列表，构造 **`PerWebUIBrowserInterfaceBroker`**，跑完所有 initializer，得到 **只含该 WebUI 接口**的 `BinderMap`。
3. **`PerWebUIBrowserInterfaceBroker` 挂到 `WebUIController`**，并把 **`BrowserInterfaceBroker` 端点发给 renderer**。
4. Renderer 侧通过该 Broker 的 **`GetInterface`** 请求具体接口；JS 侧即生成物里的 **`PageHandler.getRemote()`** 等。

**与 `InterfaceRegistrationHelper::Add` 绑定的 C++ 写法**：生成的 binder 会 **`controller->GetAs<ControllerType>()`** 后调用 **`concrete_controller->BindInterface(std::move(receiver))`**（单参数）；若需 **RFH**，应在 Controller 上提供 **`BindInterface(RenderFrameHost*, PendingReceiver<…>)` 重载**，并由 `RegisterWebUIControllerInterfaceBinder` 那条路径处理——**`WebUIBrowserInterfaceBrokerRegistry` 模板默认走的是单 `PendingReceiver` 的版本**（见 `web_ui_browser_interface_broker_registry.h` 内 `InterfaceRegistrationHelper::Add`）。

**接入步骤（Registry 路径）**：

1. 在 `RegisterTrustedWebUIInterfaceBrokers`（或 Feature 拆分的 `Populate*Parts`）中写 **`registry.ForWebUI<MyWebUIController>().Add<MyPageHandler>()`**（链式 `.Add` 同属一次注册）。
2. **`MyWebUIController`** 继承 `MojoWebUIController`（或等价），实现 **`void BindInterface(mojo::PendingReceiver<MyPageHandler>)`**（或带 RFH 的重载，视生成端与宏而定）。
3. 在 Controller 构造或其它早期逻辑里挂 **`WebUIDataSource`**，把 **Mojo WebUI 编译出的 `.js`** 暴露为静态 URL（见下文 Xenon）。
4. WebUI 的 TS/JS `import { PageHandler } from '…mojom-webui.js'`，`PageHandler.getRemote()`。

**Xenon 的懒注册**（避免改中央 `PopulateTrustedChromeWebUIFrameInterfaceBrokers` 巨文件）：在 `XenonWebUIController` 构造函数里 **`std::call_once`** 调用 `WebUIBrowserInterfaceBrokerRegistry::GetTrustedRegistry().ForWebUI<XenonWebUIController>().Add<mojom::PageHandler>()`，绑定实现 **`XenonWebUIController::BindInterface`** → **`XenonPageHandler`**。

### 10.4 `xenon.mojom-webui.js` 从哪来、怎么进页面

**生成**：

- `xenon_overlay/resources/webui/BUILD.gn` 中 `build_webui("resources")` 的 **`mojo_files`** 指向 **`$root_gen_dir/xenon_overlay/chrome/browser/ui/webui/xenon.mojom-webui.ts`**，并依赖 **`//xenon_overlay/chrome/browser/ui/webui:mojo_bindings_ts__generator`**。
- WebUI 构建链把 TS 编成 **`tsc/xenon.mojom-webui.js`**；`xenon_overlay/resources/xenon_resources.grd` 将其打包为 **`IDR_XENON_WEBUI_XENON_MOJOM_WEBUI_JS`**。

**挂载与引用**：

- `XenonWebUIController`：`WebUIDataSource::AddResourcePath("xenon.mojom-webui.js", IDR_XENON_WEBUI_XENON_MOJOM_WEBUI_JS)`。
- `resources/webui/index.ts`：`import { PageHandler } from './xenon.mojom-webui.js'`，`const handler = PageHandler.getRemote()` — 与 C++ 侧 **`WebUIBrowserInterfaceBrokerRegistry` + `BindInterface`** closure。

### 10.5 速记：该改哪份文件

| 目标 | 优先改 |
|------|--------|
| 所有页面可用的 `window.xenon` / `XenonPageHost` | `xenon_frame_interface_binder.cc`、`chrome_browser_interface_binders.cc`（`PopulateChromeFrameBinders` 调用链） |
| 仅 `chrome://xenon-overlay/` 的 `PageHandler` | `xenon_webui_controller.cc`（`EnsureTrustedBrokerKnowsPageHandler`、`BindInterface`、`AddResourcePath`）、对应 `*.mojom` 与 `build_webui` / `mojo_bindings_ts__generator` |

---

**权威引用路径**：`chrome/browser/chrome_content_browser_client_receiver_bindings.cc`（`RegisterBrowserInterfaceBindersForFrame` / `RegisterTrustedWebUIInterfaceBrokers`）、`chrome/browser/chrome_browser_interface_binders_webui.cc`（`PopulateChromeWebUIFrameBinders`、`PopulateTrustedChromeWebUIFrameInterfaceBrokers`）、`content/public/browser/web_ui_browser_interface_broker_registry.h`、`content/public/browser/web_ui_controller_interface_binder.h`、`xenon_overlay/chrome/browser/ui/webui/xenon_webui_controller.cc`。
