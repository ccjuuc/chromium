# chrome://xenon-player — Electron 播控接入

把 `F:\xl-player\xmp_xdas_2` 的 **main-renderer 编译产物**挂到 Chromium WebUI，并复用已打通的 `pc_addon.node` / `PreparePlayerHost` 链路。

## 目标

| 项 | 说明 |
|----|------|
| URL | `chrome://xenon-player/` |
| 播控资源 | Electron `app/build/main-renderer/`（不进 pak，运行时从磁盘读） |
| Native | 与 `chrome://xenon-node/` 同一套 Mojo + Utility N-API |
| HWND | `preparePlayerHost()`：顶层视频 HWND（默认隐藏）+ **不透明**播控框；半透明双窗在 Chromium 上会整窗发黑 |

## 一次同步前端

```bat
python xenon_overlay\tools\sync_xenon_player_frontend.py ^
  --src F:\xl-player\xmp_xdas_2\app\build\main-renderer ^
  --dst out\release_64\xenon_player_frontend
```

也可启动参数：

```text
--xenon-player-frontend-dir=F:\xl-player\xmp_xdas_2\app\build\main-renderer
```

默认还会探测 `<chrome.exe 目录>/xenon_player_frontend`。

## 启动链路

1. 打开 `chrome://xenon-player/`（侧栏 **XP**）。
2. `host.ts`：`preparePlayerHost` 创建播放宿主窗（不改地址栏）。
3. `electron_shim.ts`：注入 `require` / `process` / `electron.ipcRenderer`；读 `location.href` 时虚拟补上 `ph`/`ch` 给 `GetUrlArgs`；`.node` 走 Xenon `require()`。
4. 按顺序注入 `app/static/js/{lib-axios,lib-vue,881,index}.js` 与 CSS。

## 与原版 Electron 的差距（后续）

原版 renderer 还依赖：

- `@xunlei/node-net-ipc` 进程 IPC
- `player_helper.bindWnd`（Main 侧 `AplayerWndBind`）
- 账号 / 网盘 / 刮削等业务模块与真实 `fs`/`path` 目录布局

当前 shim 以「能加载编译产物 + 把 HWND/`pc_addon` 送进 `InitPlayer`」为第一阶段。完整播控需按控制台报错继续补 stub，或给 `player-comp` 做纯 Web 构建目标。

## 关键文件

- `xenon_overlay/resources/webui/xenon_player/` — 宿主页 / shim / host
- `xenon_overlay/chrome/browser/ui/webui/xenon_node_controller.*` — `XenonPlayerConfig` + `app/` 磁盘过滤
- `xenon_overlay/tools/sync_xenon_player_frontend.py`
- `xenon_overlay/docs/Windows进程存在但无界面排查与会话隔离指南.md` — 自动化拉起有 PID 但无界面排查指南
