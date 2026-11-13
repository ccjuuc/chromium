# Electron Notification 兼容

主进程 `electron.Notification` 使用当前应用所属 Profile 的 Chromium 通知服务，支持标题、正文、图标、`silent`、`timeoutType` 和 `urgency`。图标通过现有 `nativeImage` 处理后传输。不修改原发行包，不依赖应用名称、页面地址或特定项目分支。应用名仅用于显示通知来源。

Windows、macOS 和 Linux 复用已有平台通知桥；当 Chromium 选择自身消息中心作为回退时，也沿用该选择。`isSupported()` 查询已有初始化流程的结果及实际可用后端，初始化尚未完成时返回 `false`，不会注册第二个 ready callback。系统设置、权限和勿扰模式仍可能阻止通知实际显示。

`show` 事件表示 Chromium 已把请求提交给通知后端，不能证明用户看到了通知。正文点击和关闭事件通过平台回调路由给原所属应用。提交参数或服务不可用等同步错误直接抛出；目前 Chromium 的 `Display` 接口没有提供此接入所需的 OS 异步失败反馈，因此尚不能观察或保证 `failed` 事件。JavaScript 中存在 `failed` 的分发路径，不代表原生异步失败已经接通。

通知 ID 每次展示重新生成，映射绑定 Profile、应用及启动代次。应用关闭、崩溃或重启时，先删除回调身份，再关闭通知；晚到的 OS 事件不会投递到新代次，也不会自动启动应用。清理只查询已存在的通知服务，不为从未使用通知的应用初始化后端。Profile 销毁后，映射随其通知服务一起释放。

操作按钮、回复、声音、自定义 toast XML 等尚未实现的非默认选项明确报 `ERR_NOT_SUPPORTED`。平台可能忽略自身不支持的优先级或超时偏好。Chromium 源码中的枚举、能力查询和生命周期扩展均受 `ENABLE_XENON_SERVICE` 控制；关闭功能时 `isSupported()` 为 `false`，不会引用新增枚举。

当前跨平台结论来自源码路径审查；Windows 原生展示、点击/关闭及 macOS/Linux 实机验证结果必须分别记录，不能以单元测试或提交成功代替 OS 可见性验证。

产品08的 Windows 独立宿主验证 `notification-probe-01` 已通过 5 项：ready 限制、实际后端可用状态、提交后的 show 事件、close/重新展示、退出前仍存在的通知清理流程。宿主正常退出，无残留测试进程或新增 dump。这些结果不包含系统通知的目视确认和实际点击验证。
