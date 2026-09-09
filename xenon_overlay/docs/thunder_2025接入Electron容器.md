# Thunder 2025 Electron 容器接入

## 1. 接入目标

侧边栏 `TH` 运行 Thunder 2025 已编译的 Electron main、renderer 和原生 SDK，
容器标识为 `thunder-2025`，主 renderer origin 为 `chrome://thunder-2025/`。

接入原则：

- 不启动 Thunder Electron runtime；main 在 Xenon 独立 Utility Process 内执行。
- 不改写 Thunder main/renderer 的启动流程；兼容工作由通用 Electron 容器完成。
- 保留 Thunder Electron 发行目录语义，避免破坏 `__dirname`、`process.execPath`、
  `resources/app/plugins` 和 SDK 的相对路径。
- TH 和 PL-E 使用各自的 addon、`player/`、`SDK/` 和 `containor.dll`，不共享
  同名原生资源。

通用容器架构见 [Electron IPC 容器架构与接入](./Electron_IPC容器设计与接入.md)。

## 2. 源产物与同步

当前事实源：

| 内容 | 源目录 |
|---|---|
| main 与 renderer 编译产物 | `F:\thunder_2025\app\dist` |
| Thunder 原生运行时与 player SDK | `F:\thunder_2025\bin\Release` |
| 同步脚本 | `xenon_overlay/tools/sync_thunder_2025.py` |

同步命令：

```bat
vpython3 xenon_overlay\tools\sync_thunder_2025.py ^
  --src F:\thunder_2025\app\dist ^
  --out out\Release_64 ^
  --player-sdk F:\thunder_2025\bin\Release
```

默认过滤 source map。只在需要调试 renderer 时使用 `--include-maps`。
`--skip-asar` 仅用于调试同步流程；正式打包必须生成 `renderer.asar`。

脚本会校验：

- `dist/main.js` 和 `dist/main-renderer/` 存在；
- `thunder.exe` 存在；
- `dk_addon.node`、`pc_addon.node`、`player_helper.node`、`thunder_helper.node`、
  `lkhb.node` 齐全；
- `player/` 和 `SDK/` 两个原生运行目录齐全。

每次同步会重建 `out/Release_64/thunder_2025`，并清理旧版
`thunder_2025_main`、`thunder_2025_frontend`、`thunder_2025_resources.asar` 和根目录
`Thunder.exe`。这些旧路径不再是运行时输入。

## 3. 当前目录结构

```text
thunder_2025/
  Thunder.exe                         # 应用身份，容器不启动它
  *.node                              # Thunder/player 原生 addon
  *.dll
  player/
    APlayer.dll
    containor.dll
    ...
  SDK/
  service/
  addins/
  resources/
    app/
      package.json                    # main = ./out/main.js
      out/
        main.js
        <webpack chunks>
        preload/
      Release/
        node_sqlite3.node             # 存在时保留 Electron 原路径
      plugins/
        player-plugin.asar
        thunder-pan-plugin.asar
      renderer.asar
        main-renderer/
        modal-renderer/
        suspension-renderer/
        thunder-im/
        static/
```

这个布局是有意对齐 Thunder 原 Electron 发行包，不是遗留文件混放：

- `Thunder.exe` 与 addon/SDK 同级，保持原生模块的 exe-relative 语义。
- main 位于 `resources/app/out/main.js`，保持 `__dirname` 与 Thunder 已编译路径一致。
- 播放器插件位于 `resources/app/plugins`，因为 main 按 `__dirname/../plugins` 定位。
- renderer 合并到标准只读 ASAR，避免 main/frontend/ASAR 三份重复资源。

PL-E 的 `xenon_player/main + frontend` 是另一种项目布局；它不是要求所有 Electron
应用使用的通用磁盘格式。两者在运行时都使用同一套 `IpcMainConfig` 和容器架构。

## 4. 启动配置

TH 在初始 Profile 就绪后由 `XenonBrowserMainExtraParts` 自动发现。只有
`thunder_2025/resources/app/out/main.js` 可读时才注册配置；注册不创建 Utility，
用户点击侧边栏 `TH` 后才初始化容器并执行 main。

```text
container_id       = thunder-2025
virtual_main_path  = <exe>/thunder_2025/resources/app/out/main.js
app_path           = <exe>/thunder_2025/resources/app
executable_path    = <exe>/thunder_2025/Thunder.exe
runtime_directory  = <empty> -> defaults to executable_path.DirName()
app_name           = Thunder
```

有效 `runtime_directory` 因此是 `<exe>/thunder_2025`。`Thunder.exe` 仅提供
`process.execPath`、`app.getPath('exe')` 和版本资源；真正进程是 Xenon 的 Utility
service，不会另起 Thunder.exe。

## 5. Renderer 与 ASAR 映射

Thunder production main 按以下语义构造 URL：

```text
<main __dirname>/main-renderer/index.html
<main __dirname>/modal-renderer/index.html
<main __dirname>/suspension-renderer/index.html
```

容器配置两条有序映射：

| 顺序 | 原路径前缀 | 目标 |
|---|---|---|
| 1 | `resources/app/out/main-renderer` | `chrome://thunder-2025/` |
| 2 | `resources/app/out` | `file:///.../resources/app/renderer.asar/` |

第一条更具体，必须先匹配：主窗口使用受信 WebUI origin，从
`renderer.asar/main-renderer` 读资源。第二条作为其余 renderer 的 catch-all，保留
modal、suspension 和 IM 的原 file URL 语义。query/fragment（包括 `boxId`、`ph`、
`ch`）在映射后保留。

`XenonThunder2025Config` 复用 `XenonPlayerElectronController` 的通用磁盘/ASAR 资源
filter，默认 frontend 根为：

```text
thunder_2025/resources/app/renderer.asar/main-renderer
```

开发时可用 `--xenon-thunder-2025-frontend-dir=<path>` 显式覆盖。路径不存在时
不回退到未知目录，而是保持空值并记录错误。

## 6. 窗口流程

```text
Xenon 启动
  -> 注册 thunder-2025 IpcMainConfig
  -> 不启动 Utility，不执行 main

点击侧边栏 TH
  -> EnsureElectronIpcStarted("thunder-2025")
  -> Utility 执行 Thunder main
  -> Thunder main 在 app.whenReady() 创建 BrowserWindow
  -> Browser 创建真实 Widget/WebContents
  -> ActivateForContainer("thunder-2025") 激活当前或即将创建的主窗口
```

侧边栏不额外构造 TH 主窗口，否则会出现“两个窗口，第二个空白”。
TH 的播放窗口也必须由 Thunder main 在播放时创建，不应在容器启动时预创建。

BrowserWindow 的显隐、激活、bounds、父子关系和 Windows message hook 都通过
`XenonElectronWindowHost` 通用命令桥实现，没有 TH 业务方法。

## 7. 登录、UA 与用户数据

- `default_user_agent` 由 Browser 基于应用名/版本与 Chromium UA 生成，main 和 renderer
  使用同一容器配置。
- renderer 的 `process.execPath`、app 路径和用户数据目录通过 Document 级
  `GetRuntimeConfig()` 获取，不使用进程级推断。
- `app.getPath('userData')` 使用 Xenon 默认用户数据目录，不放在
  `Application/thunder_2025` 运行时目录。安装升级因此不应删除登录状态。
- 扫码、账密登录和自动登录都应使用 Thunder 自身事件/轮询；容器不注入登录
  mock，也不修改登录回调数据。

## 8. 原生 SDK 与播放

TH 的 `runtime_directory` 是 `thunder_2025` 根目录，因此：

- addon 从 `thunder_2025/*.node` 加载；
- addon 基于宿主 exe 构造的绝对 DLL 路径，在目标文件存在时被局部重定向到
  `thunder_2025` 的同一相对路径；
- addon 启动 Xenon player child 时，Utility 继承的 `XENON_HOSTED_APP_DIR` 使 child 加载
  `thunder_2025/player/containor.dll`；
- `Thunder.exe` 不是 player child，也不需要由容器启动。

addon、伴生 DLL、`player/`、`SDK/` 必须来自同一套 Thunder 构建。即使 TH 和
PL-E 的文件名一致，ABI、导出、配置和 Download SDK 版本也可能不一致，不能相互
覆盖。

## 9. 打包与安装

`chrome.release` 使用：

```text
thunder_2025\**: %(ChromeDir)s\thunder_2025\
```

因此本地构建目录：

```text
out/Release_64/thunder_2025
```

安装后必须原样成为：

```text
C:\Users\<user>\AppData\Local\xenon\Application\thunder_2025
```

mini installer 在归档前校验 `Thunder.exe`、main、renderer ASAR、plugins、addons、
`player/APlayer.dll` 和 `SDK/DownloadSDKServer.exe` 等关键产物。Setup 将该非版本目录
作为可回滚的根目录 payload 安装，不放入 Chromium 版本号目录。

## 10. 验证与故障定位

同步后首先检查：

```bat
dir out\Release_64\thunder_2025\resources\app\out\main.js
dir out\Release_64\thunder_2025\resources\app\renderer.asar
dir out\Release_64\thunder_2025\player\containor.dll
dir out\Release_64\thunder_2025\SDK\DownloadSDKServer.exe
```

启动调试：

```bat
out\Release_64\xenon.exe --remote-debugging-port=9222 --enable-logging
```

定位顺序：

1. 日志是否出现 `Electron container service initialized: thunder-2025`。
2. main 是否读取 `resources/app/out/main.js`。
3. 主窗口 URL 是否映射到 `chrome://thunder-2025/`，其余弹窗是否映射到
   `renderer.asar`。
4. renderer endpoint 的 `window_id` 是否大于 0，否则 `BrowserWindow.fromWebContents()`
   无法反查所属窗口。
5. addon 路径是否位于 `thunder_2025`，是否意外加载了 `xenon_player` 或 Xenon 根
   `SDK`。
6. player child 是否带 `server-id/client-id/process-id`，并从 TH 目录加载
   `player/containor.dll`。

修复顺序应是“源产物 -> 同步布局 -> 容器配置 -> URL 映射 -> addon/SDK”。
不应通过修改 TH 业务参数或添加播放/登录 mock 掩盖容器问题。
