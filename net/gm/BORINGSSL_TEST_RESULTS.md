# 当前 BoringSSL 单库国密验证记录

记录日期：2026-10-29。Chromium **153.0.8004.0**，Windows x64 Release。
本记录保留初次单库迁移、浏览器联合协商接入及随后增量验证；
[TEST_RESULTS.md](TEST_RESULTS.md) 保留的双库结果不计入本次通过项。
最新批次是 `XENON_GM` 开关两态的编译和软件回归，见首节；当前
`out/Release_64` 保持国密开启。后续章节分别记录早前修复、初次迁移、包审计和
性能测量的范围与产物，不能把历史哈希或测量当作本轮结果。
独立 GmSSL 的两个历史双向认证失败仍未消除；真实 Key、跨平台和实际安装
验收尚未完成，因此不能把当前功能验收写成全部通过。

## XENON_GM 编译开关验证（2026-10-29）

本轮为国密功能增加统一代码宏 `XENON_GM`。唯一 GN 配置仍是
`enable_gm_tls`：Chromium 使用 `net/gm/gm_buildflags.h` 中的
`BUILDFLAG(XENON_GM)`，BoringSSL 从同一配置取得私有编译器宏。
不向 Chromium 传播原始 `-DXENON_GM`，避免和生成 buildflag 冲突。
具体用法见 [BUILDING.md](BUILDING.md)。

关闭时排除 SM2/SM3/SM4 算法实现与注册、国密套件/组/签名算法、TLCP 版本和
SM2 PKI 支持；专用 API 保留失败 stub。浏览器不注册 GM 策略，不加载国密系统信任，
不创建 KeyManager、身份 provider 或 USBKey utility 服务，也不进行国密探测。
显式传入 GM socket 规则会在发送 ClientHello 前返回
`ERR_SSL_VERSION_OR_CIPHER_MISMATCH`。公共数据结构、枚举和 Mojo 接口保留，
避免配置开关造成接口布局分叉；类型存在不表示功能已开启。

证书页通过编译配置生成 `gmEnabled`。关闭时移除国密预设和 Key/PIN 提示，
普通证书预设、“测试连接”和“在浏览器中打开”仍可使用。
页面采用 `strings.m.js` 初始化模块配置。

两种配置均在现有 `out/Release_64` 生成并完成浏览器和测试程序编译。
最终已恢复原始 `args.gn`，其中 **`enable_gm_tls = true`**。
本轮证据根目录为 `out/gm_boringssl_tools/xenon-gm-macro-20261029/`；
`args-original.gn`、`args-disabled.gn`、`args-enabled.gn` 记录配置。

| 范围 | 宏关闭 | 宏开启 |
| --- | ---: | ---: |
| BoringSSL crypto | 1820/1820 PASS | 1880/1880 PASS |
| BoringSSL SSL | 478/478 PASS | 485/485 PASS |
| BoringSSL PKI | 1329/1329 PASS | 1330/1330 PASS |
| Chromium 网络层 | 2422/2422 PASS | 2669/2669 PASS |
| Network Service | 14/14 PASS | 66/66 PASS |
| 浏览器单元 | 29/29 PASS | 113/113 PASS |
| Content 国密请求生命周期 | 1/1 PASS（明确拒绝、不提示） | 8/8 PASS |
| 浏览器导航与 PIN 界面 | 功能关闭，不构建该专用目标 | 35/35 PASS |
| 真实生产证书页 | 8/8 PASS | 16/16 PASS |

C++ launcher 的最终结果共 **关闭态 6093 项、开启态 6586 项通过**，
均禁用自动重试并核验实际执行状态。关闭态证据在 `disabled/`；
SSL 和网络层以修正测试后的 `disabled-corrected/` 全量结果为准。
开启态证据在 `enabled/`，Content 明确限定为
`StoragePartitionImplGmCertificateTest.*`，其余表中 suite 全量运行。
生产页面证据分别是 `../release64-xenon-gm-disabled-2/report.json`、
`../release64-xenon-gm-enabled/report.json`。

关闭态验证覆盖：国密算法/API 拒绝、SM2 证书拒绝、普通 TLS 1.2/1.3 握手和恢复、
规则/能力记录/请求元数据不能启用国密、Mojo GM 配置不下发、页面请求不触发授权。
普通 TLS 原有的兼容性重试继续保留，不把它记为新增国密探测。
开启态公开 `test.gmssl.cn`、`test1.gmssl.cn` 均返回 HTTP 200，协商
TLCP 1.1 / SM4_GCM / 0xE053；普通站点继续协商 TLS 1.3 / AES_128_GCM。
这些公开测试仍使用本机既有演示站点规则/测试 CA，不是默认企业部署模板。

开启态另通过：连接配置 **130 checks**、SKF adapter **100 checks**、
软件 Key service **25/25**、独立铜锁对端 **54/54**。
见 `auxiliary-tests.json`、`auxiliary-tests.log`、`gm-key-service-tests.xml`。
该辅助 runner 只选择上述四层，其未选择的 Content/导航标志不能覆盖前表的独立结果。

国密相关 GN target 在两态的依赖检查通过；关闭态 policy、network service、Content
目标检查也通过。`disabled-dependency-checks.json` 确认生产 Chrome 依赖不含
GM policy、policy handler、KeyManager、system trust 和 Key service。
扩大到整个 Chrome core/utility 的检查仍报告 Beijing/Xenon 其他模块的引用依赖问题，
未将这些检查记为通过；对应日志 `gn-check-disabled-chrome-core.log`、
`gn-check-disabled-utility.log`，实际两态编译和链接均通过。

保留本轮中间失败证据：首次关闭编译发现 SM2 组注册数量断言需适配；首次页面检查
发现 legacy `strings.js` 不适用于模块页面，已改为 `strings.m.js`。
关闭态新增测试曾错误要求 TLS 1.2 恢复必须续发票据，且只为普通 TLS 重试提供一次
模拟 TCP 连接；修正测试契约与夹具后，SSL/网络 suite 均全量复跑通过，未修改生产
会话恢复或普通 TLS 重试逻辑。开启 GN 生成时的测试 source 列表覆盖也已改为互斥分支。

BoringSSL 补丁包已更新并完成源码一致性/回放检查，涉及既有清单中的 81 个文件，
未新增源列表条目。补丁 SHA-256：
`832e0947df49baeb8c6195db20d73647812448069ff9915a5b1923ba490de0b6`。
证据：`patch-export.log`、`patch-check.log`；真实 Git HEAD/index 未改动。

最终安装包构建 **4/4 完成**，审计 **52/52 PASS**：
`build-package.log`、`release-audit/audit-report.json`。
汇总 `validation-summary.json` 将两态测试、页面结果、最终配置和包审计关联，
确认实测 DLL/页面资源的哈希与归档中对应文件一致；安装器未执行。
打包前校验既有五个应用运行文件备份并同步规范载荷，审计后全部原样恢复，
见 `packaging-runtime-backup.json`、`runtime-restore.json`。

| 最终产物（位于 `out/Release_64/`） | 字节数 | SHA-256 |
| --- | ---: | --- |
| `xlb153.exe` | 4,149,248 | `55b2f07a87477495191696bcd946210a402e9d4f8a25505e565fa59c64bf9fdf` |
| `xlbrowser.dll` | 344,481,280 | `1ff061feb721a89a1bc1b1e451b30daecc3ca8b97bc38db511987feefa51b8be` |
| `resources.pak` | 22,989,359 | `918091bad20dd2f1d9df3a88dded9939c9d3205700368eb68a1b1c088956b8f0` |
| `xlb153_installer_1.0.0.1.exe` | 449,454,080 | `9ae756f33d19fd789a1888831755d883359f7ae3949a5adae310282bf5b9cbb4` |

本轮不增加性能结论；真实飞天 Key/SDK/PIN、macOS/Linux 原生编译运行和实际安装
仍未验收。独立 GmSSL 的两个历史双向互通失败未在本轮修复或重跑，继续保留后文记录。

## 代码审查修复与增量验证（2026-10-29）

本轮修复四项审查问题，继续使用 `out/Release_64`，没有修改 BoringSSL 算法源码，
也没有恢复生产铜锁依赖。以下结果是本轮实际运行；后续章节保留的历史全量测试、
性能数据和产物哈希不能代替本轮证据。

1. Key 身份失效时，在 `Disconnect()` 后重新取得弱引用，分别检查连接池通知、
   connect/read 回调是否已销毁或再次断开 socket，避免继续访问失效对象或调用 write。
   四项真实 TCP 回归覆盖待处理读写完成、连接池删除、read 回调删除及再次断开。
2. 严格国密连接遵守 TLS 版本范围、套件禁用和 `require_ecdhe` 限制，
   在握手前计算可用套件，并拒绝恢复使用已禁用套件的缓存会话。
   TLCP 作为独立显式协议处理；当前静态 ECC 套件不能满足 ECDHE 要求。
   十项参数化用例覆盖版本上下限、连接覆盖项、套件子集、全禁用、恢复和 ECDHE。
3. GM 策略通过专用 handler 校验并报告字段路径；存在但无效的配置保留为拒绝状态，
   不再被当作未配置。Windows 注册表转换仅为 GM 开启无效值保留，
   使用临时注册表 hive 覆盖原始值→平台 loader→handler→拒绝配置的链路；
   普通策略继续使用原转换与敏感策略过滤。macOS 同样加入 GM 专用无损转换检查，
   对不可表示的类型、超范围整数和嵌套错误保留拒绝状态；五项 Mac 测试尚未原生运行。
   Linux JSON 读取路径已保留无效值，本轮没有修改该路径。
4. 证书页对所有 HTTPS 地址提供“测试连接”和“在浏览器中打开”两项操作，
   共用 URL 校验并保留路径、查询参数。预设只提供地址和说明；
   `test2.gmssl.cn` 继续作为已知双向认证示例，不再靠精确网址决定能否正常导航。
   匿名 HEAD 与正常标签页认证流程保持各自语义，导航新标签页不保留 opener。

本轮构建和测试：

| 范围 | 本轮结果 | 证据（相对 `out/gm_boringssl_tools/`） |
| --- | --- | --- |
| 网络层全量 | **2669/2669 PASS** | `review-fixes-net.json`、`.log` |
| Network Service | **66/66 PASS** | `review-fixes-network-service.json`、`.log` |
| 浏览器单元及策略 | **113/113 PASS** | `review-fixes-browser-final-2.json`、`.log` |
| 生产浏览器证书页 | **16/16 PASS** | `release64-review-fixes-ui/report.json` |
| GN handler 依赖检查 | **PASS** | `review-fixes-gn-check.log` |
| 安装包、归档与直接导入审计 | **52/52 PASS** | `release-audit-review-fixes/audit-report.json` |

三个 C++ suite 的 launcher summary 均核对实际执行状态，关闭自动重试；
最终没有失败。浏览器单元测试首轮 112/113 通过：测试错误地把会被敏感策略过滤的
`HomepageLocation` 当作普通策略；修正为公开接口读取基线并比较完整 PolicyMap，
确认增加无效 GM 配置不会改变其他策略的值或过滤状态。
首次失败保留在 `review-fixes-browser-final.json`、`.log`，没有修改生产过滤规则。

生产页实际访问 `test.gmssl.cn`、`test1.gmssl.cn`，均返回 HTTP 200，
协商 TLCP 1.1 / SM4_GCM / 0xE053；测试使用本机既有公开演示站点策略及测试 CA。
`example.com`、`sm2test.ovssl.cn` 返回普通 TLS 1.3 / AES_128_GCM，
TrustAsia 预设仍报告 `ERR_CERT_AUTHORITY_INVALID`，未绕过证书错误。
`test2` 仅验证双操作保留和正常标签导航，不能解释为双向握手或物理 PIN 验收成功。

浏览器及安装包已完成链接。生产链接记录见 `build-review-fixes-release.log`，
最终打包记录见 `build-review-fixes-final.log`；测试最终重编记录见
`build-review-fixes-browser-verified-2.log`。输出目录有多个历史安装器，首次审计
要求显式选择后退出；随后指定本轮 `xlb153_installer_1.0.0.1.exe` 完成审计，
日志分别为 `review-fixes-release-audit.log`、`review-fixes-release-audit-2.log`。

| 本轮产物（位于 `out/Release_64/`） | 字节数 | SHA-256 |
| --- | ---: | --- |
| `xlb153.exe` | 4,149,248 | `32bd7f1edae126868277b8e0878f74b8dc77e0192d2d2129488578f09479af84` |
| `xlbrowser.dll` | 344,480,768 | `f4e6fad4e9d1ba16ebf6fbddb8582c0223d9325643509865ac1134852332bf3d` |
| `resources.pak` | 22,989,244 | `86c008446c097f0de45bb6724ccd7184d55af6e3a0687b55456638a56817576d` |
| `xlb153_installer_1.0.0.1.exe` | 449,427,456 | `e8383812b6f07ec41a3b40f6d4ee9cd350387a01b0e714289b7eb5d3823fea5f` |

`release-audit-review-fixes/runtime-evidence-binding.json` 核对 UI 实测 DLL、资源包
与安装包审计对应产物的哈希；EXE/安装器哈希由包审计记录。安装器未执行。
归档排除国密测试后端、模拟器及测试程序；浏览器直接导入表没有外部国密库。
附带应用的六个既有 OpenSSL DLL 继续按准确路径和源码哈希清单核对。

打包前保存了附带应用运行生成的五个额外文件，再同步规范打包目录；
审计完成后已逐项校验哈希并恢复原位置，未覆盖新文件。备份和恢复记录位于
`runtime-payload-backup-review-fixes/manifest.json`、`restore-report.json`。

本轮没有新增性能 A/B，不能作出普通站点零开销承诺；没有引入普通站点无条件国密探测。
没有物理飞天 Key/可信厂商 SDK，实际 PIN、设备拔插与 USB 时延尚未验证。
没有 macOS/Linux 原生构建运行结果，Mac 新增用例仅完成源代码审查。
独立 GmSSL 双向互通的两个历史失败仍未解决，见后文；本轮未改协议实现，未重跑该对端。

## 公开站点与证书诊断页增量验证（2026-10-28）

本轮修复 SM2-with-SM3 AlgorithmIdentifier 的兼容性：仅 SM2 接受省略参数或
精确 DER NULL，其他参数继续拒绝，ECDSA 等算法的规则不变。三个公开站点的
签名/加密证书都使用 NULL 编码，旧实现将“不支持的编码”映射成
`ERR_CERT_WEAK_SIGNATURE_ALGORITHM`；不能据此判断 SM2 密码强度不足。

- PKI 全量 **1330/1330 PASS**，含参数异常拒绝及独立签名验证测试。
  证据：`out/gm_boringssl_tools/pki-sm2-null-results.log` 和 `.json`。
- 侧栏真实浏览器测试 **4/4 PASS**，覆盖两种滚动模式、650→400→650 窗口高度、
  首尾按钮完整可见、底部控制固定，以及 CRT 打开证书页。
  证据：`out/gm_boringssl_tools/sidebar-regression-final.json`。
- 生产 EXE 证书页检查 **15/15 PASS**，证据：
  `out/gm_boringssl_tools/release64-gm-sites-final/report.json`。
  `test.gmssl.cn` 和 `test1.gmssl.cn` 均通过校验并返回 HTTP 200，
  协商 **TLCP 1.1 / SM4_GCM / 0xE053**。普通 `example.com`、沃通双模式站点
  返回 TLS 1.3 / AES_128_GCM。既有 TrustAsia 预设在独立配置下仍报告
  `ERR_CERT_AUTHORITY_INVALID`，未绕过该错误。
- `test2.gmssl.cn` 正常标签导航返回
  `ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED`。本机没有配置 SKF 驱动或真实 Key，
  对应 `GmKeyManager::StartNext` 拒绝取得客户端私钥的路径。
  `out/gm_boringssl_tools/release64-test2-no-key-confirmed/report.json`
  记录的是**预期拒绝检查**，不是双向认证成功或 PIN 实机验收。

证书页加入自适应、TLCP 单向、TLCP 双向/USBKey 和沃通入口，并显示真实协商信息。
双向入口打开正常标签页以保留页面绑定的身份/PIN流程。HEAD 检测使用
`kOmitBug_775438_Workaround`，不发送 Cookie、客户端证书或登录凭据。
侧栏缺失源于非 compositor 滚动模式下内容仍为 0×0，现显式设置固定按钮列表尺寸。

本机测试策略保留原 test1 规则，并为 test/test2 的 443 端口补齐精确 TLCP 规则。
三个站点使用同一组既有测试 CA，没有新增全局根证书；继续使用原公开演示规则的
吊销例外，不能将其作为企业生产配置模板。原始策略、应用记录、恢复脚本和独立
证书链 8/8 验签证据位于 `out/gm_boringssl_tools/public-gm-site-certificates/`。
`test.gmssl.cn` 接受普通 TLS 和 TLCP；本次 RFC 8998-only 探测失败，混合
TLS 1.3 ClientHello 选择 AES，因此 `tls13_auto` 无法让这个站点改用 TLCP。

本轮重建日志：`out/gm_boringssl_tools/build-release64-gm-sites-ui.log`。
下文完整迁移测试、性能和首次包审计是保留的历史快照，未在本轮全部重跑。

本轮安装包构建 **5/5 完成**，日志为
`out/gm_boringssl_tools/build-release64-gm-sites-package-final.log`；包审计
**52/52 PASS**，见 `out/gm_boringssl_tools/release-audit-gm-sites-final/audit-report.json`。
新增核对浏览器 DLL 同目录的 `resources.pak`，并排除侧栏测试程序及模拟器等测试载荷。
浏览器 EXE、DLL、页面资源的哈希均与本轮运行测试证据一致，见同目录
`runtime-evidence-binding.json`。首次资源审计把附带 Electron 应用的同名资源包也计入，
因路径歧义失败；现依据已验证浏览器 DLL 的目录精确定位，保留首次失败报告。
以下为本轮最终产物，均位于 `out/Release_64/`；安装器未执行。

| 产物 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `xlb153.exe` | 4,149,248 | `a71fbc7ad762765038d063c01f8507493ac21e02e02e98a87856118f93ed1177` |
| `xlbrowser.dll` | 344,473,600 | `cd16be63db4513edaf584c2cd0e26d658a6ab7daff1368a6cb828aa67df9c0f3` |
| `resources.pak` | 22,989,297 | `4c302efc569a20680811fa18222103c9c0481b0a5b3df6ff1cba21ba8ce28e23` |
| `xlb153_installer_1.0.0.1.exe` | 449,425,408 | `8261ebf4d21203d8a7a3eaefa631d3d1c1e2a429d2ab27af92e251c8546a9b04` |

## 基线与构建范围

- BoringSSL 基线：`52ba6a143e03e9ac84369f79ac3d06dea0128dab`，叠加本地
  SM2/SM3/SM4、RFC 8998、TLCP 客户端及 libpki 扩展。测试不是未经修改的上游结果。
- 最终构建输出：**`H:/chromium_142/src/out/Release_64`**，为 H: 上的普通目录，
  已迁入可复用产物、保留原参数重新生成 GN，并完成浏览器及测试程序链接和本轮测试。
  前期 `out/gm_release` 曾指向
  `C:/Users/Administrator/AppData/Local/CodexBuilds/chromium153-gm/Release_gm`
  的验证输出。该 junction 已移除；后续入口仅为
  `out/Release_64`。旧日志保留原路径，不改写为本轮证据。
- 配置：`target_cpu="x64"`、`is_debug=false`、`is_official_build=true`、
  `is_component_build=false`、`chrome_pgo_phase=0`、`symbol_level=0`、
  `enable_gm_tls=true`。保留现有应用构建参数；未使用 PGO 配置文件。
  本轮参数副本为 `out/gm_boringssl_tools/release64-test-evidence/args.gn`。
- 生产实现使用 Chromium 自带 BoringSSL，由 GN/Ninja 编译。
  `gm_boringssl_test_backend.dll` 和 `gm_interop_peer/` 仅用于测试，不能打入安装包。
  输出目录残留的 `gm_backend/gm_backend.dll` 属于旧铜锁实现，不能视作当前生产产物。
- 独立铜锁对端：8.5.0，源码归档 SHA-256
  `35ec0ed648e799df8b5860d3716878812f117f6563607dcdf2709023b5e337f8`。
  最初固定对端及文件清单位于 `out/gm_interop_reference/`；最新验证使用加强了
  诊断证书、空信任和授权顺序断言的测试程序，保留同一独立铜锁库。
  新程序的构建来源、命令和库哈希见
  `C:/Users/Administrator/AppData/Local/CodexBuilds/chromium153-gm/facade-verification-peer/manifest.json`。
  程序 SHA-256 为 `8e88a6d326b6891b187c471ec420a8b7d169ebf49ba653b81ab4d9df4f6133b5`，
  对应 `net/gm/tests/backend_smoke.cc` 源码 SHA-256 为
  `30d067ecd3b9a6e64832432f764f4220d7ad13625c6614ba951b96353a01aa67`。
  该 manifest 仅记录程序构建，本轮实际通过证据为
  `out/gm_boringssl_tools/release64-all-tests.json` 的 `tls_backend` 项；
  较早的 `interop-smoke-6.log` 继续保留。

下文未注明绝对路径的日志均相对于 `H:/chromium_142/src/`。

## 已完成的软件验证

以下项目已在 `out/Release_64` 重跑；明确标注“前期”的 SM4 无 SSSE3 结果除外。
完整运行输出为 `out/gm_boringssl_tools/release64-all-tests.json` 和 `.log`。
下表简称文件名的 8 份 launcher summary、Key XML 及构建参数均已另存于
`out/gm_boringssl_tools/release64-test-evidence/`。

| 层 | 实际结果 | 原始证据 |
| --- | --- | --- |
| BoringSSL crypto 全量 | **1880/1880 PASS**，包含批量 SM4 优化 | `gm-boringssl-crypto.json` |
| BoringSSL SSL 全量 | **485/485 PASS**，包含 TLCP 扩展及 RFC 8998 CCS 调整 | `gm-boringssl-ssl.json` |
| BoringSSL PKI 全量 | **1328/1328 PASS** | `gm-boringssl-pki.json` |
| Chromium 网络层全量 | **2655/2655 PASS** | `gm-network-tests.json` |
| Network Service | **66/66 PASS**，包含真实进程间身份调用 | `gm-network-service-tests.json` |
| Key service | **25/25 PASS**，软件 SKF 设备进程 | `gm-key-service-tests.xml` |
| Content 请求/页面生命周期 | **8/8 PASS** | `gm-content-tests.json` |
| 浏览器单元、策略、证书链与原生 PIN | **85/85 PASS** | `gm-browser-unit-tests.json` |
| 浏览器导航及页面模态 PIN | **35/35 PASS** | `gm-browser-navigation-tests.json` |
| 连接配置 | **130 checks PASS** | 总报告的 `connection_profile` 项 |
| SKF adapter | **100 checks PASS** | 总报告的 `skf_adapter` 项 |
| 铜锁独立对端 | **54/54 PASS**，加强断言，含真实 TCP/H1、RFC 8998 H2 及软件 Key 双向认证 | 总报告的 `tls_backend` 项 |
| GmSSL 3.2.0 独立对端 | **2/4 PASS**；两个双向认证用例失败，见下节 | 总报告的 `independent_interop` 项及独立快照 |
| SM4 无 SSSE3 路径（前期） | **37/37 PASS**，禁用 SSSE3 后运行 SM4/AEAD 用例 | `out/gm_boringssl_tools/crypto-sm4-scalar-2.log`、同名 JSON |

总 runner **退出码为 1**，原因正是独立 GmSSL 的两个失败，不能忽略或写成全量通过。
其他所选层均成功；8 个 launcher 层关闭自动重试，并核验本轮选择范围内所有
enabled test 均实际执行且为 `SUCCESS`，不是仅凭进程退出码或旧摘要判断。
Content 范围明确限定为 `StoragePartitionImplGmCertificateTest.*`。
报告器本身的 `net/gm/tests/run_tests_report_test.py` 另有 **12 项纯 Python 测试通过**。

本轮 `native_pin_dialog_unit_tested`、`page_modal_pin_dialog_tested`、
`native_pin_dialog_tested` 均为 true，Content 与浏览器导航覆盖标志也为 true。
导航认证夹具仍使用注入的 PIN 回调；另行测试了真实原生/页面模态 PIN 界面。
这不能解释为已完成物理设备输入 PIN 的端到端验收。

早期 `joint-targeted-1` 的 6 项路由测试因模拟写入数组生命周期错误失败；修复后
已包含在 `net-full-2`、`net-full-3` 及本轮 Release_64 全量通过结果中。保留失败日志以便审计，
不将中间失败计为通过。
前期服务层构建的完整输出保留在 `out/gm_boringssl_tools/service-final.json`。
此前运行的普通 SSL socket 296 项也已包含在最新网络全量结果中。

网络验证包括 17 项真实 TCP 联合协商测试，覆盖一次连接/一次 ClientHello、同一配置
选中普通 TLS 或 RFC 8998、普通 mTLS、默认连接不声明国密、普通/国密信任隔离、
软件 SM2 Key、错误 PIN、取消、身份代次、会话恢复及 GM 票据被普通服务器拒绝后的处理。
同时包含 26 项国密证书验证用例和 15 项异步 SM2 私钥桥接用例。本轮新增 8 项
异步验证及 1 项身份失效生命周期回归，覆盖请求销毁、晚到回复、独立信任快照、
后台解析异常、等待验证时普通连接继续运行，以及断开连接取消待处理 PIN 授权。
这些测试使用软件身份，不能代替物理飞天设备验收。

当前接入行为：只有显式 `tls13_auto` 规则或已认证的 RFC 8998 能力记录进入联合模式。
普通未知站点保持 GM-disabled，不增加 SM2 key_share 或无条件国密探测。
联合模式的普通结果继续使用 Chromium CertVerifier、CT 和普通客户端身份路径；
国密根不进入普通 TLS 信任。实际国密结果才进入国密验证与异步 Key 桥接。
国密失效代数与普通票据隔离，Key 失效不清理普通 TLS/HTTPS 代理会话。

联合路径将国密根/CRL 的首次解析和验链移到后台线程；请求取消或销毁后丢弃晚到
结果，不恢复握手或弹出 PIN。已开始的后台计算不承诺被强制中止。
严格 `rfc8998`/`tlcp` facade 路径则对同一次握手去除重复验链，仍同步执行一次，
不能声称所有国密操作都不会占用网络 I/O 线程。

## 可重放补丁交付

嵌套的 `third_party/boringssl/src` 有独立 Git 索引，顶层仓库提交不会自动记录
其中改动。当前补丁包见
[gm_patch/README.md](../../third_party/boringssl/gm_patch/README.md)。
明确清单包含 81 个文件（62 个已有文件、19 个新增文件），导出时拒绝未知源码变化。
已在独立临时基线索引执行 `git apply --cached --check`、应用补丁及完整树比对，
随后 `check --check-worktree` 也通过，真实 HEAD、索引和工作源码未改写。

基线为本报告开头的固定 revision，重放树为
`26db3633ed9fdd3cc03f551e9f7a56d887b8a6eb`；补丁 SHA-256 为
`a3d7366453a355a9d134aa581c9121db6453a626d506da544aea1edaad8c9ad1`。
普通 GN 构建不运行导出工具，也不因此要求 Go；升级基线需审查移植并重新执行测试，
不能静默强套现有补丁。后续 BoringSSL 源码变动后应重新导出并校验。

## 独立 GmSSL 互通结果与限制

工作目录：
`C:/Users/Administrator/AppData/Local/CodexBuilds/chromium153-gm/gmssl-interop`。
固定 GmSSL 3.2.0 commit 为 `7c9f02904ef33e59c87b4f16621cc8fd434e7579`，
归档 SHA-256 为
`68163f2098b1c996f4aa3c4ff7d439f9782e1b65f6343eeeb9b3863d73c5e391`。
独立对端源码保持上游内容，未关闭证书验证，也未删去 TLCP 客户端加密证书以换取通过。
本次 Release_64 报告和日志另存于该目录的
`results/c7b7265f144f-20261028-release64/`。
测试 shim SHA-256：`c7b7265f144fe97263f94f1a671e244c57f4dbcea56ba864bdb1d411c7718664`；
报告 SHA-256：`24153012443d78c496233e942702d0a3ce9e7908f5563384e8bc79859c6724e8`；
日志 SHA-256：`f61974e68bcceee6feae57c5a3d49f3bc70a2938a9f8b24645a0b2dd21379d24`。
先前同为 2/4 的结果保留在 `results/9e8ff56e1274-20261028-final/` 及
`results/b01623d62e9a-20261028/`，不改写其路径或哈希。

| 协议与认证方式 | 结果 | 观察到的边界 |
| --- | --- | --- |
| TLCP 服务端认证 | PASS | 当前测试不提供 session binding，未启用票据 |
| TLCP 双向认证 | FAIL | GmSSL 将第二张客户端加密叶子证书当作 CA 处理，报 `Invalid CA Certificate` |
| RFC 8998 服务端认证 | PASS | 最新客户端在确认国密套件后省略可选兼容 CCS |
| RFC 8998 双向认证 | FAIL | GmSSL 接收代码要求解密 record 恰好包含一个握手消息；客户端可在同一 record 合并 Certificate、CertificateVerify 和 Finished |

GmSSL 源码定位：`tls_vrf.c:401` 为服务端双叶子选择专用验证，而客户端进入普通
`x509_certs_verify`；`x509_cer.c:2021` 将第二张叶子当作 CA，
`x509_ext.c:1411` 因缺少 `keyCertSign` 拒绝。
RFC 8998 失败在 `tls.c:1410` 的单握手消息长度约束，由 `tls13.c:8672`
的客户端证书接收路径触发。位置对应上述固定 GmSSL commit。

TLS 1.3 允许在一个 record 中合并多个握手消息（RFC 8446 §5.1）。当前实现保留
BoringSSL 的标准分帧行为，未为该对端额外拆分 record。失败仍是当前互通限制，
不能将对端限制解释成双向认证已经通过。
TLCP 中开启会话票据的配置还可能被该 GmSSL 版本作为未知扩展拒绝；
本次无票据单向通过不证明其票据恢复兼容。铜锁 54 项中的完整/恢复测试另行通过。

## 已测性能

测试机器：Intel Core i9-13900HX，24 核/32 逻辑处理器，Windows x64。
机器信息：`out/gm_boringssl_tools/benchmark-cpu.json`。测量时未并行编译。
以下是本机微基准和内存 BIO 测量，不包含浏览器页面加载、网络 RTT、真实 USB、
SDK 进程或用户输入 PIN 的时间；未预先设定验收阈值。

### SM4 批量优化

x64 SSSE3 路径按四个块并行处理，并在批次级选择实现；保留无 SSSE3 路径。
AEAD 测量每次处理 16 KiB，预热至少 0.1 秒、单项测量至少 0.35 秒。
初始化和验证不计时，库内部动态分配计时。

| 指标 | 优化前 | 优化后 |
| --- | ---: | ---: |
| SM4-GCM 加密，MiB/s | 55.83 | 218.21 |
| SM4-GCM 解密，MiB/s | 56.19 | 217.19 |
| SM4-CCM 加密，MiB/s | 27.65 | 45.28 |
| SM4-CCM 解密，MiB/s | 27.69 | 44.84 |
| AES-128-GCM 加密，MiB/s | 14226.11 | 14373.90 |
| AES-128-GCM 解密，MiB/s | 15057.13 | 14999.64 |
| SM2 摘要签名，μs/次 | 118.53 | 116.38 |

原始数据：`out/gm_boringssl_tools/crypto-benchmark-baseline.json` 和
`out/gm_boringssl_tools/crypto-benchmark-batch.json`。
两份数据均来自本次 BoringSSL 实现，用于评估批量优化，不是与未修改 Chromium
的 A/B 页面性能对照。AES 的小幅波动不能作为所有普通网站零开销的证明。

### 同一铜锁对端的握手与记录处理

最终测量完成 **4 对、共 8 次运行**，执行顺序交替为铜锁/BoringSSL、BoringSSL/铜锁，
每项握手场景每次运行 200 次；测量期间无并行编译、测试或打包。
两组使用相同独立铜锁服务端、内存 BIO 和软件 SM2 外置签名。记录吞吐为 16 KiB
record、总计 64 MiB 双向有效载荷，包含两端处理成本。
原始数据、顺序、对端及两个测试后端的 SHA-256 均保存在
`out/gm_boringssl_tools/benchmark-paired-final/report.json`。

下表握手时延是“每轮 200 次均值”的四轮中位数，不是将 800 次样本合并后的 p50；
吞吐也是四轮中位数。比值范围按每一对的 BoringSSL/铜锁计算，列出最小值和最大值。
时延比值越小越快，吞吐比值越大越快。

| 指标 | 铜锁四轮中位数 | BoringSSL 四轮中位数 | 四对新/旧比值范围 |
| --- | ---: | ---: | ---: |
| TLCP 热上下文完整 mTLS，ms | 5.66025 | 3.31490 | 0.57234687–0.61057263 |
| TLCP 恢复 mTLS，ms | 2.43620 | 0.77040 | 0.31362647–0.33314823 |
| RFC 8998 热上下文完整 mTLS，ms | 4.69130 | 3.15205 | 0.65001174–0.71535772 |
| RFC 8998 恢复 mTLS，ms | 2.64580 | 1.41265 | 0.52225633–0.53974287 |
| TLCP 双向记录吞吐，MiB/s | 87.1365 | 96.5325 | 1.10073744–1.11547328 |
| RFC 8998 双向记录吞吐，MiB/s | 87.0425 | 94.4550 | 1.00698473–1.16340532 |

按中位数比较，TLCP/RFC 8998 完整 mTLS 分别约减少 41.4%/32.8%。四对完整握手、
恢复和记录吞吐均测得改善，但幅度仍有波动。TLCP 吞吐的四轮原始范围为铜锁
86.372–88.363、BoringSSL 95.380–97.740 MiB/s；RFC 8998 为铜锁
80.897–88.170、BoringSSL 88.520–97.763 MiB/s，其配对提升约 0.7%–16.3%。
这说明记录吞吐对具体运行条件仍敏感，不能承诺任意机器、真实 USBKey 或页面加载
都会按这些百分比改善；普通网页性能 A/B 也未完成，不能宣称普通网站零开销。

历史单轮仍保留：较早 RFC 8998 吞吐为 75.581 对 71.477 MiB/s，**约下降 5.4%**，
后一次为 87.411 对 94.563 MiB/s，方向反转。对应
`handshake-benchmark-tongsuo-reference.json`、`handshake-benchmark-boringssl.json`
及两个 `handshake-benchmark-*-final.json` 位于 `out/gm_boringssl_tools/`。
这些单轮数据不替代本次交替配对结果，也不从历史证据中删去负向观测。

这些 JSON 中的 `backend_bytes`、`backend_load_ms` 描述测试 DLL，
不是实际浏览器体积增量或生产启动时间。生产包净变化还需要相同配置下的产物对照。

## 首次单库迁移的安装包与产物绑定（历史快照）

`out/gm_boringssl_tools/build-release64-package.log` 最终 **5/5 完成**。
首次打包因已有 `thunder_2025` 资源与规范源码不一致而失败，随后按原 GN 复制规则
重新同步两个附带应用并完成重建，未修改应用源码或删除其依赖。

最终审计：**50/50 PASS**，证据为
`out/gm_boringssl_tools/release-audit-2/audit-report.json`。审计验证压缩包完整性、
`chrome.packed.7z` 内层归档与构建 `chrome.7z` 的 SHA-256、安装器 PE 嵌入归档与
构建归档的一致性，以及归档中的浏览器 EXE/DLL 与对应构建产物的字节哈希一致。
审计期间产物、规范应用库和清单保持不变。

归档不含浏览器的外置国密后端、测试 shim、独立对端或 SKF 模拟器；
`xlb153.exe`、`xlbrowser.dll` 的直接导入表没有外部国密库依赖。
这项检查覆盖归档及直接导入，不是对全部运行时 `LoadLibrary` 行为的证明。

两个原有附带应用仍携带自己的 OpenSSL 依赖。首次审计识别到六个
`libssl-3-x64.dll`/`libcrypto-3-x64.dll`（版本 3.1.8/3.3.3）后，依据明确归档路径和
规范源码字节哈希建立 `out/gm_boringssl_tools/application-openssl-manifest.json`，
二次审计逐项匹配，确认其哈希不同于独立铜锁对端。保留这些应用库不等于浏览器恢复
双库，但也**不能说整个安装包不含 OpenSSL**。

以下均位于 `out/Release_64/`，取自最终审计的 `artifacts` 字段：

| 产物 | 字节数 | SHA-256 |
| --- | ---: | --- |
| `xlb153.exe` | 4,149,248 | `e737caf8dc949a32456c7836f0ce4c7a9ece74fa8f4fc05d0e92a5287206dce9` |
| `xlbrowser.dll` | 344,469,504 | `ed227945e983e6d4af7904b0c1a0f011f2afcd984f4e4a177455d54457e7e863` |
| `xlb153_installer_1.0.0.1.exe` | 449,394,688 | `5950b4fae4d75b2bffbf56635acd3f9cb43fe4d5b9789d452638ccba7f0869c5` |

实际产物大小已测得；缺少相同应用载荷和构建设置的严格对照，不能把与旧包的大小差
直接归因于国密单库迁移。安装器尚未执行，实际安装和升级没有验收结果。

## 首次单库迁移的生产 EXE 启动检查（历史快照）

**CDP 冒烟检查 PASS**，使用 `out/Release_64/xlb153.exe`，SHA-256 与上述审计
产物相同。证据为 `out/gm_boringssl_tools/release64-product-cdp-smoke/report.json`、
`stdout.txt` 和 `stderr.txt`。驱动脚本为
`out/gm_boringssl_tools/product_cdp_smoke.mjs`，使用 Node 24 内置 WebSocket。

`Browser.getVersion` 返回 `Chrome/153.0.8004.0`；`Page.navigate` 打开本地 data HTML，
`Runtime.evaluate` 读到标记 `release64-render-ok`；`Browser.close` 后进程退出码为 0。
这项检查证明当前生产 EXE 可以启动、处理本地页面并正常退出，不证明 TLS、可见 GUI
或真实 USBKey 行为。

之前两次 `--dump-dom` 方法均在 60 秒超时，失败记录保留在
`out/gm_boringssl_tools/release64-product-smoke/report.json` 和
`out/gm_boringssl_tools/release64-product-smoke-2/report.json`。
超时根因尚未定位，不能宣称 `--dump-dom` 已修复或已排除与国密改动的关联。
通过 CDP 完成当前启动检查没有修改产品代码，也不抹去这两个失败观测。

## 验收状态与边界

| 项目 | 本记录状态 |
| --- | --- |
| `chrome` 与全部测试程序 | Release_64 链接完成，测试结果见上表；日志 `out/gm_boringssl_tools/build-release64-final.log` |
| `mini_installer`、归档/直接导入与嵌入载荷审计 | 构建成功，50/50 检查通过，最终大小和哈希见上节 |
| 真实浏览器 EXE 启动冒烟检查 | CDP 启动、本地页面标记及正常退出 PASS；范围见上节 |
| `--dump-dom` 启动检查方式 | 两次 60 秒超时，根因未定位，保留失败记录 |
| 安装、升级及真实页面性能 A/B | 未验证 |
| 后端性能重复配对测量 | 4 对交替测量完成，范围与限制见性能节 |
| 飞天 USBKey、签名厂商 SDK、PIN/PnP 与设备时延 | 无物理 Key/SDK，未验证 |
| macOS、Linux 原生编译/运行 | 未验证；USBKey/PIN 服务当前仅在 Windows x64 接通，其他系统还需接入对应服务及 SDK，不能仅以可移植密码代码视作支持 |
| 32 位 | 不在本次支持范围 |
| GM QUIC、TLCP SM2DHE、Key 内解密/密钥协商 | 不属于当前实现能力 |
| 密码合规认证、FIPS 国密认证 | 未声明；本地国密扩展不属于上游 BoringSSL FIPS 验证范围 |

## 复现入口

在工作区根目录执行；先完成构建，待 Ninja 退出后运行测试，性能测量期间停止编译。
实际 GN 参数及独立对端设置见 [BUILDING.md](BUILDING.md)。

```powershell
third_party/ninja/ninja.exe -C out/Release_64 -j24 -k0 `
  gm_integration_tests gm_crypto_benchmark chrome mini_installer

python net/gm/run_tests.py --build-dir out/Release_64 `
  --include-network --include-content --include-browser `
  --interop-work-dir C:/Users/Administrator/AppData/Local/CodexBuilds/chromium153-gm/gmssl-interop `
  --cmake "C:/Program Files/CMake/bin/cmake.exe"

python net/gm/run_tests.py --build-dir out/Release_64 --only crypto_benchmark

out/Release_64/gm_interop_peer/gm_backend_smoke.exe --benchmark `
  H:/chromium_142/src/out/Release_64/gm_boringssl_test_backend.dll 200

python net/gm/tests/run_gmssl_interop.py `
  --work-dir C:/Users/Administrator/AppData/Local/CodexBuilds/chromium153-gm/gmssl-interop `
  --backend H:/chromium_142/src/out/Release_64/gm_boringssl_test_backend.dll `
  --test-peer H:/chromium_142/src/out/Release_64/gm_interop_peer/gm_backend_smoke.exe `
  --cmake "C:/Program Files/CMake/bin/cmake.exe"
```

SSSE3 禁用检查使用 `OPENSSL_ia32cap=~0x20000000000`，完成后恢复原环境变量；
该值只用于测试 CPU 分派，不应成为生产环境配置。
`run_tests.py` 会覆盖输出目录的 `gm-test-results.json`，保留每次结果时应另存副本。
最后一条 GmSSL 命令因上述两个真实互通失败返回非零，不能忽略退出码。

重跑加强断言的铜锁对端时，可使用前述 manifest 对应的程序；历史
`interop-smoke-6` 针对前期 shim，本轮 runner 针对 Release_64 shim：

```powershell
& C:/Users/Administrator/AppData/Local/CodexBuilds/chromium153-gm/facade-verification-peer/build/Release/gm_backend_smoke.exe `
  H:/chromium_142/src/out/Release_64/gm_boringssl_test_backend.dll `
  H:/chromium_142/src/out/gm_boringssl_tools/interop-root.der
```
