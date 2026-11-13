# Node Worker 线程兼容

`require('worker_threads')` 和 `require('node:worker_threads')` 进入同一个模块。
Worker 在当前 Electron Utility 进程内建立真实线程和独立 V8 isolate；不会把
Worker 源码放回主线程执行，也不会用子进程伪装线程。

## 分层和生命周期

- `resources/ipc/main/worker_threads.js` 管理 Worker、parentPort、事件和选项校验。
- 原生 Worker 引擎管理线程、isolate、消息投递、引用计数和结构化克隆。
- Worker 复用现有 CJS/ESM/ASAR 模块解析和 Node API 初始化，但使用独立角色，
  不执行 Electron app 的 ready/quit 流程，也不暴露 `process.type`、Electron
  版本、resourcesPath 或 electronBinding。
- `online`、`message`、`error`、`exit` 来自真实引擎事件。`terminate()` 等待
  实际退出，CPU 死循环也由 V8 终止执行；退出后不再交付迟到消息。
- Worker 存活判断包含当前 libuv loop 的引用 handle、待完成请求和关闭回调，
  排除框架临时 pump guard；UDP 的 ref/unref 与 DNS/send 请求遵循同一规则。
  退出先停止接收和运行用户退出钩子，再交给共享 NativeRuntimeRetirement
  排空完整环境；销毁 V8/环境后才发布 exit。父应用退出保留 Worker 引擎和
  线程，直到所有环境完成退场，期间不阻塞 UI，也不提前停止仍有回调的线程。
- 自然空闲时发送 `process.beforeExit`，允许回调注册新的异步工作。自然退出
  和显式 `process.exit()` 发送一次 `exit`，遵循 `process.exitCode`；显式退出
  仅结束当前 Worker。强制 terminate 不保证运行用户退出钩子。
- 每次原生 Worker 事件进入新的 AsyncLocalStorage 任务上下文；事件内部
  注册的延续仍保留自己的上下文。
- Worker 与独立 Utility 的专用 V8 isolate 使用 C++ Promise hooks 传递
  AsyncLocalStorage 上下文，覆盖原生 Promise 和 async/await；不要求启用
  JavaScript Promise hooks 的全局 V8 编译选项，也不修改 Blink renderer
  的 isolate hooks。该支持范围不能外推为 renderer 已具备相同能力。
- Worker 的 HTTP/全局 fetch 网络工厂由父线程克隆为 pending factory，进入
  Worker 线程后才绑定，禁止跨线程复用已绑定的 Mojo remote。请求沿用 Node
  网络策略，不使用浏览器 Cookie 存储或 HTTP 缓存，显式 Cookie 请求头仍保留。

## 数据、入口和资源

- 支持绝对路径、以 `./` 或 `../` 开头的相对路径、file URL 和 `eval: true`。
  文件入口遵守现有应用根目录限制；不允许路径中的 NUL。
- workerData、postMessage 使用原生结构化克隆，保留 Map、Set、Date、RegExp、
  BigInt 和循环引用。ArrayBuffer transfer 在完整验证成功后才分离源缓冲区，
  失败不能部分分离；SharedArrayBuffer 共享实际 backing store。
- `markAsUncloneable` 使用对象自身的 V8 私有标记；Worker、主进程
  `structuredClone` 和 Electron MessagePort 编码器在原生序列化过程中检查，
  嵌套的标记对象同样报 `DataCloneError`，不会预遍历 getter 或提前分离 transfer。
  标记不随原型继承，对 primitive 无操作；Proxy 按 Node 抛 TypeError。
  Array、Date、Map、Set、RegExp、ArrayBuffer、typed array 等内建编码仍按
  Node/V8 原规则处理，不因该标记改变其复制/转移语义；其内部包含的标记普通
  对象仍会在遍历到时被拒绝。普通对象、冻结对象和 Error 的标记不可由 JS 删除。
- 原生传输对尚未交付的消息设置 64 MiB 预算，包含 transfer/SAB backing store
  的保守计费；超限在分离 transfer 前报错。该预算不是进程总内存上限，也不
  包含消息已经交给 JS 后，由应用或 parentPort 队列继续持有的对象。
- 支持 argv、复制 env、name、old/young generation 和 stack 资源限制。
  threadName/resourceLimits 返回原生有效值，禁止构造伪造的默认元数据。
- `trackUnmanagedFds` 默认开启：仅清理当前 Worker 新开且未关闭的描述符。
  主线程传入的共享描述符不会被错误清理；关闭此选项后新开描述符保留到
  主线程显式关闭或应用退出。
- parentPort 支持 Node 事件接口、消息队列、start、ref/unref、close(callback)。
  close 事件和回调等待真实端口关闭；停止后 postMessage 不分离传入的缓冲区。

## 明确的能力边界

当前不支持嵌套 Worker、Worker 内加载 Electron 或原生 addon、SHARE_ENV、
MessageChannel/MessagePort 转移、environmentData、profiling、非空 execArgv、
独立 stdin/stdout/stderr 选项和 codeRangeSizeMb。调用这些能力应明确返回
`ERR_NOT_SUPPORTED`，不能用空对象、假流或静默忽略替代。

parentPort 的 EventTarget 接口（包括 onmessage/onmessageerror 属性）也会
明确拒绝；当前实现的是 Node EventEmitter 消息接口。

## 验证

JS 合约测试：`resources/ipc/xenon_worker_threads_unittest.cjs`，16 项通过。
覆盖数据交接、transfer 失败不分离、真实退出等待、close 回调、角色隔离、
自然/显式退出事件和 AsyncLocalStorage 事件边界。
`markAsUncloneable` 已对照 Node 24 源码和真实运行结果；另有原生 codec 与
完整容器测试覆盖嵌套 getter、未分离缓冲区/端口、冻结对象及内建类型例外。

历史真实线程集成 `worker-threads-probe-03/verification.json` 已通过 19 项，
覆盖 CJS/ESM/TLA、双向 transfer、SAB、CPU 并行、死循环终止、进程退出隔离、
FD 所有权和应用退出清理；宿主退出 0，无归属残留进程或新增 dump。该轮未
覆盖 pending UDP/DNS 工作，不能替代以下新增退出回归。

product14 的新增 UDP 探针发现自然退出和强制终止 pending DNS/send 时均会
过早析构运行时；两次真实 dump 的顶部均为 XenonIpcMainContainer 析构检查。
Node 对照为自然退出四项、强制终止三项通过。上述引用判断与异步退场修复
已在 product15 实际复验：自然退出 4/4、强制终止 3/3；app.quit 同时终止
bound/send/DNS 三个 Worker，其 Utility 进程在关闭浏览器前已经结束。原有
19 项完整 Worker 探针也全部通过，四次宿主均退出 0、无残留或新增 dump。
四项 Worker 引擎屏障原生测试与九项 NativeAddonEventLoop 测试均通过。
证据根目录为 `F:/electron-compat-audit/20261111/`：

- `worker-udp-probe-product15-01/verification.json`
- `worker-udp-terminate-probe-product15-01/verification.json`
- `worker-udp-host-shutdown-probe-product15-01/verification.json`
- `worker-threads-probe-product15-01/verification.json`

product14 的两份 `*-red01` 失败日志和 dump 分析继续保留，未混入通过计数。

product19 的真实 Worker 与 Utility 异步上下文探针各通过 3 项，共 6/6：
两个并发 `AsyncLocalStorage.run` 的 store 经原生 Promise、外部共享 Promise、
`async/await`、两轮交错定时器和 `then` 后仍各自独立；两个 ALS 实例的嵌套
作用域在同步异常后恢复；异步拒绝被捕获后恢复外层作用域，退出后根作用域为空。
Worker 自然退出 0，Utility 显式退出 0，宿主退出 0；按 PID 和创建时间检查
无归属残留进程，专用 profile 无新增 dump，调试端口 9268 已释放。探针使用
`--no-startup-window` 且不创建 BrowserWindow。相同检查在 Node 24 的主线程
及真实 Worker 中先取得 6/6 基线，证据位于同一审计根目录：

- `async-context-node-baseline19.json`
- `async-context-probe-product19-01/verification.json`
