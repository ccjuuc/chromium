# Electron crashReporter 与宿主 Crashpad

Xenon 的 Browser、Renderer 和承载 Electron main 的 Utility 进程已经接入
Chromium Crashpad。Electron 应用不再启动第二个 handler、创建第二份数据库或替换
`CrashReporterClient`。该模块的策略由宿主统一管理，适用于所有 Electron 发行目录，
不根据应用名称或应用路径作特殊处理。

通过 `child_process` 或其他方式自行启动的外部可执行程序不属于这条管线；
它们需要自行接入崩溃处理。浏览器只负责已经接入自身 Crashpad 的进程，
不会因应用调用 `crashReporter.start()` 而自动接管外部程序。

## 上传策略的兼容边界

`crashReporter.start(options)` 接受 `submitURL`、`uploadToServer`、`compress`、
`rateLimit` 和 `ignoreSystemCrashHandler`，但不修改宿主现有配置。
`setUploadToServer(boolean)` 是明确的兼容空操作。
**`uploadToServer: false` 或 `setUploadToServer(false)` 不会关闭宿主上传。**
需要修改收集、上传地址、同意状态或企业策略时，应由宿主配置完成。

`getUploadToServer()` 每次读取 Browser 的真实 Crashpad 上传可用状态，不返回应用
上一次传入的布尔值。Windows 同时检查 handler 是否具有可用上传 URL；Crashpad
不可用或读取失败时返回 `false`。查询在允许文件 I/O 的工作线程上执行。

`getUploadedReports()` 返回共享数据库中已成功上传的记录，含服务端 `id` 和
崩溃时间 `Date`，按崩溃时间从新到旧排序。Pending/未上传记录不计入。
`getLastCrashReport()` 返回该列表第一项，无已上传记录时返回 `null`。
结果是宿主共享数据库的记录，不限于当前应用。此实现不提供应用独立上传路由。

## 应用信息与参数

首次成功 `start()` 将 `productName`（默认 `app.getName()`）、可选 `companyName`、
应用版本和 `main` 角色记录在调用应用的 Utility 进程。合法的后续 `start()` 不更新
首次启动信息。应用信息使用 `xenon-electron-` 前缀，不覆盖宿主 `prod`、`ver`、`ptype`。

`extra`、`addExtraParameter`、`removeExtraParameter`、`getParameters` 使用调用进程
内的原生 Crashpad annotation；没有把应用参数写入 Browser 进程。原生报告中的参数
名为 `xenon-electron-extra-<key>`，JS 查询仍使用原始键名，因此即使应用提供 `prod`
等键名也不会覆盖宿主身份。

参数键为 1–39 个 UTF-8 字节，值为字符串，均不接受 NUL。值最多保留 20320 字节，
截断时保留完整 UTF-8 字符。实现直接使用 `CrashKeyString<20320>`，并非通过仅支持
1024 字节的动态 crash key 接口。为符合 Crashpad annotation 节点的进程生命周期
要求并限制内存，每个进程生命周期最多登记 128 个不同的 extra 键名；移除参数不会
释放这个永久名称槽位，超过限制明确报 `ERR_OUT_OF_RANGE`。窗口上下文或主容器销毁
会清除其最后写入的值，不会清除另一个仍存活上下文后来写入的值。

Renderer 只导出 `addExtraParameter`、`removeExtraParameter`、`getParameters`。
它们直接记录在当前 Renderer，不经过 Browser IPC。主容器与 Renderer 共用实现，
适用于 Windows、macOS 和 Linux 的 Crashpad 构建。

非空 `globalExtra` 需要对应用的全部进程可靠传播，当前未实现，明确报
`ERR_NOT_SUPPORTED`；空对象可接受。`productName`/`companyName` 的记录仅限调用的
main Utility 进程，不宣称已成为所有应用进程的全局字段。

## 验证

`xenon_crash_reporter_unittest.cjs` 覆盖两套生产 bootstrap、host-managed 空操作、
真实状态查询、参数验证和错误传播。原生 annotation 测试读取真实 Crashpad keys，
验证宿主字段保留、20320 字节容量、UTF-8 截断和上下文释放；报告转换测试使用隔离
输入验证排序和上传状态过滤。测试不制造崩溃、不启动 handler、不修改用户数据库，
不据此宣称验证了网络上传。
