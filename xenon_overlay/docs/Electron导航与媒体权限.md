# Electron 导航与媒体权限

## 页面加载

`BrowserWindow`、`WebContentsView` 和 webview 的主动导航共用原生导航跟踪。
`loadURL()` 的 Promise 绑定 Chromium navigation ID，提交后继续绑定具体文档；
只有该文档完成加载才成功，同文档导航在提交后完成。被后续导航替换、停止、
窗口销毁、渲染进程退出或网络失败时拒绝，返回 `code`、`errno` 和 `url`。
错误页的 `did-finish-load` 不再当成原请求成功。

`did-fail-load` 使用 Electron 的位置参数：event、errorCode、errorDescription、
validatedURL、isMainFrame、frameProcessId、frameRoutingId。当前没有提供的
frame 标识使用 -1。导航完成回调排入任务队列，不在 NavigationController 调用栈
上执行应用逻辑。异步窗口命令按发起应用、窗口归属和启动代次认证。

页面加载与内部 IPC 注册分别确认。`executeJavaScript()` 在发送代码前，向
Browser 请求当前文档的已确认端点；原生注册完成后才返回端点 ID，JavaScript
再次核对同一文档。等待期间导航或销毁会拒绝，不能把请求移交给下一份文档。
`loadURL()` 本身仍以真实页面加载为准，不把内部 IPC 状态冒充页面加载状态。

`loadFile()` 相对应用根目录解析，再转换 file URL；文件名中的 `%`、`#`、`?`
按字面路径处理，query/search/hash 作为独立 URL 选项。BrowserWindow 和
WebContents 使用同一实现。

仍未覆盖 Electron 所有导航选项，例如 POST 请求、额外请求头和 referrer；
本文不将现有 userAgent 支持扩大解释成完整 options 兼容。

## 媒体请求

Electron Session 的权限检查和请求接入 Chromium Profile 委托。设备音视频
请求继续使用 Chromium 真实设备枚举和 OS 权限流程；应用授权本身不制造设备。
音频、视频与 PTZ 的权限归并为 media 请求，details 保留 mediaTypes。

显示捕获先调用权限 handler，再调用 display-media handler。屏幕与窗口选择
交给 Chromium `GetDevicesForDesktopCapture`，使用真实源 ID、设备信息、
捕获指示与系统后端。返回无效 ID、未支持源、系统策略不允许或应用退休时拒绝。
每个异步回包都重新检查文档及应用启动代次，旧页面不能授权新页面。

普通浏览器页面保持既有权限路径。退休 Electron Session 仍保留归属标记直到
分区销毁，因此失效页面不会退回普通浏览器权限分支。

当前显示源支持屏幕与窗口；tab-frame、多屏选择尚未实现。系统音频由平台能力
判断，不能将不支持的平台返回为成功。`enableLocalEcho` 只对 tab 音频有意义，
当前屏幕/窗口路径不提供该效果。macOS 屏幕录制仍受系统权限限制。

## 验证

product11 build06 的 Windows 导航探针已通过 7/7，证据为
`F:/electron-compat-audit/20261111/navigation-probe-product11-final01/verification.json`。
覆盖子资源及 onload 完成、同文档导航、文件不存在、无效 URL、导航替换、
WebContentsView 和窗口销毁取消。默认沙箱且没有 preload 的窗口也能在导航后
立即执行脚本；宿主退出码 0，无残留进程或新增 dump。

媒体探针 `media-permission-probe-product11-final03` 通过 3/4，验证了真实设备
授权拒绝/允许、音视频权限归并和停止轨道、无效桌面源拒绝。窗口捕获已创建
真实视频轨道，但连续帧检查失败：原生窗口可见且有焦点，页面仍报告隐藏。
通用窗口可见性同步已加入 product12 源码，尚待编译后验收；不把初帧或 live
轨道当作连续视频播放通过。

JS 窗口契约已通过；6 项原生导航测试覆盖完成、网络错误、连续导航、
窗口销毁、无效 URL 与已提交文档加载中止。同文档跳转使用真实浏览器探针验收，
Chromium 的 CreateFromPending 测试辅助方法不能恢复这类已转交 RFH 的导航。

媒体设备探针使用 Chromium 合成音视频设备验证真实 getUserMedia、track 和
stop 路径，不将其称为真实摄像头/麦克风验收；显示捕获使用自有测试窗口并检查
实际视频帧。需要手势的显示捕获由 CDP 输入点击触发，不伪造 executeJavaScript
的 userGesture 支持。Windows 实机结果不能代替 macOS/Linux 验证。
