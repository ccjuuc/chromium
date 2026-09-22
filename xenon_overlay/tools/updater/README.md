# Xenon 增量与差分更新工具链 (Xenon Updater Tools)

本目录包含 Xenon 增量更新与差分包构建、服务模拟、自动化测试与环境部署的核心工具。

---

## 目录结构

```
xenon_overlay/tools/updater/
├── make_directory_diff.js    # 全目录清单驱动差分生成与打包工具
├── mock_update_server.js     # 本地 Node.js 更新模拟服务器 (零依赖)
├── e2e_diff_test.js          # 端到端自动化更新测试工具 (CDP 协议)
├── trigger_restart.js        # 触发重启生效与原子置换工具 (quitAndInstall)
├── reset_env_1001.ps1        # 本地测试环境一键重置到 1.0.0.1 基准
└── README.md                 # 工具链使用指南 (本文件)
```

---

## 工具详细说明与使用方法

### 1. `make_directory_diff.js` - 全目录差分打包生成工具
对新旧两个完整安装目录（或安装包 `.exe`）进行深度递归对比，生成符合客户端标准的增量更新包 `test_packages/patch.zip`。

**差分策略**：
- **`copy` (零字节直拷)**：新旧版本 Hash 完全一致的文件（如 95%+ 的语言包、数据文件），不耗费任何网络带宽；
- **`patch` (Zucchini 差分)**：
  - PE 二进制（`.dll`、`.exe`）：使用 Zucchini 汇编反汇编指令感知差分；
  - 大体积资源文件（`.pak`、`.bin`、`.dat` 等 > 64KB）：使用 Zucchini `-raw` 模式二进制增量差分；
- **`add` (新增资源打包)**：小于 64KB 的极小资源或全新增补文件全量打包进入 `files/`；
- **`deletions` (删除清单)**：记录旧版本被废弃的文件。

**用法**：
```bash
# 使用默认 baseline 生成 1.0.0.1 -> 1.0.0.2 差分包
node xenon_overlay/tools/updater/make_directory_diff.js

# 指定自定义旧版本和新版本路径 (支持安装目录或安装包 exe)
node xenon_overlay/tools/updater/make_directory_diff.js --old <old_dir_or_exe> --new <new_dir_or_exe> --out test_packages/patch.zip
```

---

### 2. `mock_update_server.js` - 更新模拟服务器
原生 Node.js 实现，零外部 npm 依赖，用于本地联调与自动化测试。

**接口说明**：
- `GET /api/v1/update/check`：返回客户端需要的更新清单 JSON（包含版本、包体 URL、哈希、大小、deletions 等）；
- `GET /downloads/*`：提供 `patch.zip`、`package.zip`、`patch.zucc` 等包体流式下载；
- `GET /api/v1/scenario?mode=<diff|full|latest|corrupt>`：运行时热切换测试场景。

**用法**：
```bash
# 启动服务，监听 8999 端口，默认 diff 模式
node xenon_overlay/tools/updater/mock_update_server.js --port 8999 --mode diff

# 切换为全量包模式
node xenon_overlay/tools/updater/mock_update_server.js --port 8999 --mode full
```

---

### 3. `e2e_diff_test.js` - 端到端更新自动化测试
通过 Chrome DevTools Protocol (CDP) 连接本地运行的浏览器实例（调试端口 9222），驱动检查更新、下载、目录解构与后台校验，并抓取当前 WebUI 界面截图 `e2e_diff_screenshot.png`。

**用法**：
```bash
node xenon_overlay/tools/updater/e2e_diff_test.js
```

---

### 4. `trigger_restart.js` - 触发立即重启并安装
通过 CDP 发送 `chrome.send('quitAndInstall')`，通知客户端立即关闭并执行原子文件替换与重启。

**用法**：
```bash
node xenon_overlay/tools/updater/trigger_restart.js
```

---

### 5. `reset_env_1001.ps1` - 测试环境一键重置
将 `AppData\Local\xlb153\Application` 目录及注册表一键还原到干净的 `1.0.0.1` 状态，便于重复验证全流程。

**用法**：
```powershell
powershell -File xenon_overlay/tools/updater/reset_env_1001.ps1
```
