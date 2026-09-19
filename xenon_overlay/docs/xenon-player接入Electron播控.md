# PLE Electron 容器播控接入

## 1. 当前接入方式

侧栏 `PL-E` 使用容器 `xenon-player-test` 执行 Player Electron 的原始 main，由应用创建播放器及工具窗口。`chrome://xenon-player-electron/` 是启动入口，实际应用页面保持发行归档内的 `file://` URL。

main 在独立 Utility Process 中执行，renderer、preload 和原生模块使用同一份发行制品。`xmp.exe` 提供应用身份，不由宿主另行启动。完整目录导入、宿主配置及一致性检查见 [Electron 应用目录接入](./电子应用目录接入.md)，进程和 IPC 设计见 [Electron IPC 容器架构与接入](./Electron_IPC容器设计与接入.md)。

该入口与 `XP` 早期播控验证页、`XPE` 通用 IPC 测试页相互独立。兼容层只提供已经实现的 Electron/Node API 和兼容 native 加载能力。

## 2. 版本与来源

当前保留已验证的 **7.1.35.173**：

```text
工程    F:/xl-player/xmp_7.1.35
分支    feature/7.1.35
提交    72e98fd9a46b2338a86831d55557962f184bc9d5
标签    7.1.35_173-win-channel
```

固定的子模块为：

| 子模块 | 提交 |
| --- | --- |
| `app/src/player-comp` | `c38075f16fead545eb1c8da72ac6e55737bec3ad` |
| `cppsrc/native_core` | `58ee6d3f54daf8ae67712c28c1dd74cfcd109351` |
| `cppsrc/player_comp_native` | `b056f92566b506698f955ad7f31eeec86cef46a7` |
| `setup/xmp_xdas_pack` | `4adfacefaa308f52df2e034c9877ec025ebbf0b9` |

main、renderer、两类 preload 及网盘插件来自该版本构建；四个主 addon 和 `player/containor.dll` 来自 `cppsrc/build/Release`。原生构建为 x64 Release、标准 N-API、`XDAS_OPEN_LOG=OFF`，不能改用仅导出纯 C bridge 的产物。

SDK 以固定的 `setup/xmp_xdas_pack/ProductRelease` 为基础，叠加上述新版应用及原生产物。该目录以及同级 Release 都只是 SDK 底包：旧 `package.json` 标记 `12.1.4.1340`，旧 `out.asar` 的主脚本、preload、renderer 与 7.1.35 不同，并且没有新版 `preload-native`。不能把它直接镜像为完整新版应用。

当前保留最新加密 `out.asar`；抽样主脚本、两类 preload 和 renderer 解密内容与最新 `app/build` 一致，仅含格式规定的空格补齐。四个主 addon 及 `containor.dll` 与新 native 构建相同；`xmp.exe` 已按发布 tag 将版本资源盖章为 `7.1.35.173`，不能用构建目录中仍标记 `7.0.0.8` 的 executable 盲目覆盖。

源码、子模块、构建配置及当前目录逐文件 SHA-256 记录在外置 [xenon_player.release-manifest.json](../resources/xenon_player.release-manifest.json)。

## 3. 当前目录与配置

```text
resources/                           # 构建后对应宿主 exe 所在目录
├─ xenon_player.xenon.json
├─ xenon_player.asar-public-key.pem
├─ xenon_player.release-manifest.json
└─ xenon_player/
   ├─ xmp.exe
   ├─ dk_addon.node / pc_addon.node
   ├─ player_helper.node / xmp_helper.node
   ├─ player/containor.dll、播放器运行库
   ├─ SDK/ / Res/ / 伴随 DLL 与辅助程序
   └─ resources/app/
      ├─ package.json                # main = ./out.asar/main.js
      ├─ out.asar                    # main、chunks、preload、renderer 等
      ├─ Release/node_sqlite3.node
      └─ plugins/
```

此前 `main/` 层和独立 `frontend/` 已移除。主 addon 只保留发行根的一份；此前 `build/` 和 `build/Release/` 镜像已删除。main、两类 preload、renderer 随同一个完整加密归档读取，不再分别修补或转换为 loose 源码。

当前 [xenon_player.xenon.json](../resources/xenon_player.xenon.json)：

```json
{
  "application": "xenon_player",
  "executable": "xenon_player/xmp.exe",
  "name": "xmp",
  "archivePublicKey": "xenon_player.asar-public-key.pem",
  "parentWindowPairing": [
    "out.asar/main-renderer/clipper.html",
    "out.asar/main-renderer/gifClipper.html"
  ]
}
```

应用、executable 和公钥路径相对于 JSON 目录。加载器解析出应用根 `xenon_player/resources/app`、运行根 `xenon_player`；入口来自应用 package.json。页面配对路径相对于该应用根，使用通用配置声明，不改 PLE 源码。

## 4. 启动及窗口生命周期

```text
点击 PL-E → 读取外置配置 → 初始化或激活 xenon-player-test
Utility 加载 out.asar/main.js → app.whenReady()
PLE main 创建 BrowserWindow → Browser 创建真实 Widget/WebContents
应用 loadURL/loadFile → 读取 out.asar 内页面 → 执行归档内 preload
```

侧栏不另造空白播放页面，运行窗口也不重写为独立 WebUI frontend。每个 renderer Document 有自己的 endpoint 和 `window_id`，IPC 回复、native callback 及 `BrowserWindow.fromWebContents()` 对应真实发起窗口。

关闭遵循 BrowserWindow 关闭事件与应用退出语义，不用隐藏窗口替代销毁。正常退出清理该容器窗口、Utility 和 native 服务；再次点击侧栏创建新生命周期。异常断开后等待明确重开，避免旧页面回调触发新弹窗。

## 5. 原生播放与兼容修复

### 原生模块及 player child

PLE 使用自身 addon、播放器和 Download SDK，不与 TH 共享同名文件。原生模块按 executable 位置拼接的 DLL 路径，在运行根存在对应文件时由通用加载桥局部重定向；当前目标为 `xenon_player/SDK` 等原发行路径。

`pc_addon.node` 按原约定传递 `--server-id`、`--client-id`、`--process-id` 启动宿主的 player child。子进程依据继承的 `XENON_HOSTED_APP_DIR` 加载 `xenon_player/player/containor.dll!InitContainor`，不启动 `xmp.exe`。常规 Browser、Renderer、Utility 不进入该快速路径。

同版本 addon 与 SDK 是播放前提。容器不改写媒体 URL，不伪造 `playUrl` 或成功状态，也不改变选集、时间及 Download SDK 业务数据。

### 背景色、拖动和双窗配对

原画、字幕、选集菜单的背景问题通过通用 BrowserWindow 背景色传递修复：背景声明送达 WebContents、页面和视图，底层只接受完全透明或不透明时进行对应归一。

7.1.35 Windows 播放器的手动拖动依赖原生鼠标消息、Buffer 有符号读取和实时 screen 查询。适配层补齐相关通用接口，不用固定鼠标坐标或写死屏幕大小。

视频截取和 GIF 截取使用控制窗与视频窗组合。外置 `parentWindowPairing` 显式声明需要跟随父窗的页面；公共窗口层同步位置、大小及相关状态。该能力不按业务 channel、模块名称或页面名在核心代码中猜测配对。

### 截取窗口 GDI 预览

原生软件渲染视频与 GPU WebContents 共存时，Windows 的重定向位图和 GPU 子窗口覆盖区域曾导致预览黑屏。当前修复对显式托管的 native child 保留绘制内容，并在原生不透明子窗口覆盖区域裁剪 GPU 窗口，随窗口层级、位置和显隐更新。

修复保留浏览器 GPU 合成及 PLE 原有 `software_render`，不通过全局关闭硬件加速或修改播放器渲染配置解决。Windows 原生实现位于对应平台代码；涉及 Chromium 源码的扩展受 `BUILDFLAG(ENABLE_XENON_SERVICE)` 控制。

此前已由用户确认菜单正常、播放及拖动正常、截取视频可见，以及视频/GIF 双窗跟随正常。这些记录反映原功能验收；目录重整后的结果应以本次构建及运行回归为准。

## 6. 更新、打包与回归

更新输入必须是发布步骤已组装好的完整目录。通用导入器不负责把 SDK 底包、`app/build` 和 native 目录混合成新发行，也不再提供 frontend-only 更新。

构建输出和安装目录保持同一相对布局，应用目录与外置 JSON、公钥一起安装。release manifest 位于应用目录外，记录制品来源和哈希，不参与应用的 require 路径。

只读检查仓库源资源和输出：

```powershell
python xenon_overlay/tools/import_electron_app.py `
  --src xenon_overlay/resources/xenon_player `
  --out out/Release_64/xenon_player --check
```

运行验收应覆盖本地/在线播放、原画/字幕/选集菜单、主窗拖动、视频/GIF 截取预览与双窗拖动、退出后重开。定位顺序：

1. 外置配置、公钥、加密 `out.asar` 和 package 入口是否匹配。
2. `xenon-player-test` 是否成功初始化，页面是否位于正确的 file 归档路径。
3. native 与伴随 SDK 是否都来自 `xenon_player`，没有旧 `main/`、`build/` 或 TH 副本。
4. player child 是否加载 `xenon_player/player/containor.dll`。
5. 截取控制页是否命中声明的父窗配对，native 视频是否与 GPU 页面正确叠合。

历史测试日志保留在对应 `out/` 目录；旧 frontend/main 拆分路径、preload 提取和旧同步参数已经退役，不作为当前发布操作指引。
