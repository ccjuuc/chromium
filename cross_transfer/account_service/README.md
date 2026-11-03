# 账号与同账号设备服务

中继通过同一个进程提供 TCP 443 上的 HTTPS 账号、设备和中继 API，
以及可选的 UDP 443 STUN Binding 服务；无需其他服务进程或端口号。
启用方式是给 `relay/server.py` 添加 `--account-database`。只有账号、会话和设备目录使用 SQLite 持久化。
传输任务、状态及历史由浏览器维护，无第三方 Python 运行依赖。

部署入口：`https://115.29.16.37/api/v1`。原有中继路径仍为
`/v1/rooms/...`，继续使用原有角色令牌。

## API

非空请求体为 UTF-8 JSON，编码后最多 128 KiB。受保护接口使用 `Authorization: Bearer <token>`。
响应禁止缓存，时间戳为 Unix 秒。

| 方法 | 相对 `/api/v1` 的路径 | 作用 |
| --- | --- | --- |
| GET | `/healthz` | 服务健康状态，无需登录 |
| POST | `/auth/register` | 注册并建立会话 |
| POST | `/auth/login` | 密码登录 |
| GET | `/auth/me` | 当前账号 |
| POST | `/auth/logout` | 撤销当前会话，可同时撤销本机设备 |
| GET | `/devices` | 当前账号设备列表 |
| PUT | `/devices/{id}` | 登记或更新本账号设备 |
| POST | `/devices/{id}/heartbeat` | 更新在线心跳 |
| DELETE | `/devices/{id}` | 撤销设备登记 |

注册和登录请求：

```json
{"username":"alice","password":"<12 至 256 个字符的密码>"}
```

成功响应包含 `account: {id, username}`、随机 64 位十六进制 `token` 和
`expiresAt`；注册返回 201，登录返回 200。用户名为 3 至 64 个 ASCII
字母、数字或 `._-`，比较时忽略大小写。密码使用独立随机盐和 scrypt
保存摘要；token 仅保存 SHA-256 摘要。会话默认有效 7 天，每账号最多
保留 32 个会话，超出后替换最早会话。

退出支持空请求体或 `{}`，只撤销当前会话；传入
`{"deviceId":"<32 hex>"}` 时，在同一数据库事务中撤销该账号的设备登记、
当前会话，并清理关联消息及临时中继凭据。设备尚未登记也可退出；
其他账号的设备 ID 会被拒绝。设备变更在与退出同步后重新验证会话，
避免迟到的登记请求恢复已退出设备。

设备 ID 由客户端用 CSPRNG 生成，为 32 位小写十六进制字符串：

```json
{
  "displayName": "我的电脑",
  "platform": "windows",
  "publicKey": "<本机 Ed25519 公钥原始 32 字节的标准 Base64>"
}
```

登记响应为 `{"device": {...}}`，列表为 `{"devices": [...]}`。设备对象含
`id`、`displayName`、`platform`、`publicKey`、`lastSeenAt`、`online`。
建议每 30 秒心跳，90 秒未更新则离线。每账号最多登记 32 台活动设备。
删除保留设备 ID 的账号所有权，其他账号不能认领，原账号可重新登记。
撤销设备不会撤销账号会话。在线心跳不证明网络可达性或私钥持有权。

错误响应为 `{"error": {"code": "...", "message": "..."}}`，主要状态为
400 参数无效、401 未登录/凭据错误、404 不存在、409 冲突、413 请求过大、
429 超限。默认每来源 IP 每分钟允许 10 次登录、5 次注册。集成模式直接
使用 TLS 连接来源地址，忽略客户端伪造的转发地址。

## 部署与验证

服务为 `cross-transfer.service`，源码位于 `/opt/cross-transfer/current/`，
TLS 证书与原中继凭据位于 `/etc/cross-transfer/`，数据库位于
`/var/lib/cross-transfer/accounts.sqlite3`。非 root 用户 `crosstransfer`
通过受限能力监听 443，开机自动启动。

随附单元传入 `--stun-port 443 --stun-url stun:115.29.16.37:443`。
`--stun-port` 缺省为 0（禁用），`--stun-url` 可重复以公告多个 STUN
地址，仅支持 `stun:` / `stuns:` URL，不接受 TURN 地址或凭据。本机 Binding
监听器提供普通 UDP STUN；`stuns:` 仅用于公告外部已部署的 TLS STUN 服务。
服务器防火墙及云安全组应同时允许 TCP 443 和 UDP 443。UDP 443 用于
发现公网映射地址，文件和文本的直连数据仍在两台设备之间传送；无法
建立 ICE 直连时使用现有 HTTPS 中继。对称 NAT、UDP 被禁或运营商限制
仍可能需要中继。

浏览器的“使用 STUN”开关默认关闭，按本地 Profile 保存，修改从下一次传输
生效。开启后只使用服务端公告的地址。未部署 STUN 时省略上述两个参数；
客户端仍可使用本机网卡候选直连，失败后通过 HTTPS 中继完成传输。

服务器可通过 `--direct-min-bytes N` 公告直连阈值：载荷大小
**小于 N 字节**时直接使用中继，等于或大于 N 时优先尝试直连。文本按
UTF-8 字节数计算，文件按原始文件大小计算，不计 JSON 或传输协议开销。
有效范围为 `0` 至 `8589934593`（8 GiB + 1）；`0` 表示所有载荷均优先直连。
命令行缺省为 0，随附的 systemd 单元缺省为 1024。

生产服务器在 `/etc/cross-transfer/service.env` 中设置覆盖值：

```ini
CROSS_TRANSFER_DIRECT_MIN_BYTES=1024
```

修改后执行 `systemctl restart cross-transfer` 生效，无需重新编译浏览器。
更换 systemd 单元文件时先执行 `systemctl daemon-reload`。配置在进程启动时
读取，浏览器通过 `/transport-config` 获取阈值，并在本地选择路径。
已经成功建立的直连优先复用，包括低于阈值的小消息。浏览器还独立保存最近
32 个对端设备的直连成功记录；连接缓存清空或过期后，对这些设备仍先尝试
新直连。记录按账号和本机设备隔离，不发送到服务端；两端通过普通邀请消息
传递本次是否优先直连的选择。服务端不解析文件大小，
也不维护或裁决每个任务的直连/中继状态。

```sh
systemctl status cross-transfer
systemctl restart cross-transfer
journalctl -u cross-transfer
python3 cross_transfer/account_service/test_server.py
python3 cross_transfer/tests/test_account_relay.py
```

最后一项测试需要 PATH 中有 `openssl`，验证同一 HTTPS 监听中的账号与
原有中继功能。数据库目录权限为 0700，数据库权限为 0600。备份运行中的
数据库应使用 SQLite backup API，避免遗漏 WAL 中的已提交数据。

部署继续使用既有测试 CA；客户端需要信任该 CA，不能跳过 HTTPS 校验。
现有服务器证书有效期截止 2026-11-09 06:29:13 UTC。

## 客户端接入范围

浏览器已接入真实注册、登录、退出和同账号设备目录。在
`chrome://cross-transfer-test/` 注册账号，在另一台设备登录同一账号即可
查看彼此。`chrome://xenon-login/` 使用相同的原生账号服务。

每个正常 Profile 启动时自动用服务端 token 调用 `/auth/me` 验证会话；
不依赖账号页面是否打开。验证成功后登记本机设备，每 30 秒心跳并刷新
目录。token 和到期时间直接保存在 Profile 本地偏好中，不保存密码；
设备私钥和设备 ID 按账号分别用系统加密保存，切换账号后再登录会复用原身份。
最多保留 64 个账号身份，达到上限时拒绝创建新身份，不淘汰已有身份。
退出立即清除本地会话，随后通过一次请求
同时撤销服务端设备登记和当前 token。旧的本地登录布尔值不能授权进入登录 gate。

同账号设备通过通用消息转发发送邀请。文本内容和文件都作为独立载荷传输，
邀请仅含元数据；接收端确认后，两端交换 WebRTC SDP 和 ICE 候选，优先尝试
设备间 DTLS 直连。直连失败由两端协商回退到 HTTPS 中继，使用角色不同的
临时凭据。账号 token 不能直接作为中继角色 token 使用。

在设备列表选择在线设备的“发送”，输入文本或通过原生文件选择器选择
文件；另一端在同一页面接受或拒绝。正常发送无需填写对端地址、证书或
令牌。接收的文本可复制，文件显示进度和保存位置。任务随 Profile 存活，
关闭或刷新页面不会终止传输；退出账号或结束 Profile 会取消本地任务。

任务 ID 由发送浏览器生成。等待接收、接受、运行、完成、失败、拒绝、取消
以及近期历史全部由两端浏览器按账号和设备保存。邀请携带发送端创建时的
截止时间，重试或延迟送达不会延长；过期端发送取消通知，过期邀请不能再接受。
已经运行的传输按本地 I/O 和阶段超时控制，不会因接受已超过 30 分钟被取消。
服务端不提供任务列表，
不处理任务状态，不接收完成上报。接受、拒绝和取消只是两端解释的普通消息。
两端通过传输协议确认成功：接收端校验并保存载荷，发送完成回执，发送端
再确认收到回执。已保存的文件或文本不会因为最终确认连接断开而丢失；
同任务检查点可用于恢复确认，避免重传内容。

首次发现、邀请和新 ICE 协商仍需要服务端转发；已有直连的数据和完成回执
直接在设备间传送。中继通道的关闭只是资源释放，失败时由 TTL 清理，不能
决定任务成败。传输结果未能确认时，浏览器保留本地提示，不用服务端状态
猜测成功，也不撤销对端可能已经保存的载荷。

## 通用消息与中继通道接口

接口均需 `Authorization: Bearer <account token>`，设备必须属于该账号。
服务端按账号会话和设备目录验证权限，不宣称具备设备私钥持有证明。

| 方法 | 相对 `/api/v1` 的路径 | 用途 |
| --- | --- | --- |
| GET | `/transport-config` | 公告静态直连阈值和 STUN 配置 |
| POST | `/signals` | 投递不解释内容的设备消息 |
| GET | `/devices/{id}/signals` | 读取最多 64 条待接收消息，不删除 |
| POST | `/devices/{id}/signals/ack` | 确认已接收的消息 ID 并删除邮箱副本 |
| POST | `/channels/{id}/exchange` | 交换两端各自发布的有版本消息快照 |
| POST | `/channels/{id}/relay` | 按需取得本设备角色的临时中继凭据 |
| POST | `/channels/{id}/release` | 释放本角色的通道资源，双方释放后回收 |
| DELETE | `/channels/{id}` | 显式清理整个临时通道 |

`/transport-config` 返回 `directMinBytes` 和 `iceServers: [{urls: [...]}]`。
是否使用 STUN 由浏览器本地开关控制；阈值只由浏览器解释。

消息请求为：

```json
{"id":"<32 hex message ID>","fromDeviceId":"<32 hex>","toDeviceId":"<32 hex>","payload":{}}
```

`payload` 是编码后最多 16 KiB、嵌套最多 64 层的 JSON 对象。服务端只校验结构和大小，不解释任务
ID、消息类型、文件名或任何业务字段。成功返回 `{"signal": {...}}`，信封附加
`fromName`、`toName`、`createdAt` 和 `expiresAt`。相同消息 ID 与相同内容重试
幂等，不同内容返回 409。GET 返回 `{"signals": [...]}`；接收浏览器先持久化
本地处理结果，再以 `{"messageIds": [...]}` 确认，最多 64 个 ID。

消息有效期 5 分钟，每设备最多 128 条，全局最多 4096 条。容量不足拒绝
新消息，不静默丢弃。ACK 后保留短期去重信息，避免发布响应丢失造成重复
投递。消息与交换快照按编码字节缓存，共享 32 MiB 上限，避免 JSON 对象树
占用无限增长。消息、去重记录及通道都只驻留内存，不是可查询的传输历史。

通道 exchange 请求为：

```json
{"deviceId":"<32 hex>","peerDeviceId":"<32 hex>","role":"sender","sequence":0,"payload":{}}
```

`role` 为 `sender` 或 `receiver`。首次访问固定账号、双方设备及方向，每侧
绑定首次访问的登录会话。`sequence` 是非负递增整数；相同序号相同内容
幂等，相同序号不同内容冲突，迟到的较小序号不能覆盖较新快照。
`payload` 最多 112 KiB，服务端原样缓存，不解析或合并 SDP、ICE、路由或状态。
响应为 `{"peer": null, "expiresAt": 0}`，或把 peer 替换为对端的
`{"sequence": 0, "payload": {...}}`。浏览器验证对端内容并自行协调传输路径，
任一端回退后不被迟到的直连就绪消息反转。

relay 请求包含 `deviceId`、`peerDeviceId` 和 `role`，不包含任务状态。
通道可按需创建，无须服务端先收到或解释“接受”。返回
`{"relay":{"roomId":"own_<id>","connectionId":"<id>","bearerToken":"...","expiresAt":0}}`。
凭据仅授予绑定设备的相应角色；不能访问其他账号或其他设备的通道。
通道有效期 30 分钟，实际需要中继时才分配房间。浏览器在用完协商通道后
异步调用 release，携带与 relay 相同的三个字段；该操作不携带任务状态，
不影响本地成功判断。单侧释放仍保留另一侧的协商快照，双方释放后才回收
资源；缓存连接复用不创建或释放通道。已有资源重复释放幂等，资源消失后
返回 404，客户端无需重建。DELETE 只允许绑定会话，
已有中继连接最多有 15 秒排空最终帧；会话过期、淘汰、退出及设备撤销会
撤销相关资源，通道寿命不会超过参与会话的剩余有效期。

SQLite 自动迁移到 schema v4，删除 transfers 表，只保留 accounts、sessions
和 devices。没有旧传输接口或旧客户端兼容分支。服务重启只清空临时消息
和通道资源，不修改浏览器任务列表或伪造任务结果；已有设备直连不依赖
这些资源继续传送。

浏览器已记录的近期历史在重启后恢复；待接受的接收邀请可以保留。
源文件关联、接收授权和连接不跨进程恢复，未记录完成的任务在本地标记
中断并通知对端，不自动续传或扫描检查点恢复完成结果。
“高级传输诊断”中的手工断点续传与账号模式独立。
部署前应备份数据库；回滚旧程序时需同时恢复相应数据库备份。
WebRTC DTLS 在设备间终止；回退中继仍为受信任的 TLS 终止服务。

浏览器验证命令（先构建 `chrome xenon_account_unittests`）：

```sh
out/Release_64/xenon_account_unittests.exe --gtest_filter=XenonAccountServiceTest.*
out/Release_64/xenon_account_unittests.exe --gtest_filter=XenonTransferServiceTest.*
node cross_transfer/tests/account_webui_smoke_test.mjs
node cross_transfer/tests/account_browser_integration.mjs
node cross_transfer/tests/account_browser_integration.mjs --with-transfers
node cross_transfer/tests/account_browser_integration.mjs --with-file
```

设置 `CHROME_BINARY` 指向本次编译的浏览器。真实联调脚本连接实际账号
服务，创建两个临时 Profile 和一个随机测试账号，验证注册、同账号设备、
重启自动登录、明文 token 存储和退出。`--with-transfers` 额外验证 Unicode
文本接收、拒绝和取消；`--with-file` 包含这些检查，并打开可见的原生文件
选择器。脚本输出 `WAIT_FILE_SELECTION` 时，选择
`out/own_transfer_file_selection.json` 中记录的测试文件；随后自动验证
1 MiB 文件传输、保存目录和 SHA-256。临时 Profile、源文件和接收文件
均自动清理；测试账号标识
保存在 `out/account_browser_integration_result.json`，测试后由服务端管理员
删除该账号。结果文件不包含密码或 token。
