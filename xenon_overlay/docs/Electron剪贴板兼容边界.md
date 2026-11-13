# Electron 剪贴板兼容边界

`common/clipboard.js` 为 main 与 renderer 提供共享的 `ClipboardItem`、
`clipboard.read()`、`clipboard.write()` 和 `clipboard.has()`。原生读写通过
Chromium 的跨平台 `ui::Clipboard`，不是进程内缓存或模拟成功。

新增接口参考 Electron [v44.0.0 clipboard 文档](https://github.com/electron/electron/blob/v44.0.0/docs/api/clipboard.md)
及 [ClipboardItem 文档](https://github.com/electron/electron/blob/v44.0.0/docs/api/clipboard-item.md)。
它们返回 Promise；现有 `readText`、`writeText`、`readHTML`、`writeHTML`、
`clear` 仍保留旧版 Electron 的同步语义。因此 `await clipboard.readText()`
可用，但不保证 `clipboard.readText().then(...)`，也不宣称完整兼容 Electron 44。

## 支持格式

| MIME | 原生存储 |
| --- | --- |
| `text/plain` | 系统 Unicode 文本 |
| `text/html` | 系统 HTML；新接口读取真实 HTML fragment |
| `text/rtf` | 系统 RTF 字节 |
| `image/svg+xml` | 系统 SVG |
| `image/png` | PNG 解码为原生位图；读取返回真实 PNG 字节 |

`new ClipboardItem({...})` 保存 MIME 到 string、Blob 或 Promise 的映射。
`types` 返回该映射的键列表副本，`getType(mime)` 异步返回真实 Blob。
与 Electron 一样，构造时复制映射；Promise 和 Blob 内容在读取或写入时解析。
构造出的 bookmark payload 可作为结构化 `{title, url}` 读取，但本轮没有实现
bookmark 的系统剪贴板传输，写入时会明确拒绝。

`clipboard.read()` 返回一个 `ClipboardItem`，空剪贴板的 `types` 为空。
类型来自系统当时实际公布的标准与 Web 自定义格式；`getType()` 再读取当前
数据，不承诺整个剪贴板内容快照。平台公布的未支持 MIME 仍可被 `has()`
发现，但 `getType()` 会返回 `ERR_NOT_SUPPORTED`，不会编造空数据。
读取获得的 item 不能直接传回 `write()`，需要用其 Blob 构造新 item。

main 使用真实的内存 Blob（同时导出为 `buffer.Blob`），支持不可变字节、嵌套、
切片、UTF-8 编解码、`arrayBuffer()`、`bytes()` 和 `text()`；buffer 输入和读取
结果均隔离复制。`stream()` 尚未接入 Web Streams，会明确报错。renderer 使用
Blink 自带的 Blob，不替换页面的 Web API。字符串载荷经过 Blob 的 UTF-8 编码，
孤立代理字符转换为替换字符，不依赖 main 是否已提供原生 TextEncoder。

## 原子写入与错误

一次 `write(items)` 会先解析所有 Promise/Blob，再在原生侧验证全部格式、
Base64 载荷并解码 PNG。全部成功后才创建一个 `ScopedClipboardWriter`，
在一次提交中写入所有表示形式，所以 PNG 与 HTML 可以同时存在。
同一类型出现多次时，后面的值覆盖前面的值。空数组没有数据要提交，不会清空
原内容；显式清空使用 `clear()`。

任一 Promise 拒绝、类型错误、未知 MIME 或损坏的 PNG 都会让整次写入失败，
并保留此前的剪贴板内容。主机异常按原异常传播，不转换成成功结果。

本轮只支持系统 copy/paste 剪贴板。Linux selection、macOS find pasteboard、
bookmark、原生文件列表、JPEG 编码和任意原生/Web 自定义格式的传输尚未实现。
显式请求其他剪贴板 buffer 或写入上述格式会返回 `ERR_NOT_SUPPORTED`。
旧版 `clipboard.write({text, html, image})` 对象签名也不在本轮新增支持范围内。

## 验证

JS 回归检查真实 Blob、Promise、模块导出、跨进程调用参数和失败传播。
原生回归使用 `ui::TestClipboard` 与真实 PNG 编解码，检查多格式同时存在、
像素读回、空内容及无效批次不覆盖旧内容；不会写入用户的操作系统剪贴板。

```powershell
node --test xenon_overlay/resources/ipc/xenon_clipboard_unittest.cjs
```
