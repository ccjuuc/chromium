# Electron 示例应用宿主裁剪评估

日期：2026-11-12。范围：Windows x64 下由 Xenon/Chromium 加载的 draw.io 32.6.10、
MarkText 0.23.0、Motrix 2.4.1。按“先检查”的要求，本轮只制作独立候选并运行验证，
没有删除工程原资源、替换构建输出、重打安装器或提交制品。

2026-11-13 更新：用户要求生成安装器后，已将验证过的候选事务导入工程资源并同步
构建输出；保留文件字节不变。三个目录已取消 Git 忽略，并设置 `-text !eol`，
各自旁置的 `*.release-manifest.json` 记录官方下载地址、ZIP 哈希及完整保留/删除清单。
下文的检查结果仍针对这些相同文件；安装器生成结果见应用目录接入文档。

## 结论与体积

可以移除应用发行根目录的独立 Electron 引擎及其资源，保留应用负载的原字节与相对路径。
这得到的是供宿主加载的制品，不再是可独立运行的原版 Electron 发行包。

| 应用 | 原文件数 / 字节 | 保留文件数 / 字节 | 减少字节 |
| --- | ---: | ---: | ---: |
| draw.io | 72 / 450,131,587 | 3 / 85,105,045 | 365,026,542 |
| MarkText | 108 / 512,302,707 | 79 / 207,208,643 | 305,094,064 |
| Motrix | 228 / 422,408,480 | 157 / 53,766,932 | 368,641,548 |
| 合计 | 408 / 1,384,842,774 | 239 / 346,080,620 | 1,038,762,154 |

未压缩体积减少约 75.0%。这是目录体积，未重新测量压缩后的安装器大小。
精确删除清单、保留清单及各文件 SHA-256：
`F:/electron-compat-audit/20261112/trim-review-01/candidates.json`。

## 可移除的文件

仅针对这三个已审计的 Windows 发行版本，根目录中存在的以下文件属于候选删除范围：

- 原引擎程序：`draw.io.exe`、`marktext.exe`、`Motrix.exe`。
- 根 `locales/`，以及 `chrome_100_percent.pak`、`chrome_200_percent.pak`、`resources.pak`。
- 根 `icudtl.dat`、`snapshot_blob.bin`、`v8_context_snapshot.bin`。
- 根 `ffmpeg.dll`、`libEGL.dll`、`libGLESv2.dll`、`d3dcompiler_47.dll`、
  `dxcompiler.dll`、`dxil.dll`、`vk_swiftshader.dll`、`vk_swiftshader_icd.json`、`vulkan-1.dll`。
- Motrix 的根 `Motrix_wer.dll`、根 `version`，及 `resources/default_app.asar`。
  `version` 内容是 Electron 45.2.3；WER DLL 属于原引擎的 Crashpad/WER 处理；
  default_app 是 Electron 缺省入口，不是 Motrix 的应用 ASAR。

此清单不能直接成为对任意应用通用的文件名删除规则。例如 PLE 的原生播放器依赖部分
同名图形库；现有 TH/PLE profile 不应被这次评估改变。未来实现应使用显式构建配置和
导入依赖检查，拒绝保留的原生文件依赖已删除 DLL 的情况。

三个外置 `.xenon.json` 都未声明 `executable`。`ResolveAppExecutable()` 默认使用
`base::FILE_EXE`，因此 `process.execPath` 和 `app.getPath('exe')` 当前已经指向宿主。
draw.io 的更新模式检测、可选 `urlParams.json`，以及 MarkText 的 Jump List 不依赖
发行目录中的原 EXE。保留原版 EXE 后再删除它依赖的 FFmpeg 等文件也没有实际意义。

## 必须保留

三个 `resources/app.asar` 均不解包、不重打。所有保留文件按路径、长度和 SHA-256 与
原包核对，应用代码不修改。根 `LICENSE.electron.txt`、`LICENSES.chromium.html` 及
应用资源内的第三方许可证均保留。

draw.io 最终只需 ASAR 和两个根许可证。其页面、图标、preload 和应用自身的语言文件均在
ASAR 中；归档没有 unpacked/native/helper 成员。根 `locales/` 与应用自身语言资源不同。

MarkText 保留整个 `resources/`，包括图标、`static/locales`、`app-update.yml`、
全部 `.asar.unpacked`、5 个 `.node` 和 `rg.exe`。native 的普通/延迟 PE imports 为
系统 DLL 和宿主提供的 `node.exe`，未依赖上述根图形/媒体库。
`ced/build/vendor` 中 113,819 字节的构建记录虽然没有运行价值，但已列入 ASAR 的
unpacked/integrity 索引；为这点体积制造索引缺件不值得。`build/Release` 内含真实加载
入口，不能整体删除；重复的 `bin`/`build` native 也应保留其 fallback 关系。

Motrix 除 `default_app.asar` 外保留整个 `resources/`：SQLite native、`aria2c.exe`、
`motrix-finalize-fs.exe`、`motrix-native-host.exe`、内置插件、更新配置及许可证。
应用从 `process.resourcesPath` 解析这些路径；`app-update.yml` 被主程序实际读取。
保留的原生文件普通/延迟 imports 没有依赖候选删除的根引擎 DLL，SQLite 所需 `node.exe`
由现有宿主提供。

## 独立瘦包运行验证

候选目录：`F:/electron-compat-audit/20261112/trim-review-01/candidates/`。
使用当前已编译的 `xlb153.exe`，独立端口和独立 profile，并采用与现有侧栏一致的
`userDataPolicy: "legacy-host"`。没有访问用户的 9222/9270 实例。

| 应用 | 实际结果 | 证据目录 |
| --- | --- | --- |
| draw.io | 4/4：页面离线配置、IPC、CLI 打开图、编辑保存及磁盘回读；ASAR 哈希不变 | `drawio-core-hosted/verification.json` |
| MarkText | 5/5：CLI 打开、页面 Node 隔离、定向输入、Ctrl+S 落盘、保存后关闭 | `marktext-core-02/verification.json` |
| Motrix | 原 IPC 信任校验、路由、SQLite、真实 HTTP 下载和收尾正常；2,097,289 字节成品 SHA-256 匹配 | `motrix-core/result.json` |

三者均宿主退出码 0，无强制清理、残留进程或新 dump。Motrix 观察到候选目录的 aria2
和 finalize helper；菜单退出后进程正常结束。验证覆盖上述核心流程，不能解释为全部
应用功能或全部平台均通过。

## 需要单独记录的边界

Motrix 浏览器扩展的冷启动不是本轮已通过项目。官方 v2.4.1
[`launcher.rs`](https://raw.githubusercontent.com/agalwood/Motrix/v2.4.1/packages/native-host/src/launcher.rs)
只枚举 `C:\Program Files\Motrix\Motrix.exe` 与
`%LOCALAPPDATA%\Programs\Motrix\Motrix.exe`，找到后无参数启动；没有当前 helper 相对
路径或可配置宿主入口。`MOTRIX_BRIDGE_DATA_DIR` 只设置桥接数据目录。
保留当前 Xenon 资源目录里的 `Motrix.exe` 不能补足此冷启动功能。需要支持时应另行设计
显式宿主启动配置，不能偷偷恢复原 Electron 引擎或宣称扩展集成已完成。

首轮直接加载裸目录采用默认 isolated 策略，在较深诊断 profile 下发生单实例 listener
发布失败。draw.io 完整原包在同等长度 profile 下复现相同失败，确认与裁剪无关；
改用现有侧栏的外置声明后瘦包核心验证通过。精确文件系统根因尚未修复，本轮不扩大为
框架修改。对应原始失败和基线证据保留在 `drawio-core/`、`drawio-full/`、`marktext-core/`。

## 入库建议

只将最终确认的宿主制品、外置声明和来源/保留文件哈希清单入库。TH、PLE 仍维持原先的
本地生成政策。三个目录通过 `.gitignore` 显式纳入跟踪，制品和清单共同维护。

供应方文件已在 `resources/.gitattributes` 中对三个目录设置 `-text !eol`，防止仓库的
全局 LF 规则改写 `.asar.unpacked` 中 JS/JSON/许可证等文件字节。应用 ASAR 本身继续保留
原字节，尤其 MarkText 的 ASAR 已有 178,635,027 字节，不应为进一步减小 Git 制品而直接重打。
三个应用的 `resources/app.asar` 统一使用 Git LFS，避免 GitHub 的 100 MiB 单文件限制。
开发者须按 [应用目录接入](电子应用目录接入.md) 拉取实际归档，再运行或打包；来源清单中的
大小和 SHA-256 始终对应归档原始字节，不是 Git LFS 指针。

未来重新导入新版本时，重新进行入口、原生依赖和核心运行验证；不能静默沿用本次版本的
删除清单。2026-11-12 的 673 MB 安装器为完整原包版本；2026-11-13 已生成约 391 MB
的宿主制品安装器并通过归档校验，原安装器备份在该轮打包证据目录中。
