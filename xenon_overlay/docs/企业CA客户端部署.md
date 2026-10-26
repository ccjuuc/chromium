# Xenon 企业 CA 客户端部署

浏览器从受信 HTTPS 接口获取完整 CA 集合，并应用到同一个 User Data 目录中的所有
普通、无痕、访客 Profile 和系统网络上下文。浏览器默认启动证书下发服务，读取浏览器可执行
文件所在目录的 `xenon-managed-ca.json`。六张公共 CA 候选仅作为服务端发布资产，浏览器
不会从源码目录读取它们，也没有内置接口地址或 token；配置缺失或无效时不会新增信任。

缓存 CA 保留至服务端发布更高版本替换或撤回；离线或刷新失败不会因快照期限移除 CA。
仅检查证书自身的 NotBefore/NotAfter，证书过期且没有其他有效信任路径时显示正常证书错误页。
新快照不输出 `expires_at`，旧字段可兼容读取但不控制有效性。这是快照语义调整：旧版客户端
仍要求到期字段，新服务端发布器与新客户端须配套升级。

## 配置与启动

1. 按 [服务端说明](../tools/updater/xenon_update_tool/managed_certificates/README.md) 发布快照、配置更新服务和 HTTPS。
   在 `xenon_update_tool` 目录用 `cargo run --locked -- serve` 可首次默认启动；
   默认配置不会自动发布证书。使用同一个 Rust 工具执行
   `cargo run --locked -- publish-certificates --source company-ca --generation 1 --all`
   可发布六张候选，输出路径自动读取服务端配置，无需 Node.js 或额外填写 `--out`。
   发布器 `--source` 与客户端 `source_id` 必须相同；已有版本时需使用更大的 generation。
2. 将 [配置示例](../tools/updater/xenon_update_tool/managed_certificates/client-config.example.json) 复制到浏览器
   可执行文件旁，命名为 `xenon-managed-ca.json`。设置真实 `endpoint`、`source_id`，
   示例已设为 `enabled:true`；省略该字段同样默认启用。
   `endpoint` 必须是实际 HTTPS 地址，不能直接填写本地 Axum 的 `http://127.0.0.1:9001`。
   示例与新建服务端均默认不要求 Bearer，客户端 `auth` 使用 `{"mode":"none"}`；启用服务端 Bearer 后再配置凭据。
   文件是独立 JSON 文档，不能混入更新服务的配置键；应限制为管理员/部署账号可写、浏览器账号可读。
3. 正常启动浏览器即可加载默认配置，不需要额外开关。若配置部署在其他受信目录，
   可在启动浏览器时用绝对路径覆盖，例如：

```powershell
.\xlb153.exe --xenon-managed-ca-config=C:/XenonConfig/managed-ca.json
```

覆盖开关需要在该 User Data 的浏览器主进程启动时提供；给已运行实例的第二次启动添加开关
不会重新创建主服务。指定覆盖路径后不会回退到默认文件。默认配置路径以浏览器可执行文件
目录为准，与启动时的当前工作目录无关；同一安装目录下不同 User Data 实例默认读取同一配置，
各自保存缓存。本地配置、凭据、bootstrap CA 和缓存仅在主进程启动时加载，不做每分钟本地
轮询。修改配置、凭据或 bootstrap 文件后须退出该 User Data 的所有浏览器进程，再重新启动。
默认文件缺失时，诊断页显示配置读取错误；补齐文件后也需要重启。
需要停用时，将配置改为 `{"xenon_managed_ca":{"enabled":false}}` 并重启，会移除本来源贡献。

默认刷新间隔 6 小时，允许 5 分钟至 24 小时；超时默认 30 秒，允许 5 至 120 秒。
正常刷新有抖动，失败按 1/5/15/60 分钟重试；恢复网络也会触发刷新。
浏览器先异步读取本地缓存，不等待首次远端请求才允许浏览。

## 本机 HTTPS 联调

更新工具支持同时运行 9001 管理页与 9443 HTTPS 下发接口，配置见
[本机 HTTPS 说明](../tools/updater/xenon_update_tool/README.md#本机-https-证书下发)。
客户端旁置配置使用以下字段；`bootstrap_ca_file` 应指向受信部署的本机 HTTPS 签发 CA：

```json
{
  "xenon_managed_ca": {
    "source_id": "company-ca",
    "endpoint": "https://127.0.0.1:9443/api/v1/browser/ca-certificates",
    "auth": {"mode": "none"},
    "bootstrap_ca_file": "C:/XenonConfig/local-bootstrap-ca.pem",
    "refresh_interval_seconds": 300
  }
}
```

正常启动后打开 `chrome://xenon-certificates/`，应显示已就绪及已发布的快照。
9001 页面发布后，浏览器按配置的刷新间隔获取新版本；诊断页的“刷新状态”只读取当前状态。
本机 HTTPS 的签发 CA 只用于下发连接，不作为网站根证书下发，也不安装到系统证书库。

## Windows 凭据

Bearer 模式读取当前 Windows 用户的 Generic Credential。`credential_ref` 是目标名称，
CredentialBlob 使用 **UTF-8 token 原始字节**，无结尾 NUL。普通凭据管理器 UI/cmdkey 的密码
编码不一定相同，请使用提供的脚本或按相同编码集成安装器：

```powershell
& .\xenon_overlay\tools\updater\xenon_update_tool\managed_certificates\set-client-credential.ps1 -CredentialRef 'xenon-managed-ca/company-ca'
```

脚本通过隐藏输入读取 token，写入当前账号的 Windows Credential Manager；重复执行会替换
同名凭据。服务端环境变量的值与这里输入的 token 相同。修改凭据后须重启浏览器，才能重新
读取并发起完整请求。token 不写入配置、缓存、日志或命令行。

若接口不需要鉴权，将 auth 改为 `{"mode":"none"}`，删除 `credential_ref`。
当前 Bearer 安全存储接入仅实现 Windows；其他平台应使用 none 或另行实现本机安全存储适配，
不会回退到明文凭据。

## 私有接口的引导信任

如果配置接口自身使用私有 CA，在客户端本地配置中添加绝对路径：

```json
"bootstrap_ca_file": "C:/XenonConfig/control-plane-ca.pem"
```

该文件只能包含 PEM CA CERTIFICATE 块，不能含私钥；需要通过受信部署渠道预置。
bootstrap 只供独立下载网络上下文使用，不授予普通网页信任。服务端下发的 CA 也不会反过来
改变下载接口的信任。未配置 bootstrap 时使用 Chromium 现有公共/平台信任。
下载保留系统代理配置，禁用 Cookie、交互式 HTTP 身份认证和重定向。

## 更新、撤回与诊断

每次更新或撤回都发布更高 generation。空 `certificates` 是撤回全部本来源证书；
删除服务文件或返回 404 不代表撤回。停用客户端配置也会撤回本来源贡献。
更新通过 Chromium 原生验证器接口传播，影响新验证及后续连接，不保证中断进行中的流。

客户端缓存位于 `<User Data>/ManagedCertificates/<SHA256(source_id)>/state.json`，
含版本、原始快照、ETag 与请求配置关联，不维护快照时间水位或到期标志。部署时应审计缓存目录、父目录及备份的
有效权限（包括继承 ACL）：仅授予所属浏览器用户必需的读写权限，管理员与 SYSTEM 按运维
需要授权，其他普通账号不得读取或写入。当前代码不自动设置或验证 Windows 专用 ACL。
缓存 SHA-256 摘要是公开的一致性检查，不是防本地篡改认证；有写权限者能同时修改正文和摘要。
更新保存成功后才发布；接口失败时继续使用已接受的旧缓存，联网获取更高版本后才替换或撤回。
缓存损坏不会作为全新客户端自动重置版本；应恢复可靠备份或明确重新配置来源。
运行中删除/回退缓存也不能重置内存版本水位。

服务端快照、accepted 状态、配置和 TLS 私钥目录也须按服务、发布和部署账号职责授予最少
权限，其他普通账号不得读取或写入；具体见
[服务端目录权限说明](../tools/updater/xenon_update_tool/managed_certificates/README.md#目录权限与本地状态)。

可以用 `--enable-logging --log-file=C:/XenonLogs/managed-ca.log` 收集浏览器日志，
检索 `Managed certificate`。缓存中的快照表示已接受的数据，不能代表每张证书在所有上下文
均获得信任：原有显式不信任、策略/用户配置和公共根约束仍然优先。
打开 `chrome://xenon-certificates/` 可查看未配置、停用、拉取中、配置/刷新错误等状态，
以及本次浏览器运行的最近尝试/成功时间、当前已接受快照、证书指纹与约束，
并输入 HTTPS 地址执行证书测试。页面通过当前 Profile 的正常网络上下文发送不带 Cookie 的
HEAD 请求，展示 TLS 结果、HTTP 状态及站点证书信息；不会忽略证书错误或跟随重定向。
HTTP 404/405 与 TLS 验证失败分别展示。测试成功不能单独证明该链使用了 Xenon 下发的 CA，
系统、用户和原生策略中的信任也可能使验证通过。
这是只读诊断页面，不提供导入、删除或改变信任的操作。错误域名、过期网站证书、无效链
不会因托管 CA 而跳过检查。

## 自动验证

```powershell
autoninja -C out/Release_64 managed_certificate_unittests managed_certificate_verifier_unittests
.\out\Release_64\managed_certificate_unittests.exe
.\out\Release_64\managed_certificate_verifier_unittests.exe --gtest_filter=CertVerifyProcBuiltinTest.XenonManaged*
node xenon_overlay/tools/updater/xenon_update_tool/managed_certificates/smoke-browser.mjs --browser H:/chromium_142/src/out/Release_64/xlb153.exe
```

最后一个命令要求先构建包含本功能的新浏览器，使用临时 User Data、回环 HTTPS 服务和仓库
测试 CA，验证引导信任隔离、旧 expires_at 已过仍接受、无痕、离线缓存、空集合撤回及 HTTPS 302
保留证书诊断且不跟随跳转，不修改系统证书库。
测试证书具有有效期，过期后需随 Chromium 测试资产更新。

2026-10-26 Windows x64 验证：新快照语义下客户端 43 项测试通过，原生验证器的 6 项
`XenonManaged*` 测试通过；其中叶证书、中间 CA、根 CA 的过期和未生效情况均返回
`ERR_CERT_DATE_INVALID`。Rust 测试 118 项通过、3 项既有测试忽略，管理接口 11 项通过。
新版浏览器和服务端 Release 构建成功。真实浏览器冒烟测试通过引导信任隔离、旧快照期限已过
仍正常下发、普通/无痕生效、离线重启保留缓存，以及更高版本空集合撤回。HTTPS 302 能保留
TLS 成功、HTTP 状态和服务器证书信息，且没有跟随跳转；撤回后诊断页正常显示 TLS 校验失败。

此前的 70 天构建日期阻断已通过真实本地 HEAD 的提交元数据解除，未修改系统时钟或测试入口。
本地提交没有 Gerrit `Change-Id` 时，可在源码根目录执行以下命令，再重新构建测试目标：

```powershell
python build/util/lastchange.py --filter=. --source-dir . --output build/util/LASTCHANGE
```

该命令使用实际本地提交时间，生成的 `build/util/LASTCHANGE` 及 `.committime` 是忽略文件，
无需提交。后续 `runhooks` 可能按默认过滤规则将元数据重新生成为上游提交；应核对后再构建。

适用边界：Windows Crashpad/WinHTTP 的信任不随此更改；QUIC 的原有公共已知根要求保留。
不再进行每分钟本地轮询、快照定时撤回或时间水位写入；证书日期仍按原生验证器规则检查，
部署主机应保持正确系统时间。远端更新与撤回须在客户端联网刷新后生效。

完整字段、约束和异常处理见 [协议](企业CA下发协议设计.md)。
