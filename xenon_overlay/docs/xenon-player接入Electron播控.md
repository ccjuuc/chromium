# PL-E Electron 容器播控接入

## 1. 定位

`PL-E` 侧边栏入口严格执行 Player Electron 的 main + renderer 流程：

```text
WebUI             chrome://xenon-player-electron/
container_id      xenon-player-test
main              xenon_player/main/main.js
renderer          xenon_player/frontend/
runtime_directory xenon_player/main/
```

`PL-E` 不是 `XP` (`chrome://xenon-player/`) 早期播控验证页，也不是 `XPE`
(`chrome://xenon-player-by-elec/`) 通用 IPC 测试页。

接入原则：

- main 代码由独立 Utility Process 执行，不在 renderer 中模拟 main。
- renderer 使用原工程编译产物，容器只提供 Electron/Node 兼容能力和 WebUI 托管。
- `xmp.exe` 仅是应用身份，不会被容器或播放子进程启动。
- 在线与本地播放都使用 PL-E 自己的 addon、player 和 SDK，不借用 TH 资源。

通用进程、IPC、BrowserWindow、ASAR 和安全架构见
[Electron IPC 容器架构与接入](./Electron_IPC容器设计与接入.md)。

## 2. 源产物与同步

当前源工程：

```text
F:\xl-player\xmp_xdas_2
```

完整同步：

```bat
vpython3 xenon_overlay\tools\sync_xenon_player.py ^
  --src F:\xl-player\xmp_xdas_2\app\build ^
  --out out\Release_64 ^
  --player-sdk F:\xl-player\xmp_xdas_2\bin ^
  --native-dir F:\xl-player\xmp_xdas_2\cppsrc\build\Release
```

`--native-dir` 未指定时，脚本优先使用 `<project>/cppsrc/build/Release`；该目录
不存在时才回退到 `--player-sdk`。这保证新编译的 addon/player 优先覆盖已发布
SDK 中的旧产物。

脚本要求：

- `app/build/main.js` 和 `app/build/main-renderer/` 存在；
- SDK 中 `xmp.exe` 存在；
- native 目录中 `dk_addon.node`、`pc_addon.node`、`player_helper.node`、
  `xmp_helper.node` 齐全。

只更新 renderer 时可使用：

```bat
vpython3 xenon_overlay\tools\sync_xenon_player_frontend.py ^
  --src F:\xl-player\xmp_xdas_2\app\build\main-renderer ^
  --dst out\Release_64\xenon_player\frontend
```

## 3. 目录结构

```text
xenon_player/
  main/
    main.js
    <webpack chunks>
    package.json                  # main = main.js
    xmp.exe                       # 应用身份，不启动
    preload/
    preload-native/
    public/
    static/
    dk_addon.node
    pc_addon.node
    player_helper.node
    xmp_helper.node
    build/
      <addon copies>
      Release/
        <addon copies>
    player/
      containor.dll
      ...
    SDK/
    Res/
    resources/
    <companion DLLs>
  frontend/
    index.html
    static/
```

addon 在 main 根、`build/` 和 `build/Release/` 保留副本，是为了覆盖原 Player
main/preload 的多种标准相对 `require()` 路径；不是让不同项目共享 addon。三处必须
来自同一次 native 同步。

本地与安装后使用相同相对结构：

```text
out/Release_64/xenon_player
C:\Users\<user>\AppData\Local\xenon\Application\xenon_player
```

## 4. 启动时序

```text
点击侧边栏 PL-E
  -> XenonWebDialog::ShowXenonPlayerElectron()
  -> 若容器还没有 BrowserWindow，读取 main/main.js
  -> InitializeElectronIpc(container_id = xenon-player-test)
  -> Utility 加载 Player main
  -> app.whenReady()
  -> Player main 自己 new BrowserWindow(...)
  -> Browser 创建原生 Widget/WebContents
  -> main loadURL(file://.../main-renderer/...)
  -> renderer_base_url 重写为 chrome://xenon-player-electron/
  -> WebUI 从 xenon_player/frontend 读取原 renderer 产物
```

启动配置：

```text
container_id       = xenon-player-test
virtual_main_path  = <exe>/xenon_player/main/main.js
app_path           = <exe>/xenon_player/main
executable_path    = <exe>/xenon_player/main/xmp.exe
runtime_directory  = <exe>/xenon_player/main
renderer_base_url  = chrome://xenon-player-electron/
app_name           = xmp
```

侧边栏只触发容器初始化或激活已存在窗口，不另造一个空白播放页。
直接在普通 tab 打开 `chrome://xenon-player-electron/` 不是正常启动流程；
`PreparePlayerHost` 会提示应从侧边栏打开。

## 5. Frontend 托管与安全策略

`XenonPlayerElectronController` 默认从：

```text
<xenon.exe dir>/xenon_player/frontend
```

读取 renderer。开发时可用：

```text
--xenon-player-frontend-dir=F:\xl-player\xmp_xdas_2\app\build\main-renderer
```

显式覆盖。资源 filter 的边界：

- 只接受 frontend 根目录下的相对路径；
- 拒绝绝对路径和 `..`；
- 不拦截 Chromium 打包的 Mojo/require/bootstrap 资源；
- 支持普通目录和 ASAR 内资源，但 PL-E 当前使用解包的 `frontend/`。

WebUI 对 `xunlei.com` 的 HTTP/HTTPS 子域设置 CORS origin access，同时配置 script、
style、connect、image、media 和 font CSP。CORS、CSP 和 Electron IPC origin 授权是
三套独立策略。

## 6. Renderer 和 preload

BrowserWindow Document 提交后，`xenon_ipc_renderer_bootstrap.js` 在页面代码前安装：

- `require('electron').ipcRenderer`；
- `process.execPath = .../xenon_player/main/xmp.exe` 等 Document 级运行配置；
- Node/CommonJS 兼容模块；
- `.node` 远程导出、class/instance 和 callback 代理；
- BrowserWindow 配置的 preload。

renderer 窗口会注册独立 endpoint 和 `window_id`，因此 `ipcMain` 可以用
`event.sender`、`BrowserWindow.fromWebContents()` 和 `event.reply()` 准确返回发起页。

### 6.1 既存 Player 兼容钩子

当前通用 renderer bootstrap 中仍有四组由早期 PL-E 接入遗留的兼容逻辑：

- 收到 `AplayerWndBind` 时缓存 native player HWND；
- 调用 addon 的 `getAplayerWnd` 时，在异步结果之外使用已缓存 HWND 兜底；
- 通过 `__xenonPlayerHostApi__` 将 player HWND 同步给 Browser 侧 host；
- 在缺少 package 元数据时提供 `xmpclient` fallback。

这些钩子解释了当前 PL-E 的 HWND 绑定时序，但不属于新容器接入规范。不得为 TH
或后续项目继续添加类似分支，也不应把它们当作播放成功状态的 mock。后续重构目标是
将上述行为移动到 PL-E 专用 host adapter，renderer bootstrap 只保留通用 IPC、
CommonJS 和 addon 代理。

## 7. 原生播放链路

### 7.1 addon 与 SDK

PL-E 的 `XenonNodeExecutor` 使用 `xenon_player/main` 作为当前容器运行根。addon
通过 `GetModuleFileName(NULL)` 得到真实 Xenon 进程路径并拼接伴生 DLL 时，容器只对
该 addon 的 `LoadLibraryA/W` 和 `LoadLibraryExA/W` 导入做局部重定向：

```text
<xenon.exe dir>/SDK/<library>
  -> <xenon.exe dir>/xenon_player/main/SDK/<library>
```

只有目标文件存在时才重定向。该规则没有 PL-E/TH 名称或 SDK 文件名特判，
也不会修改其他 module 的 Windows loader。

在线播放依赖 `dk_addon.node` 及同版本 `SDK/DownloadSDKProxy.dll`、
`AssistantTools.dll` 等资源。如果 addon 与 SDK 版本不一致，常见表现是任务在
`taskStatus=9` 与中间状态之间循环、`playUrl` 为空，即使原始视频 URL
返回 HTTP 200 也无法播放。

### 7.2 player child 与 containor.dll

`pc_addon.node` 会用原约定的三个参数启动当前宿主程序：

```text
--server-id=... --client-id=... --process-id=...
```

因为当前宿主是 Xenon，子进程也是 `xenon.exe`，不是 `xmp.exe`。
`chrome_exe_main_win.cc` 在 Chromium 正常启动前识别三个参数；
`player_container_process_win.cc` 读取继承的 `XENON_HOSTED_APP_DIR`，加载：

```text
xenon_player/main/player/containor.dll!InitContainor
```

普通 Browser、Renderer 和 Utility 进程不会进入这条快速路径。

### 7.3 视频 HWND 与控制窗口

`PreparePlayerHost` 在 Browser UI 线程异步创建黑色 player host Widget，并让 PL-E
控制 WebDialog 成为其 owned window。播放器 addon 返回有效视频 HWND 后，容器将它
挂到 host client area，并在控制窗口移动、缩放、最小化和显隐时同步：

```text
player host Widget (video parent)
  |- native video HWND
  `- owned PL-E control WebDialog
```

这一层只处理 HWND 层级、bounds 和可见性，不修改播放 URL、标题、时间、选集或
Download SDK 业务数据。

## 8. 本地文件与在线 URL

- 本地文件选择由 `dialog.showOpenDialog` 通过 Browser UI 线程显示真实原生对话框。
- 在线 URL 由 PL-E main/renderer 和 `dk_addon` 原流程处理；容器不改写 URL，不伪造
  `playUrl`。
- 标题、时间、选集和播放错误 UI 属于 PL-E renderer；容器只保证 IPC 和 addon
  callback 能回到对应 Document。

## 9. 打包与安装

`chrome.release` 使用：

```text
xenon_player\**: %(ChromeDir)s\xenon_player\
```

mini installer 在归档前至少校验：

- `xenon_player/main/main.js`；
- `xenon_player/main/xmp.exe`；
- `xenon_player/main/pc_addon.node`；
- `xenon_player/main/player/containor.dll`；
- `xenon_player/frontend/index.html`。

安装时 `xenon_player` 作为 Xenon exe 同级的可回滚非版本 payload 移动。容器使用
相对 exe 布局，因此本地构建与安装版不存在两套路径逻辑。

## 10. 验证与排查

启动：

```bat
out\Release_64\xenon.exe --remote-debugging-port=9222 --enable-logging
```

检查顺序：

1. 从侧边栏 `PL-E` 打开，确认由 Player main 创建 BrowserWindow。
2. 日志中确认 `xenon-player-test` Utility 容器启动成功。
3. 确认 frontend 是 `<exe>/xenon_player/frontend`，没有从开发目录或 TH 目录读取。
4. 确认 addon 从 `xenon_player/main` 加载。
5. 在线播放时查看 `[NapiLoader] Redirecting hosted addon library`，目标应是
   `xenon_player/main/SDK`。
6. player child 应加载 `xenon_player/main/player/containor.dll`，不应启动 `xmp.exe`。
7. 分别验证本地文件与 HTTP/HTTPS 视频；两者共享播放器窗口链路，但媒体
   任务创建路径不同。

通用回归：

```bat
autoninja -C out\Release_64 ipc_main_container_unittests chrome
out\Release_64\ipc_main_container_unittests.exe
```

不应通过在 renderer 注入播放 mock、固定在线地址、强制成功状态或复用 TH SDK 来
修复 PL-E。定位应从容器、renderer endpoint、addon、SDK 和 player child 逐层进行。
