# Node URL 兼容边界

主进程 `URL.canParse(input, base)` 使用 Chromium 原生 URL 解析器判断
有效性。提供基址时先验证基址，再解析输入；非法基址不会因为输入是绝对
URL 而被忽略。全局 `URL` 和 `require('url').URL` 共用同一个实现。

GURL 解析完成后，对 WHATWG 特殊协议（HTTP、HTTPS、WS、WSS、FTP、file）
已规范化的主机名执行额外校验：使用 Chromium 的二进制 URL 解码和现成的
禁止主机字符检查，拒绝其浏览器兼容设置仍可能接受的空格主机名。IDNA、
IP 地址、端口及完整 URL 解析继续由 GURL 处理，不修改 Chromium 的全局
FeatureList。此校验也适用于基址；非特殊协议的 opaque host 不解码，所以
`custom://bad%20host/` 保持合法。路径和 opaque path 中的空格不受影响。

JavaScript 边界按 Node 语义处理参数：完全缺少输入时抛出
`ERR_MISSING_ARGS`；显式 `undefined` / `null` 等值先转成字符串；只有
`undefined` 基址表示不提供基址。输入和基址依次且仅转换一次，孤立代理项
转成替换字符。`Symbol` 及用户转换逻辑抛出的异常继续传播，不转成 `false`。

此改动只补充静态有效性查询。既有主进程 `URL` 构造器及属性解析仍是
有限的 JavaScript 适配，不能将其行为视为完整 WHATWG URL 实现。
`canParse` 不使用该宽松构造器推断有效性，也不使用模块名白名单或独立正则
模拟 URL 解析。

JavaScript 回归覆盖参数语义及模块路径分类，并以 Node parser 作为边界
测试依赖；真实 Chromium parser 的有效性与 Node 差异由原生回归和产品
对照探针单独验证，二者不混同。

## 已验证结果

历史 Windows product22 对照 Node v24.19.0：129 组中 128 组一致。普通模块名、
`node:` / `file:` / `data:`、相对路径与基址、非法端口及 IPv6、参数转换和
异常传播均在本次覆盖范围内通过。全局与 `require('url')` 的 URL 导出一致。
JavaScript 专项及既有 URL 回归共 11 项通过；真实 bootstrap 原生回归覆盖
13 组有效性输入。

该次差异：`URL.canParse('http://bad host/')` 在 Chromium parser 返回
`true`，Node v24.19.0 返回 `false`。product28 增加上述通用原生主机校验，
已验证修复空格及其百分号编码、IDNA 映射后的同类问题，没有针对应用或
具体 URL 加例外。

新增真实 bootstrap 回归包含 1,004 组主机及基址向量，期望值已在 Node
v24.19.0 核对：包括六种特殊协议、原始和编码的控制字符、DEL、百分号、
空格、IPv6、file 主机，以及 opaque host 的不同规则。product28 的两项
原生 URL 回归均通过，包含此前的 13 组有效性输入和新增 1,004 组向量。

Windows product28 的扩展产品探针与 Node v24.19.0 对照为 **2,570 / 2,572**
一致。非法主机修复通过，全局与 `require('url').URL` 导出一致。剩余两项为
`file://left#right/`、`file://left?right/`：Node 返回 `true`，当前宿主返回
`false`。这是既有 GURL file authority 解析边界：
`url/url_parse_file.cc` 的 `DoParseUnc` 使用 `FindNextSlash` 划分主机，
在首个斜杠前把 `#` / `?` 当作主机内容，GURL 因而在新增主机校验之前就
判定无效。此轮未改动 Chromium file URL 解析，也不宣称完整 WHATWG 等价。

本轮严格全一致结果仍为 `false`，证据是外置审计目录中的
`url-can-parse-probe-product28-01/verification.json`。探针使用独立 fixture
和 profile，宿主退出码 0，端口 9268 已释放，无残留进程及 crash dump。

历史产品证据为外置审计目录中的
`url-can-parse-probe-product22-01/verification.json`；严格全一致结果仍标记为
失败以保留此差异。宿主正常退出，端口释放，无残留进程及 crash dump。
