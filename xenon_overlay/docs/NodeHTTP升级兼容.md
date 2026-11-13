# Node HTTP 协议升级

## 问题与边界

Node 的 `http.request()` 既可以获取普通 HTTP 响应，也可以通过
`upgrade` 事件交付协议升级后的双向 Socket。Chromium 的 URLLoader
只提供 HTTP 请求/响应，明确禁止调用者设置 `Upgrade` 请求头，不能承担
Node 的 Socket 所有权转移。因此普通请求成功不能证明 `ws` 等原版依赖可用。

`common/http_upgrade.js` 在主进程、Utility 和渲染进程的 Node HTTP
模块中共享。仅显式设置 `Upgrade` 和 `Connection: upgrade` 的请求
使用现有原生 `net.Socket`。普通 Node HTTP/fetch 继续走原有 URLLoader，
禁用浏览器 Cookie 存储和 HTTP 缓存的规则不变。升级请求使用调用者的
请求头和独立 Socket，不接入浏览器 Cookie、缓存或 CORS 权限。

这是通用 HTTP/1.1 客户端能力，没有 `ws` 模块替换、业务名称判断、
固定端口或应用资源修改。它不扩展 HTTP 服务端的 upgrade 支持。

## 可观察行为

- 解析分片响应头和 1xx 信息响应。只有 101 同时带有效的 `Upgrade`
  和 `Connection: upgrade` 响应头，才触发
  `upgrade(incomingMessage, socket, head)`；其余响应走 `response`。
- `head` 保留响应头后已经收到的二进制数据，Socket 保持可读写。
  升级后 HTTP 解析器卸载监听器，Socket 归调用者持有；没有 upgrade
  监听器时关闭 Socket，避免遗留连接。
- 非升级响应支持 Content-Length、chunked（含 trailers）及 EOF 定界。
  暂停、恢复和读取压力沿用共享 IncomingMessage。
- 支持公共 `createConnection`、握手超时、AbortSignal 和 destroy。
  超时只通知调用者；调用者决定是否销毁请求。解析失败、取消和关闭
  不伪造成功响应，不重复发出错误或关闭事件。
- URL 用户名/密码生成 Basic Authorization；`options.auth` 覆盖 URL
  凭据，显式 Authorization 头优先。请求头名称和值经过标准校验。
- `maxHeaderSize: 0` 采用默认 16 KiB 上限。`req.maxHeadersCount`
  与 Node 一样控制公开 headers 数量，rawHeaders 也匹配 Node 原生解析器
  分批收集时的限制；内部保留完整握手和定界解析，不使用被截断的公开
  headers 判断升级。构造选项中的同名字段不改变其行为。

TLS Socket 尚未实现，因此 HTTPS/WSS 升级返回 `ERR_NOT_SUPPORTED`，
不会降级为明文。自定义 Agent、chunked 请求上传、宽松 HTTP 解析等
尚未支持的配置也明确报错。是否能在 Worker 中执行取决于该执行环境
是否提供原生 net 传输，不能仅凭模块可 require 就宣称可用。

## 验证

`xenon_http_upgrade_unittest.cjs` 对主/渲染两个入口执行真实回环 TCP
测试，覆盖逐字节拆包、二进制 head、继续读写、非 101 响应、读取暂停、
解析错误、超时、取消和 TLS 拒绝。101 判断、Basic auth、请求头数量
和参数错误码另与本机 Node HTTP 实际执行结果逐项比较。

原版第三方库验证使用从 Motrix 原发行 ASAR 只读提取的 `ws 8.22.2`，
在适配 HTTP 模块加 Node 原生 TCP 的独立对照中验证压缩文本、二进制、
ping 和正常关闭。

Windows 真实产物另通过主进程、Utility、渲染进程各 8 项、共 24 项
验证：压缩文本/二进制/ping/正常关闭、分片 UTF-8、显式 Cookie、URL
凭据、403 响应、无效 WebSocket accept、通用升级双向数据和 TLS 明确
拒绝。测试库逐文件核对原 ASAR 的 SHA-256，放入独立测试应用自身的
模块根目录；原应用没有改动。退出码为 0，无残留测试子进程或转储。
应用本身的文件提交、存储和退出结果需要在原发行包中另行验证。
