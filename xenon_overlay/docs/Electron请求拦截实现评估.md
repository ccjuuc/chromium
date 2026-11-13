# Electron 请求拦截实现评估

评估日期：2026-11-11。

本文记录通用请求拦截的需求、实现边界与验证要求。`Session.webRequest.onBeforeRequest` 和 `onHeadersReceived` 已接入 JS、Utility、Browser 与 Chromium 请求工厂；Windows 生产构建已通过。真实应用与独立集成探针仍需按下述验收项验证，不能把编译成功视为完整兼容。第三方应用的源码、ASAR 和发行产物保持不变。

## 2026-11-12：请求发送头扩展

`onBeforeSendHeaders` 复用现有 Session 注册、异步分发与 trusted header client 链，在真实请求发送前修改头，供任意 Electron 应用按自己的 filter 使用。product27 已完成正式 Windows 构建和独立本地 HTTP 验收；验收目标为标准接口行为，不依赖某个应用是否启动成功。

* `details.requestHeaders` 为网络层真实头的字符串字典，保留实际头名大小写，包含 Cookie 和前置 header client 的修改。回调的 `requestHeaders` 同样接受字符串字典；省略字段保留原头，空对象清除可改写的头，`cancel: true` 取消请求。HTTP 传输仍可生成 Host、Connection 等必要头。
* JavaScript 按插入顺序把回调头名规范为小写，后一个大小写别名覆盖前一个；回调时复制结果，之后再修改应用对象或再次回调不影响请求。Native 继续校验头名、NUL/CR/LF 以及总大小。重定向每次发送重新进入回调，不把上一跳的改头永久写入原始请求。
* 保留已有的异步 callback、超时、撤销、Session 隔离及销毁约束。请求头只能出现在 `onBeforeSendHeaders` 的回调，不能借响应头事件或 `onBeforeRequest` 改写。
* 本轮不支持请求头数组值，遇到数组明确报错。Electron v39.2.7 的 [API 文档](https://github.com/electron/electron/blob/v39.2.7/docs/api/web-request.md) 标注 `string | string[]`，但该版本 [HttpRequestHeaders 转换器](https://github.com/electron/electron/blob/v39.2.7/shell/common/gin_converters/net_converter.cc) 只读取字符串。此处不猜测数组或 Cookie 的拼接规则，也不静默删除非字符串头。响应头现有的数组支持不变。

新增 JavaScript 请求头契约覆盖异步等待、改写和删除、大小写覆盖、Cookie 原值、回调后修改隔离、取消、撤销、非法字段与注入拒绝。请求拦截专项 14/14，与 BrowserWindow 组合回归 107/107 通过。product27 原生 IPC 609/609、Session 140/140 通过。

独立探针 `F:/electron-compat-audit/20261111/probe-request-headers.cjs` 的 `product27-01` 轮次 10/10 通过：真实 renderer fetch 的服务端原始记录确认头的增改删、Cookie 改写和清空、取消和非法头的零服务端命中、异步回调之后才发送、不同 Session 隔离、重定向各跳重算、请求/响应头组合、`null` 撤销恢复原头。等待 350 毫秒的用例实际用时 368 毫秒，服务端到达时间不早于回调时间；请求详情也保留 `Cookie`、`User-Agent` 原始大小写。

证据为 `request-headers-product27-01/verification.json`，相对于上述审计目录。宿主退出码 0，无所属残留进程或新增 dump，9260 端口已释放。非法头用例故意产生 `Invalid request header value` 日志，这是拒绝注入的预期结果。本轮真实网络验收覆盖本地 HTTP，不据此宣称 HTTPS、全部预检或所有协议路径已实测。

## 请求发送头扩展前的实现与验证

以下为 product26 及更早构建已验收的两个事件；新增请求发送头事件的状态见上节。

* 支持两个事件的 listener/filter 注册、替换和 `null` 撤销，按容器与 Session partition 隔离。其他 webRequest 事件继续明确返回未支持错误。
* 请求等待真实 callback；允许异步返回后继续等待 callback。重复或过期 callback 不重复完成。异常和 Promise 拒绝失败；每个 Utility 上限 128 个挂起请求，等待上限 30 秒。
* HTTP/HTTPS 在 trusted header client 阶段修改响应头，早于 CORS 检查，并串联已有 header client。file/ASAR 使用底层工厂前取消与交付响应前改头；修改后通过 NetworkService 重建解析头。自定义协议简单跨源请求在原有 CORS 检查之前使用新响应头。
* 支持同 scheme 以及 HTTP/HTTPS 间重定向并重新匹配。需要切换其他协议工厂的跨 scheme 重定向返回 `ERR_NOT_IMPLEMENTED`；不静默放行。
* WebSocket 的显式 ws/wss pattern 或 websocket resource type 注册返回 `ERR_NOT_SUPPORTED`。`<all_urls>` 只覆盖已接线的 URLLoader 路径，不承诺 blob、Blink 直接处理的 data 子资源和 Service Worker 合成响应。自定义协议复杂请求的原有预检限制仍在，不能从简单 GET 成功推断预检支持。
* 本实现没有平台窗口 API 或 TH/PLE/draw.io 名称分支；Chromium 原文件的接线受 `ENABLE_XENON_SERVICE` 宏控制。macOS/Linux 未在本机编译，不能声明已经完成这些平台的运行验证。

验证进度：JS 契约 11/11 通过，覆盖完整 bootstrap、异步 callback、异常、取消、撤销及响应头规范化。主任务的原生集成集 388/388、Session 19/19 通过（含 12 项 WebRequest 代理测试，日志 `web-request-session-native-02.log`）。独立 fixture 与 13 项真实请求探针位于 `F:/electron-compat-audit/20261111/web-request-fixture/` 和 `probe-web-request.cjs`。

* `web-request-probe-01`：初始 HTTP 导航未完成，尚未进入 13 项检查。修复 URLLoader 与 client 两条 Mojo 管道的完成时序后，此问题不再复现。
* `web-request-probe-02`：原有 13 项断言全部通过，包括真实 CSP/CORS、取消、延迟、分区、重定向、DevTools 组合与销毁。但检查完整事件日志发现 file 子资源的 `onHeadersReceived` 重复触发；因此该轮不是最终验收。探针已补每个请求每个事件仅触发一次的断言，待去重后复验。
* `web-request-probe-03`：移除 file 子资源工厂的重复包装后，加强版 13/13 全部通过。file 主文档与脚本的两个事件各触发一次，重定向保留请求 id，自定义协议与 DevTools 组合也无重复回调。HTTP 取消的服务端命中数为 0；文件取消的底层工厂零调用另由原生测试证明。
* 三轮宿主均正常退出，无残留进程和新 dump。已通过的探针不覆盖下文列出的所有请求路径；不能据有限验收推断完整 Electron 兼容。

## 原包需求与评估时阻塞

检查对象为未经修改的 draw.io 32.6.10 Windows 发行包：

`F:/electron-compat-audit/20261111/drawio/release/resources/app.asar`

ASAR 内的 `src/main/electron.js` 有两个连续的注册：

| 位置 | 原包调用 | 必须产生的真实效果 |
| --- | --- | --- |
| 831–844 行 | `session.defaultSession.webRequest.onHeadersReceived(listener)`，省略 filter；回调返回展开原头后的 `responseHeaders`，设置数组形式的 `Content-Security-Policy` | 在内容交付、CSP 检查之前修改该 Session 的真实响应头。本地 file/ASAR 页面也在范围内 |
| 847–860 行 | `onBeforeRequest({urls: ['file://*']}, listener)`；对不属于代码目录的 URL 返回 `{cancel: true}`，其他返回 `{}` | 在读取目标文件、启动请求之前取消不允许的访问 |

代码目录来自 233–234 行的 `codeDir` 和 `url.pathToFileURL(codeDir)`。主窗口在 607 行加载本地 URL；导入、导出窗口也使用 file URL。这里不能只实现 HTTP 网络请求。

`F:/electron-compat-audit/20261111/drawio/xenon-retest-05/browser.log:38` 记录了第一个注册的未支持错误。修复第一个注册后，第二个注册仍是必须处理的能力，不能以接受空 listener 的方式越过启动检查。

评估时 `main/windows.js` 的所有 webRequest 方法均调用 `electronUnsupported`。当前 Session 改为使用独立的 `main/web_request.js`；Browser 注册表、工厂代理与过滤器分别位于 `xenon_electron_web_request.{h,cc}` 和 `xenon_electron_web_request_filter.{h,cc}`。

## 最小真实语义

注册应属于精确的 **BrowserContext、container、Session partition 和 Utility 实例 generation**，不能只按浏览器 Profile 或网页 URL 区分。

* 支持 `method(listener)` 和 `method(filter, listener)`；省略 filter 匹配该 Session 的全部已覆盖请求。每个事件只保留最后一个 listener；传入 `null` 撤销该事件。
* 使用 Chromium `URLPattern` 语义处理 `urls`、`excludeUrls`；支持 `<all_urls>`、file、scheme/host/path 通配符和端口，不把匹配规则简化为普通字符串 glob。`types` 按 Electron 的 resourceType 枚举过滤。无效参数应在注册时失败。
* 空 `urls` 的语义必须随声明兼容的 Electron 版本确定：Electron v39.2.7 将空数组按全部匹配处理并标记废弃；不能以未声明的自定义语义替代。
* listener 使用 `listener(details, callback)`。必须等待 callback，不能把 listener 的普通返回值当作完成，也不能在收到 `undefined` 后直接放行。延迟、重复 callback、抛异常、容器退出和请求取消都必须有明确的完成行为。
* `onBeforeRequest` 的结果包含 `cancel` 和 `redirectURL`。取消返回 `net::ERR_BLOCKED_BY_CLIENT`。如分阶段只实现取消，收到 `redirectURL` 时必须明确失败，不能默默忽略。
* `onHeadersReceived` 的结果包含 `cancel`、`responseHeaders`、`statusLine`。提供 `responseHeaders` 表示替换整组响应头；缺省表示保留，不能总是追加。值支持 string 或 string[]，保留多值头；头名、头值和状态行必须验证，拒绝 CR/LF 注入等非法输入。仅改大小写不应产生意外重复的安全策略。
* 浏览器生成请求的 id、URL、method、resourceType、referrer、timestamp、statusCode/statusLine 和 responseHeaders。可选 webContents/frame 信息只能从真实所属对象构造，不能由 renderer 指定 container 或 Session。上传数据、WebSocket 等未覆盖字段和路径不得伪造为空成功。
* 请求 identity 与 loader 生命周期绑定，不能使用 renderer 提供的 request_id 作为跨进程全局唯一值。重定向更新 URL/method，并重新匹配 filter；旧请求的延迟回包不能作用于重用的 id 或重启后的 Utility。

“注册接受任意 scheme 的 filter”不等于“实际覆盖所有协议”。必须分别验证每条请求路径。

## Session 与异步接口路由

现有会话设施可以复用：

| 现有位置 | 可复用的职责 |
| --- | --- |
| `xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h:29` | 声明每个 Electron Session 都使用容器专属的非默认 StoragePartition |
| 同目录 `xenon_electron_session_bridge.cc:304`、`:316`、`:339` | 生成 partition config、解析真实分区、反查 container/partition identity |
| `xenon_overlay/chrome/browser/xenon_manager.cc:1275` | `electron-session` 命令校验当前 observer receiver、container 和 generation |
| 同文件 `:960`、`:977` | 自定义协议请求的 Browser→Utility 异步调用与取消模板 |
| `xenon_overlay/public/mojom/xenon_service.mojom:220` | 浏览器发起的 HandleElectronProtocol/CancelElectronProtocol；不是 renderer IPC 通道 |
| `xenon_overlay/chrome/browser/ipc/xenon_ipc_main_container.cc:3365`、`:3415` | Utility 调用隔离上下文中的 JS Promise 并处理取消 |
| `xenon_overlay/resources/ipc/main/protocol.js:53` | JS 挂起请求、异步结果和取消的现有组织方式 |

本次新增独立 `main/web_request.js`，由 Session 创建 webRequest 对象。注册时经受容器和 generation 权限校验的 `web-request-handler` 命令，将事件、filter 和注册版本送给浏览器。函数保持在所属 Utility isolate 内，不序列化函数。

浏览器请求代理匹配 Session/filter 后，通过新的专用 Mojo 方法异步调用主进程分派函数，例如 `HandleElectronWebRequest(container, partition, event, request_id, registration_id, details)`，获得一次性结果。JS 用 Promise 包装 callback；Native 请求在 Promise 完成之前保持暂停。

Browser→Utility 不能使用同步 IPC：listener 可能调用需要返回浏览器 UI 线程的 Electron API，同步双向等待会死锁。也不能借用 renderer 的 `ipcMain` channel 作为可信浏览器控制通道。

注册替换、撤销与已挂起请求存在竞态，需定义注册版本和 in-flight 生命周期，避免新 filter 配上旧 listener，或撤销后遗留无界 handler。窗口、BrowserContext、容器销毁和 Utility generation 变化时取消挂起请求、清除注册；超时必须明确失败，不能悄悄放行。可复用现有 protocol 的 30 秒超时策略，但应记录这是实现边界。

## HTTP/HTTPS 与 Chromium 工厂接线

主接线点是 `chrome/browser/chrome_content_browser_client.cc:6822` 的 `WillCreateURLLoaderFactory`。在现有扩展接线之后，以 `factory_builder.Append()` 加入 overlay URLLoaderFactory 代理。该 hook 的工厂类型定义在 `content/public/browser/content_browser_client.h:1833`，包括导航、文档子资源、worker/service worker 请求等。

通用代理需转发 URLLoaderFactory.Clone、URLLoader、URLLoaderClient，保存真实 ResourceRequest，在转发 CreateLoaderAndStart 之前完成 onBeforeRequest。支持重定向时，必须保留请求方法和请求体规则、重新选择跨 scheme 工厂、限制循环，并在后续请求阶段重新匹配。

HTTP 响应头修改还需要 **trusted header client**：

1. 绑定 `network::mojom::TrustedURLLoaderHeaderClient`，为请求启用 `kURLLoadOptionUseHeaderClient`，每个 loader 接入 `TrustedHeaderClient::OnHeadersReceived`。接口在 `services/network/public/mojom/network_context.mojom:187`、`:213`。
2. `services/network/url_loader.cc:1949` 的路径等待异步回调，`:2485` 将原始头覆盖到实际 HttpResponseHeaders 后继续网络处理。这是可以影响随后 CORS、重定向等决策的时机。
3. CORS 响应检查位于 `services/network/cors/cors_url_loader.cc:624`、`:749`。只在外层 `OnReceiveResponse` 或 URLLoaderThrottle 中改 ACAO 已经太晚；browser 侧 CreateURLLoaderThrottles 也不覆盖所有 renderer 子资源。
4. 不得覆盖已有 header client。extensions 在 `extensions/browser/api/web_request/web_request_api.cc:895` 接线；DevTools 的组合范例位于 `content/browser/devtools/devtools_url_loader_interceptor.cc:990`，逐请求连接在 `:821`。应将旧 remote 作为下游链式调用，保留其改头结果、错误码和 redirect fragment 语义，并规定稳定的调用顺序。
5. 需要处理 `OnLoaderForCorsPreflightCreated`，不能断开或忽略预检请求。重定向、网络 loader 重建、缓存响应和 request id 冲突也必须验证。extensions 现有处理可参考 `web_request_proxying_url_loader_factory.cc:588`、`:1593`。

分区应来自 frame 的 StoragePartition，或 frame 为空时 RenderProcessHost 的 StoragePartition，并经 `FindElectronSessionIdentity` 确认归属。worker 不得回退到浏览器默认 Session；keepalive 必须在工厂创建时保存身份，不能依赖页面关闭后仍能查到 WebContents。

## file、ASAR 与自定义协议

非网络导航也会走 `WillCreateURLLoaderFactory`。`content/browser/loader/navigation_url_loader_impl.cc:1241`、`:1273` 用 `ForNonNetwork` 创建工厂，因此不能因 `is_for_network_service == false` 跳过拦截，也不能在 ASAR 导航返回处再次包装造成重复事件。

非网络子资源使用按 scheme 注册的独立工厂，但工厂注册后仍会进入统一 hook，不能据注册方式判断需要重复包装：

* `RenderFrameHostImpl` 的非网络子资源工厂接线（`content/browser/renderer_host/render_frame_host_impl.cc:13228`）通过 `url_loader_factory_utils.cc` 的 `ContentClientParams::Run` 进入 `WillCreateURLLoaderFactory`。file/ASAR 导航和子资源均在统一 hook 包装一次，注册工厂处不能再套相同代理；否则同一请求会触发两个不同 id 的回调。
* 自定义协议工厂保留内置代理，因为响应头需要在其自身 CORS 检查之前处理；统一 hook 识别已注册的 custom scheme 后绕过额外包装。其导航和子资源同样只触发一次。
* 工厂入口需要核对并传递 Session 身份；是否非网络、是否注册 scheme 不能代替请求归属验证。
* worker 非网络脚本另经 `RegisterNonNetworkWorkerMainResourceURLLoaderFactories`；`content/browser/worker_host/worker_script_fetcher.cc:648`、`:656` 说明非网络 worker 入口与 frame-id-none 情况，不能只适配有窗口的 frame。

file/ASAR 的 onBeforeRequest 必须在转发底层工厂之前完成；取消时不能已经打开文件或读取 ASAR 正文。非网络 onHeadersReceived 可以在代理接到 OnReceiveResponse 后暂存 response head、body pipe、cached metadata，暂停后续 client 消息，等待 callback 后再放行；取消则断开上游请求。

修改真实 `head->headers` 后需更新 MIME/charset，并重建对应的 `parsed_headers`，否则 CSP 等字段可能继续使用旧解析结果。`NavigationURLLoaderImpl::ParseHeaders`（`content/browser/loader/navigation_url_loader_impl.cc:1996`）只对缺失的解析结果补算；子资源不能依赖这条导航路径。

使用 `content::GetNetworkService()->ParseHeaders`（`services/network/public/mojom/network_service.mojom:415`）异步完成解析。`services/network/public/cpp/parsed_headers.h:22` 明确要求高权限进程不要直接对不可信输入调用 PopulateParsedHeaders。

自定义协议有额外时机要求：`xenon_overlay/chrome/browser/ipc/xenon_electron_protocol.cc:306` 已在自己的 loader 中检查 CORS。如果要让 onHeadersReceived 修改自定义协议的 ACAO，必须在该检查前接入同一拦截服务；单独包装外层 OnReceiveResponse 太晚。必须避免协议 loader 内部与外层代理重复发出同一事件。

## 评估时的分阶段选项

评估曾考虑先实现 onBeforeRequest，而让 onHeadersReceived 继续明确报未支持；实际实施已选择同时接入两个事件。只实现前者不需要 trusted header client，也不涉及重建响应头和解析 CSP，但不会使 draw.io 本次发行包直接启动，因为原包先注册 onHeadersReceived。

第一阶段至少包含通用工厂代理、file/ASAR/custom 子资源接线、Session/filter、异步 callback、真实取消和生命周期管理。每种工厂只包装一次。完整的 onBeforeRequest 还应支持 redirectURL；如先不支持，必须检测该返回字段并失败，不能宣称该事件已完整兼容。

不能将这阶段称为“全协议”。可验证范围与剩余边界如下：

| 请求路径 | 所需接线或边界 |
| --- | --- |
| HTTP/HTTPS 导航和普通子资源 | 通用工厂 hook 可覆盖 |
| file/ASAR 导航和子资源 | 均经过统一 hook，工厂注册处不额外包装 |
| custom 导航和子资源 | 协议工厂内置代理处理 CORS 前的改头，统一 hook 不再包装 |
| data 导航 | `navigation_url_loader_impl.cc:2219` 建立 DataURL factory，随后可经非网络包装；验证后才声明支持 |
| data 子资源 | 部分直接在 Blink 内处理，例如 `third_party/blink/renderer/core/loader/resource/image_resource.cc:136`；不经过网络工厂 |
| blob 导航 | `navigation_url_loader_impl.cc:733` 直接使用 blob factory，绕过上述 hook；需要另行接线 |
| blob 子资源 | 不能推定会经过默认网络工厂，需要单独验证和包装 |
| Service Worker 合成响应 | `content/renderer/service_worker/service_worker_subresource_loader.cc:992` 可直接响应；只有 fallback（`:472`、`:784`）进入网络工厂 |
| Worker 网络请求 | 主脚本与子资源工厂可经 hook；支持 frame=null 的分区定位 |
| Worker 非网络资源 | 独立工厂注册，不能沿用只识别窗口的接线 |
| WebSocket | 独立 `WillInterceptWebSocket` / `CreateWebSocket`，见 `chrome_content_browser_client.cc:6983`、`:7021`；URLLoaderFactory 代理不覆盖握手 |
| keepalive | `content/browser/loader/keep_alive_url_loader_service.cc:393` 保存既有工厂 bundle，可继承代理；页面关闭后的 Session 和挂起请求必须验证 |

## 跨平台、范围与实施分工

实现应使用 Chromium 的网络、Mojo、StoragePartition 和文件工厂接口，不引入 HWND、平台窗口消息或应用名称判断。Windows、macOS 和 Linux 共用同一拦截逻辑。URLPattern 依赖以及各平台 `enable_extensions_core` 构建组合需要通过 GN 核对，不能仅因为 Windows 配置已包含扩展就假定所有平台都会链接。

所有改动 Chromium 原文件的 include 和接线均须放在 `BUILDFLAG(ENABLE_XENON_SERVICE)` 下。实际实现与测试置于 overlay；关闭该宏时 Chromium 原有请求路径、extensions 和 DevTools 行为应保持可构建。不得用关闭 webSecurity、CSP、CORS 或全局网络检查代替真正拦截。

适合独立分工的范围：

1. 新 `main/web_request.js` 和 JS 契约测试：Session listener、参数、callback、取消与注册版本。
2. Browser 网络代理和原生测试：工厂/loader 转发、filter、请求生命周期、trusted headers 与非网络响应头处理。
3. 主控负责 Mojo/Utility/Manager 路由协调、Chromium 宏接线、Session 身份传入、GN 和集成验证。若单独委派 Utility 方法，需明确文件所有权，避免与其他 main container 修改冲突。

完整响应头方案横跨 JS、Utility、Browser 与网络服务，适合单独开发和验收；不能作为很小的启动兼容补丁混入路径 API 修改。

## 验收清单

* 注册：省略 filter、URL/端口/路径通配符、excludeUrls/types、非法 pattern、最后 listener 生效、null 撤销、注册替换与已挂起回调。
* 请求：HTTP、file、ASAR、自定义协议的允许/取消/异步延迟；导航和子资源各只触发一次；取消请求未访问底层网络/文件工厂；真实 redirectURL、跨 scheme 路由与循环限制。
* 响应头：真实新增、替换、删除、重复值和状态行；非法头拒绝；无修改时保持原值；取消不交付正文；重定向及缓存命中仍正确。
* 生效验证：file/ASAR 页面的 CSP 实际阻止被禁脚本；HTTP 修改 ACAO 后实际改变跨源读取结果，并覆盖预检和重定向；自定义协议在自己的 CORS 判定之前使用修改后的头。不能只断言 JS 收到 callback。
* 隔离：两个 container 的默认 Session、同容器两个 partition、persist/in-memory Session、不同 BrowserContext 互不影响；普通浏览器页无 Electron 拦截；请求身份不能被 renderer 伪造。
* 生命周期：延迟 callback 不提前放行；重复 callback 只完成一次；异常、超时、窗口关闭、Utility crash/restart、BrowserContext shutdown 不留下挂起 loader；过期 generation 回包无效。
* 组合：保留 extensions 与 DevTools 现有 header client 行为、错误码和重定向；factory Clone、预检与 loader 重建正常。
* 路径覆盖：worker 无 frame、keepalive 页面退出后仍保持原 Session；blob、data 子资源、SW 合成响应和 WebSocket 分别验证，未完成的路径保持文档化边界。
* 回归：原始 draw.io 包不做修改，验证编辑器真正加载及安全策略生效；TH/PLE 启动、片库/播放与关闭重开不回退；Windows 实编运行，macOS/Linux 至少编译验证后才能声称跨平台验证完成。

Electron 语义参考：官方 `docs/api/web-request.md`、`docs/api/structures/web-request-filter.md`，以及 Electron v39.2.7 的 `shell/browser/api/electron_api_web_request.cc`。本文仓库行号是评估时定位，后续修改可能移动，查找时应同时使用符号名。
