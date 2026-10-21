> macOS 系统级 Xenon Updater（Chromium Updater + Omaha 4.0）见
> [SYSTEM_UPDATER.md](SYSTEM_UPDATER.md)。下面的自定义 JSON/patch.zip 工具属于原有协议。

当前系统 Updater 的打包入口为 `package_macos.py`，差分入口为
`make_updater_delta.py`。可直接使用的命令及输出说明见 [PACKAGING.md](PACKAGING.md)。

# Xenon 增量与差分更新工具链 (Xenon Updater Tools)

本目录包含 Xenon 增量更新与差分包构建、服务模拟、自动化测试与环境部署的核心工具。

---

## 目录结构

```
xenon_overlay/tools/updater/
├── make_directory_diff.js    # 全目录清单驱动差分生成与打包工具
├── make_mac_pkg.js           # 将 .app 打成安装到系统 /Applications 的 .pkg
├── mock_update_server.js     # 本地 Node.js 更新模拟服务器 (零依赖)
├── e2e_diff_test.js          # 端到端自动化更新测试工具 (CDP 协议)
├── real_mac_update_test.js  # 真实 macOS 浏览器下载、安装、重启验收
├── trigger_restart.js        # 触发重启生效与原子置换工具 (quitAndInstall)
├── reset_env_1001.ps1        # 本地测试环境一键重置到 1.0.0.1 基准
└── README.md                 # 工具链使用指南 (本文件)
```

---

## 工具详细说明与使用方法

### 1. `make_directory_diff.js` - 全目录差分打包生成工具
对新旧两个完整安装目录、macOS `.app` 或 Windows 安装包 `.exe` 进行递归对比，生成清单驱动的增量更新包 `test_packages/patch.zip`。输入目录只读；每次运行使用独立临时目录，并在成功或失败后清理。打包失败时保留已有输出。

**差分策略**：
- **`copy` (零字节直拷)**：新旧版本 Hash 完全一致的文件（如 95%+ 的语言包、数据文件），不耗费任何网络带宽；
- **`patch` (Zucchini 差分)**：
  - PE 二进制（`.dll`、`.exe`）：使用 Zucchini 汇编反汇编指令感知差分；
  - 大体积资源文件（`.pak`、`.bin`、`.dat` 等 > 64KB）：使用 Zucchini `-raw` 模式二进制增量差分；
- **`add` (新增资源打包)**：小于 64KB 的极小资源或全新增补文件全量打包进入 `files/`；
- **`deletions` (删除清单)**：记录旧版本被废弃的文件。
- **`symlink`**：保留 macOS 包内的相对符号链接，拒绝指向应用包外的链接。旧链接变为普通文件时使用 `add`。
- 每个普通文件动作都携带目标内容的 SHA-256、大小及 `mode`（整数，范围 `0..0777`），包括内容相同但权限发生变化的文件。

新旧版本必须是有效的四段版本，且目标版本严格高于基准版本。macOS 从 `CFBundleShortVersionString` 读取，原生 macOS 支持二进制 plist；Windows 输入必须只包含一个版本目录，避免误选残留版本。

**用法**：
```bash
# 使用默认 baseline 生成 1.0.0.1 -> 1.0.0.2 差分包
node xenon_overlay/tools/updater/make_directory_diff.js

# 指定自定义旧版本和新版本路径 (支持安装目录或安装包 exe)
node xenon_overlay/tools/updater/make_directory_diff.js --old <old_dir_or_exe> --new <new_dir_or_exe> --out test_packages/patch.zip

# macOS 原生制包，显式选择已构建的 Zucchini
node xenon_overlay/tools/updater/make_directory_diff.js \
  --old /tmp/xl153-old.app --new out/release_arm64/xl153.app \
  --zucchini out/updater_tools_arm64/zucchini --out test_packages/patch.zip

# 自定义工具路径；--zip 与 --7zip 均支持原生可执行程序
node xenon_overlay/tools/updater/make_directory_diff.js \
  --old <old_dir> --new <new_dir> --zucchini <zucchini_path> --7zip <7z_path>
```

工具按宿主平台查找：Windows 优先使用仓库的 `7za.exe`，macOS/Linux 查找 `7zz`、`7z`、`7za`，也可使用系统 `zip`（用 `--zip <path>` 显式指定）。解压 `.exe` 需要原生 7-Zip。Zucchini 查找 `out/Release_64`、`out/release_arm64`、`out/Release`、`out/Default` 和 `PATH`，使用当前平台的可执行文件名。修改的大文件需要 Zucchini；工具缺失、执行失败或补丁回放校验失败都会以非零状态退出，不会静默退化。

#### macOS 大文件制包：使用系统 malloc 构建 Zucchini

Mach-O framework 使用 `-raw` 差分。原文件超过约 512 MiB 时，普通浏览器构建目录中的 Zucchini 可能因 PartitionAlloc 的单次分配上限而触发 `SIGTRAP`：后缀数组每个原文件字节需要一个 4 字节索引，而该分配器的上限约为 2 GiB。因此应使用独立的系统 malloc 版工具；只改变制包工具的内存分配器，生成的 Zucchini 补丁格式及客户端应用逻辑保持一致。

从源码根目录执行以下命令。构建脚本需要 **Python 3.10 或以上**；先确认 `python3 --version`，并将合适的 Python 可执行文件所在目录放在 `PATH` 前面。macOS 自带的 Python 3.9 不满足要求。下列配置使用独立目录，不改变浏览器的构建配置：

```bash
python3 --version
mkdir -p out/updater_tools_arm64
cat > out/updater_tools_arm64/args.gn <<'EOF'
is_debug = false
is_component_build = false
target_cpu = "arm64"
symbol_level = 0
use_allocator_shim = false
use_partition_alloc_as_malloc = false
enable_backup_ref_ptr_support = false
enable_pointer_compression_support = false
ffmpeg_branding = "Chrome"
proprietary_codecs = true
enable_xenon_node_uv_compat = true
EOF
buildtools/mac/gn gen out/updater_tools_arm64
third_party/ninja/ninja -C out/updater_tools_arm64 -j6 zucchini

node xenon_overlay/tools/updater/make_directory_diff.js \
  --old /path/to/1.0.0.1/xl153.app --new /path/to/1.0.0.2/xl153.app \
  --zucchini out/updater_tools_arm64/zucchini --out test_packages/patch.zip
```

此目录不在工具的自动查找列表中，制包时应保留显式 `--zucchini` 参数。GN 会生成整个仓库的构建图，Ninja 只编译 `zucchini` 及其依赖。两个 `enable_*` 配置用于关闭依赖 PartitionAlloc-Everywhere 的功能，其余项目配置用于满足本仓库的 GN 约束。不要通过手动移除链接库来混用不同 allocator 配置的对象文件。

---

### 2. `make_mac_pkg.js` - macOS 安装包生成工具
把一个 `.app` 打成系统级 distribution PKG。双击后由“安装器”请求管理员授权，安装到系统 `/Applications`，载荷所有权为 `root:wheel`；不提供用户 Home 或其他卷的安装选项。版本取 `CFBundleShortVersionString`（四段版本），不用只有后两段的 `CFBundleVersion`。

这是分发给用户的安装包，也用于 macOS 差分不可用时的全量回退。服务目录中将目标版本的最终 distribution PKG 命名为 `package.pkg`；差分包仍为 `patch.zip`。

**用法**：
```bash
node xenon_overlay/tools/updater/make_mac_pkg.js \
  --app out/release_arm64/xl153.app

# 指定输出和最终产品归档的安装器签名。包标识不要用 org.chromium.Chromium，
# 否则安装器会把它当成已安装的 Chromium 而跳过。
node xenon_overlay/tools/updater/make_mac_pkg.js \
  --app /tmp/xl153-old.app \
  --out test_packages/xl153_installer_1.0.0.1.pkg \
  --sign "Developer ID Installer: Example"
```

默认输出为 `test_packages/<程序名>_installer_<版本>.pkg`。

制包先用 `pkgbuild` 生成组件，再由 `productbuild --distribution` 生成最终归档。Distribution 固定设置 `enable_currentUserHome="false"`、`enable_localSystem="true"`、`enable_anywhere="false"`。`--ownership recommended` 将系统域载荷记录为 `root:wheel`，不嵌入构建用户的 UID，也不修改输入应用的所有权。组件禁止重定位，覆盖安装使用 `upgrade`；包内没有用于 `chown`、`sudo` 或强制终止进程的安装脚本。`--sign` 签的是最终 product archive；应用自身及嵌套 helper 的签名仍应在制包前完成。

覆盖安装前必须正常退出正在运行的应用。组件使用 `BundleOverwriteAction=upgrade`，会整体替换旧应用并删除新包不再包含的文件；它不会替旧进程保留原 framework，也不负责关闭进程。最终 Distribution 在同一组件的独立 `pkg-ref` 中声明 `must-close`，由图形“安装器”要求先退出应用后再安装，不包含强制结束进程的脚本。匹配值来自输入应用的 `CFBundleIdentifier`，不是安装包的 receipt 标识；该字段缺失或为空时制包失败。Apple 的 `app` 元素按 Bundle ID 匹配，因此运行中的其他同标识副本也可能需要退出。这一约束同样适用于重新安装或降级，不需要先卸载。

保留的 `--install-location` 参数仅接受系统路径 `/Applications`；传入 `~/Applications`、某个用户的 Home 路径或其他位置均报错。分发最终生成的 `.pkg`，不要单独分发其中的组件包。域语义见 [Apple Distribution XML Reference](https://developer.apple.com/library/archive/documentation/DeveloperTools/Reference/DistributionDefinitionRef/Chapters/Distribution_XML_Ref.html)。

命令行系统安装需要管理员权限。执行前先通过应用菜单正常退出所有同标识实例，并确认退出完成；本项目尚未验证 `/usr/sbin/installer` 是否执行 `must-close`，不能依赖它提供图形安装器的退出提示或阻止覆盖运行中的应用：

```bash
/usr/sbin/installer -dominfo -pkg /path/to/xl153_installer_1.0.0.1.pkg
sudo /usr/sbin/installer -pkg /path/to/xl153_installer_1.0.0.1.pkg -target /
```

第一条应只列出 `LocalSystem`。工具不会自动搬迁或删除旧用户级 `~/Applications/xl153.app`；系统级安装位于 `/Applications/xl153.app`，应从这一位置启动并进行后续升级。

**应用内安装流程**：macOS 全量更新使用 `.pkg`。系统级 PKG 安装的应用归 `root:wheel` 所有；正式签名、管理员已批准特权服务的应用可以由下述原生服务执行差分替换。客户端先检查安装条件，无法安全执行差分时下载清单中的全量 PKG；差分下载或应用失败也回退到该 PKG。客户端校验实际包大小和 SHA-256，将安装包复制到当前用户的持久缓存并再次验证，再通过 `NSWorkspace` 交给系统“安装器”。安装器负责管理员授权和 `/Applications` 的文件替换；浏览器不会静默提权、输入密码或强制结束其他进程。只打开安装器不会改变浏览器版本。

当前用户拥有且允许写入的应用可以使用差分：原生 `Contents/Helpers/xenon_update_helper` 在安装卷的私有目录内准备新应用，用 Security.framework 校验身份和签名，等待旧浏览器正常退出后，将准备副本提升到同父目录的唯一 `.xenon-swap-*.app`，再原子交换并通过 `NSWorkspace` 重启。若新版启动失败且仍带隔离属性，保留两版并记录 `awaiting_user_approval`；隔离属性本身不能证明启动失败由 Gatekeeper 引起。无法读取隔离属性时记录 `installed_not_started`，其余启动失败执行原子回滚。helper 不静默修改所有权或请求管理员权限。

**系统级差分权限**：新增 `Contents/Helpers/xenon_privileged_update_helper`，通过 macOS 13 及以上的 `SMAppService` 管理。配套 plist 为 `Contents/Library/LaunchDaemons/com.xl153.updater.privileged.plist`，使用包内相对 `BundleProgram` 路径，只公布固定 Mach service `com.xl153.updater.privileged`；不设置 `RunAtLoad` 或 `KeepAlive`。系统 PKG 将二者作为应用载荷安装，不通过脚本注册后台服务。服务只接受专用更新协议，安装位置由其自身 root 所有的 `/Applications/<应用名>.app` 推导；不提供任意命令、目标路径或改所有权接口。普通用户 helper 负责浏览器退出与 GUI 重启，root 服务负责经过验证的文件事务。

是否需要提权与特权服务是否可用分别判断。客户端检查应用树所有权、安装父目录的替换权限；即使管理员能写 `/Applications`，root 所有的应用也必须走特权事务，避免普通用户复制后改变所有权。正式安装前会再次检查，不能只依赖下载前的结果。只读卷、不可变或受系统保护的路径会拒绝原地更新，不修改保护标记。特权服务校验调用方签名、目标应用身份和新版本，把候选应用复制成 root 所有的私有副本，再执行原子替换。

点击“授权并安装”才注册服务。系统返回等待批准时，浏览器保留已准备的差分包，不退出、不重复下载；页面显示“授权后继续安装”，也允许显式选择“改用完整安装包”。普通退出不会自动触发服务注册或管理员审批。未满足签名条件或安装准备失败时，使用清单中的完整 PKG。

正式启用要求主应用、普通 helper、特权 helper 使用同一 Apple Team 的 Developer ID Application 身份签名，且整个应用完成公证。两个 helper 的签名标识分别为当前主应用 bundle ID 加 `.xenon-update-helper`、`.xenon-privileged-update-helper`。正式签名工具先签内层 helper，再签主应用；不能用 PKG 的 Developer ID Installer 签名代替应用代码签名。`SMAppService` 注册后仍须管理员在系统设置中批准，必须检查服务状态；注册成功本身不代表服务已获准运行。[Apple 注册与审批说明](https://developer.apple.com/documentation/servicemanagement/smappservice/register%28%29)

本地 unsigned/ad-hoc 包不启用 root 差分服务，系统级应用仍回退到 PKG；不得为开发测试放宽 Team ID、签名或调用者身份校验。当前源码与隔离测试不等于正式提权验收：证书、公证和管理员批准具备后，需另行验证真实 root 安装的差分提交、失败回滚及正常用户重启。更新已注册服务的 executable 或 plist 后必须安全重新注册；注销会终止服务，不能在活跃更新事务中注销。应先等待事务及回滚/收尾完成，再管理注册生命周期；不可在 PKG 脚本中无条件注销或覆盖正在工作的服务。[Apple 服务生命周期说明](https://developer.apple.com/documentation/servicemanagement/smappservice)

每次提权准备前，客户端还核对服务启动时记录的应用版本、应用 inode、正在运行的 helper 代码哈希和 plist 摘要。检测到旧服务时，只有服务确认没有活跃事务并停止接收新事务后，客户端才异步注销、重新注册，并再次核对版本；审批或刷新失败不会继续替换。正式签名环境应连续验证两次升级，确认第二次不会沿用旧服务代码。

root 服务在 `/Library/Application Support/XenonUpdater` 下建立私有准备目录，通过目录描述符复制文件，拒绝路径逃逸、硬链接和不匹配的签名，不把客户端提供的路径直接当 root 安装目标。提交前检查同一应用的其他主进程，仍有实例运行则拒绝替换，不强制结束它们。事务过期时只丢弃尚未提交的准备副本；已提交或目标发生变化时保留恢复副本并释放锁。

下载进度只表示收到的字节，不代表校验、差分合成或安装成功。小差分可能瞬间传完，页面随后显示校验和准备状态。刷新页面会恢复状态、进度和错误，不重复下载。PKG 的 `is_installer_package=true` 表示已准备好安装包，`installer_opened=true` 仅表示系统安装器成功接收了打开请求；两者都不是安装成功。IPC 的 `quitAndInstall` 返回 `requested` 也只是请求回执。完整系统安装必须另行确认用户授权、安装器完成、磁盘版本、所有权以及新进程实际启动。

普通用户差分 helper 的结果保存在应用旁的 `<应用名>.app.xenon-update-result.json`，以当前用户 `0600` 文件原子写入。启动新版并通过路径/进程校验后才记录 `success`；不立即重启时记录 `installed_pending_launch`，系统放行尚未完成时保留等待状态。用户稍后从原安装路径成功启动目标版本，启动流程再确认待完成记录；等待期间保留旧版备份。失败阶段和原因会在下次启动恢复显示。文件不包含启动参数或 NSError 的完整 userInfo，不用于授权启动或删除文件，也不能作为 PKG 安装成功的证明。差分成功后旧副本若因权限无法清理，会保留在日志指出的隐藏路径；helper 不为删除它自动提权。

系统级差分的用户可读结果保存在 `~/Library/Caches/XenonUpdaterResults/<安装路径 SHA-256>/result.json`，目录权限为 `0700`、文件权限为 `0600`。结果由用户会话 helper 写入，不要求普通浏览器向 `/Applications` 写日志；它不包含特权事务 token，也不能授权任何 root 操作。

需要重新构建应用，以包含两个 helper 和服务 plist；正式签名流程已包含这些嵌套代码。Windows 全量更新支持 `.exe` 和包含目标版本目录的 `.zip`；macOS 全量更新只接受 `.pkg`，不再将全量 ZIP 解成应用后自动替换。差分清单的每个普通文件必须带完整 SHA-256；新制包工具还记录目标文件 `mode`。旧清单未记录 `mode` 时继承基础文件权限。

macOS 原生 helper 隔离测试（只操作临时应用，不打开真实浏览器）：

```bash
python3 xenon_overlay/tools/updater/mac_native_update_test.py
python3 xenon_overlay/tools/updater/mac_installer_package_test.py
python3 xenon_overlay/tools/updater/mac_privileged_update_test.py
python3 xenon_overlay/tools/updater/mac_privileged_generation_test.py
```

特权事务测试在临时目录中使用仅测试编译启用的入口，不创建 root 服务、不注册后台项目，也不替代正式提权实测。

更新器 C++ 回归测试：

```bash
ninja -C out/release_arm64 xenon_updater_unittests
out/release_arm64/xenon_updater_unittests
```

---

### 3. `mock_update_server.js` - 更新模拟服务器
原生 Node.js 实现，零外部 npm 依赖，用于本地联调与自动化测试。

**接口说明**：
- `GET /api/v1/update/check`：返回客户端需要的更新清单 JSON（包含版本、包体 URL、哈希、大小、deletions 等）；
- `GET /downloads/*`：提供 `patch.zip`、`package.pkg`、`package.zip`、`patch.zucc` 等包体流式下载；
- `GET /api/v1/scenario?mode=<diff|full|latest|corrupt>`：运行时热切换测试场景。

**用法**：
```bash
# 启动服务，监听 8999 端口，默认 diff 模式
node xenon_overlay/tools/updater/mock_update_server.js --port 8999 --mode diff

# 切换为全量包模式
node xenon_overlay/tools/updater/mock_update_server.js --port 8999 --mode full

# 使用预先生成的独立包目录；macOS 必须放入目标版本的 package.pkg
node xenon_overlay/tools/updater/mock_update_server.js \
  --port 8999 --mode diff --data-dir /path/to/prepared-packages
```

macOS 请求（`platform=mac`）的 `full_package` 固定指向 `package.pkg`，包括差分清单中的全量回退地址；不会选择 ZIP 或 DMG。Windows 请求仍使用 `package.zip`。大小和 SHA-256 均从实际文件读取；服务不生成占位包、不覆盖现有包。缺少必需包、包为空或不可读取时，检查接口返回 HTTP 503 和错误说明，服务继续处理后续请求；下载不存在的文件返回 404。`diff` 模式还需提供 `patch.zip`（优先）或 `patch.zucc`；`corrupt` 模式只故意改变返回的全量包哈希，用于验证校验失败。

---

### 4. `e2e_diff_test.js` - 端到端更新自动化测试
通过 Chrome DevTools Protocol (CDP) 连接本地运行的浏览器实例（默认调试端口 9222），驱动检查更新、下载和目录准备。测试监听本次检查的原生更新事件，只有收到版本一致的 `update-downloaded` 后才截图并成功退出；更新失败、无可用更新、CDP 错误或超时均以非零状态退出。

此测试验证到 **staging 完成**，不验证重启、应用替换或重启后的版本。需要支持内置 `WebSocket` 的 Node.js。

**用法**：
```bash
node xenon_overlay/tools/updater/e2e_diff_test.js

# 默认超时120秒，可调整端口、更新源、超时和截图路径
node xenon_overlay/tools/updater/e2e_diff_test.js \
  --port 9222 --feed-url http://127.0.0.1:8999/api/v1/update/check \
  --timeout-ms 180000 --screenshot /tmp/xenon-staging.png
```

隔离回归测试（不会连接真实浏览器或更新服务）：

```bash
node --test xenon_overlay/tools/updater/make_directory_diff.test.js xenon_overlay/tools/updater/e2e_diff_test.test.js
```

回归覆盖权限、符号链接类型转换、版本歧义、制包失败清理和 E2E 的失败/超时判定；在 macOS/Linux 上还使用系统 `zip`/`unzip` 验证实际归档权限。Zucchini 工具行为使用隔离编解码替身，真实补丁算法由 C++ 测试覆盖。

#### `real_mac_update_test.js`：差分安装与 PKG 交接验收

此脚本在 macOS 上启动真实浏览器，自动提供仅监听 `127.0.0.1` 的更新源，并使用随机本地 CDP 端口完成验收。需要带内置 `WebSocket` 的 Node.js。输入包必须预先生成；测试不会生成或修改 `patch.zip` / `package.pkg`。

每次运行必须使用**可替换的独立基准 `.app` 副本和全新的 profile**。差分模式会替换 `--app`；它不能指向日常使用的应用、基准源包或 `--target-app`。PKG 模式的测试副本只用于运行下载与交接流程，PKG 自身的安装目标仍为真实 `/Applications`；脚本拒绝非空 profile。三个模式分别使用各自的应用、profile 和证据目录：

| 模式 | 验证路径 | 必需包 |
| --- | --- | --- |
| `diff` | 下载并应用差分；若转为全量则验收失败 | `--patch` |
| `full` | 下载并校验 PKG，确认原生安装器交接；不验证安装 | `--package`（`.pkg`） |
| `fallback` | 故意提供错误的差分 SHA-256，验证下载差分后回退 PKG，并完成安装器交接；不验证安装 | `--patch` 和 `--package`（`.pkg`） |

下面以真实的 `1.0.0.1 → 1.0.0.2` 包为例。先替换四个输入路径，再创建三份基准应用副本；`ditto` 保留应用的权限和符号链接：

```bash
baseline_app="/path/to/1.0.0.1/xl153.app"
target_app="/path/to/1.0.0.2/xl153.app"
patch_zip="/path/to/releases/patch.zip"
full_pkg="/path/to/releases/package.pkg"
update_test_root="$(mktemp -d /private/tmp/xenon-real-update.XXXXXX)"
for mode in diff full fallback; do
  mkdir -p "$update_test_root/$mode"
  /usr/bin/ditto "$baseline_app" "$update_test_root/$mode/xl153.app"
done

# 差分下载、重建、安装和重启
node xenon_overlay/tools/updater/real_mac_update_test.js \
  --mode diff --app "$update_test_root/diff/xl153.app" \
  --data-dir "$update_test_root/diff/profile" \
  --artifacts-dir "$update_test_root/diff/evidence" \
  --expected-version 1.0.0.2 --target-app "$target_app" --patch "$patch_zip"

# PKG 下载、校验和打开系统安装器；不自动授权或安装
node xenon_overlay/tools/updater/real_mac_update_test.js \
  --mode full --app "$update_test_root/full/xl153.app" \
  --data-dir "$update_test_root/full/profile" \
  --artifacts-dir "$update_test_root/full/evidence" \
  --expected-version 1.0.0.2 --package "$full_pkg"

# 差分校验失败后回退 PKG，只验证安装器交接
node xenon_overlay/tools/updater/real_mac_update_test.js \
  --mode fallback --app "$update_test_root/fallback/xl153.app" \
  --data-dir "$update_test_root/fallback/profile" \
  --artifacts-dir "$update_test_root/fallback/evidence" \
  --expected-version 1.0.0.2 \
  --patch "$patch_zip" --package "$full_pkg"
```

所有模式验证本次原生 `update-downloaded`、目标版本、包类型和 HTTP 下载顺序。差分模式还要求旧进程正常退出、新 PID/CDP 启动，应用 plist、原生版本及页面版本一致；`--target-app` 仅允许在差分模式中使用，用来比较完整文件树、SHA-256、大小、权限和符号链接，不比较扩展属性或 ACL。该模式的成功结果为 `acceptanceScope=diff-install-restart`、`installationVerified=true`。

`full` / `fallback` 拒绝 ZIP 输入。脚本在本轮私有 `TMPDIR` / `MAC_CHROMIUM_TMPDIR` 中验证唯一下载 PKG 的 SHA-256、大小和普通文件属性，随后请求打开安装器，并等待原生 `installer_opened=true`。成功记录 `acceptanceScope=pkg-handoff`、`outcome=awaiting-user-installation`、`installationVerified=false`；此时退出码 0 和 `passed=true` **仅表示交接验证通过**，不表示应用已安装或升级。脚本不填写管理员密码、不点击安装、不代替用户授权，也不清除 PKG 的 quarantine；系统安装器窗口会保留。完整 root 安装和启动需单独由用户执行并记录结果，未获授权时不要继续安装。`fallback` 专门要求实际请求过差分再回退，应使用当前用户拥有的独立副本；系统 root 应用可能在下载前就被判定不能差分，测试该应用的 PKG 交接请使用 `full`。

每个准备、交接或重启阶段默认超时 600 秒，可通过 `--timeout-ms` 调整。结束时只正常关闭本轮测试浏览器，最多等待 120 秒；崩溃、超时或需要强制结束进程均判失败。证据目录保存事件、请求、日志、截图及 `result.json`，必须结合其中的 `acceptanceScope` 和 `installationVerified` 解读结果。仅差分模式生成安装后目标树比较。

独立测试副本启动后可能被 macOS 注册到 LaunchServices，与正式安装的同标识应用发生混淆。脚本完成进程清理后，会确认本轮已知浏览器及该副本内的进程均已退出，再仅对位于证据父目录内、本轮实际启动的测试应用执行 `lsregister -u`；路径验证或注销失败会记录到证据并使验收失败。通过 `--installed-test-app` 授权的用户级或系统级实际安装目标始终排除，不会被注销。脚本不扫描注销其他副本，不归档或删除应用；以后再次打开保留的测试副本，系统仍可能重新注册它。

**签名、公证与本机授权**：正式 macOS 分发需要对应用及嵌套 helper 使用 Developer ID Application 签名并完成公证，安装 `.pkg` 还需相应的 Developer ID Installer 签名。保持新旧应用的签名身份一致。本地 ad-hoc 签名包可以用于开发验收，但浏览器启用 `LSFileQuarantineEnabled` 后，经下载重建的应用可能携带 quarantine 扩展属性，Gatekeeper 可能阻止 helper 通过 `NSWorkspace` 重启，即使下载、重建及签名完整性检查已通过。

脚本默认不移除 quarantine。明确授权本机测试后，可在上述三种模式命令末尾加入 `--allow-local-adhoc`。此开关要求 `--app` 和全新 `--data-dir` 位于 `--artifacts-dir` 的父目录内；默认拒绝 `/Applications`、系统目录、目录外的符号链接及硬链接文件（包括硬链接的符号链接）。脚本验证 ad-hoc 签名完整性后，仅使用 `/usr/bin/xattr` 删除这个安装副本的 `com.apple.quarantine`，并为本轮子进程设置独立的 `TMPDIR` / `MAC_CHROMIUM_TMPDIR`。差分模式收到本次原生 ready 事件后，只在这个私有目录内按 updater 的 staging 结构寻找唯一应用，核对版本和实际下载 ZIP 的 SHA-256，再清除该 staging 应用的相同属性；定位或验证失败时终止，不扩大扫描范围。所有处理路径、命令、验证数据和操作前后的属性列表都保存到证据目录。

如果用户明确授权先用基准 PKG 安装、再测试实际安装位置，必须同时用 `--app` 和单独的 `--installed-test-app` 指定同一应用。用户级安装可以写成 `--app '~/Applications/xl153.app' --installed-test-app='~/Applications/xl153.app'`；脚本按系统账户记录确定当前用户的真实 Home，不用可被覆盖的 `HOME` 环境变量授权路径。当前系统级安装使用精确的 `/Applications/xl153.app`；root 所有的应用只有正式签名、公证及已批准特权服务具备时才能执行 root 差分；本地 ad-hoc 系统包的 PKG 交接测试使用 `full`。只允许这两个位置，不接受其他用户、其他应用或符号链接别名；独立 profile、证据目录以及 staging 检查仍保持上述限制。使用本地 ad-hoc 包时，另加 `--allow-local-adhoc` 才会允许对这个已授权安装副本清除 quarantine。这些选项不能授权处理其他应用，也不能启用或绕过特权服务的签名和管理员审批。

`--allow-local-adhoc` **仅跳过本次本地 ad-hoc 应用的隔离检查，不验证正式发行信任链**，不会关闭全局 Gatekeeper，也不会处理 `--target-app`。这种开发验收不替代正式分发的签名、公证与系统启动验收，也不能用于验收 root 差分服务。现有脚本不会清除 root 服务私有准备目录的隔离属性。

没有 quarantine 时不执行属性删除。若经过验证的本次应用中，带 quarantine 的文件由当前测试用户拥有但为只读，脚本会仅为该条目临时增加 owner-write，在 `finally` 中恢复原权限并记录两次操作；不修改符号链接权限，不替其他用户拥有的只读条目提升权限。最终目录比较仍要求全部权限与目标包一致。

差分原生安装器会在 staging 之后再复制应用，系统可能给这份副本重新加 quarantine。开关启用时，脚本还会记录启动前安装父目录已有的 `.xenon-update-*` 目录，触发安装后只接受一个新目录；确认本次旧浏览器的 helper 子进程参数精确指向该副本和目标安装位置，且此时旧主进程仍存活后，仅向这个 helper 发送 `SIGSTOP` 并确认暂停。在暂停期间检查版本及完整文件树与已验证 staging 一致，再清除隔离属性和恢复权限，最后在 `finally` 中发送 `SIGCONT` 并确认已恢复，即使验证失败也执行恢复。这样旧主进程即使在此期间正常退出，helper 也不会提前交换正在处理的目录。发现多个候选、目录内容不一致或检查开始前旧进程已退出均判失败。对于显式授权的实际安装位置，这个额外范围仅为已授权安装父目录下本次新建的 `.xenon-update-*/xl153.app`，即已授权安装父目录内的差分准备副本，不会处理已有准备目录或其他应用。

上述暂停仅用于本地 ad-hoc 验收，暂停与恢复均记录在证据中；该区间人为控制了 helper 的执行时序，不代表正式分发时的原始进程调度。`--self-test` 使用独立的普通子进程验证成功及异常路径都会恢复进程，不启动浏览器。

如果新测试 profile 的系统钥匙串授权阻塞启动或退出，可为独立测试显式添加 `--browser-arg=--use-mock-keychain`。这是 Chromium 提供的测试开关，让该测试进程使用模拟钥匙串，避免系统授权对话框造成超时；它不是产品默认设置，也不修改系统钥匙串设置。使用时必须创建全新独立 profile 和证据目录，保留原先失败的结果，不能用重试覆盖或改写失败记录。在验收结果中注明没有验证真实钥匙串授权和凭据存储流程；这个开关不能替代上述签名或 Gatekeeper 验证。

---

### 5. `trigger_restart.js` - 触发立即重启并安装
通过 CDP 发送 `chrome.send('quitAndInstall')`。差分包请求正常退出并原子替换、重启；PKG 则请求交给系统安装器，不表示安装完成。

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
