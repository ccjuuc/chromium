# Electron Session 权限与媒体

Session 的 request/check/display handler 保留在应用的 Utility 主进程 V8 中。
Browser 只保存注册编号、实际 StoragePartition 归属及待完成请求；不会把闭包
序列化，也不会缓存一次回答来替代后续检查。同源的不同应用和 partition 保持独立。

`setPermissionCheckHandler` 使用独立同步 Mojo 管道，以保留 Electron 同步布尔
返回值以及回调内 `BrowserWindow.getBounds()` 等反向同步调用。专用传输线程的
5 秒 watchdog 只断开该权限管道；不依赖被阻塞的 Browser UI timer，不主动运行
嵌套 UI RunLoop。超时/异常/断连均拒绝，后续检查在同一有效应用代际上重建管道。
同步调用豁免仅授予这个经过真实 Mojo 测试的传输类，并由 `ENABLE_XENON_SERVICE`
保护，未向任意 Browser→Utility 调用开放同步权限。Chromium 在 Browser 中默认
禁止同步等待被其他同步请求中断；权限调用只在当前序列、本次有界等待期间开放
反向同步调用；这是整个序列的豁免，并非协议级 getter 白名单。各 Browser 入口仍须
单独拒绝依赖异步任务或模态操作的重入，不修改全局开关、不执行普通 UI 任务。
回归测试使用与真实 Browser
相同的全局禁用设置，并验证离开作用域及其他序列仍保持禁用。
管道在应用入口执行前预绑定；共享服务的绑定消息发出前，检查立即拒绝而不会
阻塞它依赖的 UI 队列。超时重建使用同样的时序屏障，不触发权限订阅自动重试。
同步等待期间使用当前序列的嵌套作用域识别反向调用；只读 `app.getPath()` 可以
直接返回快照，`app.setPath()` 或首次创建存储必须等待普通异步任务，因此在该
作用域内立即返回 `ERR_INVALID_STATE`。拒绝发生在排队和改路径之前，避免死锁
及回调结束后的迟到修改。其他序列和正常应用调用不受这一限制。
单实例锁查询/申请/释放、登录启动项读写、Jump List 更新和打开/保存/消息对话框
同样依赖异步后端或 UI 任务，也在该作用域内于排队前拒绝，避免权限超时后
才取得锁、修改系统设置或弹出对话框。正常作用域内仍使用真实平台后端。

同步 handler 的等待本身仍发生在 Browser UI，最坏可达五秒。任意 JavaScript
闭包可读取随时变化的应用状态，不能用缓存回答或异步默认许可无损替代其同步返回值。
因此本轮移除适配层对话框嵌套循环、封住重入后仍保留此契约；不能将“等待有界”
描述为“不会卡 UI”。彻底消除等待需要提供显式声明式权限策略或更改同步 API 契约。

`setPermissionRequestHandler` 异步请求最多等待 30 秒。回答必须匹配 partition、
注册编号及请求编号，并且只完成一次；替换 handler、退出应用、Profile 关闭、
超时及文档失效均拒绝。注册时先让 JS 新闭包可被反向调用；若原生拒绝注册则恢复
前一个闭包，避免订阅刷新抢先看到尚未提交的注册编号。

没有自定义 handler 时，存活的 Electron 应用遵循 Electron 默认授权策略，旧式
同步剪贴板读取例外；普通浏览器仍由 Chromium PermissionManager 处理。应用退出
后保留 partition 的所有权身份，撤销 preload、事件和执行状态，直到 partition
销毁。这使仍在销毁中的 Electron 文档保持拒绝，不能降级使用普通浏览器权限。

Profile 的权限代理只截取有可信 frame/process partition 的调用。缺乏文档或
进程上下文、只给 origin 的查询仍走 Chromium；不能用 URL 推测属于哪个应用。
Electron 订阅与普通浏览器订阅分别维护，原生设置变更不会覆盖应用的回答。

`setDisplayMediaRequestHandler` 接受 desktopCapturer 返回的真实 source，原生媒体
层验证 source ID、捕获政策及平台能力后创建实际捕获设备。拒绝和无可用 source
不能返回成功。当前 WebFrameMain 作为捕获 source/audio、系统 picker 选项明确返回
`ERR_NOT_SUPPORTED`。相机/麦克风与系统屏幕录制权限仍受真实平台权限和设备约束。

原生传输和策略 16 项、JS handler 6 项已通过，覆盖反向同步调用、不执行普通 UI
任务的超时、迟到回复、销毁 owner、注册替换、隔离及一次性完成。Windows 实际
产品验证覆盖网页授权/拒绝、订阅 onchange、异步回答、替换取消、跨文档取消、
Worker 和普通 Browser 同源隔离。产品 14 的独立压力探针 10 项全部通过：真实
窗口和路径 getter 正常返回；存储修改、锁操作、登录启动项读写、Jump List 和
错误对话框在 0–1 毫秒内拒绝且无迟到锁，作用域外登录启动项查询正常；抛异常
后可恢复；6500 毫秒忙循环在约 5 秒被拒绝，新权限管道随后正常授权。宿主正常
退出且没有产生 dump。macOS/Linux 尚需对应平台运行验证。
