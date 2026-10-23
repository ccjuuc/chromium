# 升级服务：macOS Omaha 与 Windows JSON + ZIP

macOS 使用 **Omaha 4.0 + CRX3 + Puffin** 和系统级 Chromium Updater。
Windows 使用 **JSON + ZIP + Zucchini**，不接入 Omaha；Windows 制包与服务见本文第 8 节。
axum 服务通过两个独立面板管理这两种发布流程，可部署在 Windows、macOS 或 Linux。

| 项目 | macOS | Windows |
| --- | --- | --- |
| 检查接口 | `POST /service/update2/json` | `GET /api/v1/update/check` |
| 应用 ID | `org.chromium.Chromium` | 原有 JSON 客户端 |
| 平台 | `mac` | `win` |
| 架构 | `x86_64`、`arm64` | `x86_64`、`arm64`、`x86` |
| 更新执行位置 | 系统级 Chromium Updater | 浏览器原有目录更新流程 |
| 全量包 | CRX3 内封装应用 PKG 和 `.install` | 完整安装目录 ZIP |
| 独立后台服务 | 首次组合 PKG 安装的 Xenon Updater | 无 |

推荐使用 [axum 一体化工具 `xenon-update`](xenon_update_tool/README.md)：
`init` 初始化配置，`serve` 启动更新服务与浏览器发布页面；网页支持上传、HTTPS 链接和服务器本地路径
导入 macOS 应用 `.pkg` 或 Windows `.exe` / 完整安装目录 `.zip`，自动制包、归档历史并生成差分。
macOS 已有 `release.json` 也可用 CLI `publish` 发布；Windows 使用网页面板。
只提供已有制品的服务不依赖 Node.js/Python；macOS 网页制包需要 Python 3.10+、OpenSSL 和原生 CRX/Puffin 工具，
Windows 网页制包需要 Node.js、7-Zip 和 Zucchini。

## 推荐：启动 axum 工具并在网页发布

在 `xenon-update.exe` 所在目录打开 PowerShell：

```powershell
.\xenon-update.exe init
.\xenon-update.exe serve
```

已有配置时只运行第二条。命令是 `serve`，不是 `--serve`；默认配置为当前目录下的
`xenon-update/xenon-update.json`，可用全局 `--config` 参数固定路径。
未配置 `public_origin` 时，打开 `http://127.0.0.1:9001/` 管理和发布。

页面上方有 **macOS** 和 **Windows** 两个面板，分别保存配置、导入安装包和查看历史。Windows 操作步骤见第 8 节。
macOS 首次在“macOS 发布配置”填写服务器上的发布私钥路径、原生工具目录及 Python/OpenSSL 路径。
公钥摘要可填写现有 SHA-256(SPKI DER)，也可留空由 OpenSSL 从私钥自动计算，必须与客户端 pin 一致。
服务保存私钥路径并在制包时读取私钥，不上传或自动生成私钥。原生工具目录需包含
`crx3_build_action`、`xenon_updater_verify` 和 `puffin`（Windows 带 `.exe`）。

在“发布安装包”选择上传文件、HTTPS 下载链接或服务器本地路径；版本及架构默认自动识别。
单个安装包最大 8 GiB，URL 路径须以 `.pkg` 结尾；只有回环测试地址允许 HTTP。
服务不会安装输入包：解析检查后将**原始应用 PKG**与 `.install` 封装，保留 PKG 字节、签名、符号链接和权限，
并确保 `.install` 为 `0755`。PKG 必须是安装到 `/Applications` 的单组件应用包，不接受含安装脚本或
浏览器与 Updater 的组合安装包。Apple 签名、公证和信任验证应在 macOS 制包阶段完成。

macOS 网页发布按应用、平台和架构保留历史，始终发布最高版本，并为最新 **32 个低版本**生成或复用差分。
支持补录旧版本；同一版本不得更换 CRX 字节。差分经回放验证且小于完整包才发布，缺少基准、工具不可用
或差分失败时保留全量回退。CLI `publish` 针对当前已发布基准生成差分，不执行网页导入的完整历史编排。

对外部署时在配置中设置 `public_origin`，例如 `https://updates.example.com`：更新监听器默认仍是
`127.0.0.1:9001`，管理页面改为独立的 `http://127.0.0.1:9002/`，可用 `admin_port` 配置。
HTTPS 只反代更新端口，管理端保持私有。远程使用 SSH 隧道：

```sh
ssh -N -L 9002:127.0.0.1:9002 user@update-server
```

然后在本机打开 `http://127.0.0.1:9002/`。本地与远端管理端口保持一致以通过 Host/Origin 校验。
Linux 网页制包需安装 Python 3.10+（含 `lzma`）和 OpenSSL，并部署 **Linux 原生** CRX 构建、验证和 Puffin 工具；
程序旁 `updater/` 目录至少放入 `prepare_installer.py`、`make_updater_crx.py`、
`updater_packaging_common.py`、`updater_host_util.py`。
Linux 大包 Puffin 应在独立构建目录使用系统分配器，当前自动切换只覆盖 Windows；尚未完成 Linux 端到端实测。
Windows 网页制包则需要 Linux 原生 Node.js、7-Zip、Zucchini，以及程序旁 `updater/` 中的
`prepare_windows_release.mjs` 和 `make_directory_diff.js`；目标是 Windows 安装包也应使用 Linux 原生工具。
完整配置、目录布局、systemd 示例和缓存空间说明见 [工具文档](xenon_update_tool/README.md)。

以下详细说明现有 Node 兼容服务及共用的清单、客户端和 HTTPS 配置。
命令在仓库根目录执行；所有 `YOUR_*`、`/ABS/...` 和示例域名均须替换。
Node 服务本身只需要 Node.js，推荐 Node.js 22 或以上，无需 `npm install`。

## 1. 准备发行目录

先按 [PACKAGING.md](PACKAGING.md) 使用 `package_macos.py` 生成目标版本，
或通过网页导入已准备的 macOS 应用 PKG；它们都输出可以直接使用的 `release.json`。
首次联调可以只发布完整 CRX3，确认下载、验签及安装成功后再增加差分。

一个 macOS 发布目录示例：

```text
releases/1.0.0.2/
  release.json
  artifacts.json
  update-1.0.0.2-mac-x86_64.crx3
  update-1.0.0.1-to-1.0.0.2-mac-x86_64.puff   # 可选差分
```

Node 兼容服务不会现场制包。`release.json` 引用的所有 CRX3/差分文件必须在清单同一目录，
`file` 字段只接受文件名，不接受 URL 或目录路径。大小和 SHA-256 必须是最终文件的实际值；
包必须是普通文件，不能使用符号链接或硬链接代替复制。

## 2. 启动发布服务

在 Windows 发布宿主上验证，PowerShell：

```powershell
node xenon_overlay/tools/updater/omaha_update_server.js `
  --manifest H:\releases\1.0.0.2\release.json `
  --port 9001
```

macOS/Linux：

```sh
node xenon_overlay/tools/updater/omaha_update_server.js \
  --manifest /ABS/releases/1.0.0.2/release.json \
  --port 9001
```

成功后打印 `listening` 和已加载的应用/版本/平台/架构。端点为
`http://127.0.0.1:9001/service/update2/json`，制品路径为 `/artifacts/<文件名>`。
终端进程需要保持运行；按 Ctrl+C 停止。服务启动时会校验清单、文件容器、大小及哈希，
配置错误会直接退出，不会返回占位包。

| 参数 | 说明 |
| --- | --- |
| `--manifest` | 必填，已准备的发布清单路径 |
| `--port` | 默认 `9001`；只监听 `127.0.0.1` |
| `--public-origin` | HTTPS 对外域名，例如 `https://updates.example.com`；用于生成包下载 URL |

没有 `--host`、`--version` 或配置环境变量。目标版本写在清单里。
`--public-origin` 不开启 TLS，也不改变监听地址；值不能带末尾 `/`、路径、账号、查询串或片段。

## 3. 一个清单发布多个应用和架构

单个制包输出的 `release.json` 只包含一个发行对象。要共用同一个服务实例，
将各发行对象放入 `apps` 数组，去掉每个子对象的 `schema_version`，在顶层保留一次。
以下是配置结构示意，大小和摘要必须从制包结果复制，不能直接使用占位值：

```json
{
  "schema_version": 1,
  "apps": [
    {
      "appid": "org.chromium.Chromium",
      "platform": "mac",
      "arch": "x86_64",
      "version": "1.0.0.2",
      "full": {
        "file": "update-1.0.0.2-mac-x86_64.crx3",
        "size": 123456789,
        "sha256": "REPLACE_WITH_EXACT_64_LOWERCASE_HEX_DIGITS"
      }
    },
    {
      "appid": "org.chromium.Chromium",
      "platform": "mac",
      "arch": "arm64",
      "version": "1.0.0.2",
      "full": {
        "file": "update-1.0.0.2-mac-arm64.crx3",
        "size": 234567890,
        "sha256": "REPLACE_WITH_EXACT_64_LOWERCASE_HEX_DIGITS"
      }
    }
  ]
}
```

将全部引用的制品复制到合并清单旁边。文件名必须在整个目录清单中唯一；
如果 macOS 原输出为 `update-1.0.0.2.crx3`，可在发布副本中增加平台/架构后缀，
同时更新 `full.file`。仅重命名文件不会改变它的大小或 SHA-256。

`(appid, platform, arch)` 不得重复，appid 不区分大小写。每个组合分别配置目标版本，
允许 macOS 不同架构发布不同版本。请求中的 `x64` 归一为 `x86_64`；
运行在 ARM64 硬件上的 x64 客户端仍选择 x64 包。
匹配不到平台或架构时返回对应的不支持状态，不会拿另一个平台的包作为回退。

## 4. 增加差分

macOS 使用 `make_updater_delta.py`，输入是已经发布、保持原始字节的旧 CRX3
和此次新 CRX3。完整命令见 [PACKAGING.md](PACKAGING.md#3-macos-omaha-差分生成)。

差分工具会生成补丁和元数据。确认元数据中的 `full_sha256` 等于目标 `full.sha256`，
再将以下六个字段复制成对象，加入对应发行对象的 `differential` 数组：

```text
from_version, previous_sha256, type, file, size, sha256
```

不要将整个元数据文件放入 `differential`；额外字段会被拒绝。
服务仅在当前版本、旧 CRX 缓存哈希、支持的补丁格式都匹配，并且补丁严格小于完整包时提供差分。
完整 CRX3 必须保留，客户端在缓存未命中或差分失败时使用它。

差分基准是客户端缓存中的旧完整 CRX3，不是磁盘安装目录。
首次安装后没有旧 CRX 缓存时，首次 Omaha 更新走完整包是正常行为。
Chromium 每个 app ID 只缓存一份完整 CRX3，新包替换旧缓存；删除缓存会使下一次更新回退全量。
这会额外占用约一份完整 CRX 的常驻磁盘空间，用来减少后续下载流量；升级期间还需要补丁、重建包、
解包和安装准备/备份空间。服务端历史 CRX 与对外制品不会自动清理，32 个差分基准上限不限制历史存储数量。
旧 `patch.zip` 不能直接作为 Puffin 补丁发布，也不能通过改后缀转换格式。

## 5. 客户端构建配置

**仅 macOS** 的 `args.gn` 中使用以下参数；Windows 保持 `enable_xenon_updater = false`：

```gn
enable_xenon_updater = true
xenon_updater_update_url = "https://updates.example.com/service/update2/json"
xenon_updater_crx_public_key_hash = "BASE64_OF_SHA256_OF_RELEASE_SPKI_DER"
```

正式构建另外设置 `xenon_updater_mac_team_id = "YOURTEAMID"`，
并完成应用、Updater 和 PKG 的签名公证，详见 [SYSTEM_UPDATER.md](SYSTEM_UPDATER.md)。
Windows JSON + ZIP 不使用上述 Omaha GN 参数，也不安装 Chromium Updater 后台服务。

公钥摘要必须是发布私钥对应的 DER SubjectPublicKeyInfo 的 SHA-256，标准 Base64 编码，
不是 CRX 文件哈希或证书指纹。制包参数 `--expected-key-sha256` 必须对应同一摘要，
它可以接受 hex 或 Base64；GN 参数使用 Base64。Node/只分发模式只部署签名后的包；
启用 axum 网页制包时，发布主机需要保存并读取对应私钥。

已有 RSA DER PKCS#8 发布私钥时，可用以下方式计算公钥摘要，避免在 PowerShell 管道中传二进制：

```text
openssl pkey -inform DER -in /ABS/PRIVATE/release-key.pk8 -pubout -outform DER -out /ABS/TEMP/release-public.der
python -c "import base64,hashlib,pathlib,sys; print(base64.b64encode(hashlib.sha256(pathlib.Path(sys.argv[1]).read_bytes()).digest()).decode())" /ABS/TEMP/release-public.der
```

本机 HTTP 联调须在构建时明确改为：

```gn
xenon_updater_update_url = "http://127.0.0.1:9001/service/update2/json"
xenon_updater_allow_insecure_localhost = true
```

HTTP 开关只允许本机回环测试，公钥摘要仍然必填。正式使用 HTTPS。
更新源和发布信任编译进客户端；启用 macOS 系统更新后，页面和 Electron `setFeedURL()`
不能将它切换到另一台服务器。修改构建参数后需要重新生成构建文件并编译浏览器。

## 6. 对外部署与常驻运行

axum 在 `xenon-update.json` 设置 `public_origin` 后用 `serve` 启动；常驻运行的 systemd 示例见
[axum 工具文档](xenon_update_tool/README.md#linux-部署与管理端访问)。
仅使用 macOS Node 兼容服务时，启动参数增加 HTTPS 域名：

```sh
node xenon_overlay/tools/updater/omaha_update_server.js \
  --manifest /srv/xenon/releases/1.0.0.2/release.json \
  --port 9001 \
  --public-origin https://updates.example.com
```

在现有已配置证书的 Nginx HTTPS `server` 中加入以下按需转发规则。
四个路径由 axum 提供；Node 兼容服务只提供前两个 macOS 路径。

```nginx
location = /service/update2/json {
    client_max_body_size 1m;
    proxy_set_header Host 127.0.0.1:9001;
    proxy_set_header Origin $http_origin;
    proxy_pass http://127.0.0.1:9001;
}

location /artifacts/ {
    proxy_set_header Host 127.0.0.1:9001;
    proxy_set_header Origin $http_origin;
    proxy_buffering off;
    proxy_pass http://127.0.0.1:9001;
}

# 以下两个路径用于 axum 的 Windows JSON + ZIP 发布。
location = /api/v1/update/check {
    proxy_set_header Host 127.0.0.1:9001;
    proxy_set_header Origin $http_origin;
    proxy_pass http://127.0.0.1:9001;
}

location /windows/artifacts/ {
    proxy_set_header Host 127.0.0.1:9001;
    proxy_set_header Origin $http_origin;
    proxy_buffering off;
    proxy_pass http://127.0.0.1:9001;
}
```

检查接口和包下载都要经过 HTTPS。后端要求 loopback Host，因此必须重写 Host；
保留客户端 Origin，避免代理使原本应被拒绝的网页请求通过。
不要将后端改成直接监听公网来绕过这些检查。

Linux 上可以由 systemd 托管 Node 进程。以下假设已有服务账号 `xenon-update`，
源码位于 `/srv/chromium/src`，账号能读取清单和制品：

```ini
[Unit]
Description=Xenon Omaha update endpoint
After=network.target

[Service]
Type=simple
User=xenon-update
WorkingDirectory=/srv/chromium/src
ExecStart=/usr/bin/node /srv/chromium/src/xenon_overlay/tools/updater/omaha_update_server.js --manifest /srv/xenon/releases/1.0.0.2/release.json --port 9001 --public-origin https://updates.example.com
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

保存为 `/etc/systemd/system/xenon-update.service` 后执行：

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now xenon-update.service
sudo journalctl -u xenon-update.service -f
```

这里的 Node 发布服务与 macOS 客户端的系统 Updater 是两个不同进程。
Windows 客户端不安装上述 systemd 服务，也不需要任何对应的 Windows 更新服务。

发布新版本时先在新的完整目录准备制品和清单，再修改服务的 `--manifest` 路径并重启。
清单没有热加载；不要覆盖正在提供下载的文件。制品运行中变化会使下载返回 409。
更改 systemd 单元后执行 `daemon-reload`，再 `restart xenon-update.service`。

## 7. 客户端使用与排查

macOS 首先安装包含系统 Updater 的正式组合 PKG，再从 `/Applications/xl153.app` 启动：

```sh
sudo /usr/sbin/installer -pkg /ABS/releases/xl153-with-updater-1.0.0.2.pkg -target /
readlink "/Library/Application Support/Xenon/XenonUpdater/Current"
sudo launchctl print system/com.xenon.XenonUpdater.wake.system
sudo tail -n 100 "/Library/Application Support/Xenon/XenonUpdater/updater.log"
open /Applications/xl153.app
```

安装 PKG 前正常退出浏览器。单独应用 PKG 不包含 Updater；开发模式组合 PKG 有安装阻断，
不能用于正式部署系统服务。macOS 更新进入安装阶段后按提示退出，完成后手动重新打开。
具体安装行为和状态以 macOS 更新页面为准，不以“下载 100%”作为安装成功。

| 现象 | 检查项 |
| --- | --- |
| 启动服务时报缺包或哈希错误 | 清单与最终制品同目录；大小、摘要、文件名是否一致 |
| 浏览器访问检查 URL 得到 405 | 正常；接口要求 Omaha 4.0 JSON POST，不能用地址栏 GET 验证 |
| HTTP 403 | Host 是否改成 loopback；是否携带网页 Origin；请求是否直接来自本机/反代 |
| HTTP 501，错误提及 CUP | 客户端仍使用 Google CUP 配置；使用本项目的 Xenon 信任配置重新构建 |
| `error-unknownApplication` | appid 是否与客户端一致 |
| `error-osnotsupported` / `error-hwnotsupported` | 是否配置对应 `platform` / 客户端 `arch` 发行对象 |
| `noupdate` | 当前版本已达到目标；或 `minimum_version` / 客户端策略排除了更新 |
| 首次更新下载完整 CRX3 | 客户端没有匹配的旧 CRX3 缓存，属于正常回退条件 |
| 明明有差分却始终下完整包 | 基准版本、旧 CRX 哈希、补丁类型及补丁大小是否满足条件 |
| 页面不能修改 Feed URL | macOS 系统更新使用构建时固定地址；检查 GN 参数及实际安装的构建版本 |

本地服务协议回归命令：

```sh
node --test xenon_overlay/tools/updater/omaha_update_server.test.js
```

## 8. Windows 继续使用 JSON + ZIP

Windows 客户端不再接入 Omaha，保持 `enable_xenon_updater = false`；`enable_xenon_updater = true` 仅支持 macOS。
Windows JSON 接口继续提供完整 ZIP 和 `patch.zip`，客户端以当前安装目录为差分基准，验证后还原完整的 `Chrome-bin`，
调用目标版本的 `setup.exe --install-source-dir=... --install-directory=...`，复用正式安装器的文件和注册表事务、快捷方式及旧版本清理。
这里的完整目录来自差分还原，无需重新下载全量包或重新压成 7z；不要求保留旧完整 CRX 或 ZIP 缓存，也不新增后台更新服务。
目标安装包必须包含支持目录输入的新 `setup.exe`，旧包需要重新构建后发布。安装和启动器切换、浏览器重启是不同阶段；
后续阶段失败时报告错误并保留重试能力，不把已经成功的原生安装误报为已回滚。系统级提权仅在用户明确点击安装时发起。

推荐直接打开 axum 管理页面的 **Windows** 面板：

1. 展开“Windows 发布配置”，配置服务器上的 Node.js、7-Zip 和 Zucchini 路径；留空自动查找，推荐 Node.js 22+。
2. 设置默认客户端架构（默认 `x86_64`），填写更新说明及强制更新选项并保存。
3. 上传本项目的完整 EXE 安装包或安装目录 ZIP；也可提供 HTTPS 链接或服务器上的完整文件路径，最大 8 GiB。
4. 服务自动识别版本和架构，生成全量 ZIP，并为同架构最高版本生成最近 32 个低版本的差分。补录旧版本不会使目标版本倒退。

输入程序不会被执行。完整 ZIP 的根目录应包含 `xlb153.exe` 和唯一的四段版本目录，
版本目录包含 `xlbrowser.dll`、`Installer/setup.exe` 及完整资源；可以整体包在 `Chrome-bin/` 下。
不要提交差分 ZIP，或仅把安装 EXE 压缩成 ZIP。手工填写的版本和架构必须与安装包一致。
差分经过回放验证，最终 ZIP 小于全量 ZIP 才发布；没有匹配基准、工具不可用或差分失败时使用全量。

服务配置中的独立 Windows 部分为：

```json
"windows": {
  "node": null,
  "seven_zip": null,
  "zucchini": null,
  "default_arch": "x86_64",
  "changelog": "修复已知问题",
  "force_update": false
}
```

三项工具路径为 `null` 表示自动查找，不需要 macOS 的 CRX 签名配置。
更新说明和强制更新选项应用于后续发布；默认架构用于客户端没有发送 `arch` 的请求。
客户端更新地址示例：

```text
https://updates.example.com/api/v1/update/check?arch=x86_64
```

现有客户端会追加 `platform=win` 和 `version=<当前版本>`，不会自动追加架构。
部署多架构时应分别指定 `arch=x86_64`、`arch=arm64` 或 `arch=x86`，避免使用错误的默认架构。
服务端按客户端当前版本精确选择差分，并始终提供 `full_package`。客户端下载不需要旧完整 ZIP 缓存，
服务器在 `publisher/windows` 保存展开的历史安装目录作为差分基准。

独立命令仍可从两版完整 EXE 或安装目录生成差分：

```powershell
node xenon_overlay/tools/updater/make_directory_diff.js `
  --old H:\releases\xlb153_installer_1.0.0.1.exe `
  --new H:\releases\xlb153_installer_1.0.0.2.exe `
  --out H:\releases\1.0.0.2\patch.zip `
  --zucchini out/Release_64/zucchini.exe `
  --7zip third_party/lzma_sdk/bin/win64/7za.exe
```

差分 ZIP 的大小和 SHA-256 按工具输出写入原 JSON 服务，完整 ZIP 仍须保留用于回退。
JSON 清单结构、完整包准备、`mock_update_server.js` 本机联调与真实升级验证见 [README.md](README.md)。
`mock_update_server.js` 是测试服务，不是本节 Omaha 服务的另一个协议入口。

axum 同时提供 Windows JSON 与 macOS Omaha，可部署在同一域名的不同路径；两者不会在客户端自动互相回退。
CLI `publish` 仍用于 macOS，Windows 自动发布通过网页进行。
macOS 发布私钥或其公钥 pin 的变更仍需要单独的信任迁移，不能只在服务端更换签名私钥。
