# Electron / Node 适配层：性能优化与模块约定

## 本轮实现

- 两端 Buffer 优先使用 V8 原生 Uint8Array Base64 API；旧 JS host 回退到有界分块编码，避免逐字节拼接长字符串。支持 Base64URL、padding、子视图和 Node 的宽松输入规则。
- Main 已实现的文件 callback/Promise API 把 I/O 投递到 MayBlock 工作线程，在原 V8 序列完成。文件字节不再通过 UTF-8 或 Base64 中转；写入提交时复制调用者的数据，避免异步读取可变内存。
- Addon 同步调用返回 pending Promise 时，以一次性 token 续接原调用。不会为了切换成异步而重新执行 native 函数；token 按连接隔离，有数量上限、过期与断开清理。
- CommonJS 按模块名称、请求路径和父模块查找；移除按文件名替换模块与缺失 addon 的同名回退。同步 addon 加载直接返回导出，不合成 `default`、`__esModule`、`__xenonReady`，也不隐藏真实 `then` 导出。
- 原生模块通过真实属性描述生成导出包装，保留根类型、标量、数组、函数静态成员和嵌套对象。复合属性首次读取时展开并缓存；加载和枚举不会执行 getter，显式读取才会调用 getter。
- Renderer 原生实例按页面归属管理，同步调用和 callback 调用共享归属。实例返回、参数和释放都携带服务生成的随机 token，防止服务重启后数字 ID 重用造成误调用或误释放；页面或直接通道关闭时清理实例、Promise 和回调。
- 原生包装器通过弱终结器请求释放不再被使用的实例，释放请求不建立新连接。方法调用保留必要的句柄直到完成；GC 释放只是尽力执行，页面关闭清理负责最终兜底。
- 原生方法不再按 `waitLoadFinish` 名称伪造超时成功，不再吞掉调用错误或按错误文本自动重建、重试有副作用的调用。失效对象明确失败，由应用决定恢复方式。
- HTTP 取消后停止排队的 JS 数据和结束事件，并清理上传分片、超时器；本地 socket 销毁后停止尚未交付的连接事件和回调。
- 修复 TH/PL-E 启动回归：`tls`、`http2`、`child_process`、`zlib`、`readline` 的导入必须保留各自模块身份，不能因部分操作尚未实现而一概在 require 时退出。TLS 不再借用 net，HTTP/2 不再借用 HTTP/1；readline 提供真实非 TTY 行读取；zlib 接入真实 Chromium 压缩库。
- 托管页面的调用栈保留应用文件路径：Browser 将已声明的 renderer URL 映射传到页面，`bindings` 等依赖从正确的包目录解析原生模块。PL-E 明确映射到原始 `resources/app/out/main-renderer`，不会通过搜索同名 addon 或替换 SQLite 实现绕过路径错误。
- PL-E 同步流程校验 preload 是否为可执行源码；厂商加密产物只通过精确匹配的原厂归档和工具恢复，原始密文字节不匹配即失败。详见 `tools/normalize_player_preloads.cjs` 和 PL-E 接入文档。
- Main 的 HTTP/HTTPS 和 fetch 接入实际 Chromium URLLoaderFactory，网络错误、状态码、上传字节和取消结果来自真实请求；支持 TH 打包 Axios 所需的 fetch 能力，移除其对占位 HTTP 成功响应的依赖。
- 两端 `fs.createWriteStream` 为日志库提供真实异步写入，按提交顺序保存字节快照，结束或销毁时等待在途 I/O。原生 append 可以创建缺失文件；已用应用实际的 file-stream-rotator 验证文件创建、追加和轮转。修复 TH Common SDK 创建日志流时失败、账号对象始终未初始化的问题。
- 原生回调传递真实接收者，同一个原生对象在同一 owner 下复用 ID/token，renderer 弱缓存复用对应包装器。带 callback 的原生调用保留立即返回值；不会仅因参数中有函数而改成 Promise，以支持 SQLite 的 `run(...).finalize()` 等接口。
- 原生类在构造前可接收 JS 新增的原型方法快照，例如 SQLite 复制的 EventEmitter 方法。每次构造使用独立原型，保留原生实例布局与原生方法，避免跨页面污染；EventEmitter 支持未调用其构造器的对象，且 `on/addListener`、`off/removeListener` 使用同函数别名，避免包装代码回调基类时递归。
- 原生类识别读取完整自有属性描述符，包含不可枚举方法且不执行 getter。导出遍历以实际内建原型为边界，只跳过描述符确认的构造器回指；真实声明的 `bind`、`call`、`apply`、`toString`、`valueOf`、`name`、`length` 必须保留，不能按名称统一过滤。
- N-API 回调返回时原样传播待处理异常并清除宿主保存的异常，防止污染下一次调用；原生代码已清除的异常仍视为已处理。`napi_create_reference` 拒绝原始值；`napi_instanceof` 遵循 V8 的实例判断，普通原始值返回 false，同时保留自定义 `Symbol.hasInstance` 与异常语义。

## require 的约定

| 请求 | 解析行为 |
|---|---|
| `require('electron')` | Electron 兼容 API |
| `require('fs')` / `require('node:fs')` | 同一个已实现的文件模块对象 |
| `require('./module')` | 相对于发起请求的模块文件，依次尝试文件及 `.js/.json/.node` |
| `require('./explicit.cjs')` | 加载明确指定的 CommonJS 文件；不会隐式追加 `.cjs` |
| `require('package')` / scoped package | 从父模块目录逐层查找 `node_modules`，受应用的既有访问根限制 |
| 目录模块 | 使用 `package.json.main` 或 `index.js/index.json/index.node` |
| `require.resolve(request)` | 返回解析结果；不执行模块，不把原请求字符串当成解析成功 |
| `require('sqlite3')` | 加载应用实际安装的 sqlite3 包 |
| `require('/.../node_sqlite3.node')` | 加载这个真实 addon；没有按文件名触发的 SQLite 替换 |
| Renderer `require('xenon:sqlite3')` | 显式使用 Xenon 内置 SQLite 桥 |

内建模块名精确匹配。`FS` 不等于 `fs`，`node:electron` 不是 Node 内建模块。

CommonJS 模块在执行前入缓存以支持循环依赖，缓存命中读取当前 `module.exports`。模块执行失败不污染缓存；抛出的原异常继续传递，避免把业务错误改成 `MODULE_NOT_FOUND`。Renderer 的 `require.cache` 可使 JS/JSON/原生导出包装失效；这不承诺卸载 Utility 中已加载的原生 DLL。

### 旧接入的迁移

- 依赖内置 SQLite 桥的 renderer 应明确使用 `xenon:sqlite3`；需要 npm sqlite3 的应用应安装正确包及其真实 addon。
- 原先写错或依赖同名搜索的 addon 路径必须改成真实路径。
- 把文件页面托管为 `chrome://` 时，接入配置必须声明 renderer 的源目录与 URL 对应关系；不能把 WebUI URL 直接作为磁盘路径，也不能用主进程目录代替另一个 renderer 包的目录。
- 原生模块的导出以 addon 实际声明为准。不能依赖适配器生成的 `default`、`__esModule` 或 `__xenonReady`。

## 明确的兼容边界

这仍是基于 Chromium/V8 的兼容层，没有接入完整 Node runtime。

- `tls`、`http2`、`child_process` 可正常导入各自独立模块对象。TLS 连接/服务器/证书、HTTP/2 会话/服务器、子进程启动等尚未实现的操作在调用时明确 `ERR_NOT_SUPPORTED`；不能使用 net 冒充 TLS、HTTP/1 冒充 HTTP/2，也不能返回假进程或假成功退出码。
- `zlib` 支持 gzip/gunzip、deflate/inflate、raw 和 unzip 的一次性同步/回调接口，回调版本在工作线程计算；默认及最大输出上限为 64 MiB。压缩流、dictionary、partial flush 等未实现能力在调用时报错。
- `readline` 支持非 TTY 输入流的 UTF-8/CRLF 行读取、结束清理、暂停/恢复和基本 question；TTY 编辑仍明确不支持。
- Main HTTP/fetch 为缓冲实现，单次响应上限 5 MiB、上传上限 32 MiB；HTTP server 与流式上传仍未实现。fetch 支持跟随或拒绝重定向；Node HTTP 遇到重定向明确失败。Chromium 已解压的 HTTP 响应去掉原压缩编码和长度头，避免调用方二次解压；fetch 保留浏览器 fetch 的原响应头语义。取消会终止 main 对应的真实 loader。
- 尚未实现 package `exports/imports` 和完整 ESM 加载。这些路径明确拒绝，避免绕过包的导出限制。
- `fs.createReadStream` 已支持真实文件内容、分块交付、范围、UTF-8、暂停/恢复和取消；当前通过整文件工作线程接口读入后分块，内存并非仅占 highWaterMark。`fs.createWriteStream` 支持日志所需的 `w/a`、编码、排队写入、背压及 finish/error/close 时序；每笔按路径异步写入，不持有 Node 文件描述符。两者均不伪造 fd 或 open 事件；写入流的 fd/start/custom fs/特殊 mode，以及 watch、chmod/chown 等未实现能力仍明确失败。整文件/目录接口也尚未覆盖 Node 的全部 flags、符号链接和 OS errno 细节。
- 其他内建模块仍属于按需实现的子集；本轮的模块身份与解析改进不等于所有 Node API 已完整实现。
- 原生导出仍经过跨 isolate 包装：普通数据属性是快照，尚不保留完整属性描述符、共享/循环对象身份和原生异常对象身份。原型包装也不等于原生对象本身；Symbol、external、原生 accessor 写入和需要通过点号路径寻址的特殊属性名仍有协议限制。无法正确寻址的复合属性明确报错。Getter 返回对象或函数暂报 `ERR_NOT_SUPPORTED`，避免后续调用通过路径重新执行 getter、产生重复副作用；返回标量的 getter 可正常使用。
- 构造时桥接的是 JS 新增原型函数；原生已有方法保留在 Utility，renderer 的包装方法可以继续调用保存的原生方法。原型后续变更不会自动同步到已构造实例。跨进程 callback 以异步事件交付，尚不支持原生代码同步取得 renderer JS 回调的返回值。
- 托管 native 模块执行器当前接受 object/function 根导出，拒绝原始值根导出；JS 包装器的原始值根类型用例不表示托管路径已经支持这类 addon。普通 CommonJS 的 `module.exports` 原始值不受此限制。
- libuv 10 ms 轮询、流式 HTTP/DataPipe 和公共 IPC 大包共享内存仍需后续专项优化。Renderer HTTP 当前只能取消 JS 交付，尚未把取消传到 Browser 网络请求。

## 整体审查与维护规范

整体结构已经按 Browser 编排、Utility 执行和 Renderer 接入分工。主要改进空间在跨层的行为契约、重复工作和资源回收；本轮沿现有架构修复，不引入另一套运行时。

| 维度 | 审查发现 | 本轮处理 |
|---|---|---|
| 模块解析 | main 热 require 仍探测磁盘，两端冷加载重复解析；renderer 删除缓存后可能沿用旧真实路径 | 只为已加载模块保留成功解析映射，热路径直接取当前 exports；失效后重新验证真实路径 |
| 原生加载 | 先生成旧导出树再丢弃，并额外同步读取根描述 | 加载回复直接携带精确根描述；旧树按需构建，避免无用遍历与克隆 |
| Buffer | 两端复制/共享语义不一致，普通 Uint8Array 被当作 Buffer | 统一 from 的复制与 ArrayBuffer 共享规则、slice 共享语义和 isBuffer 判断 |
| 能力边界 | stream 默认操作丢弃数据但报成功，main HTTP 伪造 200 | 未实现操作明确 ERR_NOT_SUPPORTED；main 与 renderer HTTP 共用真实网络桥，main 支持 fetch |
| 加密 | main 哈希/HMAC 使用占位算法，随机数使用 Math.random；renderer 无随机源时返回零 | main 接 BoringSSL 摘要/HMAC 和安全随机源；renderer 严格检查随机源并分块填充 |
| 生命周期 | 失联 client 的 native callback 缓存与转换工作继续存在，GC 两阶段清理可能交错 | 断开时清理、按 generation 隔离 ID 重用；GC 后通过 WeakPtr 投递收集任务 |
| 上传数据 | renderer HTTP write 未保证二进制快照，end 重建包装 | write 提交时保存字节快照，end 复用已保存 chunk，编码错误明确失败 |
| 原生实例 | 返回对象被执行器长期强引用，服务重启后 ID 可能复用 | 页面 owner、随机 token、GC 释放及断线批量清理；迟到结果不得重建已关闭 owner 的资源 |
| 调用语义 | 特定方法超时伪造成功、同步错误被吞掉、失败触发自动重建重试 | 保留真实完成和失败，由应用明确选择重试策略 |
| 崩溃恢复 | 旧页面的 IPC 在 Utility 断连后重新执行 main，不断创建新窗口 | 首次启动正常进行；断连后旧页面的 IPC 明确失败，只有 Browser 的显式启动入口允许重试 |
| 本地管道 | main 将连接请求广播给所有页面，无监听者的拒绝回复会抢在正确连接之前到达 | main 接入真实命名管道桥，事件异步回投指定端点，二进制统一使用 Base64 字节协议 |
| 取消时序 | HTTP 和本地 socket 的微任务在取消后继续交付事件 | 在排队事件交付边界检查关闭状态，并及时清理保留的数据和回调 |

可复现的调用次数：仪表化 bridge 下首次 `.js` 加载文件 IPC **6 → 4**；重复 require 两版均为 **0 IPC**。原生模块加载与根描述的同步往返 **2 → 1**。这些是特定调用链的工作量变化，不是整机吞吐或完整应用启动时间。解析基准位于 `resources/ipc/renderer_module_resolution_benchmark.cjs`；计时会受宿主和机器负载影响，优先比较 IPC 次数。

### 后续改动应遵守的规则

1. **模块身份**：只能返回请求模块的实际实现；Xenon 专用扩展用显式名称。不按业务包名、文件 basename 或错误文本替换实现。
2. **能力声明**：模块可导入与具体操作可执行分别判断。已有独立模块形状和部分能力时保留导入，未实现操作在调用时明确失败；不得返回别的模块、空成功数据、成功 callback、假句柄或假响应。main 与 renderer 支持范围不同的地方应记录清楚。
3. **二进制所有权**：区分共享视图和复制。异步提交需要快照的输入，在提交点复制一次；不得把 `Buffer.from`、`slice` 都当作通用复制工具。
4. **回调与重试**：先验证 callback 再提交工作；callback 按接口约定异步且只调用一次。不要通过重新执行可能有副作用的函数来切换同步/异步调用。
5. **缓存与生命周期**：模块记录是导出身份的来源，辅助解析缓存依附于有效模块。失效和执行失败后重新校验路径；断开 client 时同时清理实例、Promise、回调与排队数据；弱任务和释放请求应能抵抗 ID 重用和宿主销毁。跨异步边界保存发起调用时的归属，完成时再次检查，不能为迟到结果重新创建归属。
6. **测试与性能结论**：用同一行为表检查两端，关键字节/算法结果与本机 Node 比较；涉及 native 生命周期必须有真实 V8/N-API 回归。修改模块导入和已有能力时必须验证实际托管应用的启动，不能把接口单测通过当成 TH/PL-E 可以打开。报告调用次数、复制次数或计时的测量范围，不能从转换函数测速推断整体应用收益。
7. **原生元数据**：通过属性描述符区分自有导出与内建继承属性；不得因属性与 JavaScript 内建名称相同就删除。类识别必须覆盖不可枚举方法，构造包装必须保留调用方的 `new.target.prototype`，元数据读取不得触发 getter。
8. **N-API 行为**：状态码、pending exception、引用类型和接收者均属于接口契约。错误必须在当前调用边界交付，不得残留并污染后续调用；例如 `napi_instanceof(undefined, constructor)` 不能仅因左值是原始值就返回类型错误。使用真实 V8/N-API 用例验证，包括自定义 `Symbol.hasInstance` 和异常传播。

### 仍需独立设计的部分

- 保留 10 ms pump：它还推进 libuv、V8 microtask 和 Win32 消息。只有实现完整的外部唤醒机制后，才能安全减少空闲轮询。
- 通用 stream 队列、流式 HTTP/DataPipe、公共 IPC 大包共享内存尚未完成。原生实例桥会按 owner 和模块复用同一 V8 对象的实例身份；普通数据对象仍按值转换，不承诺共享引用或循环图。不会主动替 addon 调用析构方法或业务 `dispose`；释放的是桥保存的 V8 引用，实际资源回收遵循 addon 自身实现。main/旧 WebUI 使用的 owner 0 不随 renderer 断开释放。
- 断开清理释放的是执行器回调缓存，并阻止后续无效序列化。gin 的 FunctionTemplate 绑定函数仍可能保持到 context 销毁，不能把该改动表述为所有 V8 函数立即回收。GC 回归在可回收的临时 context 中验证真实 weak callback 与销毁/ID 重用交错。
- main 真实摘要支持 MD5、SHA-1、SHA-224/256/384/512 与对应 HMAC，按二进制输入计算；字符串/结果编码支持 UTF-8、hex、Base64/Base64URL，其他编码明确拒绝。这不是完整 Node crypto API。摘要仍累积输入后计算，回调随机接口只保证异步回调时序，不承诺工作线程计算。
- 两端独立 bootstrap 仍有重复代码。先用统一契约用例限制漂移；后续拆公共源码时，应同时规划资源打包和启动成本，不能只移动文件。

## 验证方式

### 当前结果（2026-09-16）

本节区分最新 JS/生产构建与此前 Native 专项验证。初次 TH 登录和崩溃恢复的汇总保留在 `out/th-ple-startup-check/th-ple-final-acceptance-20260916.json`；PL-E 7.1.35.173 与后续窗口修复的实际记录见下表。

| 验证项 | 结果与范围 | 记录 |
|---|---|---|
| JS 契约回归 | **275/275，零跳过**；包含 TH 实际 file-stream-rotator、PL-E 7.1.35.173 实际消费代码，以及新增背景色、拖动和标题契约 | `out/ple-update-7135-20260916/title-js-tests.log` |
| 常规 Native 回归（历史） | **134/134**；原生同名方法、真实 Mojo/OS 管道字节传输与异步拒绝连接的专项结果；后续窗口修复未重新执行此套件 | `out/Release_64/codex-native-method-names-after-20260916.log/.json` |
| N-API host 回归（历史） | **24/24**；异常边界、引用类型、原始值 instanceof、自定义实例判断与异常传播的专项结果；后续窗口修复未重新执行此套件 | `out/Release_64/codex-instanceof-after-20260916.log` |
| 完整生产构建 | **239/239，退出码 0**；最新目标 `chrome` 完成链接，生成 `xlb153.exe` 和 `xlbrowser.dll` | `out/ple-update-7135-20260916/title-production-build.log` |
| PL-E 7.1.35.173 发布与播放 | 两处 242 个发布文件哈希一致；原生播放探针通过，最近拖动复验时进度 `229 → 1075 ms`。此前实际 SQLite 包的建表、写入、查询和关闭也已通过 | `out/ple-update-7135-20260916/independent-release-verification-20260916.json`、`drag-revalidation-playback.jsonl` |
| PL-E 菜单背景 | 用户确认“界面正常了”；一张 CDP 截图确认独立菜单白底消失，三个菜单未完成逐一自动截图 | `out/ple-update-7135-20260916/menu-quality-background.png`；用户实际交互验收 |
| PL-E 窗口拖动 | 用户确认“可以拖动了”；可信鼠标事件与窗口坐标变化相符，捕获 80 条鼠标事件及 35 次位置变化。范围限于当前桌面环境 | `out/ple-update-7135-20260916/drag-revalidation-result.json` |
| TH 主窗口标题 | 原生窗口与页面标题均为“迅雷”；临时修改页面标题时同一原生窗口同步更新，之后已恢复并清理探针；SDK ready、账号初始化状态 2 | `out/ple-update-7135-20260916/title-native-validation.json`、`title-th-acceptance.json` |
| Utility 崩溃恢复 | 在独立配置 `profile7` 终止已确认归属的 PL-E Utility 后，旧 PL-E 窗口关闭，TH 保持运行；观察约 35 秒无自动重启，显式启动只创建一个新 PL-E 窗口 | `out/th-ple-startup-check/guard-profile7-result.json` |
| TH 登录二维码 | **用户实际扫码登录通过**，反馈“可以了，已经能扫码登录了”；9222 直接检查得到 `sdkInitReady === true`、`accountInitState === 2` | `out/th-ple-startup-check/th-acceptance9222-bind.json`；扫码结果来自用户确认 |
| TH 实际 SQLite 查询 | 通过实际 SDK 的 Database 构造器创建内存库，`prepare`/`get` 返回 `error: null`、`sameStatement: true`、`value: 1` | 验证脚本 `out/th-ple-startup-check/probe_th_sqlite_prepare.js` |

JS 运行时设置 `XENON_TEST_FILE_STREAM_ROTATOR` 指向 TH 实际依赖；当前 PL-E 7.1.35.173 的消费代码来自 `frontend/static/js/58.js`，真实消费用例参与执行。历史常规 Native 数量不包含两项商业 addon 测试宿主 smoke；宿主与实际浏览器不同，不能替代业务验收。服务断连重启入口的浏览器单测对象已编译，其所属大测试目标未运行；上述崩溃恢复结论来自实际浏览器操作。

TH 扫码恢复阶段的 9222 复验使用用户原配置。该次日志检查为启动 1 次、断连 0 次、fatal 0 次，记录于 `out/th-ple-startup-check/chrome_debug9222-bind.log`。当时另有 1 条非致命 N-API 诊断：`napi_get_named_property` 读取 `links` 时接收对象为 null/undefined，返回 status 10（TypeError）；未发现其与该次验收失败关联的证据。用户扫码后登录窗口已关闭，因此该次没有记录二维码图片尺寸；端到端扫码成功由用户实际操作确认，不将其表述为自动化完成登录。这是登录修复阶段的记录，不是最新标题构建的日志汇总。

### 实测回归与修复依据

早期优化和数轮接口测试通过后，TH/PL-E 的实际启动仍暴露适配问题，因此此前单测数量不作为最终业务验收。已修复的主要调用链为：

- **应用加载**：恢复独立内建模块身份、托管页面的实际包路径、PL-E preload 源码和 main 真实网络请求。
- **TH 日志与 SQLite 初始化**：提供真实 WriteStream；通过完整属性描述符识别不可枚举原生类方法，保留 `new.target.prototype` 与 JS 补充的 EventEmitter 方法；修复 callback 接收者、同步返回值和待处理异常的交付边界。实际构造已包含 JS `emit`。
- **TH 登录弹窗通信**：main 连接改用真实命名管道，避免无关页面的拒绝回复抢先断开正确连接；实际冷启动已能正常打开登录表单。
- **PL-E 可选 callback**：`napi_create_reference` 拒绝 `undefined` 等原始值，避免 addon 把缺省 callback 视为可调用引用。用户随后确认可播放。
- **TH 账号事务**：`napi_instanceof` 不再错误拒绝原始值。两个回归用例修复前失败、修复后通过；9222 的实际内存 SQLite 事务已验证 `BEGIN TRANSACTION`/`ROLLBACK` 携带 `undefined` 参数正常完成。
- **TH 账号凭据查询**：真实 `Statement.prototype.bind` 曾被名称黑名单删除，导致 sqlite3.verbose 包装器调用 `undefined.apply`。移除名称黑名单，按内建原型边界和构造器回指描述符过滤；新增真实 N-API 类用例在修复前缺失 bind、修复后通过。新版浏览器的实际 SDK 初始化与 SQLite 查询通过，用户确认扫码登录成功。
- **PL-E 菜单背景**：将 BrowserWindow 背景色传到 WebContents、页面底色和视图；底层视图颜色按其透明/不透明约束处理，修复独立菜单的白底并避免半透明值触发 `CHECK`。
- **PL-E 拖动**：补齐两端有符号 Buffer 读取，将固定 screen 返回值改为 Browser UI 线程的真实查询，支持新版基于 `WM_MOUSEMOVE` 的拖动链路。用户操作与可信 DOM 事件确认实际窗口移动。
- **TH 窗口标题**：修正真实 delegate 的标题 setter，并将页面标题变化送回 main，按可取消的 BrowserWindow 事件、`setTitle`、WebContents 事件顺序处理；默认标题为 `Electron`，显式空标题仍保留。实机验证限于 TH 主窗口及其动态标题同步。

`Common`、`Account`、`ProviderAccount` 字段存在只说明对象已创建，不能证明 SDK 已初始化。TH 业务验收必须检查 `sdkInitReady`，并验证二维码图片实际加载可见，或由用户完成真实扫码登录。自动化检查图片时使用 `complete`、`naturalWidth > 0` 和可见状态；测试记录不保存二维码内容或令牌。

JS 回归使用完整生产 bootstrap，仅替换 native transport；覆盖二进制/子视图、模块解析和身份、缓存及异常、Promise 续接、取消时序和未实现 API 的失败行为。生命周期用例启用真实 GC，验证提取的方法、调用接收者及嵌套原生参数在调用期间仍存活，完成和失败后可回收。

```powershell
node --js-base-64 --expose-gc --test xenon_overlay/resources/ipc/*_unittest.cjs
```

统一入口会自动发现全部 JS 契约测试，并可接着运行已构建的 native 回归：

```powershell
node xenon_overlay/tools/validate_ipc_compat.cjs --native out/Release_64/ipc_main_container_unittests.exe
```

Node v24 的 Base64 feature 默认关闭，测试命令显式启用；Chromium 142 中该 V8 feature 默认开启。用例同时覆盖没有原生 API 的回退路径。

服务生命周期回归使用真实 Mojo Remote/ReceiverSet 和 `test_addon.node`，覆盖同步与 callback 通路交叉调用、首次绑定、重新绑定、页面移除、跨页面隔离、GC token 校验、服务重启后数字 ID 重用、异步冷加载期间关闭 owner。它运行在单测进程中，不等同于完整浏览器跨进程启动验收。

Native 回归目标：`ipc_main_container_unittests`。生产浏览器目标为 `chrome`，本项目产物名为 `xlb153.exe` 和 `xlbrowser.dll`。完整链接及真实窗口验证是独立验收项，不能用生产对象编译或测试宿主运行代替。

本机验证记录位于 `out/Release_64/codex-js-round3-final-tests-20260916.log`、`codex-native-round3-final-tests-20260916.log/.json`、`codex-native-round3-build-20260916.log`、`codex-native-round3-integration-build-20260916.log` 和 `codex-native-round3-final-build-20260916.log`。

### Buffer 性能复现

保存优化前的 `xenon_ipc_renderer_bootstrap.js`，运行：

```powershell
node --js-base-64 xenon_overlay/tools/benchmark_ipc_buffer.cjs baseline.js results.json
```

脚本在同一 Node 进程中加载优化前后两份完整 bootstrap，采用无全局代理的 V8 context，预热后交替测七轮。输出每轮结果、中位数和环境信息，并校验全部字节。

该基准仅衡量 Buffer 转换函数，没有测量真实 Chromium IPC、磁盘、网络或 addon 吞吐。Node 的实验性 Base64 实现与 Chromium 142 的正式实现也可能不同。

首轮 Buffer 转换基准（2026-09-16，本机 i9-13900HX、Node v24.19.0，`--js-base-64`）同进程前后对比：

| 操作 | 数据量 | 优化前 | 优化后 | 加速 |
|---|---:|---:|---:|---:|
| Base64 编码 | 64 KiB | 0.435 ms | 0.00567 ms | 76.8× |
| Base64 编码 | 1 MiB | 21.023 ms | 0.2145 ms | 98.0× |
| Base64 解码 | 64 KiB | 0.0910 ms | 0.0216 ms | 4.20× |
| Base64 解码 | 1 MiB | 1.742 ms | 0.4584 ms | 3.80× |

这是转换函数的测量，不能表述成整个应用提高了相同比例。Native 测试另行验证了 Chromium 实际 V8 中的 Base64/子视图行为。
