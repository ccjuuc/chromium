# Electron 通用容器架构


## 1. 设计目标

- 在 xlbrowser 中运行既有 Electron main 与 renderer。
- 不启动 Electron runtime。
- main、renderer、原生窗口和 Node addon 分进程承载。
- 多应用按 `container_id` 隔离。
- 应用差异通过配置和 adapter 接入，不进入通用容器。
- 本地构建与安装包使用同一运行目录模型。

## 2. 总体架构

```text
Product Entry / WebUI Policy / Runtime Package
                     |
                     v
Browser Process
  XenonManager
  XenonIpcDocumentHost
  XenonElectronWindowHost
  XenonElectronGuest
          |                    ^
          | Mojo               | Window / WebContents
          v                    |
Utility Process                |
  XenonServiceImpl             |
  XenonIpcMainContainer -------+
  XenonNodeExecutor
          ^
          | IPC / NodeAddonHost
          |
Renderer Process
  XenonIpcRenderer
  Renderer Bootstrap
  Hosted Renderer / Preload
```

## 3. 分层职责

| 层次 | 职责 |
|---|---|
| 产品接入层 | 入口、应用配置、WebUI 策略、资源同步、安装清单 |
| Browser 编排层 | 容器注册、按需启动、origin 路由、进程连接管理 |
| Document 接入层 | 页面授权、renderer endpoint、IPC 路由 |
| 原生对象层 | BrowserWindow、Widget、WebContents、guest、HWND |
| Utility 运行层 | main V8、CommonJS、ipcMain、Electron main API |
| Node 运行层 | Node-API addon、对象与回调、原生运行目录 |
| Renderer 兼容层 | preload、ipcRenderer、Electron/Node renderer API |
| 资源层 | WebUI、磁盘资源、标准 ASAR、URL 映射 |
| 交付层 | 运行目录生成、归档、安装与升级 |

## 4. 核心模型

### 4.1 Container

`container_id` 对应：

- 一个 Utility service；
- 一个 main V8 context；
- 一套 CommonJS 缓存；
- 一组 renderer endpoints；
- 一套 addon 与运行目录。

正式架构保持一个 Utility service 只承载一个 `container_id`。

### 4.2 Application Config

容器配置包括：

- main 源码与虚拟路径；
- 应用根目录；
- 应用可执行文件身份；
- 原生运行目录；
- 应用名称、版本和 UA；
- renderer URL 映射。

main 路径、应用身份和原生运行目录相互独立。

### 4.3 Renderer Endpoint

每个 Document 建立独立 endpoint，关联容器、frame 和 window/guest。Document 销毁时
endpoint 注销，main 容器继续存活。

### 4.4 Window Identity

```text
window_id -> container_id -> Widget / WebContents / HWND
```

BrowserWindow 和 guest WebContents 使用 Browser 保存的对象身份归属容器。

## 5. 核心流程

### 5.1 启动

```text
注册应用配置与 origin 路由
  -> 用户激活入口
  -> 按 container_id 启动 Utility
  -> 执行 Electron main
  -> main 创建 BrowserWindow
  -> Browser 创建窗口与 WebContents
  -> renderer 建立 Document endpoint
```

产品入口只负责启动和激活，不额外创建应用主窗口。

### 5.2 IPC

```text
Renderer ipcRenderer
  -> DocumentHost
  -> Utility ipcMain
  -> handler/listener
  -> Renderer endpoint
```

支持 `send`、`sendSync`、`invoke` 和 Main 到 Renderer 消息。

### 5.3 窗口

```text
Utility BrowserWindow API
  -> Browser WindowHost
  -> Widget / WebContents / HWND
  -> Window event
  -> Utility main
```

### 5.4 Guest

```text
Hosted renderer
  -> iframe
  -> Browser owner 校验
  -> inner WebContents
  -> guest identity
  -> guest URL / preload
```

### 5.5 原生模块

```text
require(.node)
  -> NodeAddonHost
  -> Container NodeExecutor
  -> addon / DLL / SDK
```

## 6. 资源架构

- 普通磁盘 renderer；
- 受信 `chrome://` renderer；
- 标准 Electron ASAR；
- 多 renderer URL 映射；
- ASAR 外的原生资源与 unpacked 文件。

私有 ASAR 在构建阶段转换，不进入运行时通用层。

## 7. 安全边界

- main 与 addon 仅运行受信代码。
- renderer binding 由 Browser 按 Document 授权。
- origin allowlist 与容器路由分离。
- file/ASAR 页面通过 Window/guest 身份授权。
- Renderer 不持有授权列表。
- Utility 之间不共享 V8、addon、环境变量或运行目录。

## 8. 标准接入

新应用只增加：

1. 独立 `container_id`。
2. 可搬迁的应用运行目录。
3. 容器配置和 renderer URL 映射。
4. WebUI、CORS 与 CSP 策略。
5. origin 授权和容器路由。
6. 产品入口的按需启动与激活。
7. 资源同步脚本和安装清单。

## 9. 通用层边界

| 内容 | 归属 |
|---|---|
| Electron/Node 通用语义 | 通用容器 |
| BrowserWindow/WebContents/IPC | 通用容器 |
| 应用路径、UA、renderer 映射 | 容器配置 |
| 登录、播放、支付等业务协议 | 产品应用或 adapter |
| 私有资源格式转换 | 构建期同步工具 |
| 应用 SDK 与原生资源 | 应用运行目录 |


## 10. 交付目录

```text
Application/
  xlbrowser.exe
  node.dll
  node.exe
  <hosted-app-a>/
  <hosted-app-b>/
  <version>/
```

应用运行目录位于 xlbrowser.exe 同级的非版本目录。本地输出和安装目录结构一致。

## 11. 当前接入

| 应用 | WebUI | `container_id` |
|---|---|---|
| PL-E | `chrome://xenon-player-electron/` | `xenon-player-test` |
| TH | `chrome://thunder-2025/` | `thunder-2025` |
