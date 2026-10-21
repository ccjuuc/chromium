# Xenon 增量与差分更新工具链 (Xenon Updater Tools)

本目录包含 Xenon 增量更新与差分包构建、服务模拟、自动化测试与环境部署的核心工具。

---

## 目录结构

```
xenon_overlay/tools/updater/
├── make_directory_diff.js    # 全目录清单驱动差分生成与打包工具
├── make_directory_diff.test.js # 生成器 Node.js 单元测试
├── make_mac_pkg.js           # 将 .app 打成安装到 /Applications 的 .pkg
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

# macOS：输入完整、已签名的新旧 .app，Framework 的版本目录允许不同
node xenon_overlay/tools/updater/make_directory_diff.js \
  --old /releases/1.0.0.1/Browser.app \
  --new /releases/1.0.0.2/Browser.app \
  --zucchini out/Release/zucchini --out test_packages/patch.zip

# 可选：指定归档工具，Windows 默认仓库内的 7za.exe
node xenon_overlay/tools/updater/make_directory_diff.js \
  --old <old_dir_or_app> --new <new_dir_or_app> \
  --seven-zip /opt/homebrew/bin/7zz --out test_packages/patch.zip

# 不依赖 Chromium 构建的生成器测试
node --test xenon_overlay/tools/updater/make_directory_diff.test.js
```

macOS 默认使用 `out/Release_64/zucchini` 和系统 `/usr/bin/ditto`，其它 POSIX 平台使用 `zip -r -y`；可用 `--zucchini`、`--seven-zip` 指定实际工具。生成补丁后必须以旧文件回放并核对目标 SHA256，验证失败、工具不可用或收益不足时自动将该文件作为 `add` 全量收录。每次运行使用独立临时目录，不复用上次的差分文件。macOS 通过 `plutil` 读取 XML 或 binary `Info.plist` 的版本，不能读取有效版本时停止打包。

mac 差分包内部 `manifest.json` 使用 `schema_version: 2`，保留 `platform`、`base_version`、`target_version`、`actions`、`deletions` 字段，**更新服务的顶层 JSON feed 不变**。额外规则如下：

- `root_mode` 保存 `.app` 根目录权限；每层目录（包括空目录）都有 `directory` action 和整数 `mode`。权限仅保留 POSIX `0777` 位。
- 普通文件的 `copy`、`add`、`patch` 都带最终文件的 `sha256`、`size`、`mode`；`base` 指向旧 `.app` 内的真实相对路径，`source`/`patch` 指向差分包内的相对路径。
- `symlink` action 使用 `target` 和 `link_target` 保存链接，不递归遍历链接。所有路径使用 `/`，链接须为 `.app` 内的相对目标；客户端还会检查链接链、循环和实际目录边界。
- `*.framework/Versions/<当前版本>` 与新版本对应目录按同一位置匹配，避免将整个 Framework 误判为新增；`Versions/Current`、`Resources` 等链接单独记录。
- Windows 继续使用原有文件 action 和版本目录布局，不增加 mac 元数据。

新 `.app` 应来自最终发布构建，并在生成差分前完成签名。mac 全量回退 ZIP 应包含一个完整 `.app`，且保留其权限和链接；可用以下命令制作（与差分包自身的归档布局不同）：

```bash
ditto -c -k --sequesterRsrc --keepParent /releases/Browser.app test_packages/package.zip
```

ZIP 需要 Unix/macOS 的文件类型及权限元数据。全量包中的顶层应用可以使用发布名称；当用户重命名已安装应用时，客户端会在私有暂存目录中保留该安装名称，并继续验证签名身份。包内没有同名应用时必须只有一个 `.app` 候选。

测试会在真实临时目录中创建 Framework 版本变化、可执行文件、空目录和符号链接，验证匹配、最终哈希、权限元数据、失败回放回退及 Windows action 兼容。符号链接测试在 Windows 上需要允许创建符号链接。测试中仅外部 Zucchini/plutil/归档命令使用替身；实际 macOS 的签名、解包和安装验收仍需在 macOS 执行。

---

### 2. `make_mac_pkg.js` - macOS 安装包生成工具
把一个 `.app` 打成组件安装包。双击后由“安装器”安装到 `/Applications`。版本取 `CFBundleShortVersionString`（四段版本），不用只有后两段的 `CFBundleVersion`。

这是分发给用户的安装包。应用内更新仍使用 `patch.zip` / `package.zip`，不消费这个 `.pkg`。

要使用应用内更新，输入 `.app` 须已用 Developer ID Application 签名；`--sign` 使用 Developer ID Installer，只签名 `.pkg`，不会给应用签名。应用内更新要求安装目录可写，不会自动提权。

**用法**：
```bash
node xenon_overlay/tools/updater/make_mac_pkg.js \
  --app out/release_arm64/xl153.app

# 指定输出和安装器签名。包标识不要用 org.chromium.Chromium，
# 否则安装器会把它当成已安装的 Chromium 而跳过。
node xenon_overlay/tools/updater/make_mac_pkg.js \
  --app /tmp/xl153-old.app \
  --out test_packages/xl153_installer_1.0.0.1.pkg \
  --sign "Developer ID Installer: Example"
```

默认输出为 `test_packages/<程序名>_installer_<版本>.pkg`。

---

### 3. `mock_update_server.js` - 更新模拟服务器
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

### 4. `e2e_diff_test.js` - 端到端更新自动化测试
通过 Chrome DevTools Protocol (CDP) 连接本地运行的浏览器实例（调试端口 9222），驱动检查更新、下载、目录解构与后台校验，并抓取当前 WebUI 界面截图 `e2e_diff_screenshot.png`。

**用法**：
```bash
node xenon_overlay/tools/updater/e2e_diff_test.js
```

---

### 5. `trigger_restart.js` - 触发立即重启并安装
通过 CDP 发送 `chrome.send('quitAndInstall')`。macOS 的安装已在后台完成，此调用请求 Chromium 保存会话并重启；Windows 继续执行退出后的文件替换和重启。

**用法**：
```bash
node xenon_overlay/tools/updater/trigger_restart.js
```

---

### 6. `reset_env_1001.ps1` - 测试环境一键重置
将 `AppData\Local\xlb153\Application` 目录及注册表一键还原到干净的 `1.0.0.1` 状态，便于重复验证全流程。

**用法**：
```powershell
powershell -File xenon_overlay/tools/updater/reset_env_1001.ps1
```
