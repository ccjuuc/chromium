# Windows Crashpad 上传接入

浏览器和子进程复用 Chromium 的独立 Crashpad handler。报告先写入
`<User Data>\Crashpad` 数据库，再由 handler 上传。本功能不扫描 WER 的
`%LOCALAPPDATA%\CrashDumps`。

## 配置地址与开关

发布构建可以在 `args.gn` 中配置：

```gn
xenon_crash_upload_url = "https://updates.example.com/api/v1/crashes"
xenon_crash_upload_enabled = true
xenon_crash_upload_format = "multipart"  # 或 "binary"
```

上述地址仅为示例，需要替换为实际服务。默认 URL 为空，沿用上游行为；
非 Google 官方品牌构建的上游地址为空，不会启动上传线程。
`xenon_crash_upload_enabled` 默认 false，此时沿用原有用户同意状态。

调试或部署时，可在启动浏览器的进程环境中覆盖：

```powershell
$env:CHROME_CRASHPAD_SERVER_URL = 'http://127.0.0.1:9001/api/v1/crashes'
$env:XENON_CRASH_UPLOAD_ENABLED = '1'
$env:XENON_CRASH_UPLOAD_FORMAT = 'multipart' # 或 'binary'
# 然后启动实际的产品 exe，确保没有复用已经运行的浏览器实例。
```

`XENON_CRASH_UPLOAD_ENABLED=0` 明确关闭自动上传，`1` 明确开启；未设置或
其他值沿用构建默认和原有同意状态。企业策略仍优先，自动化无人值守环境和
采样门槛仍生效。环境配置须在浏览器启动前设置，修改后重启生效。
显式崩溃上传配置也应用于统计服务启动及后续许可更新，避免启动后被 UMA 的
上传状态覆盖；未配置时继续遵循原有统计同意状态。

Windows 的 `chrome://crashes` 会显示 Crashpad 数据库中的实际自动上传开关，
包含企业策略、采样及无人值守限制；handler 未成功启动或最终上传地址为空时显示
停用。状态读取在后台执行，不改变 UMA 或 JavaScript 错误报告的同意状态。
页面显示启用表示已配置自动上传，不表示服务器当前可达或报告已经上传成功。

## 服务端协议

两种格式都使用 HTTP POST。`XENON_CRASH_UPLOAD_FORMAT` 支持 `multipart` 和
`binary`，优先于构建配置；未设置或其他值使用构建默认。handler 对应的二进制
模式命令行参数为 `--upload-raw`。

默认 `multipart` 使用 `multipart/form-data`：

- `upload_file_minidump`：dump 文件，文件名为 `<UUID>.dmp`。
- `prod`、`ver`、`channel`、`ptype` 等：从 dump 提取的注解，具体字段取决于报告。
- 已配置的附件：各自作为文件表单字段上传。

默认开启请求体 gzip，服务端必须根据 `Content-Encoding: gzip` 解压后解析表单。

`binary` 模式使用 `Content-Type: application/octet-stream`，请求体直接为原始
`.dmp` 字节，不压缩，也不包含独立表单字段和附件。注解仍在 dump 内部，服务端
可以自行解析。两种格式都通过流式读取文件构造请求体。

URL 还可能携带 `product`、`version`、`guid` 查询参数。
成功返回 HTTP 200 和纯文本报告 ID；响应正文会直接作为远端报告 ID 保存，
不解析 JSON。当前 WinHTTP 实现接受 200–203，不应使用 204。

当前没有额外的服务端鉴权协议；如果服务需要 token 或签名，需要明确协议后接入。
浏览器中的 cookie、登录状态和页面代理设置不会自动成为 handler 的上传凭据。

## 使用 Xenon Update 接收报告

仓库内的 [xenon_update_tool](../tools/updater/xenon_update_tool/README.md#接收-crashpad-崩溃报告)
提供兼容接口 `POST /api/v1/crashes`。构建并按其 README 执行 `init`、`serve` 后，
即可使用上文的 `http://127.0.0.1:9001/api/v1/crashes` 在本机接收报告。
服务配置中的相关字段如下：

```json
"crashes": {
  "enabled": true,
  "directory": "crashes",
  "max_upload_bytes": 67108864
}
```

这些是默认值；旧配置缺少 `crashes` 时同样默认启用。相对目录以服务配置文件所在目录为基准，
修改后需重启服务。服务开启接收与浏览器开启上传是两个独立开关。

接口接收二进制或 multipart，两种请求均支持 gzip（当前客户端二进制模式发送未压缩数据）。
上传大小默认最多 64 MiB，收到的压缩请求体和解压后的请求体分别受此限制；multipart 的字段和
附件也占用额度。dump 须至少 32 字节且具有 `MDMP` 签名；每个文本注解最多 64 KiB。
每份报告最多保存 4096 个文本注解和 128 个附件，超出数量的辅助字段会忽略，
`metadata.json` 中的 `dropped_fields` 记录忽略数量，dump 仍会保存。
非完整 UTF-8 注解在 `annotations` 中容错显示，并在 `annotations_base64` 保存原始字节。
成功返回 HTTP 200 和纯文本报告 ID。

每份报告保存到 `<directory>/<32 位十六进制报告 ID>/`：dump 为 `minidump.dmp`，
注解和文件信息为 `metadata.json`，附件按 `attachment-NNN.bin` 命名。
附件的客户端原始文件名只记录在元数据中。服务不提供公开报告下载，也不自动清理报告。
Unix 系统的新报告目录和文件分别以 `0700` 和 `0600` 权限创建，umask 只能进一步收紧权限。
最多同时接收两份报告，忙时返回 503；接收超时为 120 秒，客户端失败后按下文规则补传。

PowerShell 手动验证示例（使用已有 dump）：

```powershell
curl.exe --fail-with-body -H 'Content-Type: application/octet-stream' --data-binary '@C:/dumps/example.dmp' 'http://127.0.0.1:9001/api/v1/crashes'
curl.exe --fail-with-body -F 'upload_file_minidump=@C:/dumps/example.dmp;type=application/octet-stream' -F 'prod=Xenon' -F 'ver=1.0.0.1' 'http://127.0.0.1:9001/api/v1/crashes'
```

服务固定监听回环地址，远程部署通过 HTTPS 反代转发 `/api/v1/crashes`，上游 `Host` 必须设置为
监听地址，例如 `127.0.0.1:9001`；具体配置见工具 README。接口无独立鉴权，拒绝带 `Origin`
头的请求，因此 multipart 是供 Crashpad 等原生客户端使用的 HTTP 格式，不是网页提交界面。

## 失败补传

Windows 上传失败后释放 `UploadReport`，数据库自动记录本次失败并恢复 pending。
下次扫描使用数据库中的尝试次数和时间进行退避，不依赖 handler 内存：

- 单报告自动上传最多尝试 5 次；达到上限后，下次扫描将其结束为上传失败。
- 第 1–4 次失败后的等待时间分别为 15、30、60、120 分钟。
- 保留原有全局每小时一次尝试的限流，因此实际等待可能更长。
- 全局限流只推迟报告，Windows 不再因此直接结束报告。
- handler 启动时扫描 pending，此后约每 15 分钟扫描；重启不会重置退避。
- 用户手动请求上传不受自动补传次数和退避限制。每次手动请求尝试一次；
  失败后结束该请求，用户仍可再次手动提交，后台扫描不会无限重试该请求。
- 原有数据库清理策略仍生效，pending 并不意味着永久保留。
- 关闭自动上传沿用上游行为：报告可能被结束为未上传，重新开启不会自动复活
  已结束的历史报告。

## 验证

```powershell
autoninja -C out/Release_64 chrome/chrome_elf:crash third_party/crashpad/crashpad:crashpad_tests
& out/Release_64/crashpad_tests.exe '--gtest_filter=CrashReportUploadRateLimitTest.*:CrashReportUploadThreadTest.*'
```

测试覆盖退避边界、最大尝试次数、时钟回拨，以及实际数据库在网络失败、限流和
重新打开后的 pending 状态与尝试次数；本地 HTTP 服务测试覆盖两种请求格式、
二进制原始字节和成功响应报告 ID。接入实际服务后，还需要验证实际服务的
gzip 解析和鉴权。完整产品构建需要重新链接 `chrome`，仅构建静态库不会
更新现有浏览器二进制。

### 真实浏览器回归测试

[test_crash_upload.mjs](../tools/test_crash_upload.mjs) 在 Windows 上用 Node.js 22+
启动已构建的浏览器和 Xenon Update 接收服务，依次执行以下 6 组真实崩溃上传测试：

| 触发地址 | 崩溃进程 | 上传格式 |
| --- | --- | --- |
| `chrome://crash` | renderer | multipart、binary |
| `chrome://inducebrowsercrashforrealz/` | browser | multipart、binary |
| `chrome://gpucrash` | gpu-process | multipart、binary |

先重新链接包含上传改动的完整浏览器，并构建 `xenon-update.exe`。如需核对客户端
数据库的上传状态和远端报告 ID，同时构建可选工具：

```powershell
autoninja -C out/Release_64 chrome third_party/crashpad/crashpad/tools:crashpad_database_util
cargo build --release --locked --manifest-path xenon_overlay/tools/updater/xenon_update_tool/Cargo.toml
```

从源码根目录执行以下示例；`--browser` 应指向实际产品 EXE，所有路径参数均须为绝对路径，
输出目录每次使用新名称：

```powershell
$crashTestRoot = (Get-Location).Path
$crashTestOutput = Join-Path $crashTestRoot ('out/crash-upload-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
node xenon_overlay/tools/test_crash_upload.mjs `
  --browser (Join-Path $crashTestRoot 'out/Release_64/xlb153.exe') `
  --server (Join-Path $crashTestRoot 'xenon_overlay/tools/updater/xenon_update_tool/target/release/xenon-update.exe') `
  --output $crashTestOutput `
  --database-tool (Join-Path $crashTestRoot 'out/Release_64/crashpad_database_util.exe')
```

`--database-tool` 可省略。可用 `--scenario renderer|browser|gpu` 和
`--format multipart|binary` 只运行指定组合；默认仍运行全部 6 组。
`--window-mode headless` 可用于对照测试，默认使用正常窗口；该选项不保证阻止
系统输入法或第三方 DLL 加载。
脚本自动选择空闲端口，在输出目录下初始化本地服务配置，
并为每组测试创建独立 profile，通过 `BREAKPAD_DUMP_LOCATION` 指定独立 Crashpad 数据库。
上传 URL、许可和格式只注入测试浏览器的进程环境。独立数据库避免前一组上传触发的
全局限流干扰下一组。测试固定进入 `MetricsAndCrashSampling/InReportingSample`，
避免新 profile 随机被排除在崩溃采样之外；通过本次实例的 DevTools 端口，
以 `address_bar` 导航访问崩溃地址（browser/GPU 调试地址要求显式导航）。

每组测试先记录触发前已收到的报告 ID 和本次浏览器进程树，再等待服务端报告最多 60 秒。
两种格式均检查 `MDMP` 签名、上传格式，以及 dump 内标准 MiscInfo 的 PID 和 Crashpad
注解中的 `ptype`；只有新报告且 PID 属于触发前对应类型的测试进程才算成功。
multipart 还要求表单 `ptype` 一致。binary 无独立表单字段，直接核对 dump 中的身份。
提供数据库工具时，脚本还等待客户端记录与接收端一致的远端报告 ID。

输出目录保留 `results.json`、各组 profile 和 Crashpad 数据库、浏览器日志，
以及 `service/` 下的服务配置、日志和接收报告；提供数据库工具时各组还保存 `database.log`。
脚本结束时清理本次启动并跟踪的进程，保留所有测试文件供复查。清理会重新枚举存活后代，
并在同一个 Windows 进程句柄上核对创建时间后再终止，避免 PID 复用误杀其他进程。
清理失败会重试；最终仍有清理错误、日志写入错误或任意测试失败时返回非零退出码，
详情写入 `results.json` 的 `cleanup`、`log_errors` 和各组结果。
这组测试覆盖真实捕获及成功上传；失败退避和重启后的补传状态仍由上方的数据库测试覆盖。

辅助逻辑可独立回归，无需启动浏览器或触发崩溃：

```powershell
node --test xenon_overlay/tools/test_crash_upload_test.mjs
```

该测试使用合成 minidump 验证解析边界、报告基线、类型/PID 筛选，并验证日志错误、清理
重试和 PID 复用保护；Windows 上还会启动并清理一个仅等待的 Node 子进程，验证句柄清理。

### 2026-10-25 本机验证与已知捕获问题

使用 `out/Release_64/xlb153.exe` 和本地 Xenon Update 接收服务进行正常窗口测试：

| 场景 | multipart | binary |
| --- | --- | --- |
| renderer (`chrome://crash`) | 通过 | 通过 |
| browser (`chrome://inducebrowsercrashforrealz/`) | 未生成 Crashpad 报告 | 未生成 Crashpad 报告 |
| GPU (`chrome://gpucrash`) | 通过 | 通过 |

4 组成功场景均核对了服务端 dump 与客户端 `Uploaded: true`、远端报告 ID；
客户端配置相关的 9 项单元测试通过。真实窗口测试结果保存在
`out/crash-upload-e2e-20261025-detached/results.json`。

本机的百度输入法 6.1.13.7 在 Crashpad 初始化后，通过
`SetUnhandledExceptionFilter` 反复将进程异常过滤器替换为
`baiducnTSF.dll+0xff20`。调试跟踪确认注册顺序为 CRT → Crashpad → 百度输入法。
主进程测试最终生成了 `%LOCALAPPDATA%\CrashDumps\xlb153.exe.<pid>.dmp`，
异常为 `EXCEPTION_BREAKPOINT` (`0x80000003`)，栈顶是 `content::HandleDebugURL`。
相关证据保存在 `out/crash-upload-debug/lldb-consent-regression-20261025/`。

这是捕获阶段的问题：报告未进入 Crashpad 数据库，自动上传和失败补传均无法处理它。
当前 `chrome_wer.dll` 只将 `0xC0000409` 转回 Crashpad，未为此次断点异常兜底。
本轮没有改动输入法、WER 系统配置，也没有增加对 `CrashDumps` 目录的收集；
第三方覆盖过滤器后的主进程捕获仍是待解决项，不能将本次结果记为 6/6 通过。

### 2026-10-25 审查修复后的回归

完整 Windows 浏览器及测试目标重新构建成功；客户端许可和实际上传状态共 17 项测试、
Crashpad 上传与退避共 10 项测试通过。后者覆盖重启后手动上传绕过退避及自动次数上限、
手动失败后消费请求且允许再次提交。接收端 9 项 Rust 测试、9 项 HTTP 集成用例、
测试脚本 9 项独立测试均通过，Clippy 全目标无警告。

两个隔离 profile 在 UMA 关闭时分别设置独立上传开关为 1 和 0，真实
`chrome://crashes` 页面及刷新后的状态均与开关一致。Renderer 和 GPU 的
multipart、binary 四组真实上传重新通过，并核对 dump 内 PID/ptype、客户端
上传记录及远端报告 ID；测试进程均已清理。结果分别保存在
`out/crash-upload-review-renderer-20261025/results.json`、
`out/crash-upload-review-gpu-20261025/results.json`，页面状态证据位于
`out/crash-upload-debug/consent-ui-regression-20261025/run-WBODh7/results.json`。

本次未重跑主进程崩溃，其已知第三方异常过滤器覆盖问题仍待解决。Unix 权限测试
已加入，Mac 模态及 Linux 阴影已恢复对应实现；这些平台的运行效果尚未实机验证。
