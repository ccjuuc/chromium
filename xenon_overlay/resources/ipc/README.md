# IPC bootstrap 源码组织

`xenon_ipc_main_bootstrap.js` 和 `xenon_ipc_renderer_bootstrap.js` 是构建入口。
它们声明源码组合顺序，**不能直接注入 V8 或传给 `vm.runInContext`**。
`// @include "..."` 只由构建工具展开，运行时不加载源码片段。

## 目录与职责

| 目录 | 内容 |
| --- | --- |
| `common/` | 两端共享的 package maps、模块工厂、事件与定时器辅助、fs.Stats、zlib、readline 等 |
| `main/` | app/窗口/session 生命周期、主进程 IPC、Node 模块及宿主调用 |
| `renderer/` | renderer IPC、Node 模块、原生对象桥、CommonJS 加载、preload/webview 安装 |

每个片段包含完整的声明或语句，按功能边界拆分，不按固定行数截断。
文件系统、窗口和原生桥接各自集中维护；保留有独立契约测试价值的模块。
入口保留必要的安装顺序和模块注册状态。Buffer 使用两端共享的工厂；有差异的 path、URL、
network 等实现继续分别维护；不能因为名字相同就覆盖其中一端。

## 构建与运行

`//xenon_overlay/resources:ipc_bootstrap` 使用 Chromium 自带 Node 执行
`xenon_overlay/tools/bundle_ipc_bootstrap.cjs`，静态展开入口并检查 JavaScript 语法。
输出为：

```text
out/<配置>/gen/xenon_overlay/resources/ipc/xenon_ipc_main_bootstrap.js
out/<配置>/gen/xenon_overlay/resources/ipc/xenon_ipc_renderer_bootstrap.js
```

GRIT 依赖该动作，把生成文件打入原有资源 ID。main 和 renderer 的 C++ 注入方式
保持不变；未初始化 ResourceBundle 的原生测试读取可执行文件目录下 `gen/` 中的
同一生成文件，该文件也列入测试运行数据。

新增或删除片段时，只修改对应入口中的 include。`bootstrap_sources.gni`
自动从入口读取构建输入，无需手工维护第二份清单。include 仅允许出现在入口中，
不形成多层引用。构建器另写依赖文件；输出未变化时不重写文件，避免无效的下游重建。
路径以入口所在目录为根，统一使用 `/`；不允许重复、循环、越界或缺失的 include。

## 作用域和模块身份

这些文件是**构建时组合的源码片段**，仍共享入口的同一个闭包，不是各自拥有
运行时作用域的 ES module。这样可以保留既有初始化次序、函数提升和对象身份。
结构拆分提交 `8fae275bbf3fc` 的完整脚本与拆分前逐字一致（统一为 LF 换行）。
后续运行逻辑优化的测量与验收见 `../../docs/适配层架构与性能评估.md`。

修改时需特别注意：

- 共享 Buffer 与真实 Node 流引擎必须先于 `fs.js` 初始化，文件流继承这些构造器。
- main 的 app 退出流程、session 和 IPC 回调会访问后方声明的窗口及事件状态。
- renderer 的 `native_bridge.js` 统一管理句柄、代理和回调状态；`modules.js`
  中的 CommonJS 加载与内建模块注册共用缓存。
- 网络和 IPC 片段有安装监听器的副作用；preload 必须在原来的页面初始化阶段执行。

不要给每个片段额外包 IIFE、调整顺序，或改成运行时文件加载。
同一 context 中的 `require` 缓存和返回对象保持一致；不同 context 仍分别创建状态。
后续引入独立工厂时应显式传入依赖，并单独验证初始化时序和模块身份。

主进程 ESM 由原生 `XenonModuleResolver` 与 `XenonEsmModuleLoader` 承担，
分别负责路径/package 解析和 V8 模块图。package maps 的 JS 算法由两端共享，
原始 package JSON 直接进入 V8，避免 native 字典重排条件优先级。
`third_party/cjs_module_lexer` 保存上游原版解析器和许可证，只做静态导出发现。

主进程同步 `module.registerHooks` 由 `main/module_hooks.js` 维护链规则，
原生 `xenon_ipc_main_container_modules.cc` 接入真实 CJS/ESM 解析、ASAR 和源码加载。
支持条件/导入属性、短路源码与注销，边界见
[Node同步模块钩子.md](../../docs/Node同步模块钩子.md)。
不支持的 data URL 模块、Wasm source-phase import 不会转成 CommonJS 执行。

Electron 的沙箱偏好由 `main/sandbox.js` 与 Browser 原生策略共同执行；
`renderer/sandbox.js` 提供受限的预加载环境，源文本来自 Browser，不能用完整模块
加载器绕过限制。支持范围和操作系统边界见
[Electron沙箱兼容.md](../../docs/Electron沙箱兼容.md)。

`electron.utilityProcess.fork` 使用独立 Utility 进程和真实 OS stdio；父端
`main/utility_process.js` 管理 Electron 对象，子端 `main/utility_runtime.js`
安装标准流与 parentPort。进程、消息和支持边界见
[ElectronUtilityProcess兼容.md](../../docs/ElectronUtilityProcess兼容.md)。

`electron.Notification` 的对象与代次状态由 `main/notification.js` 管理，
显示和交互通过真实系统通知代理；支持范围与事件边界见
[Electron通知兼容.md](../../docs/Electron通知兼容.md)。

`events.once` 与 `timers/promises` 使用真实事件和定时器，支持取消、错误传播与清理。
main 单独提供 `Event`、`CustomEvent`、`EventTarget`，遵循 Node 的单目标派发模型，
支持 once、capture 身份、passive、signal 清理与异步监听错误；它不是 EventEmitter
别名。main 的 AbortSignal 使用这套事件对象，renderer 保留 Blink 原生实现。
没有原生 `unref` 的宿主对 Promise timer 的 `ref:false` 明确报 `ERR_NOT_SUPPORTED`。
模块编译缓存没有持久化实现：`enableCompileCache()` 返回 `DISABLED` 及说明，
`getCompileCacheDir()` 返回 `undefined`。`fs.Stats` 使用宿主提供的实际元数据，
未提供的权限、inode 等字段保持未知，不能把它当成完整 Node 文件系统 ABI。

`original-fs` 与 `original-fs/promises`（含 `node:` 别名）使用独立的文件系统
实例。每个请求携带绕过 ASAR 的标志，读取物理归档文件及其原始字节，不切换
全局状态、不解释归档、不触发解密。它和普通 `fs` 共用原生传输与 API 工厂，
但各自持有 `Stats`、流构造器、方法和 Promise 对象；现有文件描述符与监听等
未实现操作继续明确报错。文件 URL、UTF-8 Buffer 路径在进入传输前统一转换。

`util.isDeepStrictEqual` 两端共用比较算法，Promise/Error 品牌由本地 V8 判断，
支持循环、集合、二进制视图等；未实现的 KeyObject/CryptoKey 内部密钥比较不在
此支持范围。`util.stripVTControlCharacters` 使用 Node 的 CSI/OSC 规则；
`module.isBuiltin` 查询各端实际注册的模块，不加载文件，也不把 Electron 自身
当作 Node builtin。`stream` 使用原版 `readable-stream@4.8.0` 的真实队列、背压、
Transform 与生命周期；`string_decoder` 也使用上游增量解码。静态模块表生成、
文件底层读取边界、Web Stream 转换及取消规则见
[`Node流生命周期支持.md`](../../docs/Node流生命周期支持.md)。

Native addon 服务接收独立的应用路径和归档公钥配置。应用路径决定其 native
模块范围，`runtimeDirectory` 只用于 DLL/资源定位，不自行授予加载权限。
目录应用允许 canonical 子路径；归档应用只允许其声明为 unpacked 的成员，
拒绝越界链接。缓存和异步加载完成时重新检查范围，公钥随服务上下文撤销。
已找到文件后的权限或加载失败保留真实错误，不伪装成 `MODULE_NOT_FOUND`。

`crashReporter` 共用宿主已有的 Chromium Crashpad，不重新初始化、不修改上传
配置；`start({uploadToServer:false})` 和 `setUploadToServer(false)` 不关闭宿主上传。
状态和已上传记录来自真实共享数据库，参数写入调用进程。详见
[`Electron崩溃报告兼容.md`](../../docs/Electron崩溃报告兼容.md)。

`ClipboardItem` 的 MIME 读写由 Chromium 系统剪贴板实现，保留旧同步文字 API；
事务、格式及版本边界见
[`Electron剪贴板兼容边界.md`](../../docs/Electron剪贴板兼容边界.md)。
main 的 `Blob` / `buffer.Blob` 提供不可变内存字节、UTF-8、切片、异步读取与
`Blob.stream()`；Web Streams 由已内置的上游 ponyfill 提供。renderer 保留 Blink 原生 Blob。

`app.requestSingleInstanceLock` 只把 `additionalData` 作为 JS 请求数据。second-instance
的 argv 来自 Browser 保存的启动参数；运行时修改 `process.argv` 不影响原生命令行，
应用需要传递自己的数组时可放在 additionalData。cwd 由原生主容器在请求时读取，
同步和异步调用都覆盖私有运行时字段，再由 Browser 校验并转发。独立 main 的真实
OS chdir 会影响该通知；不读取可被重写的 JS 函数，也不回退到过期启动目录。
`nativeImage` 支持 PNG/JPEG、文件和 ASAR 路径的真实解码及 PNG/JPEG/Bitmap 导出，
目前只支持 1x；缩放、裁剪、多表示和平台原生句柄均明确拒绝。

`webContents.executeJavaScript()` 通过已授权主文档的 Renderer 执行，保留页面
主世界、CSP 和 Node 隔离，返回真实 Promise 结果及异常；不调用 Chromium 仅允许
内部页面使用的 Browser 脚本接口。文档绑定、结构化克隆、取消和支持边界见
[`Electron脚本执行兼容.md`](../../docs/Electron脚本执行兼容.md)。

`loadURL()` 绑定真实导航和文档的完成/失败，`loadFile()` 统一处理应用相对路径。
Session 媒体权限使用 Chromium 的真实设备与桌面捕获后端，说明和验证边界见
[`Electron导航与媒体权限.md`](../../docs/Electron导航与媒体权限.md)。

### Renderer 模块格式边界

Renderer 的宿主模块加载器提供 CommonJS `require`，支持 package `exports`、
`imports` 的 `node` / `require` / `node-addons` 条件和精确目标路径。
它尚未接入 Node ESM 模块图：`.mjs` 以及最近 package scope 声明
`"type": "module"` 的普通 `.js` 模块会明确报 `ERR_REQUIRE_ESM`，不会作为
CommonJS 执行，也不会启用 `module-sync` 条件。未知扩展名和无扩展名文件继续
按 CommonJS 加载；不会模拟新版 Node 的 ESM 语法检测。浏览器自身的
`<script type="module">` 及 `import()` 仍遵循 Chromium 的网页模块规则，
不等同于 Node 模块解析。

Preload 入口按 Electron 规则处理：`.js` 入口忽略 package `type`，其
`require()` 的依赖仍使用普通模块规则；`.mjs` preload 尚不支持，会发出
`preload-error`。格式依据文件实际扩展名区分大小写；Windows 的路径大小写
及符号链接先归一到宿主返回的 canonical filename，再选择加载器。

## 测试与诊断

完整集成测试和基准使用 `bootstrap_test_support.cjs` 的 `readBootstrap()`，
与生产打包调用同一构建器。独立契约测试可用 `readBootstrapPart()` 读取明确命名的
源码片段，避免按字符串位置截取整个 bootstrap。自定义 `XENON_TEST_BOOTSTRAP`
仍支持历史单文件脚本。

```text
node xenon_overlay/tools/validate_ipc_compat.cjs
```

构建器测试覆盖共享作用域、初始化顺序、单层 include、错误 include、源码覆盖、
特殊字符路径和增量输出。修改原生加载路径时还要构建并运行原生 IPC 回归。

V8 报错行号对应生成文件；先打开该配置的 `gen/` 文件，再定位到相应职责片段。
本次调整减少重复维护，不增加运行时 I/O、IPC 或加载器，也不据此宣称启动提速。
