# Electron 应用状态与主进程事件

本文记录 2026-11-11 原发行包接入时补齐的应用路径、单实例、启动参数及主进程 EventTarget。实现针对通用 Electron/Node 契约，不检查应用名称，不修改供应方源码或归档。接口回归通过不等于整款应用通过启动、核心功能和关闭重开验收；最新应用结果仍以 [Electron原包兼容性验证.md](Electron原包兼容性验证.md) 的实际记录为准。

## 应用路径

`XenonAppPaths` 由每个 main container 独立持有。系统目录通过平台 API 和 Chromium PathService 读取，应用覆盖项只存放在该实例中，不能通过全局 `PathService::Override` 影响其他应用或宿主。

`app.setPath` 要求绝对路径，允许尚不存在的目录，且不创建目录。`app.setAppLogsPath` 创建日志目录成功后才更新配置；macOS 默认 `~/Library/Logs/<应用名>`，其他平台默认用户数据目录下的 `logs`。应用名不能包含路径分隔符、越界片段或不安全的 Windows 文件名字符。

`userData`、`sessionData` 使用 Browser 保存的真实应用路径，支持受生命周期约束的显式重定向。新应用的普通文件默认按稳定身份隔离，已有接入可以通过 descriptor 的 `userDataPolicy: "legacy-host"` 明确保留历史路径；默认会话分区不会随之移动。目录创建、冻结及迁移边界见[应用数据目录设计](Electron应用数据目录重定向设计.md)。`crashDumps` 继续使用宿主 Chromium 的真实报告目录，不能另设上传或存储路径。`getAppPath()` 与旧的只读 `app`/`appPath` 别名保持应用入口身份。

electron-store、日志组件等使用这份真实路径状态。已有会话冻结后，不支持运行期存储迁移。

## 单实例与第二次启动

`XenonAppSingleInstanceBroker` 位于 Browser 侧，按实际应用路径、宿主用户数据和当前系统用户区分实例。它持有操作系统文件锁；进程结束时由操作系统释放。持锁成功后才对 main 返回成功，第二次启动通过本机传输把通知送给已有实例。

Windows 使用当前用户范围的命名管道，POSIX 使用检查对端用户身份的本机 socket。消息具有大小限制和 I/O 截止时间；随机 lease 区分每次持锁生命周期，requestId 去重防止重试投递两次。容器 owner 包含代次，旧容器的 release 或排队通知不能影响重开的实例。

`requestSingleInstanceLock(additionalData)`、`hasSingleInstanceLock()`、`releaseSingleInstanceLock()` 使用该原生所有权。`second-instance` 从 Browser 通过当前容器投递，JS 不拥有锁文件路径、管道名称或消息身份的选择权。第二次启动是否聚焦窗口仍由应用自己的事件处理逻辑决定。

## 启动参数的来源

Browser 根据启动配置建立应用 `argv`，经 Mojo 传给 Utility，再初始化该应用的 `process.argv`。独立启动只把宿主命令行中独立 `--` 之后的参数传给应用；浏览器、调试端口、Utility 内部开关不会混入。未显式提供应用参数的嵌入接入使用 `[应用可执行文件身份]`。

单实例通知使用 Browser 保存的原始应用启动参数和有效工作目录，仅 `additionalData` 来自 JS。应用可像 Node 程序一样修改 `process.argv`，但这不会改变操作系统意义上的下一次启动通知；也不会因数组中插入 `null`、替换 `process.cwd` 或覆盖 JS 全局而破坏通知。当前未实现 `process.chdir`；将来支持时必须同步 Browser 侧有效 cwd，不能继续使用启动时目录冒充当前目录。

## main 的 EventTarget

main 是独立 V8 上下文，没有 Blink DOM。`main/event_target.js` 为该上下文提供独立的 `Event`、`CustomEvent`、`EventTarget`，采用 WeakMap 检查真实实例品牌；不把 EventEmitter 改名后冒充 EventTarget。renderer 继续使用 Blink 自带实现。

已实现单目标分派、函数和 `handleEvent` 对象监听器、按 callback 与 capture 去重/删除、once、passive、AbortSignal 清理、取消默认行为、分派中的增删监听器、重入检查及监听器异常处理。没有 DOM 节点树，因此不提供 DOM 捕获/冒泡路径。AbortSignal 继承同一 EventTarget，并使用真实 `Event('abort')`；内部清理不会被用户 `stopImmediatePropagation` 截断。

同步抛错或监听器返回的 rejected Promise 按异步错误路径报告；支持处理的 `process.uncaughtException` 在本 main 上下文接收，否则继续抛出，不把错误交给其他应用或 renderer。相关网络 factory 测试必须加载真实 EventTarget 依赖，不能在测试中退回 EventEmitter 来掩盖依赖遗漏。

## 语言、缩放与 Windows 应用标识

`app.getLocale()` 从 Browser 的真实应用语言读取，调用先核对容器与代次，不使用固定语言。`webContents` 的 `zoomFactor`/`zoomLevel` 属性及方法修改真实 HostZoomMap；`setVisualZoomLevelLimits` 更新当前 WebContents 的 WebPreferences，并在后续导航/偏好刷新时保留。新增 Chromium 调用点仍位于 Xenon 宏内。

`app.setAppUserModelId()` 将标识绑定到所属容器的新旧窗口；Windows 使用窗口属性设置，不调用修改整个 Browser 进程标识的 API。调用 `setJumpList` 而未声明标识时，根据真实应用路径生成稳定的独立标识。容器断开时释放内存中的窗口标识配置。

`app.setJumpList()` 支持 Recent/Frequent、Tasks、自定义分类以及 task/file/separator 项。全部参数先完成验证，再在 Windows COM STA 上更新系统列表；`null` 删除自定义列表。系统失败按 Electron 的返回状态传回，包括权限、文件关联和自定义分类分隔符错误，不返回虚假的成功。该操作不会执行列表中的程序。Windows 实现在平台宏内，其他平台明确报告不支持 JumpList。`getJumpListSettings`、自动注册文件关联和最近文档更新不在此次实现范围。

## 验证状态与边界

应用路径、单实例、参数和 EventTarget 的相关 native 回归已随本轮主线构建执行通过。JS EventTarget 专项 22 项包含与本机 Node 的差分及独立子进程错误顺序检查；EventTarget/网络依赖相关 4 个测试文件共 73 项通过。修正测试依赖后，HTTP 文件 61 项通过。证据目录为 `F:/electron-compat-audit/20261111/`，包括 `app-paths-native-targeted.*`、`app-paths-singleton-webrequest-native-02.*` 和 `web-request-build-06.log`。

以上实际执行平台为 Windows。macOS/POSIX 分支按公共接口实现，但本文没有 macOS/Linux 实机编译或运行结果。原包应用的后续阻断点、新增文件描述符/NAPI 集成以及完整用户流程仍需各自构建和实际验收，不能用本节基础接口测试替代。
