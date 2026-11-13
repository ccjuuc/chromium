# Node 调试日志兼容

`util.debuglog(section[, callback])` 与别名 `util.debug` 在 main、Worker 和启用 Node 的 renderer 使用同一实现。`NODE_DEBUG` 在启动时快照，匹配大小写不敏感，支持逗号分组、`*` 通配符和正则特殊字符的字面匹配；运行期间修改 `process.env.NODE_DEBUG` 不改变启用状态。

section 转换与内部 logger 延迟到 `.enabled` 查询或首次调用。每次 `debuglog` 返回独立包装函数，同一大写 section 复用内部 logger。可选 callback 仅在首次实际初始化时收到优化后的 logger；先读取禁用 logger 的 `.enabled` 会使后续调用直接返回，不触发 callback。HTTP/HTTP2 类别保留 Node 的调试输出警告。

启用的日志以 `SECTION pid: message\n` 写入当前 `process.stderr.write`，因此应用替换该方法仍可截获输出。默认 stderr 连接到现有宿主日志：main/Worker 进入 native 日志，renderer 进入 Chromium console.error。它不是终端，`isTTY` 为 false；没有新增终端色彩、可写文件描述符或管道的声明。renderer 仅新增 `NODE_DEBUG` 这一项原生环境元数据。

格式化委托现有 `util.format`。当前 main 与 renderer 的 `util.format`/`inspect` 尚未完整对齐 Node（例如额外参数、对象展示及 renderer 的格式占位符）；本修改没有宣称这些既有差异已修复。差分测试将 Node formatter 注入工厂，单独验证日志匹配、延迟行为、缓存身份和参数转发；bootstrap 测试另行确认真实输出及可替换 stderr。

`xenon_debuglog_unittest.cjs` 的 14 项测试通过，含 10 组独立 Node 子进程对照及两端 bootstrap 输出检查。相关 util/module/builtin 合计 55 项通过。原生资源打包和第三方应用启动由统一构建继续验证。
