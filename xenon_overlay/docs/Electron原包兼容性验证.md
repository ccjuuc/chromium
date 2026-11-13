# Electron 原发行包兼容性验证（2026-11-12）

目标是验证通用 Electron / Node 兼容能力，直接加载未修改的官方 Electron 发行目录。draw.io、MarkText 已通过真实打开、编辑和保存验收；Motrix 已完成真实下载、最终文件提交和哈希校验；TH、PLE 核心流程通过。按用户要求，VS Code 已退出验证目标，其历史失败记录仅作为缺口来源留存，不再为了推进该应用启动而扩展框架。不能据此宣称兼容任意 Electron 应用或全部功能。

原包源码、ASAR、依赖和入口均不修改。非核心优化和辅助接口可以采用明确记载的占位行为，不要求一次实现全部 API；文件内容、下载状态、权限、加密和网络结果不能伪造成功。

## 实际应用结果

以下完整核心基线来自 product28，build02 成功，seal 无待构建项。五个应用均在该版重新验收；请求头网络实现沿用 product27 的专项实测。后续侧栏和关闭流程验证见下节，不将基础打开/保存通过等同于全部关闭路径可用。产物目录为 `out/Release_64`。证据根目录为 `F:/electron-compat-audit/20261111/`，以下路径均相对此目录。

| 应用 | 验证范围 | 结果与证据 |
| --- | --- | --- |
| draw.io 32.6.10 官方原包 | 桌面入口、preload IPC、CLI 打开诊断图、编辑保存并重新读取磁盘 | product28 4/4；`drawio/core-probe-product28/verification.json` |
| MarkText 0.23.0 官方原包 | CLI 打开 Markdown、页面 Node 隔离、真实输入、Ctrl+S 保存、关闭已保存文档 | product28 5/5；`marktext/core-roundtrip-product28/verification.json` |
| Motrix 2.4.1 官方原包 | 真实数据库、文件处理 Utility、aria2 启动、WebSocket RPC、创建任务、HTTP 下载 | product28 任务 completed、进度 100%；最终 sample.bin 为 2,097,289 字节且 SHA-256 与测试源一致，无临时文件残留，应用菜单退出成功。`motrix/xenon-core-product28-01/result.json` |
| TH | 登录恢复、云盘、片库、真实 app.quit | product28 3/3；`th-retirement-ports-product28/verification.json` |
| PLE | 真实播放进度、320×180、错误码 0；业务关闭后媒体释放、ph/ch 原生两窗隐藏 | product28 通过；`ple-core-product28/result.json`。本轮未重新目视验收拖动和截取视频层 |

上述实测宿主均退出 0，无所属测试残留进程或新增 dump。Motrix 仍有不影响本次闭环的诊断（如 session.setProxy 尚未支持）；本轮没有验收代理、全部下载协议或插件功能。当前测试使用独立 profile、目录和专用端口，不使用或关闭用户 9222；退出按 PID 与创建时间共同归属。已停止的 VS Code 早期 profile 隔离问题见历史记录。

## 侧栏与未保存文档关闭

product29 在 TH 后增加 DIO、MT、MX，分别读取相对发行目录描述文件。用户确认三款均能打开，
随后明确反馈 DIO 无法关闭。`electron-sidebar-product29-02/browser.log` 记录 draw.io
调用 `dialog.showSaveDialog` 和 `dialog.showMessageBoxSync` 时抛出未支持错误。
原包关闭处理先设置等待确认标志，异常使其无法复位，之后关闭事件被原包忽略。
因此 product28 已保存文档的关闭结果不能覆盖此路径。

product30 补齐通用真实确认框和保存框，详见 [Electron对话框兼容.md](Electron对话框兼容.md)。
不修改原包、不自动选择“不保存”，也不通过清除应用私有标志绕开确认。
未保存的绘图已只读备份为
`electron-sidebar-product29-02/drawio-readonly-backup/2026-11-12T07-00-20-614Z.drawio`，
4,166 字节、一页、17 个 graph cells；备份前后 dirty 状态均为 true。

同轮修正 `webContents.getURL()` 在同文档导航后滞后的通用缺口。Browser 从可信已提交导航
更新现有 renderer 元数据，JS 仅用当前显式主框架的 committed URL 更新窗口地址。
子框架、过期 endpoint 和尚未提交的 loadURL 请求不能覆盖该值。Motrix 原包 IPC 来源检查
保持原样，不放宽 `sender.getURL() === senderFrame.url`。

product30 build03 成功，seal 无待构建项；JavaScript 1396 通过、14 跳过，原生 IPC
610/610、原生对话框 10/10 通过。draw.io 基础闭环 4/4 通过
（`drawio/core-probe-product30/verification.json`）。真实导航探针 9/9 通过，包含没有后续
IPC 的 hash/pushState/replaceState、子框架隔离和跨文档主框架替换
（`navigation-url-product30-01/verification.json`）。
用户在侧栏 product30-01 确认“取消、保存关闭、重开都正常”。宿主日志记录 draw.io
两次独立启动、两次 code=0 退出，第二次已正常重新初始化；证据为
`electron-sidebar-product30-01/{browser.log,events.jsonl}`。原生保存框的实际操作由用户完成，
不将桌面工具窗口绑定失败表述成自动化验收通过。

用户随后继续操作其他测试应用。为保留窗口而停止观察脚本时，关联宿主也一并退出；
该次宿主结束不计作正常退出验收。之后使用相同测试 profile 重新启动独立浏览器，
取消自动关闭计时，交由用户继续使用。操作结果和此边界记录在
`electron-sidebar-product30-01/operator-verification.json`，后续日志为
`browser-user-session.log`。

Motrix product30 原包切到 `#/downloads` 后严格 IPC 校验通过，真实下载 2,097,289 字节，
SHA-256 匹配，菜单正常退出，无所属残留进程或 dump
（`motrix/xenon-navigation-product30-01/result.json`）。首次引导窗口销毁时仍出现一次
无效 sender 的拒绝，导航后的持续 URL 不一致已消失；代理与配对相关的既有诊断仍保留。

侧栏探索还再次记录 MarkText 退出后原生环境在 30 秒内未排空，以及 Motrix 后续刷新路径
缺少 `events.addAbortListener`。二者未在本轮 DIO 修复范围内处理；不能据此宣称三款应用
全部功能与退出路径已经兼容。

## 回归证据

不同版本分别列出，不将历史专项表述为最新产物全部重跑。

| 范围 | 版本与结果 | 证据 |
| --- | --- | --- |
| JavaScript 全量 | product30：1396 通过、14 跳过、0 失败 | `javascript-full30-root03.log` |
| 对话框与 IPC | product30：Views / 选择器合同 10/10、完整 IPC 610/610 | `dialog-native30-01`、`native-full30-01`；Manager 新增回归仅编译，未运行完整 Chromium unit_tests |
| 真实导航 URL | product30：9/9；导航后无后续 IPC 仍及时更新 | `navigation-url-product30-01/verification.json` |
| JavaScript 全量 | product28：1387 通过、14 跳过、0 失败 | `javascript-full28-root01.log` |
| 应用配置、URL、协议权限 | product28：配置与 URL 10/10、协议声明 13/13；包括 1,004 组新增 URL 向量 | `native-contract28-01`、`schemes-full28-01` |
| URL 产品对照 Node | product28：2570/2572 一致；两项既有 file URL 解析差异，严格全一致为 false | `url-can-parse-probe-product28-01/verification.json`；[NodeURL兼容.md](NodeURL兼容.md) |
| 原生 IPC、Session（请求头支持） | product27：609/609、140/140 | `native-full27-01`、`session-full27-01` |
| 通用请求头真实 HTTP | product27：10/10，服务端验证 Cookie、增改删、清空、取消、延迟、重定向和分区隔离 | `request-headers-product27-01/verification.json` |
| 模块查找钩子、ESM | product23：33/33；当轮完整 IPC 604/604 | `Node同步模块钩子.md`；product23 原生日志 |
| 原生 IPC | product20：600/600 | `native-full20-01.{log,json}` |
| Session | product20：132/132 | `session-full20-01.{log,json}` |
| N-API、Context、权限传输/策略 | product17：62、19、16，均通过；对应源未变 | `napi-full17-01`、`context-full17-01`、`permission-full17-01` |
| BigInt Stats | product20：原生新增 4/4（已包含在 IPC 600 项），真实四角色 15/15 | `native-stats20-02.json`、`fs-bigint-probe-product20-01/verification.json` |
| 原版 ws 实际网络 | product19：main、Utility、renderer 各 8 项，共 24/24 | `node-http-upgrade-product19-01/result.json` |
| AsyncLocalStorage | product19：原生专项 7/7；真实 Worker/Utility 6/6 | IPC 原生套件；`async-context-probe-product19-01/verification.json` |
| 初始隐藏绘制 | product17：四组合 58/58；无 Node/无 preload/sandbox 4/4 | `window-visibility-product17-01` 至 `04`、`window-no-node-product17-01` |
| View、导航、权限 | product17：View 30/30、隐藏导航 10/10、权限传输 10/10 | `view-product17-01/02`、`window-navigation-product17-02`、`permission-transport-product17-01` |
| 文件权限 | product16：四角色真实 chmod 31/31；Windows 原生新增 3/3 | [Node文件权限兼容.md](Node文件权限兼容.md) |
| Timer、execFile | product15：四角色 Timer 8/8 主组；main/Utility/renderer execFile 24/24 | `node-timers-product15-01/result.json`、`node-exec-file-product15-01/result.json` |
| Worker 退出 | product15：UDP 自然退出 4/4、强制终止 3/3、关闭三个活动 Worker、完整 Worker 19/19 | `worker-udp-probe-product15-01`、`worker-udp-terminate-probe-product15-01`、`worker-udp-host-shutdown-probe-product15-01`、`worker-threads-probe-product15-01` |
| 存储隔离与持久化 | product14：Unicode 长路径四阶段通过 | `storage-probe-product14-02-01`：Cookie、LocalStorage、IndexedDB、CacheStorage、默认分区 Service Worker、Utility 首次使用 Session |
| Node HTTP/fetch 与协议 | product12：网络隔离 9/9、协议 15/15，无新增 dump | `node-network-product12-08`、`protocol-probe-product12-08b`；未知协议返回 -302 |

原版 ws 覆盖压缩文本、二进制、UTF-8 分片、ping/关闭、显式 Cookie、URL Basic Auth、403、错误 accept、升级后的双向 Socket/head、TLS 明确拒绝。独立 fixture 的 ws 从原 ASAR 只读提取，逐文件比对归档 SHA-256；Motrix 自身仍直接运行原 ASAR。

早期隐藏窗口和捕获连续帧失败记录保留。当前窗口结论采用 product17 的真实帧和生命周期检查，不以 document.visibilityState、target 存在或退出 0 代替绘制与业务成功。全部媒体设备/格式组合未验收。

## 通用实现与边界

- 识别标准发行根目录，接受一级版本子目录中唯一的标准布局，拒绝歧义和越界。main/renderer 共用 package exports/imports 解析。
- Main 使用 V8 ESM 图，支持静态/动态 import、循环/live binding、JSON attributes、import.meta、同步 require(ESM)、顶层 await；不转写业务源码。CommonJS 导出由上游 cjs-module-lexer 识别。Renderer Node 加载器仍限 CommonJS，`.mjs` preload 不在支持范围。
- 异步入口完成后才 ready，失败离开 V8 栈再退出。nodeIntegration 关闭时页面无 Node 全局，preload 保留独立能力。executeJavaScript 遵守授权文档、CSP、异常与生命周期取消。
- stat/lstat/fstat 和 FileHandle.stat 的 BigInt 整数、纳秒直接来自 libuv，以十进制字符串跨 IPC 传输，不经 Number 舍入；路径与句柄共享转换，保持文件身份核对语义。ASAR/虚拟挂载只转换已有字段。见 [Node文件元数据兼容.md](Node文件元数据兼容.md)。
- fs 识别 ASAR；original-fs 读取物理字节。addon 按应用 canonical 路径授权，ASAR 只允许 unpacked 成员；加载错误不伪装成 MODULE_NOT_FOUND。
- 流复用 upstream readable-stream。TCP Socket 具有真实 Duplex 语义、背压和 noDelay；HTTP Upgrade 交回活 Socket，普通 HTTP/fetch 继续使用不附带浏览器 Cookie/缓存的 URLLoader。TLS Socket/WSS 升级明确不支持，不降级为明文。见 [NodeSocket兼容.md](NodeSocket兼容.md)、[NodeHTTP升级兼容.md](NodeHTTP升级兼容.md)。
- main/Worker/Utility 独占 isolate 的原生 PromiseHook 支持真实 ALS，不改 V8 编译配置；renderer 共享 Blink isolate，当前构建不支持激活 ALS。不能用同步保存 store 假装跨 await 传播。见 [NodeWorker线程兼容.md](NodeWorker线程兼容.md)。
- electron.net 使用实际 Session；MessagePort 使用 structured clone 和真实端口转移；Utility 是独立 OS 进程。退出等待原生运行时排空，不用永久引用或延时掩盖析构错误。
- 默认隐藏窗口通过真实 nonempty layout 触发 ready-to-show，局部维持 Surface 和帧调度，未全局关闭 Viz 节流或伪造 UI 可见状态。
- crypto 随机填充来自安全随机源；UTF-8 有界写入保持字符边界。少数非核心 Node 参数重载和回调时序仍有差异，不宣称逐参数完全一致。
- crashReporter 共用 Chromium Crashpad，不另起 handler、不改宿主上传地址、不真正关闭采集。自行启动的外部 exe 不因此纳入浏览器崩溃管线。

产品实现无第三方应用名特判或原包补丁。Chromium 新增行为受 `ENABLE_XENON_SERVICE` 保护；只读审查未发现遗漏，宏外部分限头文件/构建依赖和等价重构。平台 API 使用相应条件。**仅 Windows 非 component 构建实际编译运行；宏关闭的整库、macOS、Linux、component 构建未实测。**

## 通用性复核与保留范围

针对 product21–27 的复核只保留通用 Electron / Node 契约：

| 修改 | 处理与边界 |
| --- | --- |
| `Module._nodeModulePaths`、`_resolveLookupPaths`、`require.resolve.paths` | 保留真实 live hook、父模块身份与缓存更新；没有自动搜索某个 ASAR 或应用名特判，最终仍核对应用根目录和原生扩展授权。 |
| preload `additionalArguments` | 保留从可信 Document 的 webPreferences 复制参数；不解释业务参数，不共享跨窗可变数组。 |
| `webRequest.onBeforeSendHeaders` | 保留真实网络实现及通用 HTTP 回归；不按市场域名或应用名称选择请求。 |
| `URL.canParse` | 保留通用接口，修正非法主机名误判；2570/2572 产品对照一致，剩余两项 file URL 解析差异明确记录，不扩展旧 URL 构造器。 |
| GPU 状态、协议关联、标题栏装饰、代理接口 | 按通用能力边界保留空状态、明确失败或装饰降级；不报告网络/系统操作成功，见 [Electron可选能力降级.md](Electron可选能力降级.md)。 |
| `allowServiceWorkers: true` | 撤回接受声明后禁用的绕过，恢复 `ERR_NOT_SUPPORTED`；未实现的功能权限不能当作性能提示。 |
| `codeCache` | 保留可选优化降级，接受声明但不启用缓存，不影响真实协议权限。 |

生产路径中未发现 VS Code 名称、目录、业务域名或模块名分支；URL 单测中的应用包名示例已改成通用 scoped package。没有实施后续 frame/mainFrame 补齐或启动协议架构重构；外置探针、描述符和历史证据保留，但不再作为运行目标或支持承诺。

## 已停止的 VS Code 探索（历史记录）

Chromium 在 `ChromeContentClient::AddAdditionalSchemes` 读取外部声明后锁定协议表，应用 main 更晚才在 Utility 执行。运行期 `registerSchemesAsPrivileged` 只能校验启动快照，无法新增 `standard/secure` 等全局权限。直接传发行目录不会预执行源码猜测声明。

原版声明四个 vscode-* 协议，没有 stream。product20 纯目录实测停在 `allowServiceWorkers` 拒绝。product21 起曾使用外置 `vscode/vscode.xenon.json` 指向原发行目录，预声明原包需要的协议权限；发行包内文件不改动。product21–27 曾接受两个可选字段后禁用相应能力，其中 Service Worker 的降级已在本轮复核撤回，不能按历史描述符推断当前支持情况。实际 URL、standard、secure、fetch、CORS 和 CSP 权限始终按启动快照校验。

`codeCache` 属于优化，可以降级为不缓存。`allowServiceWorkers` 支撑真实资源加载，是未实现的功能权限；这两个字段都不能替代 URL 和来源语义。

外置声明不等于纯目录自动加载。完整无配置方案需在 Chromium 冻结协议表前运行 main，在 ready 屏障协调声明并下发子进程；已运行的共享宿主还涉及独立宿主或重启。预执行 main 再重放会重复文件、网络和原生模块副作用；运行期解锁注册表或应用名特判也不成立。本轮未实施这项启动架构重构。

后续实测按首个真实阻断继续推进，未把窗口存在或容器正常退出计为工作台成功：

| 版本 | 实际结果与修复方向 |
| --- | --- |
| product21 | 越过协议注册，缺少 `URL.canParse`。之后接入通用 GURL 校验；129 组 Node 对照中 128 组一致，空格主机名一项差异保留记录。 |
| product22 | `bindings` 模块查找失败。原包通过 `Module._resolveLookupPaths` 插入归档路径，适配层未使用 live hook；之后接通通用搜索钩子，没有自动搜索 ASAR 或应用特判。 |
| product23–25 | 依次遇到默认协议关联、标题栏装饰、离屏状态和 `app.setProxy` 接口缺口；非核心能力采用明确降级，见 [Electron可选能力降级.md](Electron可选能力降级.md)。 |
| product26 | 最后一次运行：preload `additionalArguments` 已正确进入隔离的 `process.argv`，主初始化在注册 `onBeforeSendHeaders` 时失败。此后通用发送头支持由独立 HTTP fixture 验收，未再运行 VS Code。 |

这些故障说明当前仍有通用 Electron/Node 契约缺口，并不能证明必须增加 VS Code 专属 Chromium 补丁。网络来源身份、文件读写和权限不以占位成功绕过。

product21/22 验证脚本最初仅向宿主传入 `--user-data-dir`，未向应用参数再次传入，导致默认 `C:/Users/Administrator/AppData/Roaming/Code` 生成 124 个文件，共 1,083,293 字节。现有记录显示目录与文件创建于测试启动后，但没有事前快照，不能绝对证明未改历史数据；保留原目录及审计清单，不删除。product23 起在应用 `--` 后显式传独立 profile，每轮核查默认目录内容、时间与文件清单；至 product26 均未再次变化。证据为 `vscode/profile-isolation-audit22.json` 和各轮 `default-profile-integrity.json`。

证据：`vscode/diagnostics-next-compat/privileged-schemes-review-product17.md`。原包 27 个物理 `.node` 均有 N-API 注册入口，未发现传统 Node/V8 C++ ABI 导入证据；符号存在不代表异步、线程和退出全部可用，不能凭目录或版本归因于 ABI。

## 原包完整性与复跑

product28 全部应用验收结束后，只读逐文件对照官方 ZIP：draw.io 72、MarkText 108、Motrix 228、VS Code 3278 个文件字节一致，均无新增文件。证据为各项目 `integrity-product28-final.json`。其中 VS Code 只核对原包未改动，没有启动验证。

product30 再次核对上述四份 ZIP，文件数量与内容全部一致，无新增文件；VS Code 仍仅做
只读校验。draw.io、MarkText、Motrix 的原发行目录 → 本地忽略的 resources → out
目录与每个文件的 SHA-256 均一致，证据为 `release-layout-integrity-product30.json`
和各项目的 `integrity-product30-final.json`。

```powershell
node xenon_overlay/tools/validate_ipc_compat.cjs
out/Release_64/ipc_main_container_unittests.exe --test-launcher-jobs=4
node F:/electron-compat-audit/20261111/probe-drawio-core.cjs <新后缀>
node F:/electron-compat-audit/20261111/marktext/run-core-verification.cjs <新绝对输出目录> xenon
node F:/electron-compat-audit/20261111/probe-th-retirement-ports.cjs <新后缀>
node F:/electron-compat-audit/20261111/probe-ple-core.cjs <新后缀>
```

输出目录须独立；真实输入测试独占前台。构建前退出所属测试宿主，避免占用链接产物。历史失败和中间诊断保留，不混入最终通过计数。
