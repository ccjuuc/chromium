# Electron IPC 容器架构与接入

## 1. 定位与边界

Xenon Electron 容器是 Chromium 内的 Electron 兼容层，用于托管已编译的
Electron main 和 renderer 代码。它不启动 Electron runtime，也不把业务补丁
写进通用容器；容器通过 V8、Mojo、Chromium WebContents/Widget 和 Node-API
兼容层实现项目需要的 Electron 语义。

核心目标：

- Electron main 在独立 Utility Process 运行，Browser Process 不执行业务 V8 代码。
- `ipcMain`、`ipcRenderer`、`BrowserWindow` 和 Node-API addon 保持 Electron 常用调用形式。
- main 生命周期与单个 renderer Document 解耦；页面刷新不丢失 main 全局状态和 `ipcMain` handler。
- 每个 `container_id` 拥有独立的 Utility service、V8/CommonJS 缓存、addon 实例和运行目录。
- TH、PL-E 的新增接入只提供入口、资源映射、CORS 和运行时路径；核心服务、
  addon loader 与运行目录重定向不按项目名、业务 channel 或 SDK 文件名分支。

当前实现是“按需兼容 Electron API”，不等同于完整 Electron。未在主干或
自动化测试中覆盖的 Electron/Node API，不应默认为已实现。

## 2. 进程架构

```text
Browser Process
  XenonManager
    container_id -> ContainerServiceConnection -> Utility Process
    origin       -> container_id
    保存 IpcMainConfig，Utility 重启后重放配置

  XenonElectronWindowHost
    window_id -> BrowserWindow 原生 Widget/WebContents
    window_id -> container_id

  XenonIpcDocumentHost (content::DocumentService)
    每个已提交 Document 一个 endpoint
    校验 origin，路由 IPC，提供 fs/net/sqlite/os 桥接
        |                         ^
        | Mojo                   | BrowserWindow 创建/事件
        v                         |

Utility Process (kNoSandbox，每个 container_id 独立)
  XenonServiceImpl
    XenonIpcMainContainer
      V8 context + CommonJS loader + Electron main bootstrap
      ipcMain / app / BrowserWindow / webContents / session / dialog ...
    XenonNodeExecutor
      Node-API ABI + addon 对象/回调管理 + libuv + Win32 message pump
        |
        | IpcRenderer remote / NodeAddonHost
        v

Renderer Process
  XenonIpcRenderer + xenon_ipc_renderer_bootstrap.js
    require('electron') / ipcRenderer / process / Node 兼容模块
  Hosted renderer (chrome://...)
```

关键边界：

- `XenonManager` 以 `container_id` 为键启动 `ServiceProcessHost`。一个容器崩溃或重启
  不应污染另一个容器的 V8、addon 和环境变量。
- `XenonIpcDocumentHost` 随 Document 销毁。跨文档导航、frame 删除或 renderer 崩溃
  只解除当前 endpoint，不销毁 main 容器。
- Utility 通过 `XenonBrowserObserver` 同步创建/查询 BrowserWindow，而真实
  Widget 和 WebContents 始终归 Browser UI 线程所有。
- Renderer 首次由 Browser 代理 `NodeAddonHost` receiver；绑定后的同步 addon 调用
  直达 Utility，避免 addon 同步向 Browser HWND 发消息时形成 UI 线程死锁。

## 3. 容器配置模型

`IpcMainConfig` 是容器启动的唯一配置事实源：

| 字段 | 语义 |
|---|---|
| `container_id` | 容器、Utility service、callback 和 renderer endpoint 的隔离键；空值归一为 `default` |
| `embedded_main_source` | Browser 读取后传入 Utility 的 CommonJS main 源码；空值时由命令行解析入口 |
| `virtual_main_path` | main 在 CommonJS 中的真实语义路径，决定 `__filename`、`__dirname` 和相对 `require()` |
| `app_path` | `app.getAppPath()`、模块边界和 renderer 运行配置的应用根目录 |
| `executable_path` | 托管应用的虚拟可执行文件身份，用于 `process.execPath`、`app.getPath('exe')` 和版本资源；不会被容器启动 |
| `runtime_directory` | addon 伴生 DLL、`player/`、`SDK/` 和子进程资源的绝对根目录 |
| `app_name` / `app_version` | Electron `app`/`process.versions` 和 UA 兼容元数据 |
| `default_user_agent` | `session.defaultSession` 及 BrowserWindow WebContents 的默认 UA |
| `renderer_url_mappings` | 有序的“原 file 路径前缀 -> 目标 URL 根”列表；多 renderer 工程应使用它 |
| `renderer_base_url` | 旧的单目标 catch-all；新接入优先使用 `renderer_url_mappings` |

三类路径必须区分：

1. `virtual_main_path` 回答“main 代码从哪里执行”。
2. `executable_path` 回答“应用认为自己是哪个 exe”。
3. `runtime_directory` 回答“原生依赖和子进程资源在哪里”。

将三者强制绑在同一目录会导致安装目录、分离 main/frontend 或多个 SDK 并存时
解析错误。

## 4. 启动与生命周期

1. `PreProfileInit` 开启 renderer binding 并合并内置 origin 白名单，不创建 V8。
2. 初始 Profile 就绪后，Browser 构造并通过 `RegisterElectronIpc` 保存
   `IpcMainConfig`，同时记录 `origin -> container_id`；注册阶段不启动 Utility。
3. 用户点击对应侧边栏入口时，`EnsureElectronIpcStarted(container_id)` 才为该容器
   启动独立 Utility。重复点击复用同一 service；显式命令行 Electron app 本身视为
   激活请求，仍立即启动。
4. `XenonServiceImpl` 创建 `XenonIpcMainContainer` 和对应 `XenonNodeExecutor`，注入
   window/native hooks，加载 main。
5. `InitializeElectronIpc` 先回复 Browser，再异步标记 app ready。这个顺序避免
   `app.whenReady()` 中的同步 `new BrowserWindow()` 与 Browser UI 线程互等。
6. main 创建 BrowserWindow；Browser 分配全局 `window_id`，建立 Widget/WebContents，并绑定
   `window_id -> container_id`。
7. renderer Document 提交后建立独立 endpoint，向 Utility 注册 `process_id`、`frame_id`、
   `window_id`。
8. Utility 断开时，该容器 generation 递增；Browser 使用保存的配置重建 service。
   持有旧 remote 的调用会得到明确失败，调用方应重新绑定后重试。
9. Browser 关闭时先销毁托管 BrowserWindow，再进入 ProfileManager 拆卸，避免 WebContents
   在 Profile 之后析构。

## 5. Main 和 Renderer 兼容层

### 5.1 Main

`XenonIpcMainContainer` 在 Utility V8 中运行 `xenon_ipc_main_bootstrap.js`，再执行应用
main。CommonJS loader 支持：

- `.js`、`.cjs`、`.json`、`.node`；
- 相对/绝对模块、目录 `package.json.main`、`index.*`；
- 从当前目录向 `app_path` 逐层查找 `node_modules`；
- 规范化后的应用内路径与模块缓存；
- 容器内建 `events`、`path`、`os`、`fs`、`net`、`http(s)`、`crypto`、`stream`、
  `buffer`、`util`、`url`、`querystring`、`async_hooks` 等已接入子集。

Electron main API 由 bootstrap 提供，包括 `app`、`ipcMain`、`BrowserWindow`、`webContents`、
`session`、`Menu`、`dialog`、`screen` 等当前工程需要的子集。方法是否具备真实
Browser 行为，应以 bootstrap 实现和单测为准，不以同名判定完整兼容。

### 5.2 Renderer

`XenonIpcRenderer` 在托管 Document 的页面脚本前执行 `xenon_ipc_renderer_bootstrap.js`，
注入：

- `require('electron').ipcRenderer` 及常用 renderer Electron API；
- 来自当前 Document 的 `process.execPath`、`appPath`、用户数据目录等；
- CommonJS/Node 兼容模块和 `.node` 代理；
- preload 加载与 preload-error 回传。

运行配置是 Document 级的，不从 renderer 进程命令行推断。同一 renderer 进程承载
多个容器 Document 时，每个 Document 仍会获得自己的 app/exe 语义。

## 6. IPC 语义

| API | 路径 | 返回/失败语义 |
|---|---|---|
| `ipcRenderer.send` | Renderer -> DocumentHost -> Utility `ipcMain.emit` | 异步，无返回值 |
| `ipcRenderer.invoke` | Renderer -> DocumentHost -> Utility `ipcMain.handle` | Promise；普通值或 Promise 均可，throw/rejection 转为 `IpcResult.error` |
| `ipcRenderer.sendSync` | Renderer 面向 Browser 保持同步；Browser -> Utility 使用异步 Mojo reply | handler 通过 `event.returnValue` 返回；返回 Promise 会显式报错 |
| `event.reply` | Utility -> 发起 endpoint 的 IpcRenderer remote | 只回到原 Document |
| `event.sender.send/sendToFrame` | Utility -> 已注册 renderer | 保留 window/process/frame 身份 |
| renderer `.node` 调用 | Browser 只代理绑定，随后 Renderer -> Utility `NodeAddonHost` | 同步构造/方法调用，callback 可跨调用存活 |

Main 端已支持 `ipcMain.on/once/off/removeListener/removeAllListeners`、
`handle/handleOnce/removeHandler`；Renderer 端已支持 `send`、`sendSync`、`invoke`、
`postMessage`、`sendToHost` 和 EventEmitter 监听。`postMessage` 当前不支持 MessagePort
transfer，非空 transfer list 会抛出异常。

`__xenon:fs`、`__xenon:net-request`、`__xenon:sqlite` 和
`__xenon:os-network-interfaces` 是兼容层内部通道，由 Browser 中的专用桥执行，
不是项目业务 IPC。

## 7. BrowserWindow 与 renderer URL 映射

Utility 里的 `new BrowserWindow(options)` 通过同步 window hook 在 Browser UI 线程创建
原生 Widget/WebContents，返回 `window_id` 和 HWND。后续窗口命令使用通用
`ElectronWindowCall(window_id, command, arguments)`，不使用业务方法名。

当 main 调用 `loadURL(file://...)` 时，`mapRendererUrl()` 按配置顺序匹配
`renderer_url_mappings`：

```text
source_path_prefix = D:\app\dist\main-renderer
target_base_url    = chrome://hosted-app/

file:///D:/app/dist/main-renderer/index.html?x=1
  -> chrome://hosted-app/index.html?x=1
```

规则保留 query 和 fragment。具体路径规则必须放在宽泛前缀之前；TH 就是先
将 `main-renderer` 映射到 `chrome://thunder-2025/`，再将其余 renderer 映射到
`renderer.asar`。

BrowserWindow 事件从 Widget/WebContents 反向通知 Utility，包括显隐、激活、尺寸、
最小化/最大化/全屏、关闭、window message 等已实现事件。容器通过
`window_id -> container_id` 保证 TH 和 PL-E 只激活自己的窗口。

## 8. ASAR 与资源模型

当前 ASAR 能力是标准 Electron ASAR 的只读加载器：

- 解析 ASAR header、文件 offset/size、link 和 `unpacked` 文件；
- 支持 `.../renderer.asar/path/in/archive` 虚拟路径；
- WebUI 资源 filter 可直接从 ASAR 读取 renderer 文件；
- `file:` renderer 可经 `renderer_url_mappings` 映射到 ASAR URL；
- 不包含项目专用解密、mock 或文件名补丁。

Main CommonJS 入口当前仍以普通文件或 Browser 传入的 source 执行，不应将
“renderer ASAR 可读”理解为“任意 main/app.asar 均能直接启动”。

## 9. 原生 addon 与运行时隔离

`.node` 先在可阻塞序列上做扩展名、真实路径和安全边界检查，再在 Utility V8
序列初始化。默认只允许加载 Xenon 可执行目录下的 addon；外部开发目录需
显式使用 `--allow-external-xenon-node-addons`。

Windows 上对每个已加载 addon 安装局部 IAT 重定向，只拦截该 addon 自身导入的
`LoadLibraryA/W` 和 `LoadLibraryExA/W`。当 addon 传入的绝对 DLL 路径位于真实
Xenon exe 目录下，且 `runtime_directory` 下存在同一相对路径时，才重定向到
该容器目录。它不全局 hook Windows loader，不改写 `GetModuleFileName()`，也不拦截
`CreateProcess()`。

此机制解决多个项目均含同名但不兼容原生资源时的串用问题，但不取代
正确打包：addon、伴生 DLL、`player/`、`SDK/` 必须来自同一套构建产物。

更完整的 Node-API ABI、callback、libuv 和 player child 细节见
[标准 N-API 扩展加载流程](./标准NAPI扩展加载流程.md)。

## 10. 安全模型

- Utility 因原生 addon 和 JIT 使用 `kNoSandbox`，因此只允许托管受信 main 和 addon。
- Renderer binding 默认不向任意页面开放。Browser 使用 `url::Origin` 对内置映射和
  Browser 启动时接收的开发白名单做精确校验；Renderer 不持有 origin allowlist。
- Renderer 子进程命令行只携带非敏感的 IPC 启用位。安装 bootstrap 前，Renderer
  通过 Document 级 Mojo 请求运行配置；Browser 拒绝时不注入 `require`、`process`
  或 `ipcRenderer`。
- 托管 BrowserWindow 根据已登记 `window_id` 自动获得容器身份，不仅依赖 URL origin。
- 磁盘 frontend filter 拒绝绝对路径和 `..`，只读取配置根目录内文件。
- CORS 解决“该 WebUI origin 能否向外请求”，CSP 解决“页面能加载和执行什么”，
  IPC origin 白名单解决“该 Document 能否连接 main”；三者不可互相替代。
- `--xenon-ipc-allow-all-origins` 和外部 addon 开关只用于开发调试，不应进入发行参数。

`--xenon-ipc-allowed-origins` 和 `--xenon-ipc-allow-all-origins` 是 Browser 启动输入，
不会复制到 Renderer、Utility 或 GPU 子进程命令行。隐藏参数不是安全边界，最终授权
始终由 `XenonIpcDocumentHost` 根据当前 Document 和 BrowserWindow 身份执行。

## 11. 通用工程接入

外部开发工程可使用命令行配置默认容器：

```text
xenon.exe \
  --xenon-electron-app=C:\path\to\electron-app \
  --xenon-electron-ipc \
  --xenon-ipc-allowed-origins=chrome://app-host,https://app.example.com
```

容器读取 `package.json.main`，缺省为 `main.js`。也可直接指定入口：

```text
xenon.exe \
  --xenon-main-js=C:\path\to\main.js \
  --xenon-electron-ipc \
  --xenon-ipc-allowed-origins=chrome://app-host
```

内置项目应在 Browser 启动编排层构造并注册 `IpcMainConfig`，在其产品入口被激活时
调用 `EnsureElectronIpcStarted`。项目接入层只提供：

1. 唯一 `container_id`。
2. 可验证的 main/app/exe/runtime 路径。
3. renderer 路径映射和 WebUI 资源根。
4. origin 到 container 的绑定。
5. 最小 CORS/CSP 策略。

新项目不应在 `XenonIpcMainContainer`、`XenonNodeExecutor` 或 renderer bootstrap 里添加
项目名判断、业务 channel 特判、mock 数据或固定 SDK 路径。

早期 Player 接入形成的兼容技术债（`AplayerWndBind`、`getAplayerWnd`、`__xenonPlayerHostApi__`、
`xmpclient` package 写死以及 `'xmp'` 默认名称兜底）已全部从通用 renderer bootstrap 中清理：

1. **纯净通用 IPC 透传**：`ipcRenderer.send` 仅透明转发 Mojo transport，不再包含任何业务 channel 判断与全局 hostApi 钩子；
2. **纯净通用 Addon 导出代理**：`createMojoExportFunction` 对所有导出函数统一走纯净代理调用，无函数名特判或 HWND 兜底；
3. **动态虚拟 `package.json` 合成**：虚拟挂载（`chrome:\`）对 `package.json` 向上查找按容器的 `hostedAppName` 与 `appVersion` 动态合成，彻底替代了固定 URL 路径与写死 `xmpclient` 的硬编码文件；
4. **窗口与 HWND 能力归位**：HWND 获取与窗口层次管理统一由通用窗口接口（`getNativeWindowHandle` 等）提供，业务进程通过自身 IPC 闭环。

## 12. 内置容器

| 侧边栏 | WebUI | `container_id` | 用途 |
|---|---|---|---|
| XPE | `chrome://xenon-player-by-elec/` | `xenon-ipc-test` | 点击 XPE 后启动；通用 IPC/Node addon 回归测试，使用内置最小 main |
| PL-E | `chrome://xenon-player-electron/` | `xenon-player-test` | 点击 PL-E 后启动；严格执行 Player Electron main + renderer 流程 |
| TH | `chrome://thunder-2025/` | `thunder-2025` | 点击 TH 后启动；执行 Thunder 2025 main，托管主窗口、业务弹窗和播放窗口 |

XPE 测试 `send`、`sendSync`、`invoke`、Main -> Renderer 推送、刷新后 main 状态保持和
Node-API 调用。它是通用容器的测试面，不应从侧边栏或发行包中删除。

TH 与 PL-E 的项目布局、窗口流程和 SDK 约束分别见：

- [Thunder 2025 Electron 容器接入](./thunder_2025接入Electron容器.md)
- [PL-E Electron 容器播控接入](./xenon-player接入Electron播控.md)

## 13. 打包与安装

TH 和 PL-E 资源位于 Xenon exe 同级的非版本目录，因为托管应用和原生 SDK
依赖 exe-relative 布局：

```text
Application/
  xenon.exe
  node.dll
  node.exe
  thunder_2025/
  xenon_player/
  <version>/xenon.dll ...
```

- `chrome.release` 以 `thunder_2025\**` 和 `xenon_player\**` 递归保留工程内相对路径。
- `create_xenon_installer_archive.py` 在进入 Chromium 通用归档脚本前校验关键入口和 SDK 文件。
- 递归目录编目保持子目录结构，并将每个叶子文件记入 depfile。
- Setup 用可回滚 `MoveTreeWorkItem` 将解压根目录的非版本 payload 安装到
  `Application/`；不会将它们遗留在临时解压目录。

本地 `out/Release_64` 和安装目录使用同一相对布局，容器代码不应按绝对
开发路径分支。

## 14. 验证与排查

通用容器回归：

```bat
autoninja -C out\Release_64 ipc_main_container_unittests chrome
out\Release_64\ipc_main_container_unittests.exe
node xenon_overlay\resources\ipc\xenon_ipc_renderer_bootstrap_unittest.cjs
```

重点日志：

- `Registered Electron container configuration: <container_id>`（只注册配置，没有 Utility）
- `Launched isolated Electron container service: <container_id>`
- `Electron container service initialized: <container_id>`
- `RegisterElectronIpcRenderer ... window_id=... endpoint=...`
- `[NapiLoader] Redirecting hosted addon library <old> -> <runtime>`
- `Electron BrowserWindow loadURL id=... url=...`

常见定位顺序：

1. 仅启动 Xenon、尚未点击内置 Electron 入口时，不应存在
   `xenon.mojom.XenonMainService` Utility。
2. main 入口和 `virtual_main_path` 是否存在且匹配。
3. origin 是否映射到正确 `container_id`。
4. BrowserWindow 是否由 main 创建，而非侧边栏另造窗口。
5. `loadURL` 是否命中正确 renderer 映射，WebUI/ASAR 资源是否可读。
6. addon 是否从当前项目目录加载，其伴生 DLL 是否重定向到当前
   `runtime_directory`。
7. 播放子进程是否带 `server-id/client-id/process-id`，并加载当前项目的
   `player/containor.dll`。

`--xenon-electron-ipc` 是当前 Renderer 的全局能力位，因此普通 Chromium WebUI
Renderer 也会显示该参数；它既不是 origin 授权，也不会创建 `XenonMainService`。
最终注入仍由 Browser 的 DocumentHost 校验决定。
