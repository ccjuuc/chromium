# Electron 网络接口兼容

`electron.net` 与 Node 的 `net`、`http`、`https` 是不同模块。本实现通过 Browser 中实际所属 Profile、容器与 Session 的 StoragePartition 发起 Chromium 请求，不将 Electron API 别名到 Node HTTP，也不绕过 Session 的网络设置和 webRequest。

## 调用与生命周期

* `net.request(options[, callback])` 使用默认 Session，或 `options.session` / `partition` 指定的 Session；显式 `session` 优先。支持 URL 和常用 Node URL 字段、method、headers、cache、redirect。
* `ClientRequest` 支持 header 操作、`write` / `end`、`response` / `finish` / `error` / `abort` / `close` 事件、`abort` / `destroy`。默认上传在 `end` 前缓冲，最多 32 MiB；没有将未发送的数据声称为已上传进度。
* 响应在收到头时交付。`IncomingMessage` 支持真实分块 `data` / `end`、`read`、`pause` / `resume`、`pipe` 与异步迭代，并保留 status、headers、rawHeaders 和 HTTP version。UTF-8 字符跨 native 块不会被拆坏。
* Browser 每次最多传递 64 KiB 数据，消费方明确恢复后再发送下一块。暂停时不继续无限推送，`net.request` 的完整响应没有 32 MiB 聚合上限。
* `net.fetch` 与 `session.fetch` 在响应头阶段返回 Response。调用 `text`、`json`、`arrayBuffer`、`bytes` 或 `blob` 才消费实际网络数据，聚合体上限 32 MiB。支持字符串、二进制、URLSearchParams 与 Blob 上传，以及 AbortSignal。
* 取消、错误、容器退出均终结挂起请求；完成后的 id 不接受迟到事件。没有默认全局请求超时，调用者可以通过取消信号控制自己的期限。
* `login` 事件接收真实认证挑战与 callback。异步等待凭据、只回复一次；没有 listener 或 callback 未提供凭据时取消，abort 后的迟到回复无效。该路径已经过真实服务端挑战、重试和取消验证。
* `net.isOnline()` / `net.online` 查询实际 Browser 网络状态，不固定返回 true。该状态表示网络接口连接，不能证明具体服务器可达。

## Cookie 与重定向

Node 的主进程全局 `fetch`、`http`、`https` 不使用浏览器 Cookie 存储，也不读写浏览器 HTTP 缓存；`fetch({credentials: 'include'})` 不会因此获得 Electron Session cookies。调用者显式设置的 `Cookie` 请求头仍会发送，但响应的 `Set-Cookie` 不写入浏览器 Cookie 存储。该规则同样适用于 Worker、Utility 和 renderer 的 Node HTTP 桥接。需要使用应用 Session 的请求应使用独立的 `electron.net` / `session.fetch`。

按 Electron v39 的 ClientRequest 语义处理：省略 `credentials` 时使用 Session 认证状态，但默认不收发 Session cookies；`useSessionCookies: true` 开启 cookies。显式 `credentials: 'include'` 开启认证与 cookies，`'omit'` 关闭，两者均优先于 `useSessionCookies`。

`redirect: 'follow'` 自动跟随，`'error'` 拒绝。`'manual'` 必须在 `redirect` 事件处理器中同步调用 `followRedirect()`；未调用就取消，不能无限等待一个晚到的回调。同一规则适用于 HTTP 与 HTTPS 之间的正常重定向。

## 当前明确边界

* 仅 HTTP(S)。file、自定义协议以及其他跨协议路由尚未接入此模块；不会将其伪装成 HTTP 请求。`bypassCustomProtocolHandlers: true` 仍保留 webRequest。
* Electron Session fetch 尚未接入 Web Streams 响应体，因此 fetch `response.body`、流式 Response 克隆、FormData/流式上传及 fetch manual redirect 明确返回 `ERR_NOT_SUPPORTED`。现有 Response 聚合方法可用。
* `credentials: 'same-origin'` 的显式 ClientRequest 选项、origin、referrerPolicy、priority 控制尚未支持。fetch 缺省的 same-origin 在没有 origin 的主进程请求中映射为 omit；include 必须显式指定。
* `chunkedEncoding: true`、`flushHeaders`、上传进度与 ClientRequest.setTimeout 尚未支持。TLS/socket/Agent 设置不能覆盖 Chromium Session；请求级关闭证书校验明确报错。
* `IncomingMessage` 使用真正的 `Readable`，由 `_read` 请求原生数据、`push` 交付字节；编码、`read(size)`、`pipe`、异步迭代和背压由公共流实现处理。`ClientRequest` 使用 `Writable` 的 `_write` / `_final` / `_destroy`，不再直接赋值只读生命周期属性。

## 验证状态

JS 契约测试 `xenon_electron_net_unittest.cjs` 18/18 通过，含完整 bootstrap、Session 接线、分块背压、pipe 下游关闭、UTF-8、取消、重定向、Cookie 选项、认证 callback、错误与 shutdown。

真实集成探针：`F:/electron-compat-audit/20261111/probe-electron-net.cjs`，使用独立 profile、随机 loopback HTTP 端口与 9252/9260 调试端口。`electron-net-probe-01/verification.json` 记录 11/11 通过，宿主退出码 0，无残留进程或新 dump。响应头比服务端完整正文提前约 362 ms 到达；暂停时仅交付首块、恢复后交付其余正文；Session Cookie 隔离、默认不收发 Cookie、显式 omit 的优先级、拦截器取消后的服务端零命中、重定向与退出时取消均通过真实链路验证。

语义参考：Electron v39.2.7 的 `docs/api/client-request.md` 与 `lib/common/api/net-client-request.ts`。

Node 网络真实对照见 `node-network-product12-08/result.json`，9 项通过；此前 `product12-06-red02` 在相同探针下复现了宿主 Cookie 泄露、Set-Cookie 污染、HTTP 缓存混用以及 Worker 缺失网络工厂。新实现分别对照普通宿主页和应用 Session 的 Cookie/缓存，验证显式 Cookie 保留、Node 请求禁读写浏览器缓存、Worker 独立线程工厂和 Utility 请求，宿主退出码 0，无残留进程或 dump。

认证扩展探针 `electron-net-probe-03/verification.json` 全部 15 项通过，宿主退出码 0，
无残留进程或新 dump。真实 Basic 挑战、延迟凭据、omit、无人处理和取消挑战、
中途 abort 及响应 Set-Cookie 可见性均已验证。前一轮发现的 JS/native 取消参数数量
不一致已修复，并在 JS 契约测试中覆盖省略凭据的实际传输调用。
