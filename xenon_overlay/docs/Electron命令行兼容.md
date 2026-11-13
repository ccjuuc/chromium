# app.commandLine

实现已纳入 product10 并通过原生与 JavaScript 回归。product11 build05 的
Windows 真实宿主探针已通过 4/4：命令行元数据、共享沙箱变更拒绝、新渲染进程
实际启用 `--expose-gc`，以及移除开关只影响后续进程、保留既有进程状态。
证据为 `F:/electron-compat-audit/20261111/command-line-probe-product11-preseal05-01/verification.json`；
宿主退出码 0，无残留进程或新增 dump。这个结果不代表整个 product11 已封版。

命令行使用原生 `base::CommandLine`，每个应用启动代次各有一份独立快照。`hasSwitch`、`getSwitchValue`、`appendSwitch`、`removeSwitch` 和 `appendArgument` 不修改 `process.argv`，也不修改其他应用或宿主的全局参数。开关名称按 Electron 规则忽略 ASCII 大小写；不存在或没有值时返回空字符串。参数由原生代码处理引用，`appendArgument('--name=value')` 不重新解析为开关。

应用对以下参数的修改会传给自己的新渲染进程，在 Chromium 创建 FeatureList 和初始化 Blink/V8 前应用：

- `enable-features`、`disable-features`、`enable-blink-features`、`disable-blink-features`。
- `js-flags`、`lang`、`max-active-webgl-contexts`、`autoplay-policy`。
- `disable-background-timer-throttling`、`disable-renderer-backgrounding`、`disable-lcd-text`、`disable-smooth-scrolling`。
- `enable-experimental-web-platform-features`、`enable-precise-memory-info`、`force-color-profile`、`force-device-scale-factor`。

独立传输参数避免被 Content 随后复制的宿主默认值覆盖；它只接受上述渲染配置，不能覆盖子进程类型、沙箱、Mojo 或 Crashpad 参数。既有渲染进程不重新初始化。

共享 Browser/GPU 已在 Electron main 运行前完成初始化。`remote-debugging-*`、`no-sandbox`、`enable-sandbox`、`single-process`、`renderer-cmd-prefix`、`type`、`user-data-dir` 和代理启动选项不能在这里重新配置，修改时明确返回 `ERR_NOT_SUPPORTED`；需要在宿主启动参数或已有 Session 接口中配置。`app.enableSandbox()` 会在验证和启用真实策略后单独维护可查询的沙箱开关。

其他开关仍可作为命令行元数据查询，但没有因此获得运行期副作用。尤其 Browser/GPU 的功能开关不能因为出现在 `enable-features` / `disable-features` 字符串中，就被认定已修改共享 Browser/GPU。既有的 GPU 启动选项和 `app.disableHardwareAcceleration()` 尚不等于逐应用 GPU 配置能力。

源测试覆盖原生引用、Unicode、大小写、应用间隔离、失败原子性、渲染覆盖顺序及无效/跨进程类型载荷拒绝。所有 Chromium 接线必须由 `ENABLE_XENON_SERVICE` 控制；macOS/Linux 不宣称已运行通过。
