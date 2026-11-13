# webContents.executeJavaScript

普通 BrowserWindow 和 webview 使用同一条异步文档 IPC 路径执行脚本。
执行前由 Browser 确认当前主文档的原生注册已完成，再返回具体端点；主进程
再次核对端点身份，避免页面加载完成与 Utility 注册回包的竞争。注册等待最多
30 秒，导航、销毁和权限失效会拒绝，不把请求转交下一份文档。
脚本始终在页面 main world 执行，
`contextIsolation: true` 时不会误在 preload world 执行，也不会把 preload 的
`require`、`process` 或原型对象带到页面中。

原实现通过 Browser 的 `RenderFrameHost::ExecuteJavaScript` 执行。该 Chromium API
限制为 WebUI/DevTools；普通 `file://` 或 `https://` 页面会触发 Browser 的
`CHECK(CanExecuteJavaScript())`。新实现使用已授权 Renderer 文档的 V8 context，
不修改 Chromium CHECK、不启用全局注入、不调用 `ForTests` API。旧同步窗口命令
`execute-javascript` 明确返回 `ERR_NOT_SUPPORTED`。

`executeJavaScript(code, userGesture = false)` 返回实际执行结果的 Promise。普通
值、Promise 和 thenable 均等待执行完成；语法错误、同步 throw 和异步 reject
使调用 Promise 拒绝。结果和异常经 structured clone 复制，支持 undefined、
BigInt、Date、Map、循环对象和二进制数据；不可克隆值明确拒绝。

页面禁用 `eval` 的 CSP 不阻止应用主进程授权的脚本执行。执行不会放宽 CSP；
页面自己调用 `eval` 仍受原策略限制。

`code` 必须为字符串，`userGesture` 必须为布尔值。当前尚未实现用户激活语义，
传入 `true` 明确返回 `ERR_NOT_SUPPORTED`，不会悄悄忽略。

每次请求绑定具体 endpoint 和浏览器登记的主 frame 身份；其他窗口、子 frame、
旧文档的回复不能完成请求。跨文档导航、文档断连、窗口销毁会拒绝未完成请求；
同文档 URL 更新不取消请求。代码发送后最长等待 30 秒，到时拒绝并通知 Renderer 清理
原生 Promise/context 引用。取消后的迟到回调通过原生 generation 校验丢弃。

JS 回归覆盖生产 bootstrap、请求关联及生命周期。V8 原生回归覆盖不同世界、
CSP eval 限制、Promise/thenable、结构化复制、错误、取消与销毁。Windows 真实宿主
已经验证 `contextIsolation` 两种配置共 24 项，包括 DOM 主世界、CSP 保持、
Promise/二进制/undefined 返回、三类错误拒绝和 Node 隔离；宿主正常退出。
证据：`F:/electron-compat-audit/20261111/script-execution-probe-01/verification.json`。

product11 build05 的真实导航探针额外确认了默认沙箱、无 preload 的 HTTP 窗口
和 WebContentsView：加载完成后立即执行脚本可以等到当前文档注册确认，不依赖
应用先发送 IPC，也不使用固定延时。证据为
`F:/electron-compat-audit/20261111/navigation-probe-product11-preseal05-01/verification.json`。
