# Electron utilityProcess 兼容

`electron.utilityProcess.fork()` 通过 Chromium `ServiceProcessHost` 启动独立的 OS Utility 进程。每个子进程有自己的 V8 上下文、模块缓存和标准 N-API 运行时；不复用父应用的原生插件实例，也不运行 Electron 主窗口逻辑。子进程执行原发行目录中的入口，不修改或重新打包应用文件。

## 启动与通信

- Browser 根据已认证的父应用连接补齐应用配置和启动代次；JavaScript 不能自行指定另一应用身份。父代次取消或连接断开后，所属 Utility 会被回收，包括尚未完成启动的请求。
- `env`、`cwd` 和三个标准句柄在 OS 启动阶段配置。Windows 继承真实句柄，并在执行入口前建立 CRT 的 fd 0/1/2；macOS/Linux 使用原生描述符重映射。保留 Chromium 的 Mojo、Crashpad 通信句柄，不在线程启动后修改整个进程的环境或工作目录。
- `env` 采用完整快照；省略时继承父应用可见的环境。空字符串保留为真实的 `KEY=`，与键缺失区分；空对象不继承其他父环境。启动器通过专用选项启用此语义，普通 Chromium 调用仍保留“空值表示删除”的默认行为。
- 主进程 `process.env` 从 libuv 获取真实 OS 环境快照，不设变量白名单；保留任意变量、空值、Unicode 和同名原型属性。Windows 的 `=C:` 等隐藏驱动条目按 Node 枚举规则排除；主进程/Utility 的 `Path`、`PATH` 等大小写别名访问同一属性，枚举保留原拼写。Worker 保持大小写敏感；Worker/Utility 使用各自传入的环境快照，不合并宿主其他变量。JavaScript 环境修改作用于该上下文快照，不修改共享宿主进程的 OS 环境。
- `stdin` 固定为 `ignore`；`stdout`、`stderr` 支持 `pipe`、`inherit`、`ignore`。JavaScript 与原生插件写入相同的 OS 标准流。父端每次最多读取 64 KiB、每个流仅一个未完成读取，按消费者需求读取；退出后仍可排空管道中已写入的数据。子端写入为真实同步 OS 写入，慢消费者可以阻塞子进程写操作。
- 父端 `stdout.destroy()` / `stderr.destroy()` 关闭相应原生管道。退出后两个输出完成才释放控制对象；从未消费的输出在 `exit` 回调后自动排空，避免强引用保留子进程。应用主动暂停的输出仍由应用控制，不会强制丢弃尾部数据。
- `postMessage` 使用现有 MessagePort 和结构化克隆实现，支持端口转移。`process.parentPort` 提供 `postMessage`、`start`、`pause`；默认暂停，第一个 `message` 监听器加入时恢复，最后一个移除时暂停。早于订阅到达的消息由原生端口保留，不另设无限 JavaScript 队列。
- Node HTTP 请求使用 Browser 为该应用默认 StoragePartition 克隆的 URLLoaderFactory；这不会向 Utility 授予 BrowserWindow 或主进程 observer 权限。
- Utility 的 `electron` 模块仅导出已实现的 `net`。`net.request` / `net.fetch` 通过专用 observer 向 Browser 请求父应用 Session 的网络 factory，保留父应用身份、partition 和代次检查；`net.isOnline()` 使用同一受限连接查询网络状态。子进程不获得 `app`、`BrowserWindow` 或 `session` 模块，父连接断开时未完成的 factory 请求报告 `ERR_ABORTED`。

## 退出和崩溃

`spawn` 在真实 OS 进程启动时发出，不等待入口模块执行完成。正常退出先回收原生运行时，再结束进程；Browser 依据实际 OS 退出通知发送 `exit`，子进程的退出意图不等于已经退出。`kill()` 和父应用代次取消针对所属进程，不接受任意外部 PID。

`pid` 在 `spawn` 时有效，退出后恢复为 `undefined`。`process.exit()` 终止当前 JavaScript 执行栈；退出回调与 N-API 清理在栈退出后的任务中执行。未捕获的异常先交给应用的 `uncaughtException` listener，没有处理者则退出。Promise rejection 使用 Electron 的 `warn-with-error-code` 行为：没有 `unhandledRejection` listener 时记录警告并设置 `exitCode=1`，有监听器时由应用处理。

不增加基于空闲计时器的自动退出，也不把普通 Node/Worker 的自然退出或
`beforeExit` 调度套用到 Electron Utility。消息端口的暂停只控制收信，不代表
关闭进程；只剩 `unref()` timer 也不意味着 Utility 应自动结束。官方
[NodeService](https://github.com/electron/electron/blob/v44.10.2/shell/services/node/node_service.cc)
会调用
[NodeBindings::PrepareEmbedThread](https://github.com/electron/electron/blob/v44.10.2/shell/common/node_bindings.cc)，
后者创建保持引用的 dummy `uv_async`，明确用于避免 libuv 无任务时退出。
此内部保活与应用 timer 自身的 `ref()/unref()` 状态是两件事。崩溃沿用
Chromium Utility 的 Crashpad 管线，不创建第二套 crash reporter。

## 明确边界

以下请求目前以 `ERR_NOT_SUPPORTED` 拒绝，不静默忽略：

- 非空 `execArgv`：尚未接入启动阶段的 Node/V8 参数处理。
- `allowLoadingUnsignedLibraries: true`。
- `respondToAuthRequestsFromMainProcess: true`。

Electron 官方 Utility 模块还包含 `systemPreferences`；当前未实现该模块，不能把仅导出的 `net` 视为 Utility 的完整 Electron API。`net` 的协议、认证等剩余边界与 [Electron网络兼容.md](Electron网络兼容.md) 一致。

Windows 主进程/Utility 当前通过 Proxy 提供环境变量的大小写别名，因此直接 `structuredClone(process.env)` 会拒绝该 Proxy；可枚举快照 `structuredClone({...process.env})` 可用。Node 原生环境对象本身可克隆，这一差异尚未消除。

入口必须在已授权的应用根目录内，`cwd` 必须是存在的绝对目录。环境、参数和路径在启动前快照并验证，拒绝 NUL、无效类型和不支持的选项。

Electron API 对照来自官方 `lib/utility/init.ts`、`shell/services/node/parent_port.cc`、`shell/common/node_bindings.cc` 和 `shell/services/node/node_service.cc`。新功能在 Chromium 源码中的入口均受 `ENABLE_XENON_SERVICE` 控制。

## 验证状态

已通过 JavaScript 标准流、1.25 MiB 输出与背压、退出后排空、无消费者输出回收、主动暂停、退出事件、parentPort 订阅切换、真实 MessageChannel 消息排队/端口转移及 bootstrap 组合测试。

产品08的 Windows 实际宿主验证 `utility-process-probe-05` 已通过 11 项：独立 PID/环境/cwd、1.25 MiB 二进制标准流、跨进程端口转移、ESM/TLA、HTTP、父 Session 的实际请求重定向、kill、入口/定时器异常、Promise rejection 退出码，以及应用异常监听器处理。宿主正常退出，无残留测试进程或新增 dump。早期探针误用了未实现的 `onBeforeSendHeaders`，并误要求普通 JS 异常产生仅用于 V8 致命错误的 `error` 事件，已按公开 API 语义修正，原始失败证据保留。

product10 增强探针 `utility-process-probe-07/verification.json` 已通过 11/11，
入口异常、定时器异常和未处理 rejection 均写入真实子进程 stderr；宿主正常
退出，无残留测试进程或新增 dump。product09 的 rejection 原因丢失失败证据
保留在 `utility-process-probe-06`。macOS/Linux 当前仅完成源码和启动路径审查，
未宣称已实机验证。

2026-11-12 使用 draw.io 官方发行包中的 Electron 44.10.2 / Node 24.22.2
运行时做了独立基线对照：仅将未修改的运行时文件硬链接到外置测试目录，
装入新建的测试 ASAR，不修改 draw.io 原包。无窗口启动后，空入口和仅剩
`setTimeout(..., 600000).unref()` 的 Utility 在启动后 1.5 秒仍保持存活，
随后由父进程 `kill()` 结束；`process.exit(7)` 的对照入口退出码为 7。
宿主退出码为 0，无残留测试进程或新增 dump。原始结果在外置审计目录的
`official-utility-liveness-02/result.json`，生成脚本为
`probe-official-utility-liveness.cjs`。该结果验证当前版本的 Utility 保活契约，
不声称所有 Node 或所有 Electron 版本的进程都具有相同退出行为。
