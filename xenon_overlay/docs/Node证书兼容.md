# Node X509Certificate 兼容

`crypto.X509Certificate` 使用本地 BoringSSL 解析调用方提供的证书。它不读取系统信任库，不改变根证书配置，不建立网络连接。main、Worker、Utility 与启用 Node 的 renderer 共用同一类和原生实现。

## 支持范围

- 接受 PEM 字符串、Buffer、TypedArray、DataView；TypedArray 按字节范围读取。拒绝 ArrayBuffer 本身和其他类型，单次输入限制 16 MiB。
- 真实解析 PEM 或 DER，支持 PEM 前后的其他文本；输出规范化 PEM 和 DER。无效证书明确失败，不把输入字符串直接当作有效证书返回。
- 提供 subject、issuer、serialNumber、SHA-1/256/512 指纹、validFrom/validTo 及对应 Date、ca、raw、toString/toJSON。
- `checkHost`、`checkEmail`、`checkIP` 使用证书实际名字匹配，匹配时返回证书/请求名字，不匹配返回 undefined；`checkIssued` 校验证书签发者元数据。它们不验证信任链或当前有效期。
- 公开的 raw Buffer 和 Date 可被调用方修改，但不能修改内部用于匹配的证书。构造自独立字节的证书没有已关联签发者链，issuerCertificate 返回 undefined。

## 明确边界

BoringSSL 与 Node 所用 OpenSSL 的匹配选项不完全相同。`subject:'always'`、`multiLabelWildcards:true`、`singleLabelSubdomains:true` 以及以点开头的子域查询明确返回 `ERR_NOT_SUPPORTED`，不传递 BoringSSL 中值为零的兼容宏来假装支持。

普通整标签通配符、禁用通配符、`subject:'never'` 可用。默认允许部分标签通配符时，如果证书包含这种名字，返回 `ERR_NOT_SUPPORTED`；显式 `partialWildcards:false` 时保留标准整标签匹配。证书只有 IP SAN、没有 DNS SAN 时，保留 Node 对 subject/CN 的默认回退；通过专用证书副本处理，不修改原证书。

BoringSSL 只对 DNS 格式的 commonName 做回退；对实际参与回退的带空格、Unicode 等非 DNS 格式 CN，宿主明确返回 `ERR_NOT_SUPPORTED`，不把 Node 可以匹配的名字静默视为不匹配。`subject:'never'` 不使用 CN，不受此边界影响。

subjectAltName/infoAccess 的安全字符串表示、keyUsage、publicKey/KeyObject、checkPrivateKey、verify、toLegacyObject 尚未实现，对这些属性或方法的调用明确返回 `ERR_NOT_SUPPORTED`。不能把支持上述基础信息视为完整 Node crypto 实现。跨线程/IPC 的 X509Certificate 类型保留和重建也未实现；Node main/Worker/Utility 标记实例为不可克隆，使其 structuredClone/Worker 消息明确失败，避免静默变成空对象。Renderer 的 Blink 类型克隆不在本轮支持范围。

错误类型和参数校验遵循 Node API；底层无效证书解析统一报告 `ERR_CRYPTO_OPERATION_FAILED`，不伪造 OpenSSL 专属错误栈。

## 验证

JavaScript 差分测试使用 Node 的真实 X509Certificate，验证 PEM/DER、TypedArray 范围、元数据、编码、名字匹配、公开缓冲修改隔离及错误选项。原生测试使用 Chromium 已有证书夹具，覆盖真实 BoringSSL 解析、指纹/时间、签发者检查、IP-only SAN 的 CN 回退、通配符及明确拒绝的选项。

当前 product09 源码已接入主进程、Worker、Utility、渲染进程及 ServiceWorker 的原生绑定，JavaScript 全 bootstrap 接线测试已通过；仍待统一原生编译和产品复验。通过的 JavaScript 测试不能替代原生或第三方原包验收。未修改 VS Code 或其他应用原包。
