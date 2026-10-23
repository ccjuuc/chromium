# Windows / macOS 打包与差分脚本

Windows 使用 JSON + ZIP + Zucchini，macOS 使用 Omaha 4.0 + CRX3 + Puffin。以下命令在仓库根目录执行：

| 入口 | 输入 | 输出 |
| --- | --- | --- |
| `package_macos.py` | 已签名的 `xl153.app`、CRX 私钥，可选 `XenonUpdater.app` | 应用 PKG、签名 CRX3、Omaha 清单，可选首次安装 PKG |
| `make_directory_diff.js` | Windows 旧、新安装目录或完整 EXE | JSON 重建清单及 `patch.zip` |
| `prepare_installer.py` | 已准备的 macOS 应用 PKG、CRX 私钥 | 保留原 PKG 字节的签名 CRX3 和 Omaha 清单 |
| `make_updater_delta.py` | 已归档的旧、新完整 CRX3 | 优化 Puffin 补丁和校验元数据 |

从 `.app` 生成、签名和公证 PKG 的 `package_macos.py` 须在 macOS 上执行。已生成的应用 PKG 可由
Windows/Linux 上的 `prepare_installer.py` 或 axum 网页继续封装，不会改写原 PKG。
Windows JSON 差分需要 Node.js、原生 7-Zip 和 Zucchini；macOS PKG 封装需要 Python 3.10+、OpenSSL 与宿主平台原生 CRX 工具。
脚本只生成、验证文件，不安装更新、不启动服务、不修改输入或 GN 配置。Windows 客户端不接入 Omaha，不新增 Updater 服务。

macOS Omaha 服务与客户端配置见 [UPDATE_SERVICE.md](UPDATE_SERVICE.md)；Windows JSON 工具见 [README.md](README.md)。
工具构建目录若未启用 `enable_xenon_updater`，在它的 `args.gn` 的
`root_extra_deps` 列表中加入 `"//xenon_overlay/tools/updater:release_tools"`，再运行 `gn gen`。
构建 macOS 原生工具：

```sh
autoninja -C out/release_arm64 xenon_overlay/tools/updater:release_tools
autoninja -C out/updater_tools_arm64 puffin
```

大文件差分推荐独立系统 malloc 版本的 Puffin。首次配置该构建目录见
[SYSTEM_UPDATER.md](SYSTEM_UPDATER.md#pkg-压缩载荷的差分优化)。

## 1. macOS 打包

应用必须事先签名。产品版本自动读取 `CFBundleShortVersionString`，不改写版本或
重新签名应用。CRX 私钥使用 RSA、DER PKCS#8、当前用户拥有的 `0600` 普通文件；
`--expected-key-sha256` 必须与客户端 GN 中的发布公钥摘要相同，接受 hex 或 base64。
私钥始终保存在输出目录和仓库之外，同一发布通道应持续使用同一密钥。

当前不购买 Developer ID，可对已 ad-hoc 签名的应用使用开发模式：

```sh
python3 xenon_overlay/tools/updater/package_macos.py \
  --app out/release_arm64/xl153.app \
  --key /ABS/PRIVATE/release-key.pk8 \
  --expected-key-sha256 PUBLIC_KEY_SHA256 \
  --tools-dir out/release_arm64 \
  --development \
  --out-dir /ABS/ARTIFACTS/1.0.0.4
```

将占位路径和摘要替换为实际值，输出目录必须不存在。一次成功输出：

```text
1.0.0.4/
  application-1.0.0.4.pkg
  update-1.0.0.4.crx3
  release.json
  artifacts.json
```

- `application-*.pkg`：仅浏览器，可用于应用安装或更新载荷，不包含独立 Updater 服务。
- `update-*.crx3`：固定发布公钥签名的完整更新包，包含 PKG 和 `.install`。
- `release.json`：可直接交给 `omaha_update_server.js --manifest` 的完整更新清单。
- `artifacts.json`：开发模式标记、制品大小、SHA256、公钥摘要及安装包信息。

增加 `--updater-app /ABS/XenonUpdater.app` 会额外生成
`xl153-with-updater-1.0.0.4.pkg`，包含浏览器和系统 Updater。默认只接受正式命名和
标识的 Updater；不自动换成 `XenonUpdater_test.app`。开发模式的这个组合 PKG
保留安装前阻断，不能用来安装 ad-hoc root 服务；此前获授权的本机测试安装器是独立
测试流程。单独的应用 PKG 没有这项服务安装阻断。

正式发布先准备已签名、公证的浏览器与 `XenonUpdater.app`，移除 `--development`，
提供以下参数：

```sh
python3 xenon_overlay/tools/updater/package_macos.py \
  --app /ABS/SIGNED/xl153.app \
  --updater-app /ABS/SIGNED/XenonUpdater.app \
  --key /ABS/PRIVATE/release-key.pk8 \
  --expected-key-sha256 PUBLIC_KEY_SHA256 \
  --tools-dir out/release_arm64 \
  --team-id YOURTEAMID \
  --installer-identity 'Developer ID Installer: YOUR IDENTITY (YOURTEAMID)' \
  --notary-profile YOUR_PROFILE \
  --out-dir /ABS/ARTIFACTS/1.0.0.4
```

正式模式默认启用，缺少证书参数即失败；会验证应用签名和公证票据，对两个 PKG
签名、公证并装订票据，最后通过 Chromium 原生 CRX 验签。`--notary-profile` 指向已
配置的钥匙串凭据。所有步骤成功后才整体输出目录，失败不会留下可误用的半成品目录。

架构从应用和 Updater 内的 Mach-O 交集推导；通用应用需显式指定
`--arch arm64` 或 `--arch x86_64`，表示此次清单服务的客户端架构。
`--wait-seconds` 默认 180 秒，控制更新安装器等待浏览器正常退出的上限。

## 2. Windows JSON + ZIP 差分

Windows 不生成 CRX3。现有工具以旧安装目录或旧 EXE 为输入，构建逐文件重建清单及差分 ZIP。
客户端从当前安装目录读取基准文件，不要求缓存旧完整安装包；新增文件和不划算的差分会使用完整文件。

PowerShell 示例：

```powershell
autoninja -C out/Release_64 zucchini
node xenon_overlay/tools/updater/make_directory_diff.js `
  --old H:\releases\xlb153_installer_1.0.0.1.exe `
  --new H:\releases\xlb153_installer_1.0.0.2.exe `
  --out H:\releases\1.0.0.2\patch.zip `
  --zucchini out/Release_64/zucchini.exe `
  --7zip third_party/lzma_sdk/bin/win64/7za.exe
```

`--old` 和 `--new` 也可以是完整 `Chrome-bin` 目录，必须分别只包含一个版本目录并保留 `setup.exe`。
工具自动处理 Chromium EXE 中的 CAB 和 7z 浏览器载荷，不执行安装程序。输出报告含补丁大小、SHA-256 和 JSON 重建清单；
将它们配置到 Windows JSON 更新服务，并保留对应完整 ZIP 用于差分失败回退。
完整目录格式、JSON 服务和验证方法见 [README.md](README.md)。

也可在 [axum 工具](xenon_update_tool/README.md) 的 Windows 面板上传 EXE/完整安装目录 ZIP，
或填写 HTTPS 链接、服务器路径，自动生成完整 ZIP、历史版本差分及 JSON 更新配置。
macOS 面板独立发布 PKG/Omaha，两个平台的工具配置与发布协议分别保存。
Windows 构建保持 `enable_xenon_updater = false`；这个开关仅用于 macOS 系统 Updater。

## 3. macOS Omaha 差分生成

保留每次发布的原始 CRX3。差分基准是客户端缓存的完整 CRX 字节及其 SHA256；
重新生成的同版本包不能替代已发布基准。同一条差分的旧、新包必须来自相同 macOS 架构。
旧 `patch.zip` 不适用于这个流程。

macOS 示例：

```sh
python3 xenon_overlay/tools/updater/make_updater_delta.py \
  --old-crx /ABS/ARTIFACTS/1.0.0.3/update-1.0.0.3.crx3 \
  --new-crx /ABS/ARTIFACTS/1.0.0.4/update-1.0.0.4.crx3 \
  --from-version 1.0.0.3 --to-version 1.0.0.4 \
  --puffin out/updater_tools_arm64/puffin \
  --out /ABS/ARTIFACTS/1.0.0.4/update-1.0.0.3-to-1.0.0.4.puff \
  --metadata /ABS/ARTIFACTS/1.0.0.4/delta-1.0.0.3-to-1.0.0.4.json
```

默认 `--compression auto` 启用已验证的嵌套 PKG gzip 优化。脚本执行原生还原并与
目标 CRX 逐字节比较，通过后才输出补丁。`recommended: true` 表示补丁小于完整包；
否则保持完整更新。`--compression opaque` 仅用于对比旧算法。

打包生成的 `release.json` 初始只含完整包。发布差分时，从差分 JSON 中取
`from_version`、`previous_sha256`、`type`、`file`、`size`、`sha256`，作为一个对象
加入目标 `release.json` 的 `differential` 数组；先确认 `full_sha256` 与清单中的
`full.sha256` 相同。不要把整个差分 JSON 复制进去，服务端拒绝未知字段。
完整 CRX 必须保留，供缓存未命中或差分失败时回退。

在 Windows 宿主上为 macOS 制包时，`puffin.exe` 使用与 Chromium Zucchini 命令行工具相同的系统内存分配器。
生成差分需要为中间数据建立较大的后缀数组；默认 PartitionAlloc 的约 2 GiB 单次分配上限
可能使数百 MB 的完整包在生成阶段失败。此调整只影响离线制包工具，客户端解码器和补丁格式不变。
这不是 Omaha 的 500 MB 包大小限制。仍须为制包机器预留足够内存；生成失败或补丁不小于全量时不发布差分。
完整格式见 [omaha_update_server.md](omaha_update_server.md#release-manifest)。

## 离线验证脚本

macOS 制包集成验证：

```sh
XENON_PACKAGE_TOOLS_DIR="$PWD/out/release_arm64" \
XENON_PUFFIN_TOOL="$PWD/out/updater_tools_arm64/puffin" \
  python3 xenon_overlay/tools/updater/package_macos_test.py
```

使用临时签名应用和临时密钥，实际生成并解包 PKG、生成并验签 CRX、加载 Omaha 清单、
生成差分并还原，同时验证失败不发布、输出不覆盖和输入不变。不调用系统安装器。

跨平台 macOS PKG 导入验证：

```sh
python3 xenon_overlay/tools/updater/prepare_installer_test.py
```

验证原 PKG 字节保留、`.install` 的 0755 权限、元数据拒绝条件及原生 CRX 签名/验签。
这些离线测试不会安装更新或改动已安装浏览器；真实客户端升级需另外验收。
Windows JSON 差分回归使用 `make_directory_diff.test.js` 和 `xenon_updater_unittests`，详见 [README.md](README.md)。
