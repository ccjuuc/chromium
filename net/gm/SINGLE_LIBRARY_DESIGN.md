# Chromium 153 国密单库方案

2026-10-28。状态：当前基线移植的 Windows x64 软件集成、构建、项目测试及包审计
已完成；独立 GmSSL 双向认证仍有兼容性失败，真实设备及其他系统尚未验收。
`README.md`、`BUILDING.md` 已改为单库入口；`TEST_RESULTS.md` 的旧结果仍属于
BoringSSL 与隔离铜锁后端方案，不能作为本次迁移通过验收的证据。

## 决策

以 Chromium 当前配套的 BoringSSL 为唯一浏览器密码/TLS 库，增加模块化
SM2、SM3、SM4、RFC 8998 和 TLCP 支持。保留 Chromium 的 socket、证书验证、
连接池和异步私钥调用路径。飞天 SKF 驱动继续在独立 Utility 进程运行。

这里的“单库”指浏览器自身的密码与 TLS 实现；不要求把厂商 USBKey 驱动合并进来，
也不要求所有协议共用完全相同的状态机。TLCP 仍需要独立的协议状态分支。
把两份完整 SSL 实现静态链接进同一 DLL，不满足此目标。

| 候选 | 结论与原因 |
| --- | --- |
| 当前 BoringSSL 增加国密扩展 | 采用。保持 Chromium 现有 API 和普通连接路径，但承担算法、协议及 PKI 扩展的持续维护。 |
| 铜锁处理全部 TCP TLS，其他组件继续 BoringSSL | 可以统一 TCP 协商，但仍有两个库，不符合本次收敛目标。 |
| 整个 Chromium 全部替换为铜锁 | 不采用。需广泛适配 Chromium、QUICHE、libpki、WebCrypto、WebRTC、Rust 绑定等接口和行为。 |

这不是“单库天然维护更少”的承诺：它减少双引擎的运行时适配，增加对 BoringSSL
内部扩展的维护。选择该路线，是在用户明确重视联合协商、连接性能和单库的前提下，
尽量缩小对 Chromium 其他组件的替换范围。

## 源码依据

- 当前 BoringSSL 基线为 `52ba6a143e03e9ac84369f79ac3d06dea0128dab`，见
  `third_party/boringssl/README.chromium`；未经本地扩展的基线在
  `src/include/openssl/opensslconf.h` 声明 `OPENSSL_NO_SM2/SM3/SM4`。
  本地已补入相应实现；仅删除这些宏不能获得国密支持。
- `third_party/boringssl/BUILD.gn` 同时编译 crypto、SSL、libpki，并生成 Rust 绑定。
  BoringSSL 并非仅是一个可替换的 TCP 握手插件。
- `net/socket/ssl_client_socket_impl.cc` 使用 `TLS_with_buffers_method`、
  `SSL_CTX_set_custom_verify`、`SSL_PRIVATE_KEY_METHOD`、ECH 和 ALPS；
  `net/ssl/openssl_ssl_util.cc` 使用 `SSL_CREDENTIAL`。
- `net/cert/x509_certificate.h` 使用 `CRYPTO_BUFFER`；
  `net/cert/cert_verify_proc_builtin.cc` 使用 `bssl::CertPathBuilder`。
  不能用另一套默认 X509 验证替换 Chromium 的根约束和证书验证规则。
- 本地铜锁 8.5.0 已有 QUIC 接口和 `X25519MLKEM768`。放弃全库替换的依据是
  Chromium 当前 API、回调与验证语义的兼容成本，不能描述成铜锁没有 QUIC/PQC。

旧目录 `F:/aiswork/src` 当前源码标识为 Chromium 110。找到的国密实现是
`third_party/boringssl/src/0001-add-sm-tls.patch`，日期为 2023-10-13，未应用于该目录的
BoringSSL checkout。只读 `git apply --stat` 显示 80 个文件、11730 行新增、49 行删除；
这个数量不等于迁移工程量。

旧补丁中的算法、曲线、测试向量、TLCP 消息处理可逐项审查后复用，不能整体移植：

- Windows x64 上存在把 32 位 `unsigned int` 对象的地址作为 `size_t*` 写入的风险，
  不能直接复用旧签名适配代码。
- 直接加载 SKF DLL、同步签名不能进入新的网络/密码库进程路径。
- 全局 RFC 8998 状态和动态修改共享 SSL 上下文，不能用于并发、跨站点连接。
- 通用签名路径中未经密钥类型检查的 EC 字段访问必须重做。
- 旧内部结构和当前 BoringSSL 已不同。需要重新接入当前 EVP、握手和 PKI 边界，
  不能冻结或回退整个 BoringSSL 版本。
- 补丁存在国密 TLS 1.3 相关实现，不等于 transcript、HRR、PSK 和会话恢复已验证完整。

补丁署名、代码来源声明和较新公开分支的完成度，见
[LEGACY_PATCH_PROVENANCE.md](LEGACY_PATCH_PROVENANCE.md)。目前没有证据表明公开社区分支
就是这份本地补丁的直接后续版本。

## 协商行为

当前浏览器实现采用按连接启用：普通网站仍为 GM-disabled，不生成 SM2 key_share。
显式 `tls13_auto` 规则和已经验证成功的 RFC 8998 能力记录，进入
`SSLClientSocketImpl` 的联合协商分支。严格 `rfc8998`/`tlcp` 规则保持原语义。
标准套件结果沿用 Chromium CertVerifier、CT、普通客户端证书及 ECH；只有实际
国密结果进入 `gm_tls13_client` 的国密验证与异步 SM2 Key 桥接。管理的国密根不会
因联合协商而成为普通 TLS 的额外信任根。普通结果不新建或延长国密能力记录。

联合模式缓存保留完整规则与授权身份代次，使用独立的国密缓存失效代数；拔 Key
不会清理普通 TLS/HTTPS 代理的票据，也不阻止普通连接的晚到票据入缓存。国密根、
CRL 的解析按不可变规则复用，每次认证仍重新检查证书期限和吊销状态。
联合路径的首次根/CRL 解析和验链在线程池执行，请求取消或销毁会丢弃晚到回复，
不恢复握手或触发 PIN。严格 `rfc8998`/`tlcp` facade 对同次握手去重验链，
但仍同步执行一次；不能声称所有国密工作均已离开网络 I/O 线程。

普通 TLS 1.3 与 RFC 8998 使用一个 TLS 入口。在同一 ClientHello 中提供适用的
密码套件、签名算法、曲线和 key_share，由双方协商。保留当前普通算法及后量子配置，
不通过顺序失败重连区分 AES 与 SM4。

要避免因缺少 SM2 key_share 触发 HRR，必须首包提供相应参数。当前 BoringSSL 的
`SSL_set1_client_key_shares` 可配置多个 share；扩展后的数量上限、序列化、ECH 内外层
以及服务器兼容性仍需验证。增加 SM2 会带来临时密钥计算和握手字节开销，不能承诺免费。
单个非压缩 SM2 share entry 约增加 69 字节，完整 ClientHello 的增量还包括其他声明。

TLS 1.3 的顺序是 ClientHello、ServerHello、加密握手参数和证书。服务器根据客户端
能力选择身份，再发送所选身份的证书链；浏览器不能先读这条证书链再选择初始 TLS 引擎。

TLCP 的 wire version、双证书和握手消息不同。单库不会自动使传统纯 TLCP 服务端接受
普通 TLS ClientHello。必须保留明确的 TLCP 入口；强制协议策略、已验证的能力记录或
受限的首次协议发现，属于后续兼容设计。不能把 TLS 和 TLCP 的首次零探测当作验收承诺。
任何协议发现都不得绕过证书、吊销或 Key 授权失败。

QUIC 继续使用已支持的标准 TLS 1.3/报文保护路径。增加 TCP 的 RFC 8998 算法，不等于
已经支持国密 QUIC；未经单独定义和验证的 SM4 套件不得自动进入 QUIC 广告和选择路径。

## 实现边界

| 层 | 必须完成的工作 |
| --- | --- |
| 算法 | SM3/HMAC/HKDF、SM4 及所需记录模式、SM2 曲线/公钥解析/签名验证/加密等。优先审查成熟实现与来源；秘密相关运算需满足恒定时间要求。 |
| RFC 8998 | 套件、签名方案、群组和密钥交换；SM3 transcript、HKDF、Finished、HRR、PSK binder、恢复、KeyUpdate、exporter。不能只注册套件编号。 |
| TLCP | 在单库内增加协议分支；签名/加密证书角色、相应记录层和密钥派生、外置签名、版本与套件边界、会话恢复。 |
| PKI | 扩展公钥、OID、签名算法、曲线准入和链验证，沿用 Chromium 信任与名称/用途/约束验证。TLS 的公钥解析与 libpki 的算法白名单都需要修改。 |
| USBKey | 把现有独立服务接入 `net::SSLPrivateKey` / `SSL_PRIVATE_KEY_METHOD`，保留异步暂停、取消、超时、拔出及晚到回复处理。 |
| 浏览器 | 标准证书选择生命周期、协议元数据和错误展示；现有池、恢复和身份授权的隔离条件保留。 |

算法文件尽量独立，注册和状态机改动集中在明确接点，但不保证所有修改都能限制在新增目录。
不将 SM2 当作换曲线后的 ECDSA。握手签名、证书签名使用的 SM2 标识和 ZA 规则不同，
必须按各自规范与证书生态验证；RFC 8998 的相关规定见第 3.2.1 节。

PKI 特别需要覆盖 `src/pki/signature_algorithm.*`、`verify_signed_data.cc`、
`simple_path_builder_delegate.cc` 和 `src/ssl/ssl_privkey.cc` 中的独立准入条件。
还要覆盖 OCSP/CRL 等签名验证链路，以及现有 CT、根约束、显式不信任等规则的适用行为。
支持 SM2 证书不表示自动信任任意国密 CA；公共根和管理员信任仍通过证书验证配置管理。
根与用于补链的中间证书不得混为一类。

## USBKey 选择

收到服务端 CertificateRequest 后，根据接受的 CA、签名方案、用途、有效期及协议要求，
筛选本地可用证书；由浏览器完成用户选择和 PIN 授权。TLCP 另外检查双证书配对及两条链。
策略可限制身份和强制协议，但不应成为普通自动协商必须配置的逐站开关。

现有 `KeySigner.Sign` 接口接收 32 字节摘要，可以保留。新的桥接层负责按照当前协议的
SM2 标识和公钥计算正确摘要，SKF 服务负责真正的私钥操作；不能把已经计算的摘要再次
按普通消息重复哈希。生产私钥留在 Key 内，PIN 不进入网络服务。服务端没有请求客户端
认证时，不应仅因具备国密能力便弹出 PIN 或发送客户端证书。

复用 `services/gm_key` 的驱动验证、专用 SDK 线程、授权能力和故障隔离，以及
`chrome/browser/gm` 的 PIN 界面。Key 进程保持独立与“密码库单库”并不冲突。

## 迁移与验收顺序

1. 在当前 BoringSSL 基线上完成独立算法和 PKI 支持，以标准向量、负例和独立实现交叉验证。
   审查许可证、上游来源与必要的恒定时间实现，不直接落入旧补丁的全局状态和 SKF 调用。
2. 打通标准 TLS 1.3 与 RFC 8998 联合协商、恢复及异步 Key；抓取真实握手记录，验证两类
   服务端均不因客户端漏发参数而额外重连或 HRR。测试自然需要 HRR 的合法场景也仍正确。
3. 补齐 TLCP 客户端和双证书验证。保留当前静态 ECC 能力范围，不把单库迁移顺带描述为
   已完成 SM2DHE 或 Key 解密/密钥协商。用独立服务端验证互操作。
4. 接入浏览器并回归普通 TLS、ECH、后量子组、H2、H3、代理/WSS、两种连接池、信任更新、
   会话隔离和 Key 生命周期。显式身份与信任代次仍参与复用约束，不能因库统一而删除。
5. 对照现有版本测量普通网站及国密网站的首连/复用延迟、p50/p95、CPU、内存、ClientHello
   大小、吞吐和实际安装包增量。软件 Key 与真实硬件结果分别报告。
6. 生产构建切为单库，删除 GM DLL 加载/预加载和安装器铜锁载荷。暂保留现有
   socket/context 适配层的生命周期与授权契约，其内部统一调用当前 BoringSSL；
   这不是另一份密码库。铜锁/GmSSL 留作 test-only 互操作对端。

至少保留以下失败测试：错误密钥/签名/SM2标识、非法曲线点、认证标签篡改、跨算法恢复、
错误证书角色、未知根/过期/域名不符/撤销、Key 拔出/错误 PIN/取消、以及无客户端认证请求。
策略更新和普通/国密并发访问不能串用身份、连接、会话或响应缓存。

旧的独立 GmSSL TLCP 双客户端证书互操作失败仍是待解决项，不能用同库两端自测覆盖它。
手头没有物理飞天 Key，软件模拟可验证密码和 IPC 流程，不能替代真机 SDK、驱动及设备时延验收。

维护上保存锁定 revision、每项扩展的来源、补丁边界与测试矩阵。每次 Chromium/BoringSSL
升级在新基线上重放并执行普通协议和国密协议回归。未完成迁移前不删除已有可测试实现。

## 当前验证与性能边界

当前 `out/Release_64` 已完成浏览器与测试程序链接并重新运行：crypto 1880 项、
PKI 1328 项、SSL 485 项、网络层 2655 项、Network Service 66 项、Key service 25 项、
Content 8 项、浏览器单元 85 项、导航 35 项及独立铜锁互通 54 项通过。
连接配置 130 checks、SKF adapter 100 checks 通过；前期无 SSSE3 的 SM4 路径另有
37 项通过。浏览器联合协商、后台验证取消和 PIN 生命周期均有软件回归覆盖，
真实 Key 仍未验收。安装包已构建并通过 50 项归档/产物绑定/直接导入检查，
重复性能测量已完成 4 对交替运行；同一生产 EXE 的 CDP 启动、本地页面标记与正常
退出检查通过，不据此声明 TLS、可见 GUI 或真 Key 验收。早前两次 `--dump-dom`
60 秒超时的记录保留，根因未定位，不能宣称该方式已修复或排除国密关联。
GmSSL 独立复跑仍为 2/4，因此包含该项的总 runner 退出码为 1，不能写成全量通过：
两种协议单向认证通过，双向认证仍存在其客户端
双证书链/同 record 多握手消息处理的兼容性限制。完整结果与复现入口见
[BORINGSSL_TEST_RESULTS.md](BORINGSSL_TEST_RESULTS.md)。

已有本机微基准：x64 四块并行优化将 16 KiB SM4-GCM 加密从 55.83 提高到
218.21 MiB/s。最终 4 对交替测量在无并行编译、测试或打包时串行执行，每轮每项
200 次，使用相同铜锁对端、内存 BIO 和软件 Key。完整 mTLS 每轮均值的跨轮中位数：
TLCP 为 5.66025→3.31490 ms，RFC 8998 为 4.69130→3.15205 ms，约减少 41.4%/32.8%。
RFC 8998 吞吐中位数为 87.0425→94.4550 MiB/s，四对新/旧比值为
1.00698473–1.16340532，仍有波动。历史单轮下降约 5.4% 的观测继续保留；
不能将本机结果解释为任意场景稳定提升或普通网站零开销。测量不含网络、浏览器 IPC、
真实 USB 和 PIN 输入，不代表页面性能或硬件验收。

已提供 [BoringSSL 补丁包](../../third_party/boringssl/gm_patch/README.md)，明确列出
81 个改动文件；在独立临时基线索引检查、应用补丁后，完整树与工作源码快照一致。
此交付解决嵌套 Git 源码不能随顶层提交自动保存的问题。普通 GN 构建不依赖导出工具
或 Go；更新基线必须审查移植并重新测试，不能静默强套旧补丁。

当前安装包为 449,394,688 字节，最终哈希和浏览器产物大小见测试报告。没有严格相同
应用载荷的对照，不能将大小差直接归因于国密迁移。旧铜锁 DLL 为 4,468,224 字节，
新增国密实现也占用空间，删除 DLL 的大小不等于最终净减量。两个附带应用按规范源码
保留自己的 OpenSSL 库；“浏览器单库”不表示整个安装包无 OpenSSL。
测试 shim 的大小和加载时间也不能作为浏览器增量或生产启动时间。
前期验证输出为 `out/gm_release`，实际产物位于
`C:/Users/Administrator/AppData/Local/CodexBuilds/chromium153-gm/Release_gm`；
这些历史记录保留原路径；`gm_release` junction 已移除，C: 路径只用于标识前期记录。
当前及后续均使用用户指定的普通目录 `out/Release_64`，保留原
GN 参数并启用国密，完成 GN 重生、浏览器/测试程序链接及上述本轮测试。
安装包首次构建遇到既有应用资源不同步，已按原 GN 规则重新同步、重建并完成包审计。
安装/升级、真实设备与其他系统的验收状态仍以测试记录的对应阶段为准。

参考规范：[TLS 1.3 / RFC 8446](https://www.rfc-editor.org/rfc/rfc8446.html)、
[RFC 8998](https://www.rfc-editor.org/rfc/rfc8998.html)。
