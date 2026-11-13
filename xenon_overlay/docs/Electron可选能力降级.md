# Electron 可选能力降级

`app.getGPUFeatureStatus()` 当前返回新的空对象，表示尚未提供 GPU 功能状态。
它不报告任何功能已启用或已禁用，不改变硬件加速设置；调用者应把缺少的
状态字段视为未知。这是临时兼容降级，不能作为 GPU 功能探测结果。

此行为对所有应用一致，不根据应用名称、版本或调用位置选择返回值。
依赖具体 GPU 状态的调用方仍需处理未知状态；此次未扩展 `app.getGPUInfo()`。

`app.setAsDefaultProtocolClient()` 和 `app.removeAsDefaultProtocolClient()`
返回 `false`，表示未完成系统协议关联的注册或移除。宿主不修改系统默认
应用设置，也不报告注册成功。应用可以继续运行，但不能据此依赖系统深链
入口。这两个操作与 Chromium 内部自定义协议的资源加载权限相互独立。

原版应用是否能继续启动、显示主窗口和使用编辑功能，以产品实测报告为准。

`BrowserWindow.setTitleBarOverlay(options)` 校验装饰参数后保持原生标题栏
的现有样式和几何，不实现自定义按钮颜色或 overlay 高度。应用内容与窗口
创建可以继续，但不据此声称 Window Controls Overlay 已实现。
`webContents.isOffscreen()` 返回 `false`：当前宿主实际使用普通窗口，
没有实现 offscreen 渲染。它不把调用方请求的配置冒充已生效的渲染模式。

`app.setProxy()` 提供 Promise 接口，当前以 `ERR_NOT_SUPPORTED` 拒绝，
与尚未实现的 `session.setProxy()` 一致。不再因方法缺失而同步中断应用
初始化，但代理配置仍未应用；应用必须自行处理拒绝。普通网络请求继续
使用宿主已有网络配置，不以 Promise 成功冒充应用代理设置已生效。

`protocol.registerSchemesAsPrivileged()` 接受 `codeCache` 的布尔声明，
但不将它写入有效协议权限或子进程快照；请求开启时记录 disabled。
这只是性能优化的降级。未实现的功能权限 `allowServiceWorkers: true`
和 `stream: true` 返回 `ERR_NOT_SUPPORTED`，不接受开启后静默禁用。
URL 来源、standard、secure、fetch、CORS 和 CSP 权限继续严格核对启动时
冻结的声明。

以上均为通用 Electron 接口边界，不以某个第三方应用能否启动作为正确性
标准。文件、网络、加密、权限和进程生命周期不能采用返回成功的占位实现。
