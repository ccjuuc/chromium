# Xenon 企业 CA 下发协议 v1

状态：已实现服务端接口、发布工具及浏览器接入，依据当前 Chromium 153 源码。
浏览器默认启动证书服务并读取程序目录的本地配置；部署入口及示例见 [客户端部署](企业CA客户端部署.md) 和
[服务端发布工具](../tools/updater/xenon_update_tool/managed_certificates/README.md)。示例域名需要替换为实际服务地址。
服务端统一使用 Rust `xenon-update`：`publish-certificates` 子命令发布快照，
`serve` 子命令运行 Axum 下发接口；证书发布不依赖 Node.js。

本次调整快照语义：缓存 CA 保留至服务端发布更高版本替换或撤回，只由原生验证器检查证书
自身有效期。新快照不输出 `expires_at`；该字段仅为兼容旧快照而可选读取，不控制信任期限。
旧版客户端仍要求该字段并执行快照到期撤回，因此新服务端发布器与新客户端必须配套升级。

## 1. 生效范围

一个浏览器进程统一管理同一 User Data 下的企业 CA，覆盖所有普通、无痕、
访客 Profile，以及已有和以后创建的 StoragePartition。使用不同 User Data
目录启动的实例各自配置；此范围不包含其他操作系统账号或其他应用。

Xenon 更新器使用的 System NetworkContext 也接入同一份快照。
证书下发请求使用独立的控制面 NetworkContext，信任来源见第 3 节。
Windows Crashpad 上传使用 WinHTTP，不会自动获得浏览器内 CA，本版不改变其信任。

只授予 TLS 服务器认证所需的 CA 信任；不分发私钥、客户端身份凭据或代码签名证书。
不写操作系统证书库，不修改 Chrome Root Store，不关闭网站证书的正常验证。

## 2. 客户端配置

配置由受信本地 JSON 文件提供，默认读取浏览器程序目录的 `xenon-managed-ca.json`，
可通过 `--xenon-managed-ca-config=<绝对路径>` 覆盖。本地配置、凭据和 bootstrap CA 仅在
浏览器主进程启动时加载，不做每分钟本地轮询；修改后需重启该 User Data 的浏览器主进程。
文件缺失或无效时报告配置错误，不添加信任，不猜测下发地址；不开放给普通网页或任意渲染进程。
v1 仅支持一个托管来源，避免多个服务器争夺同一信任配置。

```json
{
  "xenon_managed_ca": {
    "enabled": true,
    "source_id": "example-enterprise",
    "endpoint": "https://config.example.com/api/v1/browser/ca-certificates",
    "auth": {
      "mode": "bearer",
      "credential_ref": "xenon-managed-ca/example-enterprise"
    },
    "refresh_interval_seconds": 21600,
    "request_timeout_seconds": 30
  }
}
```

| 字段 | 规则 |
| --- | --- |
| `enabled` | 默认 true；显式 false 并重启后不激活本来源的信任，保留版本水位和缓存元信息 |
| `source_id` | 必填，本地固定的来源标识；1–64 个 ASCII 字母、数字、`.`、`_`、`-` |
| `endpoint` | 必填，绝对 HTTPS URL；禁止 URL 用户信息和 fragment；不得在 query 中存 token |
| `auth.mode` | `none` 或 `bearer`，默认 `none` |
| `auth.credential_ref` | bearer 模式必填，引用浏览器级安全凭据存储；none 模式不允许此字段 |
| `bootstrap_ca_file` | 可选；受信本地配置指定的绝对路径，内容为预置 PEM CA 集合，仅供控制面使用；不是下载 URL |
| `refresh_interval_seconds` | 默认 21600（6 小时），范围 300–86400 |
| `request_timeout_seconds` | 默认 30，范围 5–120 |

Bearer token 不写入证书缓存、普通配置、命令行或日志，不复用任意 Profile 的网页
Cookie 或账号 token。安全凭据设施缺失时 bearer 模式报告配置错误，不降级成明文保存。
控制面请求不携带 Cookie，不接受响应写入 Cookie，也不进行交互式 HTTP 身份认证。

同一 `source_id` 更换 URL 仍沿用最高版本水位。改变 `source_id` 是本地重新配置来源：
撤回旧来源的活动信任，并隔离新来源的缓存和版本；响应本身不能改变来源。

## 3. 下发接口与引导信任

客户端向配置的完整 URL 发起 GET。示例路径不要求服务端固定使用：

```http
GET /api/v1/browser/ca-certificates HTTP/1.1
Host: config.example.com
Accept: application/json
Accept-Encoding: identity
Authorization: Bearer <从安全凭据存储读取的值>
If-None-Match: "example-enterprise-42"
```

`Authorization` 仅在 bearer 模式发送。`If-None-Match` 仅在同来源、同 endpoint、
同鉴权配置且存在已验证缓存和 ETag 时发送。URL 或凭据变更并重启后先进行完整请求。
v1 禁止所有 HTTP 重定向，3xx 不跟随；303、307、308 也不例外。

控制面使用 Chrome 公共信任、既有平台本地信任，或安装时预置的 bootstrap CA。
它不注入本协议下载的托管 CA，避免下发结果反过来扩大下发接口自身的信任。
bootstrap CA 只能通过本地安装/受信配置入口提供，不能靠同一尚未可信的接口取得。
配置了 `bootstrap_ca_file` 时，完整验证文件里的所有 CA 后才启用控制面下载；
文件缺失或任一条目非法即报告配置错误。允许多个 CERTIFICATE 块，但不允许私钥或其他
块类型，最多 256 张、每张 DER 最多 16 KiB、整个文件最多 4 MiB；不通过此字段授予网站信任。
本地文件更新后需重启浏览器加载新的控制面信任，不接受服务端响应指定替代路径。
如果接口自身使用未预置的私有 CA，首次请求报告 TLS 验证错误，不能临时忽略该错误。

v1 的来源认证依赖上述 HTTPS 链和可选 Bearer，不含离线签名封装。
DER 指纹、缓存摘要和 ETag 均不等同于数字签名。未来如需签名配置，另行定义签名算法、
受信发布密钥和轮换规则，不允许响应自行选择关闭客户端认证。

## 4. 成功响应

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Cache-Control: private, no-cache
ETag: "example-enterprise-42"
```

下面的 `data` 是格式占位符，不是可用于验证的真实证书：

```json
{
  "schema_version": 1,
  "source_id": "example-enterprise",
  "generation": "42",
  "issued_at": "2026-10-25T06:00:00Z",
  "certificates": [
    {
      "name": "Example Enterprise Root CA",
      "role": "trust_anchor",
      "encoding": "der_base64",
      "data": "<一张CA证书DER字节的Base64编码>",
      "constraints": {
        "permitted_dns_names": ["corp.example.com"],
        "permitted_cidrs": ["10.20.0.0/16", "fd00:20::/48"]
      }
    },
    {
      "name": "Example Enterprise Intermediate CA",
      "role": "intermediate",
      "encoding": "pem",
      "data": "-----BEGIN CERTIFICATE-----\n<一张中间CA的Base64 DER>\n-----END CERTIFICATE-----\n"
    }
  ]
}
```

| 字段 | 规则 |
| --- | --- |
| `schema_version` | 整数，固定为 1；不支持的版本拒绝整份响应 |
| `source_id` | 必须与客户端本地配置完全一致 |
| `generation` | `1` 到 `18446744073709551615` 的十进制字符串，无前导零、符号或小数；避免 JSON 大整数精度丢失 |
| `issued_at` | 必填发布日期，UTC 格式 `YYYY-MM-DDTHH:mm:ssZ`；不作为信任期限 |
| `expires_at` | 兼容旧快照的可选 UTC 时间，格式同 issued_at；新发布不输出，其数值及与 issued_at 的先后关系不控制快照有效性或撤回 |
| `certificates` | 必填数组，允许空数组；最大 256 项，包含所有应由该来源保留的证书 |
| `name` | 可选显示名称，最多 128 个 Unicode 字符；不参与身份判定，不作为文件名或 HTML |
| `role` | 必填；`trust_anchor` 授予 CA 信任，`intermediate` 仅辅助构链 |
| `encoding` | 必填；仅 `pem`、`der_base64` |
| `data` | 必填；严格一张 X.509 CA 证书，不接受私钥、PKCS#12、PKCS#7 或证书链包 |
| `constraints` | 可选，仅允许用于 trust_anchor；一旦存在至少包含一种非空的约束列表 |

v1 拒绝未知字段、重复 JSON 键、错误类型、重复 DER 指纹和任何非法证书/约束，
不静默跳过坏条目后接受剩余证书。仅通过 Chromium 的“忽略错误解析列表”接口不够，
客户端必须在调用它之前完成整份快照的严格验证。

DER Base64 使用标准字母表和合法 padding，不含空白；PEM 允许 LF/CRLF 和首尾空白，
但只能有一个 `CERTIFICATE` 块，块外不得有其他内容。拒绝 DER 尾随字节。
每张证书解码后最多 16 KiB；CA 必须具有 `BasicConstraints CA=true`，如有 KeyUsage，
必须包含 `keyCertSign`。`trust_anchor` 可以是受信中间 CA，不强制自签名。
客户端自行计算 SHA-256 DER 指纹作为身份，不把服务端显示名称作为唯一键。

快照没有独立有效期。结构合法的未来/过期 CA 可以保留在缓存与列表中，但验证器仍检查
CA 及网站证书自身有效期；无有效信任路径时，普通访问按 Chromium 正常逻辑显示证书错误页。

### 名称约束的准确含义

每种列表最多 64 项。DNS 使用小写 ASCII/A-label，允许 `example.com`（本域及子域）
和 `.example.com`（仅子域）；拒绝 `*`、URL、端口、尾点及空名称。
CIDR 支持 IPv4/IPv6，要求合法前缀和网络地址，不接受含主机位的输入。

这是 X.509 NameConstraints 语义：

- 缺失或空的某种列表表示该名称类型不受外加约束。只填写 DNS 不会禁止所有 IP SAN。
- 检查叶证书的全部相关 SAN，而非只检查本次请求的 URL；一个额外的不允许 SAN 也会使验证失败。
- CIDR 检查证书里的 IP SAN，不限制 DNS 解析后的远端 IP。
- 未填写 constraints 表示不施加额外名称约束，证书内部约束和正常校验仍然生效。
- 这些约束只限定本来源贡献的信任路径；不能承诺阻止其他来源、交叉签名或另一条可信链。

## 5. 版本、清空与轮换

`generation` 是来源级单调发布序号，不能以客户端时间或浏览器版本代替。
服务端在发布时生成一次不可变 JSON 正文；同一 generation 的响应字节必须保持一致。
客户端记录响应 UTF-8 原始字节的 SHA-256 摘要，相同 generation 不同摘要直接拒绝。
这意味着修改 JSON 格式、名称、证书、约束或发布日期都需要发布新 generation；
服务端不能每次请求重新填写 issued_at。ETag 只优化传输，不替代版本检查。

| 收到的版本 | 行为 |
| --- | --- |
| 高于已接受的最高版本 | 完整验证、持久化成功后采用新快照 |
| 等于最高版本且正文摘要相同 | 幂等处理，必要时恢复缓存 |
| 等于最高版本但正文不同 | 拒绝，记录 `generation_conflict` |
| 低于最高版本 | 拒绝，记录 `generation_rollback` |

清空或停用不会清除版本水位。检测到已有状态文件损坏时报告
`state_corrupt`，关闭本来源的信任与下载，不自动当作首次注册接受任意版本，
需从可靠备份恢复或本地重新配置来源。运行中删除或回退状态文件也不能降低内存版本水位。
整个 User Data 被删除属于重新配置实例，
客户端不能承诺在本地可信状态完全丢失后仍保留原来的水位。

清空必须返回一份有效的新快照：

```json
{
  "schema_version": 1,
  "source_id": "example-enterprise",
  "generation": "43",
  "issued_at": "2026-10-25T07:00:00Z",
  "certificates": []
}
```

数组中删除某张证书表示撤回本来源的这一份配置，不删除用户或其他企业策略中的同一证书。
v1 不提供远程 `distrusted_spkis`；“撤回己方信任”与“无论其他来源如何都禁用该公钥”是两种操作。
业务回滚也要用更大的 generation 发布旧证书集合，不能降低 generation。

正常轮换顺序：发布旧 CA + 新 CA → 客户端更新 → 服务器切换证书链 → 发布仅新 CA。
自定义服务器需要保留足够重叠期；替换 CA 不会自动修复网站端缺失的中间证书或域名错误。

## 6. 缓存与失败处理

缓存路径：`<User Data>/ManagedCertificates/<SHA256(source_id)>/state.json`。
一个原子提交的状态包含来源、最高 generation、正文摘要、原始完整正文、ETag、
endpoint/鉴权配置关联；不包含 token，也不维护快照时间水位或到期标志。使用受限文件权限，
临时文件写入并同步后原子替换；替换失败时保持旧活动快照，不对外声明新版本已采用。
SHA-256 只能检测意外损坏，不能代替文件访问权限或来源认证。

更新使用启动时加载的配置，并通过 `request_context` 关联配置、bootstrap 和凭据。磁盘 I/O
在同一任务队列串行执行，候选快照写入成功后才发布。Shutdown 或重新启动服务会使旧回调
失效，旧回调不能向新运行实例发布；已经派发的旧 I/O 仍可按队列完成，新启动的读取排在其后。
配置、凭据或 bootstrap 变化在浏览器重启后加载，缓存提交继续遵守来源与版本规则。

启动时先验证并加载已有缓存，同时后台刷新。首次启动没有有效缓存时，
普通浏览不等待远端服务；私有 CA 网站在配置准备好前按正常规则报不可信。
已有缓存的启动加载与新 NetworkContext 创建需要协调，避免初始请求绕过应有配置。

网络请求在启动、网络恢复和定时器触发时进行；同一来源只允许一个进行中请求。
正常刷新默认 6 小时并加入 ±10% 抖动。失败重试以 1、5、15、60 分钟逐步退避，
最大 60 分钟。429/503 可使用合法 Retry-After，等待上限 1 小时。

| 结果 | 行为 |
| --- | --- |
| 200 + 合法快照 | 按版本规则原子保存并分发 |
| 304 | 仅在本次有条件请求且存在对应已验证缓存时接受；不改变版本或正文 |
| 304 但无有效缓存 | 协议错误；下一次去掉 If-None-Match 请求完整响应，不授予信任 |
| 204、404 或缺失 certificates | 错误，不解释成清空 |
| 401/403、超时、TLS/解析/磁盘错误、5xx | 保留已接受的旧快照，记录状态并按规则重试 |
| 兼容旧快照的 expires_at 已过 | 继续保留缓存与 CA，证书自身有效期仍由验证器检查 |
| 配置 enabled=false 并重启 | 不激活本来源的信任，不启动拉取，保留版本状态 |

没有本地每分钟轮询、快照到期定时撤回或时间水位持久化。离线时继续保留最后已接受的 CA，
直到获得更高版本替换/撤回，或本地停用配置后重启。服务端更新与撤回需要客户端重新联网
获取；快照保留不豁免证书自身的 NotBefore/NotAfter、域名、链、约束及不信任检查。

传输仅接受 UTF-8 JSON、Content-Type application/json 和 identity 编码；
响应体最多 4 MiB（包括分块传输累积），JSON 嵌套最多 16 层。
证书数量、单证书大小与总正文上限分别检查，满足其中一项不豁免其他上限。

## 7. 与 Chromium 现有信任合并

先取得完整的企业策略、用户证书和现有不信任配置，再合并本来源，最后发送
`AdditionalCertificates` 完整快照。该 Mojo 接口是全量替换，不能仅传新下发的 CA。
不更改 `include_system_trust_store` 的既有取值。

| 本协议条目 | Chromium 映射 |
| --- | --- |
| trust_anchor，无外加约束 | xenon_managed_trust_anchors，空外加约束 |
| trust_anchor，有外加约束 | xenon_managed_trust_anchors，携带 DNS/CIDR 约束 |
| intermediate | all_certificates |

新增独立字段保留托管来源身份，验证器使用 enforce constraints + expiry 的信任类型，
保留证书内部约束和根有效期检查，不使用无约束的 trust_anchors 或显式信任叶证书接口。
下发入口先严格解析，避免 Chromium 的容错列表解析造成部分接受。

去重及冲突规则：

1. 远端快照内部重复 DER 指纹，即使 role/constraints 相同，也拒绝整份快照。
2. 不改写原有 SPKI 不信任列表，也不绕过验证器的不信任判定。
3. 在同一网络上下文中，某 DER 已由上游策略/用户配置指定信任类型或约束时，
   不再追加 Xenon 的重复信任项；
   仅作为构链提示存在时，可以添加 Xenon 的显式信任。
4. 上游约束与 Xenon 约束不同，保留上游配置，不取并集、覆盖或声称已取交集。
   Chromium 同类重复条目没有完整冲突定义，依赖追加顺序会产生错误结果。
5. 平台信任、不同 DER 的交叉签名、其他 CA 构建出的路径保持原有语义。
   Xenon 的约束不保证成为所有信任来源共同遵守的全局限制。
6. 托管来源不为当前生效的公共信任锚或其同公钥变体重复授予本地信任；这可能绕过 CRS 的
   SCT 等约束。验证器按 DER 与 SPKI 识别重复公共信任锚，跳过该托管信任贡献。
   判断使用当前生效 CRS 的完整信任锚集合，包含组件更新和受信任中间 CA，
   在 CRS 变化后重新计算托管贡献，而非只检查编译时的 104 张根。
   公共根更新沿用原生根库机制，不能把其他浏览器清单的差集自动解释为缺失并重新授信。
   构链提示本身不授予信任。

快照“已接受”表示来源数据已完整验证并保存，不代表每个条目在每个 Profile 中都有效。
局部遮蔽不导致整份下发失败；快照中的数量不表示验证器实际采用的数量或网站一定可访问。
当前测试页面展示已接受快照与 HTTPS 测试结果；未实现逐 Profile 的冲突计数或完整证书管理
界面，不能把快照中的条目显示成已全部生效。

## 8. 客户端接入结构

浏览器进程级 `XenonManagedCertificateService`：

```text
受信本地配置 → 独立控制面下载 → 验证完整快照 → 原子缓存 / 不可变内存快照
                                                        ↓
                              所有 Profile + 无痕/访客 + System NetworkContext
                                                        ↓
                           合并既有信任 → CertVerifierServiceUpdater
                                                        ↓
                                  原生证书验证器 / 验证缓存 / 连接池
```

- 服务生命周期由浏览器进程管理，早于 Profile 网络上下文使用、晚于它们销毁。
  下载、解析和磁盘 I/O 不阻塞 UI 线程；按第 6 节串行执行缓存 I/O，写入成功后才发布，
  通过运行代次与弱回调使旧运行实例的回调失效。
- Profile 接入需覆盖初始创建、已加载分区更新、新分区、无痕和访客，以及 network service
  崩溃后的重建。合并位置应在所有原生来源汇总之后，不能只对最后使用的 Profile 操作。
- 用户证书 DB 是用户所有，不作为托管来源的持久化存储。证书管理界面可增加
  “Xenon 托管”来源，显示来源、版本、指纹、约束、证书有效期和冲突状态。
- SystemNetworkContextManager 已接入初始快照、updater 和重建恢复。
  控制面 NetworkContext 始终不订阅本托管服务，以保持引导信任独立。
- 原子性只承诺本地已接受快照提交；各进程/网络上下文通过 Mojo 最终更新。
  UI 区分“快照已接受”和“更新已派发”；现有生产更新接口没有完成回调，不能用
  WaitUntilNextUpdateForTesting 冒充生产确认或宣称全浏览器瞬时切换。
- 信任变化会刷新验证缓存和连接池状态，但不保证中断所有已在传输的请求。
  需要强制终止现有连接的紧急撤回应另行定义，不包含在 v1 清空语义中。

HTTP/3/QUIC 默认要求公共已知根，普通 HTTPS 在允许时可回退 TCP；
不能承诺仅 H3/WebTransport 场景可用。v1 不放宽 QUIC 的已知根规则。

## 9. 可观测状态与验收

当前通过浏览器日志记录配置、下载、解析、持久化错误类别，缓存可查看已接受的
generation、issued_at 与原始快照。不记录 Authorization、响应全文或证书完整 PEM。
`chrome://xenon-certificates/` 提供启停/拉取状态、配置及刷新错误代码、本次运行的尝试/成功时间、
只读快照与 HTTPS 测试；逐上下文派发计数及遮蔽/冲突展示
尚未实现。页面区分快照已接受与实际验证结果，不提供修改信任操作。

至少覆盖以下测试：

- 接口：PEM/DER、非法 CA、私钥/链包/尾随字节、重复 JSON 键/证书、大小上限、来源不匹配。
- 版本：新增、替换、空列表、发布更高版本回滚、旧版本重放、同版变更、URL 变更保留水位。
- 缓存：写入中断保持旧版本、重启恢复、损坏缓存、304 保留正文、旧 expires_at 已过仍接受、离线保留。
- 异步竞态：服务停止或较新版本更新后，旧回调不能重新注入信任；保存失败不激活候选快照。
- 约束：DNS 本域/子域、IPv4/IPv6、未配置某类型、多个 SAN 越界、CIDR 不等于连接 IP。
- 合并：用户与策略已有证书、同 DER 不同约束、显式不信任、撤回只删除本来源贡献。
- 公共根：当前公共信任锚及同 SPKI 变体不被重复授信；组件更新纳入新锚后重新过滤已有托管
  贡献，包含受信任中间 CA；CRS 约束不因下发而失效。
- 生命周期：多 Profile、无痕、访客、新 StoragePartition、系统更新器、网络进程重启。
- 控制面：默认可信接口、预置 bootstrap CA、下发 CA 不改变控制面信任、凭据变化、拒绝重定向。
- 端到端：未下发时网站报不可信，下发后新握手通过，错误域名/过期网站证书仍失败；
  测试 CA 仅由 Xenon 来源授信，等待目标上下文收到更新后再建立新连接，断言撤回后新握手失败。
  同一 CA 仍被用户、策略或系统独立授信时不能要求失败；已有请求不保证立刻中断。
  WinHTTP 不随此修改自动获得信任。

## 10. 源码依据

- `chrome/browser/net/profile_network_context_service.cc`：原生 CA 策略汇总、用户证书合并、分区更新。
- `services/network/public/mojom/cert_verifier_service_updater.mojom`：附加证书结构、全量替换语义。
- `services/cert_verifier/cert_verifier_creation.cc`：Mojo 参数到验证器实例参数的转换。
- `net/cert/cert_verify_proc_builtin.cc`：信任类型优先级、重复条目限制、根约束/有效期与外加 SAN 约束。
- `components/policy/resources/templates/policy_definitions/CertificateManagement/`：原生 CA 策略格式。
- `chrome/browser/net/system_network_context_manager.cc`：系统上下文快照与 updater 接入。
- `xenon_overlay/chrome/browser/updater/xenon_update_browser_context.cc`：更新器使用系统上下文。
- `third_party/crashpad/crashpad/util/net/http_transport_win.cc`：Windows Crashpad 使用 WinHTTP。

## 11. 常用浏览器补充证书调查

已补充同目录 `常用浏览器证书差异清单.md` 和 `浏览器证书差异清单_2026-10-25.json`，
覆盖 Chrome、Edge 的根计划来源、Firefox、Apple 平台及 360 官方公开清单；QQ、搜狗
缺少可核验完整清单的部分明确标记为未知。调查按 DER SHA-256 比较，并区分用途与约束。

本地已包含 CFCA EV ROOT、GDCA R5、TrustAsia TLS RSA/ECC、vTrus RSA/ECC、UCA 等根。
360 清单中的国内指纹差异为 GDCA E5、数安时代 R5、BJCA Global CA1/CA2、TrustAsia Global
G3/G4；这 6 项现作为服务端发布资产，由部署方显式选择发布，客户端配置启用后拉取。
它们没有编入浏览器默认根库；客户端未配置时不新增信任。SM2 国密证书与密码/协议支持另行设计。
