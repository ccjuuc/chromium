# Electron / Node 适配层：性能优化与模块约定

## 此前优化实现

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
- `fs.createReadStream` 已支持真实文件内容、分块交付、范围、UTF-8、暂停/恢复和停止交付；工作线程按 start/end 读取普通文件及 ASAR，仅将选中范围返回 JS，再按 highWaterMark 交付。仍缓存整个范围；无 end 时读到 EOF，destroy 不取消已提交的原生读取。`fs.createWriteStream` 支持日志所需的 `w/a`、编码、排队写入、背压及 finish/error/close 时序；每笔按路径异步写入，不持有 Node 文件描述符。两者均不伪造 fd 或 open 事件；写入流的 fd/start/custom fs/特殊 mode，以及 watch、chmod/chown 等未实现能力仍明确失败。整文件/目录接口也尚未覆盖 Node 的全部 flags、符号链接和 OS errno 细节。
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
- 两端 bootstrap 已按职责拆成构建时组合的源码片段，zlib/readline 共用实现；GRIT 和测试统一消费生成结果。仍共享各自入口闭包，Buffer/path/URL 等有差异的实现没有机械合并。后续收敛行为或引入独立工厂时，仍需用两端契约用例限制漂移，保留初始化次序和模块身份。组织方式见 `resources/ipc/README.md`。

## Bootstrap 审查修复（2026-09-16）

本轮针对 `xenon_ipc_main_bootstrap.js` 和 `xenon_ipc_renderer_bootstrap.js` 的行为契约修复，不将此前 TH/PL-E 的业务验收或 275 项 JS 回归视为所有 API 已符合 Electron/Node 规范。

### 页面隔离与模块身份

- Guest 页面在 `nodeIntegration: false` 时，除清理 `require`、`process` 和原生传输入口外，同时撤销自动注入的 `globalThis.electron`，堵住通过 `electron.ipcRenderer` 绕过页面隔离的路径。
- 可信 preload 可以保留其 Node/IPC 闭包，但页面只取得 preload 明确暴露的 API。显式启用 Node integration 的页面仍保留其配置允许的能力。
- Renderer 的 `require('process')`、`require('node:process')` 与全局 `process` 使用同一模块对象；模块导入仍按真实名称和身份处理，不因个别方法未实现而替换整个模块。

### BrowserWindow 与能力边界

- 移除 BrowserWindow 为任意未知属性生成函数的 Proxy。不存在的属性保持 `undefined`，包括 `then`；`Promise.resolve(window)` 不再把窗口误认成永不完成的 thenable。
- TH 依赖的 `setParentWindow`、`moveTop` 和 `isAlwaysOnTop` 使用 Chromium Widget 的跨平台窗口操作或查询，`getFocusedWindow` 根据真实焦点状态查找。父窗口设置先校验对象、销毁状态和循环关系，原生操作成功后才更新 JS 关联；相同父窗口的重复设置保持幂等。普通透明弹窗不会因为设置 owner 就启用播放器覆层的尺寸联动。
- 新增窗口逻辑不直接操作 HWND。任务栏 `setProgressBar` 没有接入通用平台实现时明确 `ERR_NOT_SUPPORTED`，不伪造成功，也不把 Chrome 下载管理器的全局进度冒充指定窗口进度。
- Session 的 cookie、代理、缓存清理、权限回调、protocol/webRequest 等尚未实现的方法明确报 `ERR_NOT_SUPPORTED`，不再返回空数据、`DIRECT` 或已完成 Promise 冒充结果。保留 Session 对象和模块可导入性，失败发生在不支持的操作被调用时。
- 全局快捷键注册、未实现的保存/消息对话框等同样明确失败；未显示对话框时不会再返回 `response: 0`。已有真实打开文件对话框继续传递用户选择、取消和原生错误；缺少宿主能力不再等同于用户取消。

### 剪贴板与 shell 桥

- Main 与 renderer 通过同一 Browser 原生桥调用 `clipboard.readText/writeText/readHTML/writeHTML/clear`，同步 API 保留同步结果和错误语义。
- `shell.openExternal/openPath/showItemInFolder` 通过统一接口分发到独立平台后端；本轮实现 Windows 后端，其他平台明确 `ERR_NOT_SUPPORTED`，不宣称已完成 macOS/Linux 的 shell 功能。Windows 可能阻塞的 shell 工作投递到 COM STA 工作线程，回复回到 Browser UI 序列。参数校验和平台失败向调用方传递，`openPath` 的失败按 Electron 契约返回非空错误字符串。
- 自动测试使用内存 `TestClipboard`，不覆盖用户的系统剪贴板。shell 测试覆盖参数拒绝和不存在路径；**成功启动外部程序、浏览器或文件管理器的分支未做自动实际启动验证**。JS 传输测试通过不代表这些系统交互已完成实机验收。

### 异步上下文

**当前生产 V8 未启用 JavaScript Promise hooks，因此本轮明确不支持激活 AsyncLocalStorage 异步上下文。** `require('async_hooks')`、构造 AsyncLocalStorage、`getStore` 和 `disable` 仍可使用；`run`、`enterWith`、snapshot、bind 和 AsyncResource 构造在需要原生能力时抛出 `ERR_NOT_SUPPORTED`，不会返回只在同步代码中有效的假上下文。

V8 在编译时关闭该能力的情况下调用 `SetPromiseHooks` 会直接终止进程。两端原生入口均用 `V8_ENABLE_JAVASCRIPT_PROMISE_HOOKS` 编译条件保护，缺少能力时在进入该 API 前返回明确错误；本轮未修改全局 V8 编译选项，也未替换 Blink 的 isolate hooks。以下传播实现及行为测试为支持该能力的构建保留，不能据此宣称当前生产构建已经支持 ALS：

- AsyncLocalStorage 使用 `v8::Context::SetPromiseHooks` 跟踪 Promise 与原生 `await`，在 reaction 前后恢复对应上下文；不通过覆盖 `Promise.then` 模拟，也不占用 Blink 已用于任务归属的 isolate continuation data。
- 定时器、`queueMicrotask`、`process.nextTick` 保留注册时的上下文。独立 IPC 消息以新的根作用域执行，防止一次 `enterWith` 污染后续无关消息；通过 IPC 返回的 net socket/server 事件和 NAPI 回调则恢复所属资源或注册时的上下文。应用手工调用 EventEmitter 的 `emit` 仍使用当前调用者的作用域。
- Promise 上下文保存在 WeakMap 中，实例使用独立键，不再用全局 Set 永久保留 AsyncLocalStorage 实例。`disable` 使旧异步帧中的 store 失效；snapshot、bind、嵌套作用域和异常退出均恢复调用前的状态。
- AsyncResource 的作用域执行和 bind 使用实际捕获的上下文；尚未实现的 `createHook`、异步资源 ID/生命周期查询和 `emitDestroy` 明确 `ERR_NOT_SUPPORTED`。原生 Promise hooks 缺失或安装失败时，首次需要异步上下文的操作明确失败，不降级成仅同步保存 store 的实现。新 isolate 重建时重新初始化 hooks 安装状态。

### 本轮范围与验证

两个 bootstrap 本轮**未整体拆分为公共源码模块**，仍存在需要后续处理的重复实现。以上是明确问题及关联调用链的修复，不代表完整 Electron runtime、Node runtime 或全部 API 已受支持；其他限制仍以本文“明确的兼容边界”为准。

| 本轮验证项 | 状态与范围 | 记录 |
|---|---|---|
| JS 契约回归 | **311/311，零跳过**；包括 Guest 隔离、模块身份、窗口属性、明确失败、原生桥传输；异步上下文用例通过 Node 的真实 V8 hooks 验证支持构建的实现，不代表当前生产 V8 具备该能力 | `out/bootstrap-contract-js.log` |
| 原生 V8 与桥回归 | **144/144 通过**；包含 ALS 能力不足时安全拒绝、稳定错误码、8 项原生桥用例及 renderer 参数列表封装。await 能力用例只在支持构建编译；排除没有 Browser 窗口的两项商业应用 smoke，用下列实际应用复验覆盖启动 | `out/bootstrap-contract-native-tests.log`、`out/bootstrap-contract-native-results.json` |
| 完整生产构建 | `ipc_main_container_unittests chrome` 完成，**退出码 0**；最新 DLL/EXE 已重新启动。运行验证限于本机 Windows，macOS/Linux 未在本机编译或运行 | `out/bootstrap-contract-final-build.log` |
| TH 登录弹窗 | 主界面 SDK ready、账号初始化状态 2；点击登录后原生“迅雷登录”窗口出现、表单加载完成，用户确认“正常了”。此前重复 `setParentWindow(null)` 的失败已消除，本轮不把窗口恢复表述为重新完成扫码认证 | `out/bootstrap-contract-fix-20260916/th-final-acceptance.json`、`th-login-click.jsonl`；用户确认 |
| PL-E 播放 | 本地受控测试视频播放成功，进度 **0 → 1062 ms**，画面 **320×180**，`errCode: 0`；未覆盖全部格式、网络视频或平台 | `out/bootstrap-contract-fix-20260916/ple-playback.jsonl` |
| 实际运行契约与日志 | TH/PL-E 的 process 模块身份、ALS 能力不足报错、剪贴板不支持 buffer 报错、shell 缺失路径错误字符串均通过；两应用各启动一次、断连/fatal/N-API 失败均为 0 | `out/bootstrap-contract-fix-20260916/th-runtime-contract.jsonl`、`ple-runtime-contract.jsonl`、`final-log-summary.json` |

实际日志仍有已知兼容诊断（例如未实现的 `webRequest.onBeforeRequest`、`nativeTheme.setCustomColor`、`app.setJumpList`），因此上述结论不是“零 JavaScript 错误”或所有 Electron API 可用。修复过程中另发现 renderer IPC 将请求包装为单元素列表、main 直接传对象的差异；共享桥现在同时校验这两种内部封装，并有原生回归覆盖。

## OS 模块真实数据补齐（2026-09-16）

此前 renderer 的 `os` 写死了 Windows 版本、主机名、用户名、CPU 型号/数量和总/可用内存；main 也存在 hostname 依赖环境变量、目录回退到应用目录和 POSIX type 大小写错误。这些属于适配缺陷，不能用“性能优化”解释为正确行为。

- 两端统一通过 `PerformOsCall` 查询 libuv 系统接口，覆盖系统版本、机器类型、主机名、用户、CPU、内存、运行时间、负载和网卡。CPU 与可用内存等每次调用重新读取，不缓存成固定快照。
- `os.platform()/arch()/endianness()` 使用共享的编译目标信息；renderer 同时修正 `process.platform/arch` 和默认 path 平台选择。元数据随已有 runtime config 提供，不增加启动同步 IPC，也不在 require 时枚举系统信息。
- `homedir/tmpdir` 尊重调用侧环境变量；无覆盖时查询系统，移除应用目录兜底。`userInfo` 使用实际用户信息并支持 buffer 和字符串编码，保留原生 null shell。
- `EOL/devNull` 按目标平台选择约定常量。Windows 的 uid/gid=-1、shell=null、loadavg=[0,0,0] 属于 Node/libuv 的平台约定，不是虚构系统状态。
- 当前 libuv 1.43 缺少 `uv_available_parallelism`；优先级操作还缺少可信调用进程身份。这三个接口明确抛 `ERR_NOT_SUPPORTED`，不使用 CPU 数量冒充可用并行度，也不对 Browser 进程误操作。`os.constants` 尚未补齐。
- 新增 OS 契约和原生查询回归。验证记录仅保存身份一致性布尔值，不输出真实用户名、主机名、主目录或网卡地址。

本轮验证：JS **336/336**、常规 Native **152/152** 通过，最终 `chrome` 与测试目标链接成功。9222 实机对照 Node 原生查询，TH/PL-E 均返回真实 **32 个逻辑 CPU、约 63.74 GiB 总内存**，身份、路径与系统信息的相等性检查通过。Windows uptime 按内置 libuv 的整秒精度与新 Node 对照。TH SDK ready、账号初始化状态 2、登录弹窗可打开；PL-E 本地测试视频进度 **225→1064 ms**、320×180、错误码 0。记录位于 `out/os-contract-full-js-tests.log`、`out/os-contract-native-tests.log`、`out/os-contract-final-build.log` 和 `out/os-contract-fix-20260916/`。本轮未执行真实扫码登录，也未在 macOS/Linux 实机验证。

接口语义参考 [Node OS 文档](https://nodejs.org/api/os.html)。`process.env` 当前仍是 JS 环境快照；删除其中的目录变量后，原生查询仍会看到继承的进程环境，尚不等同于修改真实进程环境。此修复不表示其他模块或 `process` 的所有兼容字段均已完成标准化。

## TH 重启自动登录：二进制凭据持久化修复（2026-09-16）

OS 修复后的实机验收暴露了此前未覆盖的“扫码成功后退出、再次启动自动登录”场景。启动日志已进入 autoSignIn，但在本地 `CredentialsManager.getCredentials` 返回 `unauthenticated/16`。OS 修改前的日志有相同错误；TH 设备标识、凭据加密和 Windows profiles 路径均不依赖本次修正的 hostname/release/userInfo。

只读检查发现，当前账号对应的 `credentials2.credentials` 是长度 15 的 TEXT，内容为 `[object Object]`，并非 SDK 所需的加密 BLOB。此前扫码后的内存凭据可以使用，但重启从数据库恢复时无法解密。登录窗口可打开、SDK ready 和单次扫码成功均不足以证明自动登录可用。

根因在通用 addon 二进制契约：

- Renderer 参数包装将 Buffer/TypedArray 当普通对象枚举，丢失类型；返回值包装也有同类问题。
- Native wire 将二进制统一恢复成 ArrayBuffer，无法保留 Buffer/TypedArray；main 的 JSON 中转不支持嵌套 BLOB。
- `napi_is_buffer` 原先只认内部 Buffer 标记，和 Node 16/24 的 ArrayBufferView 判断不同。该错误还被旧单测中的错误期望掩盖。

修复采用携带种类与活动范围字节的二进制 wire，覆盖同步、异步、嵌套值和回调；保留 Buffer、ArrayBuffer、DataView、各 TypedArray 的类型与字节内容，子视图不传出范围外字节。N-API 的 buffer 检查与取值按 Node 语义修正。用户数据库中的损坏凭据不自动删除、不切换其他账号；仅存下 `[object Object]` 的记录无法还原，需修复后重新登录生成有效凭据。

回归使用固定公开测试字节，以及 TH 原有 CredentialsTableManager/DatabaseManager 在独立 `out` 测试库中执行 AES 保存、关闭、重开、解密；不复制真实凭据或调用认证接口。修复前两项实机探针均复现 TEXT 写入和重开恢复失败，记录位于 `out/os-contract-fix-20260916/sqlite-binary-before.jsonl`、`credential-persistence-before.jsonl`。

修复后验证：JS **347/347**（零跳过）、常规 Native **158/158**、N-API **25/25** 通过，`chrome` 与两套测试目标构建成功。9222 新进程中，真实 SQLite addon 的 Buffer/TypedArray、子视图、空值均正确保存为 BLOB 并返回 Buffer；TH SDK 独立测试库保存后为 **240 字节 BLOB**，关闭重开后的解密及字段逐项比较全部通过。TH SDK ready、账号初始化状态 2、登录弹窗及 148×148 二维码正常显示；PL-E 测试视频进度 **330→1191 ms**，320×180、错误码 0。实测期间两个 Electron 容器均无断连、原生 fatal 或 N-API 调用失败。

用户重新扫码后，真实账号当前凭据已存为 BLOB。退出浏览器后再次启动，**无需再次扫码即自动登录成功**：20:16:04.307 开始自动登录，20:16:05.280 发出登录成功通知，用时 **973 ms**；`isSignIn=true`、`isAutoLogin=true`、非匿名账号，主界面头像正常，当前账号凭据仍为 BLOB。仅检查类型、长度、状态等元数据，没有导出用户凭据。

用户曾观察到扫码后延迟显示成功。该轮 `SIGNED_IN`（20:12:29.778）到界面成功通知（20:12:29.795）相隔 **17 ms**。延迟在该事件之前；源码显示这段流程包含设备授权轮询、用户信息请求及旧账号会话同步。由于没有采集手机确认时刻与各请求耗时，不能将本次延迟明确归因于网络、轮询或某一接口。

记录：`out/th-autologin-binary-js.log`、`out/th-autologin-binary-native-tests.log`、`out/th-autologin-binary-napi-tests.log`、`out/th-autologin-binary-final-build.log`，以及 `out/th-autologin-binary-20260916/` 中的实机探针与结果。

## Renderer bootstrap 增量优化（2026-09-16）

本轮生产代码范围限定 `xenon_ipc_renderer_bootstrap.js`，保持现有模块身份和原生传输契约。

- **包配置重复读取**：裸包名冷解析先检查 `exports`，后读取 `main`，此前会读取并解析同一份 `package.json` 两次。现在按规范文件路径在单次解析内复用结果；下一次解析、失败重试或删除 `require.cache` 后仍重新读取当前文件。配置缓存按需分配，直接文件加载不增加 Map。
- **原生参数临时分配**：原始值、二进制、回调和原生句柄不需要遍历对象；现在仅递归数组或普通对象时分配 `seen Map`。保持每个顶层参数独立的遍历状态、二进制提交时快照及句柄保活时序。
- **Buffer 契约缺口**：补齐 renderer `Buffer.isEncoding` 的 Node 编码别名、大小写和非字符串判断。TH 消息同步组件的真实 Writable 路径曾因此抛错并进入重试；离线加载其实际 bundle 可复现旧错误，并验证修复后的中文 UTF-8 写入。此修复不表示 MQTT 网络连接一定成功，也不表示其他尚缺失的 Buffer API 已补齐。

可复现基准 `tools/benchmark_renderer_bootstrap.cjs` 使用完整 bootstrap 和仪表化传输：100 个独立包冷加载同步文件 IPC **1201→1101**（减少约 8.3%），其中 `package.json` 读取 **200→100**；1000 次重复加载两版均为 **0 额外文件 IPC**且保持同一导出引用。10000 次、每次 3 个原始值参数的同步 addon 调用，遍历 Map 分配 **30000→0**。这些是指定工作负载的调用与分配计数，不外推为 TH/PL-E 整体启动耗时或吞吐提升。

新增回归先在旧实现上验证失败，再验证修复；覆盖包别名、删除缓存后更换入口、失败重试、同步/异步参数图及真实 TH Writable 消费路径。JS **355/355** 通过、零跳过，包含 GC 与真实 file-stream-rotator 回归。记录位于 `out/renderer-bootstrap-opt-20260916/`。

生产资源重新打包成功，常规 Native **158/158** 通过。9222 新进程中 TH 自动登录成功，真实 SDK 独立测试库的保存、关闭、重开、解密及字段比较继续通过；TH/PL-E 的 `buffer`、`node:buffer` 与全局 Buffer 身份一致，`isEncoding` 实际可调用。PL-E 测试视频进度 **232→1061 ms**、320×180、错误码 0，用户也确认播放正常。验收日志中未再出现 `isEncoding is not a function`，两个 Electron 容器无断连、原生 fatal 或 N-API 调用失败；这不代表已修复日志里的其他既有 API 缺口。

## Bootstrap 剩余硬编码修正（2026-09-16）

本轮修正审查发现的 renderer 行为硬编码，并移除 main 中单实例锁、协议注册的无条件成功返回：

- **业务 RPC**：删除按 `openElectronSelectFileDialog` / `showOpenDialog` 方法名截获调用的逻辑，不再修改 `Object.prototype.callRemoteClientFunction` 或全局 `Object.defineProperty`。应用方法保持原始身份、描述符、接收者、参数和返回值，文件选择经应用原有 IPC handler 调用标准 `dialog`。主进程原生 picker 缺少 hook 或 Browser 调用失败时分别抛出 `ERR_NOT_SUPPORTED` / `ERR_FAILED`；只有成功回复的空路径列表才表示取消。
- **运行时信息**：PID、Chrome/V8 版本取当前原生进程；contextId 随文档 binding 创建，隔离与沙箱状态反映当前 binding/进程。显式排除 `--no-sandbox`，因为 Chromium 的断言辅助 API 在该参数下仍返回 true。Node/Electron 继续以 `0.0.0-compat` 表示适配器身份，不伪称嵌入了某个发行版。托管状态与 main 的容器配置保持一致。
- **工作目录与路径**：Browser 启动早期只捕获一次工作目录，文档配置只复制快照；查询失败不伪造根目录、不在 UI 请求阶段重试阻塞读取。这是 brokered 文件 API 使用的逻辑工作目录，并非 Linux sandbox 改变根目录后的物理 cwd。Win32 盘符相对路径、UNC 与 POSIX 路径分别处理；POSIX 文件名保留反斜杠和大小写，CJS 缓存与允许目录检查遵循宿主平台。
- **URL**：相对 URL 不再凭空补 localhost；file URL 正确处理 `#`、`?`、`%`、空格、Unicode 和 UNC，解析后的磁盘路径仍经过模块目录边界检查。
- **Buffer**：编码别名、大小写、Latin-1、ASCII、UTF-16LE 与 UTF-8 BOM 的行为按 Node 对照修正；未知编码报错，字符串 fill 按编码字节重复，write 不截断多字节字符。原生 base64 快速路径和二进制子视图范围保留。
- **renderer net**：IP 校验按完整格式判断；Windows named pipe 只有收到原生 bind 成功通知才进入 listening 状态，address 与关闭状态一致。空闲超时随连接、读写活动更新。尚无原生后端的 TCP/Unix 操作异步报告 `ERR_NOT_SUPPORTED`，不生成虚假监听成功。pipe 上的 TCP 参数 setter 保持 Node 的可链式无操作语义。
- **main app**：没有后端实现的单实例锁、协议注册/移除明确抛出 `ERR_NOT_SUPPORTED`，不再返回 true 声称已取得锁或完成系统注册。

JavaScript 全量回归 **388/388，零跳过**，包含真实 TH/PL-E 依赖。原生构建、实机验收记录集中在 `out/ipc-hardcode-fix-20260916/`。这些修改不表示所有 Node/Electron API 已完整实现；main net 的其他兼容缺口、TCP/Unix 后端及 stream 半关闭/背压不在本轮实现范围内。

使用上一提交与本轮源码运行既有 bootstrap 基准，调用/分配计数一致：100 个包冷加载文件 IPC **1101**、package.json 读取 **100**；1000 次热加载 IPC **0**；10000 次原生基础参数调用遍历 Map 分配 **0**。结果见同目录 `renderer-bootstrap-benchmark.json`，仅证明这些计数未退化，不等同于 CPU 耗时或整体启动性能无变化。

### 关闭后再次打开的生命周期缺口

用户补充实测发现 TH/PL-E 关闭后不能再次打开。现场两个 Utility 容器仍存活、无断连或 fatal，而主页面已消失；TH 仍有辅助播放器窗口。源码核对确认这些关闭/激活分支在上述硬编码修正前已存在：已绑定容器的再次启动只返回成功，不通知应用 `activate`；原生/DOM 关闭绕过主进程可取消的 `close`；`closed` 不发出 `window-all-closed`。只重新执行初始化也无效，因为 Service 对已初始化容器直接返回。

实际 TH 主窗的原生 close handler 默认 `hide()` 并 `preventDefault()`，右上角 X 同样绑定隐藏；菜单“退出”及 DOM beforeunload 的退出分支会先执行退出业务、清理 SDK，再调用 `app.quit()`；旧适配只发 `before-quit`，留下已清理但仍存活的容器。TH 的 activate 仅在所有 BrowserWindow 都消失后才重建，因此残留辅助窗进一步阻碍恢复。PL-E 的 activate 按主窗引用重建或置前，直接退出时已有原生清理；托盘关闭的 `hidePlayer()` 默认也会关闭当前媒体。修复不能将所有关闭统一改成隐藏，也不能按业务名称绕过原有清理逻辑。

显式激活现在按容器派发 `app.activate(event, hasVisibleWindows)`，ready 前请求在 ready/whenReady 回调后交付，不广播到其他应用或为未知容器启动服务。原生与 DOM 关闭先发 `close-requested`，主进程执行可取消的 `BrowserWindow.close()`；获得销毁许可后走跨平台 Widget 隐藏/立即关闭。DOM 请求在进入 WebDialogView 的关闭状态机前拦截，避免一次 veto 后原生窗口下次无法取消。容器故障及浏览器退出仍强制清理。

`closed` 与 `window-all-closed` 统一处理，覆盖成对/嵌套销毁及回调中新建窗口。激活入口只选择已显示过的内容窗，隐藏主窗可恢复，从未显示的辅助窗不会被强行拉起。托管 BrowserWindow 不采用 WebDialog 默认 Escape 关闭行为，由应用页面处理该按键。

2026-09-17 继续补齐 `app.quit` 的可取消 `before-quit → close → will-quit → quit` 通路，以及 `app.exit/process.exit` 的强制退出通路。退出 owner 能力在清理前检查；原生通知延后到 V8 调用栈返回后。Browser 校验容器/observer/代次，断开所有共享服务连接副本并清理窗口，保留配置供下一次显式启动；旧页面消息和旧代次回调不能自动重启或误关闭新实例。退出期间不交付 activate，正常退出与崩溃分开记录。

另发现 PL-E 的 UA 更新竞态：新页面导航期间 `setUserAgent` 可能触发 Chromium 重载旧的已提交 `about:blank`，现场导航历史确认 `index.html → about:blank (reload)`。Host 暂存导航期间的 UA 更新，等待导航完全停止加载后安全应用，并在窗口销毁时取消待执行任务。已发出的请求仍使用原 UA，不通过重载补发；更新后的 UA 用于后续请求。

JavaScript 回归现为 **411/411，零跳过**，包括退出取消、隐藏辅助窗、重复/重入退出、缺失 owner 和停止后的激活。新增原生退出回归、manager 隔离用例和 NavigationSimulator 用例；后两类所属完整 `unit_tests` 测试宿主尚未执行，不能以对象编译代替执行结果。`BrowserWindow.close()` 尚未实现主进程到 renderer 的完整 `beforeunload/unload` 握手；TH 菜单退出及 PL-E 播放窗关闭先执行业务清理，但 AI 转高清等依赖页面退出保存的扩展窗口仍需单独补齐与验证。

本轮最终生产构建 `build-reopen-final3.log` 成功；原生 IPC 回归 **170/170**（不含独立商业 addon smoke）。UI delegate/manager/8 个 UA 导航用例的测试对象均已编译，未执行完整浏览器单测宿主。实机 `chrome-reopen-final.log` 已确认 PL-E 首页稳定停留在 `index.html`，两次测试视频播放通过（320×180、进度推进、错误码 0）；播放界面关闭后媒体对象归空。TH SDK ready、自动登录及头像加载通过。TH 普通 Hide 后保持原页面且 visibility 为 hidden；TH、PL-E 关闭后重新打开由用户实际验证通过（用户反馈“重开我已验证”），不记为自动化完成的验收。后续检测到 PL-E 正播放非测试媒体，自动关闭探针按保护规则未执行。结果文件集中在 `out/ipc-hardcode-fix-20260916/` 的 `ple-final-*`、`th-final-*` 和 `reopen-final-summary.json`。

## 验证方式

### 上一轮结果（2026-09-16，Bootstrap 审查修复前）

本节保留上一轮 JS/生产构建与此前 Native 专项验证，不作为上述 Bootstrap 审查修复后的验证结果。初次 TH 登录和崩溃恢复的汇总保留在 `out/th-ple-startup-check/th-ple-final-acceptance-20260916.json`；PL-E 7.1.35.173 与后续窗口修复的实际记录见下表。

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

## Bootstrap 源码组织（2026-09-17）

- main 入口从 5,069 行缩至 60 行，renderer 从 6,634 行缩至 53 行。按职责维护
  main 14 个文件、renderer 15 个文件及 2 个公共文件；文件系统、窗口及原生桥接
  保留完整功能边界，避免拆成大量只有几行代码的文件。
- 入口是唯一的源码登记位置。GN 自动收集输入，构建器展开为原有两份资源；
  没有增加运行时文件读取、IPC、模块包装或加载层。源码仍共享各自入口闭包。
- zlib/readline 两份相同实现合并维护。JS 回归与基准调用同一构建器，部分独立
  契约测试直接读取命名片段；原生测试的回退路径改为读取生成文件。
- 两份生成脚本与提交 `365ce67bbc03f` 中的原脚本逐字一致（LF 换行），pak 资源
  解压后也与生成文件一致。renderer 基准的冷加载文件 IPC 为 1,101、热缓存
  1,000 次 require 为 0 IPC、10,000 次原生调用的基础参数 map 分配为 0，前后相同。
- 验证：422 项 JS 测试及 170 项原生回归通过（不含 `XenonRealAppSmokeTest.*`）；
  资源与原生测试目标构建成功。随后完成正式 chrome 构建，核对最终 `resources.pak`
  与生成脚本一致，并使用原用户配置、9222 重启实测。
- 实际应用：TH SDK 就绪、自动登录成功、头像加载正常；PLE 两次播放仓库测试视频，
  画面 320×180、进度持续增长、媒体错误码为 0。原画、字幕、选集弹窗的 DOM 和截图
  均已核对；关闭测试视频后媒体清空，重开窗口无残留播放。
- 用户明确确认“拖动和关闭后重开都正常”。桌面自动化工具仍遇窗口归属错误，
  真实拖动与侧栏重开的结论来自本次用户验收，不将 CDP 前置窗口当作侧栏点击验证。
  本轮不宣称启动提速，也不将既有兼容性报错表述为全部消除。
- 日志对照：未见 native FATAL、容器断连、app/window 事件失败；TH 启动阶段的
  `nativeTheme.setCustomColor`、`crypto.createPublicKey`、`app.setJumpList`、
  `isOnline`、`Failed to fetch` 报错在旧版也出现。TH 的
  `napi_get_named_property(links)` 状态 10 告警在新旧日志中各有 1 条，签名一致。
  该状态为 `napi_pending_exception`。另记录到一条 `index.js` 的 `Network Error`，
  本次旧日志对照中没有同脚本签名，原因尚未定位；不能将所有报错归为既有问题。

维护方式见 `resources/ipc/README.md`。本机验证记录位于
`out/ipc-bootstrap-split-20260917/`。

## 架构评估后的性能优化（2026-09-17）

基于 `8fae275bbf3fc` 测量后实施三项局部优化：renderer 原生绝对路径不再
反复做 URL 解析；文件读写使用原生后端已有 BLOB 通道；原生导出调用复用
参数数组并移除临时包装。模块身份、同步/异步契约、二进制快照和保活不变。

1 MiB 文件传输有效载荷减少约 25%、Base64 编解码 2 → 0；10 万次热 require
的 Windows/POSIX 路径 URL 构造分别为 200,000/100,000 → 0。派发微基准
中位耗时下降约 5%–8%；这些不代表 TH/PLE 整体启动或播放吞吐的相同比例提升。
431 项 JS、170 项原生回归通过，正式构建及 TH 自动登录、PLE 播放、两端
真实二进制文件桥均已验证。完整架构、测量方法、范围和后续优先级见
[适配层架构与性能评估.md](适配层架构与性能评估.md)。

用户确认正常后继续检查：修复 URL 父模块缓存键不一致造成的第二次 require
多余文件 IPC，补齐 CommonJS 源码读取的 BLOB 请求；服务端自有 addon 参数
改用移动转换，去除额外深拷贝，四类高频调用日志改为可开启的 VLOG(1)。
第二轮 **436 项 JS、176 项原生回归**及正式构建通过；TH 自动登录、大 BLOB
SQLite、两侧实际模块缓存与 PLE 播放/关闭后重开通过。该轮发现 ReadStream
范围读有 256 倍返回放大，随后增加原生 start/end 范围协议修复，详见架构评估
第 8 节。renderer 网络请求取消缺口仍未处理。

ReadStream 修复后，**447 项 JS、183 项 Windows 原生回归**及正式构建通过。
实机 TH/PLE 各 26 项普通文件和 ASAR 范围验证通过，64 KiB 请求的返回和
JS backing 均由 16 MiB 降至 64 KiB；TH 自动登录、PLE 播放及测试媒体关闭后
重开正常。仍缓存所选范围，尚未实现原生到 JS 的流式交付和已提交 I/O 的取消。
