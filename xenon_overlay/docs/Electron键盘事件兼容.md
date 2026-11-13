# Electron 键盘事件

BrowserWindow、WebContentsView 和 DOM webview 的
`webContents.on('before-input-event', (event, input) => ...)`
在 Chromium 向网页分发键盘事件之前执行。`input` 提供 Electron 的按键类型、
`key`、`code`、修饰键、自动重复、组合输入和位置字段；监听器同步调用
`event.preventDefault()` 会阻止该按键进入网页及后续 Chromium 快捷键处理。
字符事件不会重复生成 `before-input-event`，但与按下、松开事件保持 FIFO 顺序。

应用主进程在 Utility 中运行，因此 Browser 使用异步 Mojo 请求获取实际监听器
决定，不同步等待 JavaScript。只有订阅事件的窗口安装过滤器；普通网页不分配
该队列，也不产生额外 IPC。最后一个监听器移除后不再发送新请求；已有输入保持
顺序处理，包含 `once` 在当前回调内移除自身的情况。

队列保留原始 widget、按键、延迟记录和编辑命令。跨文档导航、窗口销毁及服务
代际变化会丢弃旧输入，防止迟到回调进入新的页面。单次响应最多等待 5 秒，
待处理输入上限 512；超时或溢出时丢弃该批输入，下一次输入重建队列，不永久
锁死窗口，也不将未获得决定的旧按键重新注入页面。

Chromium 输入入口的修改受 `ENABLE_XENON_SERVICE` 宏保护，使用通用键盘类型和
跨平台按键转换，不依赖 HWND。原生视图与独立 WebContents 共用相同的过滤器，
不依赖窗口所有权。尚未纳入容器管理的浏览器 WebContents 明确拒绝订阅；
不伪造成功，也不把这个能力扩展为操作系统全局快捷键注册。
