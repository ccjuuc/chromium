# Xenon Update：axum 更新服务与差分发布工具

`xenon-update` 在同一服务中提供 macOS 和 Windows 两个发布面板：macOS 使用 PKG → CRX3、Puffin 差分和 Omaha 4.0；
Windows 使用 EXE / 安装目录 ZIP 导入、Zucchini 目录差分和 JSON + ZIP 更新接口。
发布服务本身可以运行在 Windows/Linux；服务宿主平台与更新目标平台是两回事。
Windows 客户端保持原有 JSON + ZIP 方案，不接入 Omaha，也不新增客户端后台服务。
单独制差分仍可使用 [make_directory_diff.js](../README.md)。

## 构建与首次启动

从 Chromium 源码根目录执行，需要 Rust 和宿主平台的 C/C++ 链接工具：

```sh
cargo build --release --locked --manifest-path xenon_overlay/tools/updater/xenon_update_tool/Cargo.toml
```

Windows 输出为 `xenon_overlay/tools/updater/xenon_update_tool/target/release/xenon-update.exe`。
在该目录打开 PowerShell，首次运行：

```powershell
.\xenon-update.exe init
.\xenon-update.exe serve
```

浏览器打开 `http://127.0.0.1:9001/`。启动命令是子命令 **`serve`**，不是 `--serve`。
前台进程需保持运行，Ctrl+C 停止。已有配置时直接 `serve`，不要再次 `init`。
macOS/Linux 可执行文件名为 `xenon-update`，使用 `./xenon-update`。

默认配置为当前目录下的 `xenon-update/xenon-update.json`。也可在所有命令中指定：

```sh
xenon-update --config /ABS/state/xenon-update.json init
xenon-update --config /ABS/state/xenon-update.json serve
```

配置中的相对路径以配置文件所在目录为基准。将配置、`releases`、`publisher/history` 和 `publisher/windows` 放在持久目录；
只提供已有制品的更新服务不需要制包工具，网页制包需要下一节对应平台的依赖。

## 在浏览器中发布安装包

页面上方可切换 **macOS** 和 **Windows**，两边分别保存配置、查看发行项和历史，并支持文件上传、HTTPS 链接或服务器本地路径。
切换面板会保留未提交内容；保存一个平台的配置不会覆盖另一平台的草稿。同一服务一次执行一个发布任务。

### macOS 面板

首次展开“macOS 发布配置”，填写**服务所在电脑**上的以下路径：

1. 发布签名私钥：现有 RSA DER PKCS#8 私钥，例如 `release-key.pk8`，必须对应客户端信任的发布公钥。
2. 发布公钥 SHA-256：SHA-256(SPKI DER)，支持 hex 或 Base64；留空保存时会调用 OpenSSL 从私钥计算。
3. 原生工具目录：包含宿主平台的 `crx3_build_action`、`xenon_updater_verify` 和 `puffin`，Windows 文件带 `.exe`。
4. Python 3.10+、OpenSSL：可填写可执行文件完整路径，留空时自动查找。

页面只保存私钥的路径，不上传私钥文件，也不自动生成密钥。执行制包时服务会读取私钥完成 CRX3 签名。
Apple 签名和公证须在上传 macOS PKG 前完成；CRX 发布签名不能替代 Apple 签名。

“发布安装包”支持三种来源，单个文件上限为 8 GiB：

| 来源 | 使用方式 |
| --- | --- |
| 上传文件 | 选择浏览器所在电脑的 macOS `.pkg`，上传到服务器 |
| 下载链接 | 填写以 `.pkg` 结尾的 HTTPS URL 路径，由服务器下载；仅本机回环地址允许 HTTP |
| 服务器本地路径 | 填写服务所在电脑上的安装包完整路径，不是远程浏览器所在电脑的路径 |

版本和架构默认从安装包识别；填写版本时必须与包内版本一致。含多个架构的 macOS 通用包需选择
`x86_64` 或 `arm64`。同一服务一次处理一个发布任务，页面显示制包、差分、发布进度及失败原因。

**不需要先安装，也不需要手工解压。** 服务端不会执行输入安装程序：

| 输入 | CRX3 制包行为 |
| --- | --- |
| macOS `.pkg` | 接受安装到 `/Applications` 的单组件应用包，解析元数据和载荷；把原始 PKG 字节与固定 `.install` 脚本封装进 CRX3 |

macOS 不接受浏览器与 Updater 的组合安装包或含安装脚本的 PKG。工具不解包重建 `.app`，
因此 PKG 中的签名、符号链接和权限保持原样；CRX3 中 `.install` 显式保留 `0755` 执行权限并检查。
跨平台封装不执行 Apple 信任验证，正式 PKG 应先在 macOS 上完成签名、公证和验收。

每次网页导入会归档原始完整 CRX3，按 `(appid, platform, arch)` 发布历史中的最高版本，
对**最新 32 个低版本**分别生成或复用到最高版本的 Puffin 差分。可以补录低版本，线上目标不会倒退。
同一版本只接受相同 CRX 字节；不要对已发布版本重新签名或换包。

差分生成后会用 `-puffpatch` 回放并核对完整字节和 SHA-256，只有比完整 CRX 小才写入清单。
没有历史基准、超过 32 个基准范围、Puffin 缺失、生成/回放失败或补丁不够小时，均保留全量更新路径，
页面显示原因。完整包始终发布，生成的差分文件为 `.puff`。

### Windows 面板

展开“Windows 发布配置”，可设置服务器上的 **Node.js、7-Zip 和 Zucchini** 可执行文件路径；留空时自动查找。
建议 Node.js 22 或以上。7-Zip 用于解包和生成 ZIP，Zucchini 用于文件差分及回放验证。
Windows 发布不需要 macOS 的 CRX 签名私钥、Python、OpenSSL 或 Puffin。

1. 选择默认客户端架构，默认 `x86_64`。这是请求没有 `arch` 时的选择，不是导入安装包的架构。
2. 可填写更新说明、勾选强制更新，保存后用于后续发布。
3. 导入本项目的完整 `.exe` 安装包，或包含完整安装树的 `.zip`；支持上传、HTTPS 链接和服务器文件路径，单个文件最多 8 GiB。
4. 版本及架构自动识别；手工填写时必须与包内产品版本和 PE 架构一致。Windows 版本必须为四段数字。

服务仅解包输入 EXE，不运行或安装它。完整 ZIP 应具有以下布局，也允许整体包在 `Chrome-bin/` 中：

```text
xlb153.exe
1.0.0.2/
  xlbrowser.dll
  Installer/
    setup.exe
  ...其他版本文件
...其他根目录文件
```

每包只允许一个版本目录；不要上传 `patch.zip`、只包含 EXE 的 ZIP 或日常安装目录中混有多个版本的 ZIP。
服务保存完整安装树作为服务器差分基准，为同架构最高版本生成最近 32 个低版本的目录差分；补录低版本不会降低线上目标。
补丁经过回放校验，并按最终 ZIP 大小与全量 ZIP 比较；基准缺失、生成失败或补丁不够小时保留全量更新。
同版本不能替换为不同内容，已发布下载 URL 不会被新制品覆盖。

Windows 客户端检查地址为：

```text
http://127.0.0.1:9001/api/v1/update/check?arch=x86_64
```

`chrome://xenon-update/` 页面在未设置更新源时默认使用上述地址；已有自定义更新源仍优先使用。
服务修改了端口或部署到其他机器时，在页面填写实际地址并保存配置。

客户端会追加 `platform=win` 和 `version=<当前版本>`，现有客户端不会自动追加架构。
多架构分发时分别在客户端更新地址中指定 `arch=x86_64`、`arch=arm64` 或 `arch=x86`；未指定时使用已保存的默认架构。
服务只给当前版本精确匹配的差分，始终返回 `full_package` 用于回退。下载路径为 `/windows/artifacts/<文件名>.zip`。
Windows 客户端仍直接使用已安装文件重建更新，不需要缓存旧完整 ZIP。

## 初始化配置与客户端设置

`init` 创建服务配置和空清单，不安装客户端服务，不修改浏览器构建。
可以读取已签名制包结果的公钥摘要并查找原生工具：

```sh
xenon-update --config /ABS/state/xenon-update.json init \
  --public-origin https://updates.example.com \
  --artifacts /ABS/releases/1.0.0.1/artifacts.json \
  --build-dir /ABS/chromium/src/out/Release_64
```

也可使用 `--key-hash` 提供公钥摘要。摘要优先取 `--artifacts` 制包结果，其次是 `--key-hash`，
都未提供时再读取显式构建目录的 `args.gn`；
Puffin 可从 `--puffin`、`--build-dir`、`XENON_PUFFIN_TOOL`、程序旁边、源码常用 `out` 目录和 `PATH` 查找。
没有公钥摘要时仍可启动管理页面，但不会生成 `client-updater.gni`。

`client-updater.gni` 只是 macOS 客户端构建配置参考：将内容复制到 macOS 浏览器 `args.gn`，执行 `gn gen` 并重新编译。
网页后续保存发布配置不会重新生成该文件。客户端更新 URL 和公钥 pin 必须与最终发布配置一致，
修改服务端配置不能改变已安装客户端的信任。macOS Apple Team ID 与系统服务配置见
[SYSTEM_UPDATER.md](../SYSTEM_UPDATER.md)。

完整服务配置示例：

```json
{
  "schema_version": 1,
  "port": 9001,
  "admin_port": 9002,
  "manifest": "releases/catalog.json",
  "public_origin": "https://updates.example.com",
  "puffin": "/opt/xenon/tools/puffin",
  "crx_public_key_sha256": "BASE64_OF_SHA256_SPKI_DER",
  "packaging": {
    "key_path": "/srv/xenon/private/release-key.pk8",
    "tools_dir": "/opt/xenon/tools",
    "python": "/usr/bin/python3",
    "openssl": "/usr/bin/openssl"
  },
  "windows": {
    "node": "/usr/bin/node",
    "seven_zip": "/usr/bin/7zz",
    "zucchini": "/opt/xenon/tools/zucchini",
    "default_arch": "x86_64",
    "changelog": "修复已知问题",
    "force_update": false
  }
}
```

`packaging` 和 `crx_public_key_sha256` 用于 macOS，`windows` 是 Windows 的独立配置。
只发布 Windows 时可以不配置 macOS 私钥和公钥；`init` 关于缺少公钥或 Puffin 的提示不阻止 Windows 发布。
`public_origin` 只接受 HTTPS origin，不能带路径、尾部 `/`、查询串或账号；它控制客户端所见的更新及下载 URL，不开启 TLS。
省略时使用本机 HTTP 地址。网页保存的制包配置立即生效；手工修改配置文件或端口后需重启。
发布清单约每 2 秒重新加载，校验失败继续提供上一份有效清单；网页发布成功后也会立即加载。

## CLI 发布与单独生成差分

已有 [macOS 制包结果](../PACKAGING.md) 时仍可通过 CLI 发布：

```sh
xenon-update --config /ABS/state/xenon-update.json publish \
  --release /ABS/releases/1.0.0.2/release.json
xenon-update --config /ABS/state/xenon-update.json doctor
```

CLI `publish` 仅用于 macOS Omaha；Windows JSON + ZIP 使用网页的 Windows 面板发布。
CLI `publish` 自动合并应用/架构清单，并针对当前已发布版本生成差分；`--full-only` 只发布完整包。
它不执行网页导入的历史版本编排。输入支持单个 `release.json` 或 `{"schema_version":1,"apps":[...]}`，
也会校验导入随附的差分。CLI 拒绝降低当前已发布版本，相同版本只能重新导入相同 CRX。
发布使用锁、不可变制品文件名和原子清单替换。`doctor` 检查配置、工具、容器、大小和哈希；
网页制包另用原生工具验证 CRX 签名，客户端仍会独立验证。

无需初始化服务即可单独生成差分：

```sh
xenon-update delta \
  --old-crx /ABS/releases/1.0.0.1/update.crx3 \
  --new-crx /ABS/releases/1.0.0.2/update.crx3 \
  --from-version 1.0.0.1 --to-version 1.0.0.2 \
  --puffin /ABS/tools/puffin \
  --out /ABS/releases/1.0.0.2/update.puff \
  --metadata /ABS/releases/1.0.0.2/update.delta.json
```

默认检测 macOS PKG 内嵌 gzip，并调用本项目 Puffin 的 `-puffdiff-gzip`；其他 CRX 使用 `-puffdiff`。
`--opaque` 关闭 PKG 优化。元数据兼容 `make_updater_delta.py`。
补丁过大时单独 `delta` 仍输出经校验的补丁并设置 `recommended:false`，自动发布不会采用它；
生成失败时 `delta` 返回非零状态，自动发布则回退完整包。

## 历史、客户端缓存与磁盘空间

服务器的 `publisher/history` 保存 macOS 历次原始 CRX3，`releases` 保存 macOS 对外制品；
`publisher/windows` 保存 Windows 历史安装树、全量和差分制品及发布目录。历史内容不会自动按版本清理。
32 是每个当前目标最多生成差分的低版本数量，不是只保存 32 个历史包。
macOS 旧下载 URL 在当前服务进程内保持可用，重启后只加载当前清单引用的制品，应避开正在进行的升级重启服务。

macOS Omaha 客户端差分基准是缓存的**完整签名 CRX3**，不是安装目录或旧 `.pkg`。
Windows JSON + ZIP 流程直接使用已安装文件，不依赖本节的 CRX 缓存。
Chromium 缓存每个 app ID 只保留一份完整 CRX；新 CRX 替换旧缓存。首次安装或首次迁移到 Omaha 时
通常没有基准，第一次更新下载全量；之后版本与哈希匹配才使用差分。清掉缓存会让下次更新回退全量。

这是用一份完整 CRX 的常驻磁盘空间换后续下载流量。升级期间还需容纳补丁、重建后的完整 CRX、
解包目录和安装准备/备份文件，不能只按差分大小预留空间。服务端生成差分也需要数倍包体大小的内存及临时磁盘。

## Linux 部署与管理端访问

以下为按实现整理的部署要求，**尚未完成 Linux 主机端到端实测**。
只分发已有制品时部署 `xenon-update`、配置和制品即可；按网页发布的目标平台准备依赖：

- macOS 面板：Python 3.10+（含 `lzma`）、OpenSSL，以及本项目构建的 Linux 原生 `crx3_build_action`、`xenon_updater_verify`、`puffin`。
- Windows 面板：Node.js 22+、Linux 原生 `7zz` / `7z` / `7za`，以及本项目构建的 Linux 原生 `zucchini`。
- 所有工具都必须与**服务所在系统**一致；部署 Linux 不能直接复制 Windows `.exe` 工具。
- 程序旁的 `updater/` 脚本目录。可以复制整个目录；最小文件如下。

```text
/opt/xenon/
  xenon-update
  tools/
    crx3_build_action
    xenon_updater_verify
    puffin
    zucchini
  updater/
    prepare_installer.py
    make_updater_crx.py
    updater_packaging_common.py
    updater_host_util.py
    prepare_windows_release.mjs
    make_directory_diff.js
```

macOS CRX/Puffin 工具来自 `//xenon_overlay/tools/updater:release_tools`，构建入口见 [PACKAGING.md](../PACKAGING.md)。
Windows 目录差分所需的 Zucchini 另行构建 `zucchini` 目标，例如 `autoninja -C out/updater_tools_x64 zucchini`。
大型差分应在独立工具构建目录使用系统分配器，关闭 `use_allocator_shim`、`use_partition_alloc_as_malloc`
及依赖它的 `enable_backup_ref_ptr_support`、`enable_pointer_compression_support`。
当前 `third_party/puffin/BUILD.gn` 自动切换系统分配器的逻辑**仅覆盖 Windows**；Linux 不会自动切换。
500 MB 不是协议上限，不能据此判断大包一定能生成差分。

所有监听器固定绑定 `127.0.0.1`：

| 配置 | 更新监听器 | 管理页面 |
| --- | --- | --- |
| 未配置 `public_origin` | 默认 `9001` | 同一端口 `http://127.0.0.1:9001/` |
| 配置了 `public_origin` | 默认 `9001`，只提供更新和只读状态 | 独立 `http://127.0.0.1:9002/` |

管理端口可用配置文件的 `admin_port` 调整；默认是更新端口加一（更新端口为 65535 时使用 65534）。
管理接口校验会话令牌、Host 和请求来源，只供本机访问；不要反向代理管理端口。
HTTPS 反代按需转发更新端口的 `/service/update2/json`、`/artifacts/`（macOS），
以及 `/api/v1/update/check`、`/windows/artifacts/`（Windows），配置见
[UPDATE_SERVICE.md](../UPDATE_SERVICE.md#6-对外部署与常驻运行)。

在另一台电脑上，通过 SSH 隧道访问远程管理页面：

```sh
ssh -N -L 9002:127.0.0.1:9002 user@update-server
```

保持 SSH 连接，在本机打开 `http://127.0.0.1:9002/`。隧道本地端口应与服务器管理端口相同，
以匹配 Host/Origin 校验；如冲突，修改服务器 `admin_port` 并将隧道两侧端口同步调整。

systemd 示例，假设服务账号和持久目录已准备好，该账号可以读取私钥并写入配置、发布及历史目录：

```ini
[Unit]
Description=Xenon update service
After=network.target

[Service]
Type=simple
User=xenon-update
ExecStart=/opt/xenon/xenon-update --config /srv/xenon/state/xenon-update.json serve
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

保存到 `/etc/systemd/system/xenon-update.service` 后执行：

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now xenon-update.service
sudo journalctl -u xenon-update.service -f
```

这是发布端服务；Windows 客户端不新增后台更新服务。原有 Node Omaha 服务、独立目录差分工具和 Python macOS 制包入口仍保留。

## 验证

```sh
cargo test --locked --manifest-path xenon_overlay/tools/updater/xenon_update_tool/Cargo.toml
cargo clippy --locked --manifest-path xenon_overlay/tools/updater/xenon_update_tool/Cargo.toml --all-targets -- -D warnings

# 实际 Puffin 差分/回放；环境变量指向宿主平台可执行文件
XENON_PUFFIN_TOOL=/ABS/tools/puffin cargo test --locked \
  --manifest-path xenon_overlay/tools/updater/xenon_update_tool/Cargo.toml -- --ignored

# HTTP 管理端和旧 Node 服务兼容测试；Windows 网页制包需要 Node.js
XENON_UPDATE_TOOL=/ABS/xenon-update XENON_VERIFY_TOOL=/ABS/xenon_updater_verify \
  node --test xenon_overlay/tools/updater/xenon_update_tool/tests/compatibility.mjs \
  xenon_overlay/tools/updater/xenon_update_tool/tests/admin.test.mjs
```

Windows 制包单测：

```sh
node --test xenon_overlay/tools/updater/prepare_windows_release.test.mjs
```

真实 Windows HTTP 发布回归需要两版 `prepare_windows_release.mjs` 的输出目录，
目录内保留 `release.json`、`package.zip` 和 `application/`。测试使用独立配置和随机端口，
覆盖路径/URL/上传、补录基线、完整下载哈希、Zucchini 重构与重启，不修改已安装浏览器。
从源码根目录运行，例如 PowerShell：

```powershell
$env:XENON_UPDATE_TOOL = 'H:\chromium_142\src\xenon_overlay\tools\updater\xenon_update_tool\target\release\xenon-update.exe'
$env:XENON_WINDOWS_OLD_PACKAGE = 'H:\releases\prepared-old\package.zip'
$env:XENON_WINDOWS_NEW_PACKAGE = 'H:\releases\prepared-new\package.zip'
$env:XENON_WINDOWS_7ZIP = 'H:\chromium_142\src\third_party\lzma_sdk\bin\win64\7za.exe'
$env:XENON_WINDOWS_ZUCCHINI = 'H:\chromium_142\src\out\Release_64\zucchini.exe'
node --test xenon_overlay/tools/updater/xenon_update_tool/tests/windows.test.mjs
```

未提供真实包路径时，该 HTTP 原生回归会跳过；大包验证会重复制包，需预留数分钟及临时磁盘空间。
