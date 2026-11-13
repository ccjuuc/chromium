# Electron 渲染器沙箱

`app.enableSandbox()` 由 Browser 为已认证的应用及服务代次保存强制策略。
调用须在 `ready` 之前，并且不能已有向该应用暴露 Node 运行时的页面或旧版原生接口。
原生端确认成功后才更新 main 的策略和 `app.commandLine` 快照；退出清理该代次。
其他应用及其后续重启的代次不受影响。

## 窗口偏好

BrowserWindow、WebContentsView 与 webview 采用相同规则：未指定 `sandbox` 时，
默认启用；显式启用 `nodeIntegration` 或 `nodeIntegrationInWorker` 时默认关闭。
显式 `sandbox: true` 或应用级 `enableSandbox()` 覆盖 Node 集成请求。
显式 `sandbox: false` 保留原有完整预加载运行时，除非应用级强制策略已启用。
`contextIsolation` 默认 true，允许显式 false；`nodeIntegrationInSubFrames`
仍决定子框架是否执行预加载，子框架继承同样的受限能力。

`webPreferences.additionalArguments` 随窗口的 Document 配置传入，在执行
preload 前追加到该窗口的 `process.argv`。字符串的顺序、空格、空字符串、
Unicode 和重复项保留，不按应用名或参数前缀分支；非字符串项忽略。
普通运行时复制参数数组，沙箱 preload 再复制自己的有限 process 元数据，
因此修改 argv 不会改写窗口偏好，也不会影响其他窗口或共享渲染进程的页面。

main 计算对外可见的偏好快照，避免每个窗口增加同步 IPC。Browser 在创建窗口、
创建 guest 和绑定 Document 时独立执行相同规则，不信任 renderer 传入的权限声明。
普通已授权 WebUI 不自动成为 Electron 窗口；它也受所属应用的显式强制策略约束。

## 预加载与原生权限

沙箱 preload 只允许 `require('electron')`、`electron/common`、
`electron/renderer`、`events`、`timers` 和 `url`，后三者支持 `node:` 别名。
相对文件、第三方包、`.node`、`fs`、`path`、`module` 和 `buffer` 模块不可加载。
与 Electron 的 sandbox preload 一样，依赖必须在应用发行时打包进 preload。
词法作用域提供 Buffer、有限的 process、global、setImmediate 和 clearImmediate。

Electron 导出限于 contextBridge、crashReporter、ipcRenderer、nativeImage、
webFrame 与 webUtils。页面只能获得 preload 主动暴露的 API；完整 require、
进程对象、Node addon 入口及原生传输别名在执行 preload 前被撤销。
实际操作系统沙箱未启用时，也先撤销这些入口，再报告初始化失败。

Browser 读取已批准的绝对 preload 路径，包括 ASAR 内路径，随 Document 配置提供源文本；
renderer 不使用通用文件 IPC 读取 preload。Session preload 仍先于窗口 preload
依次执行；脚本错误通过标准 `preload-error` 上报，不回退到完整 CommonJS 运行时。

Browser 的每个 Document 保留不可由 renderer 改写的有效偏好。沙箱页面不能通过
ipcRenderer 手工拼接保留通道来访问文件、网络套接字、SQLite、进程、原生 addon、
shell、剪贴板或应用管理接口。旧 WebUI 的 PageHandlerFactory 同样执行该检查。
应用自己注册的 IPC 通道、contextBridge、内存图片转换和获准的 webview 操作保留。
主进程仍须根据自己的业务权限校验应用 IPC 的参数和发送方。

## 操作系统边界

Chromium 的 renderer 进程由既有原生沙箱启动；这里补齐 Electron 的预加载能力和
Browser broker 权限边界，不另造 OS 沙箱。`process.sandboxed` 来自当前进程的
实际沙箱状态，不从 webPreferences 或应用 argv 推导。Service Worker 同样报告原生状态。

Windows 的 EXE/DLL 构建可能分别链接 SandboxFactory，直接查询 DLL 的副本会误判。
此时按 Chromium 的既有方式查询宿主 EXE 的 `IsSandboxedProcess` 导出，
并要求当前进程的真实 token 同时受限且低于中完整性；查询失败仍返回 false。
macOS、Linux 等平台沿用各自的 Chromium 原生检测；`--no-sandbox` 始终返回 false，
不沿用 Chromium 为 DCHECK 特意返回 true 的行为。

共享宿主已经启动后，单个应用不能撤销宿主全局的 `--no-sandbox`、
`--single-process` 或调试器启动参数。存在这些冲突时明确报不支持。
Linux 还拒绝禁用 seccomp，或同时禁用 namespace 与 setuid 两种第一层后端的配置；
单独禁用 setuid 不等于完全关闭沙箱。Windows、macOS、Linux 共用权限策略，
没有 TH、PLE、路径或应用名称特判。

## 尚未补齐的 API 与验证

有限 process 不包含 binding、_linkedBinding、cwd、nextTick、exit 等 Node 入口。
缺少真实后端的 process 指标和 webUtils.getPathForFile 明确报 `ERR_NOT_SUPPORTED`；
完整 Electron process 生命周期事件尚未实现。webFrame 现有未实现方法继续显式报错。
nativeImage 支持现有内存转换，路径读取不会绕过文件访问限制。

JavaScript 测试覆盖完整 production bootstrap 的 Browser 提供源、Session 顺序、
受限 require、撤销全局、contextBridge 与应用 IPC、无原生沙箱时失败关闭，以及
BrowserWindow/WebContentsView 的强制策略。原生策略测试覆盖默认值、参数验证、
代次隔离、延迟启用、启动标志冲突与保留通道拒绝。
这些测试不能代替真实宿主及各操作系统运行验证；实际结果应单独记录。
