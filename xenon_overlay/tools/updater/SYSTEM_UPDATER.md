# macOS 系统级 Xenon Updater

本模式复用 `chrome/updater` 的系统服务、IPC、下载缓存、更新流水线及差分失败回退。
浏览器通过原生 `UpdateService` API 请求更新，不启动提权 shell，也不同时调用旧的
SMAppService 差分安装流程。首次安装使用管理员授权的系统 PKG，浏览器固定安装到
`/Applications/xl153.app`，更新后保留 root 所有权。

## 构建配置

在现有 macOS GN 参数中增加：

```gn
enable_xenon_updater = true
xenon_updater_update_url = "https://YOUR_UPDATE_HOST/service/update2/json"
xenon_updater_mac_team_id = "YOURTEAMID"
xenon_updater_crx_public_key_hash = "BASE64_OF_SHA256_OF_RELEASE_SPKI_DER"
```

上面的域名、Team ID 和摘要均须替换，不能直接发布。开发环境可显式使用
`xenon_updater_allow_insecure_localhost = true` 和
`http://127.0.0.1:9001/service/update2/json`。默认禁止 HTTP；公钥摘要始终必填。
私钥由发布流程保管，不嵌入 app、PKG、CRX 或服务端清单，不提交到代码仓库。

```sh
autoninja -C out/release_arm64 chrome chrome/updater/mac:updater_bundle \
  chrome/updater/mac/signing:signing xenon_overlay/tools/updater:release_tools
```

Xenon 的网络信任是 HTTPS 加固定发布公钥的 CRX3 签名校验，不使用 Google CUP
公钥或 Google publisher proof。浏览器不能覆盖服务端地址；生产信任配置也不能通过
Updater 的开发 override 改掉。Google 的事件上传、云策略和 Enterprise Companion
自动安装/卸载在此品牌下关闭。未启用该参数的现有平台行为保持不变。

## 安装、身份与更新语义

- Updater bundle/app ID：`com.xenon.XenonUpdater`。
- 浏览器 bundle/app ID：`org.chromium.Chromium`，保持现有应用身份。
- 服务安装树：`/Library/Application Support/Xenon/XenonUpdater`，活动版本由
  `Current` 指向。Updater 自身版本独立于浏览器的 `1.0.0.x` 产品版本。
- 首次 PKG 的 root postinstall 调用 Chromium 的 `--install --system`，再由
  `ksadmin` 注册固定浏览器路径和 `CFBundleShortVersionString` 版本读取位置。
- 普通浏览器只调用系统作用域的查询/更新 RPC，不使用 `RegisterApp`、`Install`
  或 `RunInstaller` 等特权接口，不放宽系统 IPC 的调用者校验。
- `autoDownload=false` 使用现有查询 RPC，用户下载时才请求完整更新事务。
  服务自己的周期更新由系统服务管理；浏览器设置不是全系统的停更策略。
- 下载完成不代表安装成功。服务进入安装阶段时，页面提示“退出并安装”；安装器等待
  浏览器退出，不杀进程。安装失败/等待超时是错误。此版完成后由用户手动重新打开，
  没有承诺自动重启，也没有把 Gatekeeper 放行等同于更新成功。
- `/Users/.../Applications` 下的副本不会作为系统更新目标，须从系统目录启动。

## 发布制包

先按 Chromium 的 macOS 签名流程签名、公证浏览器及 `XenonUpdater.app`。
正式制包要求匹配的 Developer ID Application Team、Developer ID Installer
签名以及公证票据。签名与 CRX 发布密钥是两种不同的身份。

`make_system_updater_pkg.py --include-app` 将浏览器和服务放入同一个 root 组件，
避免组件安装顺序问题；禁止自动重定位和按 Chromium 内核版本跳过安装。
制包时检查浏览器、Updater 及 bundle 内 Mach-O 辅助程序/库的架构交集，
没有共同支持的架构则拒绝制包。Distribution 只声明共同支持的架构，
组件 preinstall 在复制载荷前检查本机原生架构；即使在 Rosetta 下运行，
也按 Apple Silicon 的原生架构判断，不依赖转译环境安装不匹配的系统服务。

```sh
python3 xenon_overlay/tools/updater/make_system_updater_pkg.py \
  --updater-app /ABS/SIGNED/XenonUpdater.app --app /ABS/SIGNED/xl153.app \
  --include-app --app-id org.chromium.Chromium --updater-id com.xenon.XenonUpdater \
  --team-id YOURTEAMID --installer-identity 'Developer ID Installer: YOUR IDENTITY' \
  --notary-profile YOUR_PROFILE --out /ABS/RELEASE/xl153-with-updater.pkg
```

`--development` 仅生成本地验包用的未正式签名制品；系统服务 PKG 有前置阻断，不能拿
它证明 root 服务已安装。证书未配置时不要把开发制品当作可发布安装包。

浏览器更新载荷由原有 `make_mac_pkg.js` 生成应用 PKG，再用
`make_updater_crx.py --help` 所列参数封装为带 `.install` 的签名 CRX3。
完整回退载荷内部仍是 PKG，由系统服务调用 Apple Installer 安装。
安装器等待浏览器退出后、调用 Apple Installer 前重新读取产品版本，并按四段
uint32 数字比较，拒绝降级和非法版本。同版本允许重装，以便重试或修复未完成的安装。
`make_updater_delta.py` 使用上游 Puffin 对旧、新 CRX 做差分，生成后必须实际还原并
逐字节对比；若差分不小于完整包，则不推荐发布该差分，服务端也不会提供该差分流水线。

Omaha 4.0 的差分基准是客户端缓存中的旧 CRX 的 SHA256，而不是已安装 app 目录。
首次 PKG 安装没有这个缓存时，第一次更新走完整 CRX/PKG；后续缓存匹配才下发差分。
旧版自定义 `patch.zip` 不兼容此协议。CRX 发布 key 变更需要规划 Updater 信任迁移，
不能只更换服务端签名私钥。

Updater 自更新可使用上游生成并签名的 Updater installer ZIP，使用同一发布 key
生成 CRX，再作为独立 app 的 release 加入同一 Omaha 清单。不要把浏览器安装器
脚本用于 Updater 自更新。

## 服务端与验证

完整清单和运行说明见 [omaha_update_server.md](omaha_update_server.md)。端点是
`POST /service/update2/json`，支持同端点发布浏览器和 Updater 两个 app。
开发服务默认仅监听 `127.0.0.1:9001`；正式部署须 HTTPS 反代、管理发布制品及清单。
现有 8999/9000 测试服务与此接口独立。

`xenon_updater_verify` 直接调用 Chromium 自身的 Omaha parser 和 CRX verifier，
只验文件，不运行安装器：

```sh
out/release_arm64/xenon_updater_verify --response=/ABS/response.json
out/release_arm64/xenon_updater_verify --crx=/ABS/update.crx3 \
  --key-sha256=HEX_SPKI_SHA256 --file-sha256=HEX_CRX_SHA256
```

## 离线回归

在仓库根目录运行以下命令。测试只使用临时制品，不安装 PKG、不注册系统服务。
原生测试工具的环境变量不能省略，否则对应的集成测试会跳过。

```sh
autoninja -C out/release_arm64 xenon_updater_unittests \
  xenon_overlay/tools/updater:release_tools
out/release_arm64/xenon_updater_unittests --test-launcher-jobs=1
python3 chrome/updater/branding_xenon_test.py
XENON_PUFFIN_TOOL="$PWD/out/release_arm64/puffin" \
XENON_CRX_TOOL="$PWD/out/release_arm64/crx3_build_action" \
XENON_VERIFY_TOOL="$PWD/out/release_arm64/xenon_updater_verify" \
  python3 -m unittest discover -s xenon_overlay/tools/updater -p '*_test.py'
XENON_VERIFY_TOOL="$PWD/out/release_arm64/xenon_updater_verify" \
  node --test xenon_overlay/tools/updater/*.test.js \
  xenon_overlay/resources/webui/xenon_update/index_unittest.cjs \
  xenon_overlay/resources/ipc/xenon_auto_updater_unittest.cjs
```

真实签名 CRX 的生成、正确/错误发布公钥校验、Puffin 差分逐字节还原，以及完整/
差分响应的 Chromium 原生 Omaha 解析均在这些测试中覆盖。macOS 进程等待测试需要
允许 `pgrep` 读取本机进程列表；HTTP 测试需要允许监听本机回环端口。

差分工作进程另有真实 Mojo 子进程回归，验证补丁还原、损坏补丁错误返回和正常退出。
文件操作在带 `MayBlock` 的任务序列执行，退出回调回到主序列，避免主线程的阻塞检查
使工作进程崩溃并导致有效差分也回退完整包。

```sh
autoninja -C out/release_arm64 updater_tests
ISOLATED_OUTDIR="$(mktemp -d)" out/release_arm64/updater_tests \
  --gtest_filter='AppPatchWorkerTest.*' --test-launcher-jobs=1
```

这里的过滤器限定为仅操作临时文件和子进程的两项测试。不要去掉过滤器直接运行整套
品牌 Updater 集成测试；其中的安装/卸载用例会修改本机 Updater。

代码/协议/离线制包测试不等于系统升级验收。正式证书配置后，仍须验证首次管理员
安装、系统服务注册、完整更新、缓存命中差分、损坏差分自动回退、安装锁、root 所有权、
退出再打开、Gatekeeper、网页正常加载及 Updater 自更新。

## 开发签名下的本机验证

开发签名可以验证本机更新逻辑。以下测试自动创建临时的 ad-hoc 签名应用和真实 PKG，
执行生成的完整 `.install` 脚本，使用原生 plist 读取、PKG 解包、文件哈希和 codesign
检查。测试仅在临时脚本中替换 root 身份、所有权查询、进程查询及 Apple Installer
调用；不会调用系统 `installer`，不会安装或注册系统服务，也不会修改 `/Applications`。

```sh
python3 xenon_overlay/tools/updater/system_updater_install_test.py
python3 xenon_overlay/tools/updater/mac_native_update_test.py
```

第一组覆盖作用域和注册路径错误、包损坏、符号链接、等待超时、等待期间目标或版本
变化、安装返回错误、部分写入后的新进程重试，以及安装返回成功但签名、应用身份或
版本不符。部分写入用例保留有效的 Info.plist，破坏资源签名，再要求同版本完整包修复；
它不能证明元数据丢失、真实 Installer 中断或断电后的恢复。第二组测试现有旧后端的
原生辅助进程和应用替换，不代表系统 Updater 的 PKG 安装流程已经通过。

开发签名仍会被正式发布身份检查拒绝。`--development` 系统服务 PKG 的安装阻断保持
有效，不能用它模拟正式 Developer ID、Gatekeeper、公证和 root 服务安装验收。

真实浏览器的 ad-hoc 冒烟测试应复制构建产物并使用全新的临时 `--user-data-dir`。
若本机钥匙串授权导致自动化等待，可在该临时实例使用 Chromium 自带的
`--use-mock-keychain`；这不验证真实钥匙串访问，也不适用于日常用户配置。
系统更新模式从临时目录启动时应报告固定安装路径错误，并允许重试；该检查通过
仅证明浏览器与原生更新接口的错误处理正常，不表示系统服务安装或升级成功。

## 正式签名制品验收入口

`system_updater_release_test.py` 检查正式签名旧、新应用的身份、递增版本和公证票据，
检查 PKG 签名及其载荷是否与新版应用一致，再用 Chromium 原生 CRX verifier 校验发布
公钥，并验证 CRX 内的 PKG 和 `.install` 与预期一致。所有操作只读取输入并使用临时
解包目录，不执行输入中的安装脚本。

```sh
XENON_REQUIRE_RELEASE_FIXTURES=1 \
XENON_RELEASE_OLD_APP='/ABS/OLD/xl153.app' \
XENON_RELEASE_NEW_APP='/ABS/NEW/xl153.app' \
XENON_RELEASE_PKG='/ABS/RELEASE/application.pkg' \
XENON_RELEASE_CRX='/ABS/RELEASE/update.crx3' \
XENON_RELEASE_TEAM_ID='YOURTEAMID' \
XENON_RELEASE_KEY_SHA256='RELEASE_PUBLIC_KEY_SHA256_HEX_OR_BASE64' \
XENON_VERIFY_TOOL="$PWD/out/release_arm64/xenon_updater_verify" \
  python3 xenon_overlay/tools/updater/system_updater_release_test.py
```

如果制包指定了非默认的 `--wait-seconds`，同时设置相同的
`XENON_RELEASE_WAIT_SECONDS`。日常离线回归未配置任何发布输入时明确显示 `SKIPPED`；
只配置部分输入、配置错误，或者设置 `XENON_REQUIRE_RELEASE_FIXTURES=1` 后缺少输入，
均会失败。发布 CI 必须启用该要求，并确认三项 `SignedReleaseTest` 实际通过。

制品检查之后，仍需在独立 macOS 测试机或虚拟机执行真实系统验收：首次管理员安装、
普通用户请求更新、服务注册与 root 所有权、等待所有浏览器退出、安装后正常启动；
再验证下载损坏与差分回退、安装失败和服务重启后的重试，以及安装中断和磁盘不足的
恢复结果。不能以跳过的测试、开发签名通过或制品检查通过替代这一步。
