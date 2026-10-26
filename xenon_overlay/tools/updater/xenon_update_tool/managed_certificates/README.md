# 托管 CA 服务端发布工具

证书发布和 HTTP 服务统一由父目录的 Rust 工具 [xenon-update](../README.md) 提供：
`publish-certificates` 子命令生成
[企业 CA 下发协议 v1](../../../../docs/企业CA下发协议设计.md) 的不可变 JSON 快照，
`serve` 启动 Axum，通过 `GET /api/v1/browser/ca-certificates` 下发快照，
与更新和崩溃上传共用进程。证书发布无需 Node.js，六张候选资产随可执行文件内嵌；
发布命令只准备服务端文件，不下载证书、不启动服务，也不写系统证书库或 Chromium Root Store。

`smoke-browser.mjs` 会启动临时 Node HTTPS 模拟服务来验证浏览器，
正式 Axum 路由由父项目的 Rust 测试覆盖。

浏览器启动参数、配置和 Windows 凭据接入见 [客户端部署](../../../../docs/企业CA客户端部署.md)。

本次快照语义调整为：缓存 CA 保留至更高版本替换或撤回，仅检查证书自身有效期。
新发布不输出 `expires_at`；兼容读取旧字段但不据此到期。旧版客户端仍要求该字段并执行
快照到期撤回，因此新服务端发布器与新客户端必须配套升级。

## 默认启动

在 `H:/chromium_142/src/xenon_overlay/tools/updater/xenon_update_tool` 目录执行：

```powershell
cargo run --locked -- serve
```

未显式传 `--config` 时使用可执行文件旁的 `xenon-update/xenon-update.json`；文件不存在时
按默认参数初始化，监听 `127.0.0.1:9001`，创建同一配置目录中的 `managed-ca/`。新配置启用证书接口、无 Bearer，
但不会自动发布任何证书；首次发布前接口返回 503。
已有默认配置直接读取，损坏配置会报错且不重建。显式 `--config` 指向不存在的文件时需先
执行 `init`；已有配置省略 `managed_certificates` 时仍保持关闭并返回 404。

默认启动只准备服务端配置。浏览器仍需实际 HTTPS `endpoint` 和来源配置，不能直接使用
Axum 的本地 HTTP 地址作为控制面接口；HTTPS 部署见下文。

## 证书资产

`certificates/manifest.json` 记录六张证书的名称、单张 PEM 文件、DER SHA-256、SPKI SHA-256、
原始官方下载地址和 UTC 有效期。发布时重新检查这两种指纹、证书有效期元数据、CA 属性、
keyCertSign、自签名与重复证书；PKCS#7 官方下载已转换为单张 PEM。

| PEM 文件 | 证书 |
| --- | --- |
| `gdca-trustauth-e5-root.pem` | GDCA TrustAUTH E5 ROOT |
| `gdca-chinese-r5-root.pem` | 数安时代 R5 根 CA |
| `bjca-global-root-ca1.pem` | BJCA Global Root CA1 |
| `bjca-global-root-ca2.pem` | BJCA Global Root CA2 |
| `trustasia-global-root-ca-g3.pem` | TrustAsia Global Root CA G3 |
| `trustasia-global-root-ca-g4.pem` | TrustAsia Global Root CA G4 |

这六张证书来自已核验的浏览器根清单差异，选择依据和公钥去重结果见
[证书差异清单](../../../../docs/常用浏览器证书差异清单.md)。文件出现在服务端仓库不会自动赋予信任。
发布时必须显式选择 `--all`、一个或多个 `--include`，或 `--empty`；没有默认选中集合。
生成条目的 role 为 `trust_anchor`，不加额外 DNS/IP 约束，浏览器仍须执行证书内部约束、
有效期和既有不信任规则。需要企业专属约束或中间证书时，可由自己的协议发布器生成；
本工具只管理表中资产，不覆盖其他发布器生成的不同条目结构。

## 生成、更新和撤回

也可直接使用本机管理页面：默认 `serve` 启动后，在服务所在电脑打开
`http://127.0.0.1:9001/certificates`（拆分监听时使用管理端口）。
页面显示“已发布文件”和“当前下发”的状态，便于识别未生效或被拒绝的版本。
首次选择需要的证书、填写与客户端一致的来源，再点击“发布所选证书”；
下一版会自动填入更大的版本号，刷新状态会保留未提交的输入。
清空勾选不会撤回信任，需明确选择“撤回全部”并发布新版本。
页面与以下 CLI 共用 Rust 发布器；浏览器客户端仍需通过 HTTPS 地址获取快照，
并在 `chrome://xenon-certificates/` 验证客户端状态。

以下发布及验证命令均在 `xenon_overlay/tools/updater/xenon_update_tool` 目录执行，
本地路径为 `H:/chromium_142/src/xenon_overlay/tools/updater/xenon_update_tool`。
发布命令默认读取服务端配置的 `managed_certificates.snapshot_file`，无需手填输出路径；
配置中关闭该功能时会报错。未指定 `--config` 且默认配置不存在时自动初始化，规则与 `serve` 相同；
已有配置不会重写，显式 `--config` 指向缺失文件时需先执行 `init`。

```powershell
# 显式发布全部六张；首次发布使用 generation 1。
cargo run --locked -- publish-certificates --source company-ca --generation 1 --all

# 重新发布同一集合也须增加 generation；无需为快照定期续期。
cargo run --locked -- publish-certificates --source company-ca --generation 2 --all

# 下一版只保留两个根；--include 可重复。文件名从上表选择。
cargo run --locked -- publish-certificates --source company-ca --generation 3 --include gdca-trustauth-e5-root.pem --include bjca-global-root-ca1.pem

# 显式撤回本来源全部证书，不能用删除文件或 HTTP 404 代替。
cargo run --locked -- publish-certificates --source company-ca --generation 4 --empty
```

使用预编译程序时，将 `cargo run --locked --` 替换为 `xenon-update`（Windows 本地运行可用
`.\xenon-update.exe`）。自定义配置用全局参数 `--config C:/XenonConfig/xenon-update.json`。
`--source` 和 `--generation` 必须显式指定；`--all`、可重复的 `--include`、`--empty`
三种选择方式只能使用一种。`--include` 接受上表的资产文件名，不读取任意外部证书文件。
发布成功后，正在运行的 Axum 服务会在下次请求时加载新快照，无需重启。

若只准备离线快照，可显式指定 `--out`；此时不读取或创建服务端配置，也不要求配置启用 CA 接口：

```powershell
cargo run --locked -- publish-certificates --source company-ca --generation 1 --all --out C:/XenonStaging/company-ca.json
```

默认 `encoding=der_base64`，可用 `--encoding pem`。快照无独立有效期，不再提供 `--valid-days`。
签发时间使用执行时 UTC，精确到秒；时间在发布时只生成一次，不会随 GET 请求改变。
generation 必须是无前导零的十进制字符串，范围 `1..18446744073709551615`。
调整格式、名称、集合或发布日期都要增加 generation。重复执行已发布的同一 generation 会失败，
已发布正文保持不可变。

发布器检查输出文件及服务端的 `<snapshot_file>.accepted.json`，拒绝换 source、同版改写和回滚。
临时文件与输出位于同一目录，先同步再原子替换；并发发布使用 `<snapshot_file>.publish.lock`。
进程中断留下该标记时，先确认没有活跃发布进程，再删除该单个标记重试；不自动抢占锁。
损坏的已有快照会阻止发布，应从可靠备份恢复，或隔离损坏的输出后以高于 accepted 水位的版本发布。
不要删除 accepted 文件来重置版本。输出和状态目录应只允许服务账号与受信发布账号写入。

轮换流程：发布旧根加新根 → 等客户端更新 → 切换站点证书链 → 新版本移除旧根。
业务回滚是用更大 generation 重新发布旧集合。离线客户端保留缓存 CA，联网更新后才接受替换或撤回。
若要完全关闭功能，先发布空快照并给在线客户端刷新时间，再移除服务端配置。

## 服务端配置与 HTTPS

首次默认启动、默认配置下首次发布和显式 `init` 会在新配置顶层写入以下字段：

```json
{
  "managed_certificates": {
    "snapshot_file": "managed-ca/current.json"
  }
}
```

这是配置片段，保留已有 `schema_version`、`port`、`manifest` 等字段。
已有配置省略 `managed_certificates` 时仍不启用该接口；`"managed_certificates": {}` 使用
默认 `snapshot_file`。相对路径按配置文件目录解析，默认是可执行文件旁的
`xenon-update/managed-ca/current.json`。`init` 会创建该目录，服务启动时也会自动创建
`snapshot_file` 的父目录，因此旧配置添加 `"managed_certificates": {}` 后无需手工建目录。
服务账号仍需具有创建目录以及持久写入 accepted 状态和锁文件的权限。

默认无需 Bearer。需要鉴权时，在 `managed_certificates` 对象中增加
`"bearer_token_env": "XENON_CA_BEARER_TOKEN"`；指定环境变量必须在服务启动时存在且非空，
否则启动失败。仅允许 RFC 6750 token 字符，最长 4096 字节。配置只保存环境变量名，不保存 token。
服务只保留 token 的 SHA-256 用于等长比较，认证检查先于读快照或条件请求。

PowerShell 可以通过隐藏输入给服务进程提供凭据，避免把 token 写进命令历史和进程参数：

```powershell
$secret = Read-Host 'Managed CA bearer token' -AsSecureString
$env:XENON_CA_BEARER_TOKEN = [System.Net.NetworkCredential]::new('', $secret).Password
try {
  cargo run --locked -- serve
} finally {
  Remove-Item Env:XENON_CA_BEARER_TOKEN -ErrorAction SilentlyContinue
  $secret.Dispose()
}
```

正式服务由部署平台的凭据注入机制设置环境变量，token 不放在公开配置、URL、日志或网页中。
客户端的凭据由浏览器安全凭据入口另行配置，相同 token 对应客户端 Bearer 值；环境变量名不是
客户端 `credential_ref`。轮换 token 后更新客户端安全凭据，并重启服务和浏览器；鉴权配置不会从证书快照下发。
浏览器客户端的 Bearer 安全存储目前仅实现 Windows；非 Windows 配置 Bearer 会报错，不会
降级到明文凭据或无鉴权请求。其他平台需实现安全存储适配，或在接口无需鉴权时使用 `auth.mode=none`。

本机联调可启用工具自带的 [9443 HTTPS 监听](../README.md#本机-https-证书下发)，
9001 继续提供管理页面，两者读取同一份证书快照。
正式部署可沿用 `127.0.0.1:9001` 回环监听，在已有反向代理终止 HTTPS。
Nginx 的现有 TLS server 块中可添加：

```nginx
location = /api/v1/browser/ca-certificates {
    proxy_set_header Host 127.0.0.1:9001;
    proxy_set_header Authorization $http_authorization;
    proxy_set_header Accept-Encoding "";
    proxy_cache off;
    gzip off;
    proxy_pass http://127.0.0.1:9001;
}
```

客户端 `endpoint` 必须填写实际部署的 HTTPS 地址，例如
`https://config.example.com/api/v1/browser/ca-certificates`，不接受 HTTP 或重定向；
不能直接填写 `http://127.0.0.1:9001/api/v1/browser/ca-certificates`。
接口 HTTPS 证书应能通过控制面既有公共/平台信任或本地 bootstrap CA 验证。
保留上游的 ETag、Cache-Control、Vary、Authorization 和原始正文，不对 JSON 重排、压缩或缓存为公开响应。
不要代理 `/admin/`。包含 Origin 的网页请求被现有服务入口拒绝；本接口面向浏览器原生控制面。

## 目录权限与本地状态

部署和备份恢复时，审计快照、`<snapshot_file>.accepted.json`、配置及 TLS 私钥所在目录和
父目录的有效权限，包括继承 ACL。服务账号读取配置和快照、写入 accepted 状态及锁文件；
受信发布账号读取版本状态并写入快照；配置和私钥仅由受信部署账号更新，TLS 服务账号读取私钥。
管理员与 SYSTEM 按运维需要授权，其他普通账号不得读取或写入，备份同样受限。
当前代码不自动设置或验证 Windows 专用 ACL，权限由安装器和部署负责。

客户端缓存位于 `<User Data>/ManagedCertificates/<SHA256(source_id)>/state.json`，
仅所属浏览器用户获得必需的读写权限，管理员与 SYSTEM 按运维需要授权，其他普通账号不得
读取或写入。缓存摘要、ETag 和快照哈希用于一致性检查，不是防本地篡改认证；有写权限者能
同时修改正文和摘要，不能用版本水位或哈希代替目录权限。客户端配置及 bootstrap 部署另见
[客户端部署](../../../../docs/企业CA客户端部署.md#更新撤回与诊断)。

## 快照加载及 HTTP 行为

每次已认证 GET 检查文件，合法高版本在 accepted 文件持久化成功后才提供给客户端；
不需要重启服务来更换快照。原始 UTF-8 字节不重序列化，ETag 为带引号的正文 SHA-256。
`<snapshot_file>.accepted.json` 保存最后接受的原始快照，重启可恢复，并继续拒绝回退；
`<snapshot_file>.serve.lock` 的系统锁保证单个服务进程拥有这组状态，不应在服务运行时删除。
备份要同时包含输出文件与 accepted 文件；accepted 损坏时启动失败，不静默重置水位。

| 条件 | HTTP 行为 |
| --- | --- |
| 配置省略 managed_certificates（包括旧配置） | 404 |
| 新初始化默认配置，尚未发布快照 | 503，Retry-After: 60，no-store，无 ETag |
| 认证失败 | 401，no-store，无 ETag，未读取快照 |
| 非 GET / 带 query | 405 / 400 |
| 有效快照 | 200，application/json，private/no-cache，Vary: Authorization，完整原始字节 |
| 有效快照且 If-None-Match 匹配 | 304，空正文，保持版本和正文 |
| 新文件非法、丢失、回滚或同版冲突，已有已接受快照 | 继续提供旧快照及其 ETag |
| 没有可用的已接受快照 | 503，Retry-After: 60，no-store，无 ETag |
| 高版本空 certificates | 200/304 的有效撤回快照 |

服务完整校验 v1 字段、JSON 重复键/未知字段、UTC 时间、版本、证书与约束。
最大正文 4 MiB、256 张证书、单张 DER 16 KiB、JSON 嵌套 16 层；CA 必须具有
BasicConstraints CA=true，如有 KeyUsage 必须含 keyCertSign。
服务端 accepted 文件保存版本和正文；不再维护快照时间水位或到期撤回。
旧 `expires_at` 仅兼容读取，`issued_at` 是发布日期元数据；证书自身有效期由客户端原生验证器检查。

## 测试

```powershell
cargo test --locked
```

覆盖六张证书指纹、公钥、自签名、子集/空快照、严格 JSON、不可变版本、原子发布、持久版本水位，
以及服务端 200/304/鉴权/旧 expires_at 兼容/坏文件/回滚/撤回、持久化失败和省略配置时关闭路由。

构建包含本功能的浏览器后，可从同一目录执行浏览器验证；`--browser` 使用实际浏览器的绝对路径：

```powershell
node managed_certificates/smoke-browser.mjs --browser H:/chromium_142/src/out/Release_64/xlb153.exe
```

该脚本使用临时 User Data 和回环 HTTPS 服务，验证引导信任隔离、旧 expires_at 已过仍接受、
普通/无痕下发、离线缓存、空集合撤回及诊断页的 HTTPS 302 结果，不修改系统证书库。

## 资产来源与使用说明

核验日期：2026-10-25。每个证书的原始下载地址保存在 manifest；发布工具不会自动从网络更新。
来源是 [GDCA 官方证书查询](https://www.gdca.com.cn/customer_service/knowledge_universe/ca_cq/)、
[BJCA Global 官方证书库](https://repo.bjca.cn/global/) 和
[TrustAsia 官方证书库](https://repository.trustasia.com/)。证书属于各 CA，
本项目的源码许可证不自动授予第三方证书再分发许可。

[TrustAsia CP&CPS v2.2.0 §9.5](https://repository.trustasia.com/en/repo/cps/adoc/v2.2.0)
保留所发证书的知识产权，并对未获书面许可的商业转载网站信息作出限制；该节给 CPS 文档的
CC BY-ND 授权不能当作证书文件的授权。GDCA 历史公开
[EV CPS v3.2 §9.5](https://www.gdca.com.cn/export/sites/default/customer_service/.content/attachments/GDCAEVCPSV3.2.pdf)
也载有商业转载限制；本次未据此断言当前各类证书的具体再分发许可。
未确认 GDCA、BJCA 的这些资产具有独立开放再分发授权。这里记录已查明的来源与条款，
不作新的许可声明；对外商业分发前应核对适用现行条款及已有授权。本次改动没有执行外部发布。
