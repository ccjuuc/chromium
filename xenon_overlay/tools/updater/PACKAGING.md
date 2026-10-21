# macOS 打包与差分脚本

当前 Chromium 系统 Updater 使用下面两个入口，在仓库根目录执行：

| 入口 | 输入 | 输出 |
| --- | --- | --- |
| `package_macos.py` | 已签名的 `xl153.app`、CRX 私钥，可选 `XenonUpdater.app` | 应用 PKG、签名 CRX3、Omaha 清单，可选首次安装 PKG |
| `make_updater_delta.py` | 已归档的旧、新完整 CRX3 | 优化 Puffin 补丁和校验元数据 |

要求 macOS、Python 3.10+、Node.js 和已构建的原生工具。脚本只生成、验证文件，
不安装 PKG、不启动服务、不修改输入应用、不修改 GN 配置。

```sh
autoninja -C out/release_arm64 xenon_overlay/tools/updater:release_tools
autoninja -C out/updater_tools_arm64 puffin
```

大文件差分推荐独立系统 malloc 版本的 Puffin。首次配置该构建目录见
[SYSTEM_UPDATER.md](SYSTEM_UPDATER.md#pkg-压缩载荷的差分优化)。

## 1. 打包

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

## 2. 生成差分

保留每次发布的原始 CRX3。差分基准是客户端缓存的完整 CRX 字节及其 SHA256；
重新生成的同版本包不能替代已发布基准。不要对两个 `.app` 使用旧 `patch.zip` 协议。

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
完整格式见 [omaha_update_server.md](omaha_update_server.md#release-manifest)。

## 离线验证脚本

```sh
XENON_PACKAGE_TOOLS_DIR="$PWD/out/release_arm64" \
XENON_PUFFIN_TOOL="$PWD/out/updater_tools_arm64/puffin" \
  python3 xenon_overlay/tools/updater/package_macos_test.py
```

使用临时签名应用和临时密钥，实际生成并解包 PKG、生成并验签 CRX、加载 Omaha 清单、
生成差分并还原，同时验证失败不发布、输出不覆盖和输入不变。不调用系统安装器。
