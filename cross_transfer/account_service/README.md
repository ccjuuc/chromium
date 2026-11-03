# 账号与同账号设备服务

中继通过同一个 HTTPS 进程、同一个 443 端口提供账号和设备 API。
启用方式是给 `relay/server.py` 添加 `--account-database`。数据使用 SQLite
持久化，无第三方 Python 运行依赖。

部署入口：`https://115.29.16.37/api/v1`。原有中继路径仍为
`/v1/rooms/...`，继续使用原有角色令牌。

## API

非空请求体为 UTF-8 JSON，编码后最多 128 KiB，以覆盖 16 KiB 文本的最坏
JSON 转义体积。受保护接口使用 `Authorization: Bearer <token>`。
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
当前会话和关联传输，并撤销临时中继凭据。设备尚未登记也可退出；
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
设备私钥另用系统加密保存。退出立即清除本地会话，随后通过一次请求
同时撤销服务端设备登记和当前 token。旧的本地登录布尔值不能授权进入登录 gate。

同账号设备通过下方邀请接口发送文本或文件。接收端确认文本后邀请立即
完成；确认文件后双方通过角色不同的临时中继凭据传输。账号 token 不能
直接作为中继角色 token 使用。现有手工传输仍保留在“高级传输诊断”中。

在设备列表选择在线设备的“发送”，输入文本或通过原生文件选择器选择
文件；另一端在同一页面接受或拒绝。正常发送自动取得连接参数和临时
凭据，无需填写对端地址、证书或令牌。接收的文本可直接复制，文件显示
进度和保存位置。邀请轮询及传输任务随 Profile 存活，关闭或刷新页面
不会终止账号传输；退出账号或结束 Profile 会取消本地任务。

申请中继凭据或文件传输失败后，取消本次邀请并提示用户重新发送，不自动
重连。文件完成和取消的状态通知也只发送一次；临时错误时保留结果，提示
点击传输列表的“刷新传输”手动重试通知，每次点击最多提交一次且不超过
邀请有效期。后台轮询只查询状态，不重新发送通知或重新传输文件。
接受请求的响应丢失时保留当前文件任务，直到新列表确认已接受、未接受
或已结束；期间不允许启动第二个文件任务，也不自动重发接受请求。

## 同账号传输接口

所有接口均需 `Authorization: Bearer <account token>`，设备必须属于该账号。
服务端目前按账号会话和设备目录验证权限，不宣称具备设备私钥持有证明。

| 方法 | 路径 | 用途 |
| --- | --- | --- |
| POST | `/api/v1/transfers` | 创建邀请，返回 `{"transfer": {...}}` |
| GET | `/api/v1/devices/{id}/transfers` | 本设备相关的最多 32 条邀请，活跃邀请优先 |
| POST | `/api/v1/transfers/{id}/accept` | 接收端确认；重复确认幂等 |
| POST | `/api/v1/transfers/{id}/reject` | 接收端拒绝 |
| POST | `/api/v1/transfers/{id}/cancel` | 任一参与设备取消 |
| POST | `/api/v1/transfers/{id}/complete` | 接收端确认文件已完整接收 |
| POST | `/api/v1/transfers/{id}/relay` | 获取当前设备角色的临时房间凭据 |

创建请求的六个字段都必须提供：

```json
{"senderDeviceId":"<32 hex>","receiverDeviceId":"<32 hex>","kind":"text","text":"你好","fileName":"","fileSize":0}
```

文件使用 `kind: "file"`、空 `text`、单个安全文件名及整数 `fileSize`。
文本最多 16 KiB UTF-8，文件最多 8 GiB，允许空文件。发起时双方必须在线。
其他 POST 操作请求均为 `{"deviceId":"<32 hex>"}`，动作返回完整 `transfer`；
relay 返回 `{"relay":{"roomId":"...","connectionId":"...","bearerToken":"...","expiresAt":0}}`。
邀请列表包含 id、senderDeviceId、receiverDeviceId、senderName、receiverName、
kind、text、fileName、fileSize、status、createdAt、expiresAt，不包含中继凭据。
列表包含多条文本邀请，JSON 转义会增加编码体积；浏览器允许最多
4 MiB JSON 响应。

状态为 pending、accepted、completed、rejected、cancelled、expired 或 failed。
待确认邀请有效 5 分钟，文件接受后有效 30 分钟。每账号最多 32 个活跃邀请，
保留有界的近期终态历史。动态中继房间全局最多 64 个，角色 token 为独立的
随机 256 位值，仅在进程内存中存在。房间只接受邀请指定的 connectionId。
取消、会话退出或设备撤销会立即撤销关联房间；文件完成保留最多 15 秒供
已有连接读取最终 ACK，同时禁止新连接和新数据，随后销毁房间。

SQLite 自动迁移到 schema v2。重启保留账号、会话、设备和待确认邀请，
把正在传文件的 accepted 邀请置为 failed，因为临时中继凭据不跨服务重启。
账号模式不支持跨浏览器进程续传：源文件关联、接收确认和中继租约不跨
Profile 重启保存。重启后取消失去本地任务的文件邀请（包括尚待接收的
发送邀请），提示发送端重新发送。页面刷新不影响存活的原生任务。
“高级传输诊断”中的手工断点续传与账号模式独立。
部署前应备份数据库；回滚旧 v1 程序时需同时恢复相应数据库备份。
中继仍为受信任的 TLS 终止服务，不提供端到端加密承诺。

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
