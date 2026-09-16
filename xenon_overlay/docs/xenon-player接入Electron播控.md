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

当前接入分支为 **7.1.35**，完整应用版本为 **7.1.35.173**，源工程：

```text
F:\xl-player\xmp_7.1.35
branch  feature/7.1.35
commit  72e98fd9a46b2338a86831d55557962f184bc9d5
tag     7.1.35_173-win-channel
```

该 tag 与当前 HEAD 完全一致；应用版本的构建号 `173` 来自 tag，写入同步生成的
`main/package.json` 与 `xmp.exe` 版本资源。

2026-09-16 构建使用该提交锁定的四个子模块：

| 子模块 | 提交 |
|---|---|
| `app/src/player-comp` | `c38075f16fead545eb1c8da72ac6e55737bec3ad` |
| `cppsrc/native_core` | `58ee6d3f54daf8ae67712c28c1dd74cfcd109351` |
| `cppsrc/player_comp_native` | `b056f92566b506698f955ad7f31eeec86cef46a7` |
| `setup/xmp_xdas_pack` | `4adfacefaa308f52df2e034c9877ec025ebbf0b9` |

main、frontend、两类 preload 和 `thunder-pan-plugin` 均从上述分支重新构建。
原生使用标准 **x64 Release、`XDAS_OPEN_LOG=OFF`**，完成 `dk_addon`、
`pc_addon`、`player_helper`、`xmp_helper`、`xmp`、`containor` 六个目标。
addon 必须保留标准 N-API 注册入口；`XL_BROWSER_NATIVE_LOAD` 的纯 C 接口产物
不适用于当前 Electron 容器接入。本次源工程工作区保持干净，未修改业务源码。

SDK 以锁定的 `setup/xmp_xdas_pack/ProductRelease` 为基础，叠加本次原生和应用
构建产物，准备到 `out/ple-update-7135-20260916/sdk`。完整同步使用显式路径和版本：

```bat
vpython3 xenon_overlay\tools\sync_xenon_player.py ^
  --src F:\xl-player\xmp_7.1.35\app\build ^
  --out out\Release_64 ^
  --player-sdk out\ple-update-7135-20260916\sdk ^
  --native-dir F:\xl-player\xmp_7.1.35\cppsrc\build\Release ^
  --app-version 7.1.35.173
```

同步源资源时，将同一命令的 `--out` 改为 `xenon_overlay\resources`。
`--app-version` 写入生成的 `main/package.json`，必须与本次应用版本一致。
脚本也复制 SDK 根目录的伴随 DLL、证书、XML、图标、VSR 和明确允许的辅助程序，
保留 `player/SDK/Res/resources` 目录，再用新构建的 native/player 覆盖对应产物。

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
  --src F:\xl-player\xmp_7.1.35\app\build\main-renderer ^
  --dst out\Release_64\xenon_player\frontend
```

本次完整发布清单位于
`out/ple-update-7135-20260916/staged/xenon_player/release-manifest.json`，记录源码、
子模块、构建配置及逐文件 SHA-256。更新源资源和运行目录时必须使用同一份产物，
不能只替换 frontend 后沿用其他版本的 addon 或 SDK。

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
<宿主 exe 目录>/xenon_player/frontend
```

读取 renderer。开发时可用：

```text
--xenon-player-frontend-dir=F:\xl-player\xmp_7.1.35\app\build\main-renderer
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

### preload 构建产物校验

原工程的 `asar/security-asar/lib/crawlfs.js` 会在打包时原地加密 `app/build`
中的脚本。加密后的 `preload/index.js` 不能直接作为 CommonJS 源码执行；这会在
renderer 报 `SyntaxError: Invalid or unexpected token`，与 Windows 路径无关。

`sync_xenon_player.py` 现在先校验所有 `preload` / `preload-native` JavaScript。
遇到编码或语法无效的脚本，使用原工程自带的 ASAR 工具读取匹配归档中的源码，
并再次校验 UTF-8 和 CommonJS 编译。只有归档中保存的原始字节与源文件完全一致
才接受提取结果，防止混用另一版本的 preload。缺少归档、工具或源码无效时，
同步在删除旧运行目录之前失败。工具的密钥和私有格式不进入通用运行时。

默认归档为 `<player-sdk>/resources/app/out.asar`，工具为
`<src>/../asar/security-asar/lib/asar.js`；可用 `--source-archive` 和
`--vendor-asar-module` 指定实际位置。这项规范化限于 preload，完整同步仍要求
main 和 frontend 使用可执行的正常源产物。

历史修复（2026-09-16，升级 7.1.35 之前）：当时两份加密 preload 与下列原工程
产物的 SHA-256 完全一致：
`F:/xl-player/xmp_xdas_2_1/xmp_xdas_2/app/build`。若仅修复已安装的 preload，
保留现有 main/frontend，且仍满足上述原始字节完全匹配条件，可使用当时的命令：

```powershell
node xenon_overlay/tools/normalize_player_preloads.cjs `
  out/Release_64/xenon_player/main `
  F:/xl-player/xmp_xdas_2_1/xmp_xdas_2/bin/resources/app/out.asar `
  F:/xl-player/xmp_xdas_2_1/xmp_xdas_2/app/asar/security-asar/lib/asar.js `
  --write-to out/Release_64/xenon_player/main
```

该命令不运行商业源码，所有文件校验完成后才写入目标；结果列出源文件及输出
SHA-256。首次修复记录位于 `out/ple-preload-normalization-20260916.json`。
当前 7.1.35 的 preload 来自本次正常源码构建，不使用这份历史归档替换。
重新创建 PL-E 页面后才会执行恢复后的 preload。可选真实脚本回归明确使用
Electron mock，仅验证 preload 导出与 IPC 转发，不代表窗口或播放验证：

```powershell
$env:XENON_PLAYER_PRELOAD_ROOT = (Resolve-Path out/Release_64/xenon_player/main).Path
node --test xenon_overlay/tools/normalize_player_preloads_unittest.cjs
```

### 6.1 早期 Player 兼容钩子的清理与架构解耦

早期在通用 renderer bootstrap 中遗留的四组兼容技术债：

- 收到 `AplayerWndBind` 时特判缓存 native player HWND；
- 调用 addon 的 `getAplayerWnd` 时特判函数名并注入 3000ms 超时兜底；
- 通过全局 `__xenonPlayerHostApi__` 钩子向宿主分发 IPC；
- 写入硬编码路径与写死 `xmpclient` 的 `package.json`。

现已全数清理并完成通用化：
- `ipcRenderer.send` 纯净透传，无业务 channel 特判；
- Node addon 导出代理统一处理，无函数名分支；
- 虚拟挂载对 `package.json` 的查询统一按容器运行时配置动态合成，无需写死文件；
- HWND 获取与窗口层叠统一由通用窗口接口与 Native Window Handle 体系提供，业务 IPC 自闭环。

## 7. 原生播放链路

### 7.1 addon 与 SDK

PL-E 的 `XenonNodeExecutor` 使用 `xenon_player/main` 作为当前容器运行根。addon
通过 `GetModuleFileName(NULL)` 得到真实 Xenon 进程路径并拼接伴生 DLL 时，容器只对
该 addon 的 `LoadLibraryA/W` 和 `LoadLibraryExA/W` 导入做局部重定向：

```text
<宿主 exe 目录>/SDK/<library>
  -> <宿主 exe 目录>/xenon_player/main/SDK/<library>
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

子进程使用当前宿主可执行文件，本构建为 `xlb153.exe`，不会启动 `xmp.exe`。
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

### 7.4 菜单窗口背景

7.1.35 的原画、字幕、选集使用独立菜单窗口。此次白底异常来自容器未将
`BrowserWindow` 声明的背景色完整传到实际页面和视图。修复在通用窗口层将背景色
传递到 WebContents、页面底色及视图，使透明菜单按应用声明显示。

底层 `RenderWidgetHostView` 的背景色仅接受完全透明或完全不透明；半透明输入
在这一层归一到支持的透明/不透明值，避免触发 Chromium 的 `CHECK`。
修复位于 Electron 适配层，未修改 PL-E 业务包。

### 7.5 Windows 窗口拖动（后续修复）

新版 Windows 播放器从 CSS drag 改为经 `WM_MOUSEMOVE` 手动拖动。实际日志出现
124 次 `Buffer.readInt16LE` 缺失，导致鼠标消息解析中断；main 和 renderer 已补齐
对应 Buffer 能力。`screen` 原先返回固定光标坐标 0 和屏幕尺寸 1920×1080，现将
五个查询转发到 Browser UI 线程读取真实屏幕与光标状态。

本轮 **JS 270/270、零跳过**，覆盖有符号 Buffer、64 位窗口消息 hook 和实时
screen 转发；完整生产构建 **229/229 成功**。最终在 9222 复验 PL-E 7.1.35.173，
用户明确确认“可以拖动了”。页面观测捕获 `isTrusted: true` 的标题栏按下
（`buttons: 1`）、多次移动和抬起事件；首次拖动的窗口坐标从 `(740, 472)` 变为
`(1003, 616)`，随后多轮操作也有坐标变化。**实际拖动验收通过**，依据为用户
操作与页面观测；本次结论限于当前桌面环境，未覆盖所有 DPI 和多屏组合。

同轮 TH 为 `sdkInitReady: true`、`accountInitState: 2`；播放探针 `passed: true`，
进度 `229 → 1075 ms`，验证后已暂停。最终日志记录容器启动 2 次；窗口事件、
`readInt16LE`、screen 查询错误、fatal、断连及 N-API 失败均为 0。
观测结束后已清理临时监听器和轮询。

记录位于 `out/ple-update-7135-20260916/`：原始错误与构建测试见
`drag-errors-before.json`、`drag-js-tests.log`、`drag-production-build.log`；
最终验收见 `drag-revalidation-observation.jsonl`、`drag-revalidation-playback.jsonl`、
`th-drag-revalidation.json` 和 `chrome-debug-drag-revalidation.log`。
摘要 `drag-revalidation-result.json` 记录 `passed: true`、80 条鼠标事件及 35 次位置变化。
此项作为后续拖动回归单独记录，此前菜单背景验收结论保持有效。

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

### 7.1.35 构建与同步验证（2026-09-16）

| 项目 | 结果 | 记录 |
|---|---|---|
| frontend/main/preload 与插件 | 从指定分支完成构建 | `out/ple-update-7135-20260916/frontend-build.log`、`plugin-build.log` |
| 六个原生目标 | 标准 x64 Release 构建完成 | `out/ple-update-7135-20260916/native-build.log` |
| 原生接口和 PE 导入 | 四个 addon 保留 N-API 注册，未使用纯 C bridge；宿主导入符号缺失数为 0 | `out/ple-update-7135-20260916/native-contract.json` |
| 源资源与运行目录 | 242 个文件已同步至 `xenon_overlay/resources/xenon_player` 和 `out/Release_64/xenon_player`，SHA-256 全部一致 | `out/ple-update-7135-20260916/staged/xenon_player/release-manifest.json` |
| preload 回归 | **8/8 通过** | 实际构建产物的 preload 检查 |
| 实际消费代码回归 | **20/20 通过，零跳过** | `out/ple-update-7135-20260916/consumer-tests-7135.log` |
| 背景修复后 JS 回归 | **263/263 通过，零跳过**；包含新增 4 项背景色契约测试 | `out/ple-update-7135-20260916/background-js-tests.log` |
| 背景修复后生产构建 | **228/228 完成**，浏览器重新链接 | `out/ple-update-7135-20260916/background-alpha-build.log` |
| 9222 实际播放 | **通过**；最终探针 `passed: true`，媒体路径匹配、错误码 0、时长 24109 ms、视频尺寸 320×180，进度 `0 → 227 → 1059 ms` | `out/ple-update-7135-20260916/background-playback-probe.jsonl` |
| 原画、字幕、选集菜单 | **用户验收通过**，明确反馈“界面正常了”；单张 CDP 截图确认独立画面菜单的白底消失 | `out/ple-update-7135-20260916/menu-quality-background.png`；用户实际交互确认 |
| TH 初始化复验 | `sdkInitReady: true`、`accountInitState: 2` | `out/ple-update-7135-20260916/th-background-acceptance.json` |
| 最终运行日志 | 容器启动 2 次，断连 0 次、fatal 0 次、N-API 失败 0 次 | `out/ple-update-7135-20260916/chrome-debug-background.log` |

本次升级、播放及所反馈菜单问题已完成验收。菜单结论来自用户实际交互确认与
一张独立菜单截图；自动 hover 探针受到播放结束和用户切换界面影响，出现
`Missing button`，未逐个完成三个菜单的截图，不能将其报告为自动化全部通过。
独立发布核验和最终验收汇总见
`out/ple-update-7135-20260916/independent-release-verification-20260916.json`。

播放状态依据源工程 `ui/consts/playerConsts.ts` 的真实契约判断，状态 `4` 和 `2`
均属于播放态。初版探针仅接受 `4`，原记录因此带有 `passed: false`；检查实际
状态定义后修正探针接受 `[2, 4]`，保留原记录并单独保存判定，不修改播放器运行时。
首次记录的进度为 `308 → 1173 → 2239 → 3305 → 4374 → 5446 → 6521 ms`；背景
修复后再次播放得到上表的成功探针。播放结论覆盖本次测试媒体，不代表全部格式
和在线来源均已验证。

### 启动与检查

启动：

```bat
out\Release_64\xlb153.exe --remote-debugging-port=9222 --enable-logging
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
