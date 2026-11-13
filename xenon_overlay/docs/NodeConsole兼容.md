# Node Console 兼容

`console` 与 `node:console` 在 main、Worker 和启用 Node 的 renderer 指向同一内置模块实例，导出 `Console` 和绑定的输出方法。main/Worker 的全局 `console` 使用该实例；renderer 的全局 `console` 保持 Chromium 的 DevTools 行为，Node 模块通过捕获的浏览器日志入口输出。

共享实现 `resources/ipc/common/console.js` 支持位置参数和 options 形式的 `Console` 构造、子类方法绑定、stdout/stderr 分流、`ignoreErrors`、`assert/dir/trace`、计数、计时、分组，以及普通数组/对象/Map/Set 表格。`clear` 只在输出流声明为 TTY 且 TERM 不为 dumb 时写清屏转义。未创建终端、管道或虚假的 OS 文件描述符能力。

默认 stdout 写入真实宿主日志，stderr 复用 debuglog 已建立的日志流。应用替换 `process.stdout/stderr` 或其 `.write` 后，内置 Console 会使用当前输出流；自建 Console 使用构造时给定的流。stdout 不再静默丢弃写入。主进程输出同步发布对应 `console.log/info/debug/warn/error` diagnostics channel。缺失计时标签、重复标签、无效 countReset 通过 warning 事件与 stderr 报告；计时使用单调时钟。

格式化复用现有 util，而不是复制第二套格式化器。当前 main/renderer 的 `util.format/inspect` 仍有既知差异，包括额外参数、对象展示、格式选项和 renderer 占位符；本次不宣称这些差异已消除。Console 差分测试注入系统 Node util，验证格式化参数和 inspectOptions 正确传递；另外通过真实 bootstrap 检查输出与重定向。表格显示宽度目前按去除 VT 后的 Unicode code point 计数，未承诺 CJK/emoji 的终端列宽及 Map/Set iterator 内部预览与 Node 一致；浏览器 inspector 的 profile/timeStamp 功能不属于该 Node 模块实现。

验证：`xenon_console_unittest.cjs` 9 项通过，涵盖系统 Node 差分（构造/输出/计数/单调计时/分组/表格）、同步和异步 Writable 错误、真实 trace、两端 bootstrap 接线。相关 debuglog/builtin/module/diagnostics/Worker 72 项通过。原生新增 `ConsoleBuiltinSharesCjsEsmAndGlobalIdentity` 验证真正模块加载与输出，等待统一编译执行。
