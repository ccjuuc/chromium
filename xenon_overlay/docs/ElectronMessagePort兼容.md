# Electron MessagePort 通信

## 公开接口

适配层提供 `MessageChannelMain`、`MessagePortMain`、
`webContents.postMessage(channel, message, transfer)` 和
`ipcRenderer.postMessage(channel, message, transfer)`。它们传递真实的 Blink
MessagePort 端点，因而可以与渲染页面的 DOM `MessageChannel`、
`MessagePort.postMessage`、`window.postMessage` 配合使用。

`MessagePortMain` 由 `MessageChannelMain` 或收到的转移列表创建，不能直接构造。
主进程端口使用 EventEmitter：

```js
const { MessageChannelMain } = require('electron');
const { port1, port2 } = new MessageChannelMain();
port1.on('message', ({ data, ports }) => {
  console.log(data, ports);
});
port1.start();
browserWindow.webContents.postMessage('connect', null, [port2]);
```

主进程必须调用 `start()` 才开始接收消息；添加监听器本身不会启动端口。
渲染侧 IPC 事件通过 `event.ports` 提供 DOM MessagePort，主进程 IPC 事件通过
`event.ports` 提供 MessagePortMain。每个端口的消息按顺序交付。

## 所有权与关闭

每个端口由 `blink::MessagePortDescriptor` 和 Mojo message pipe 持有。
转移时交出真实端点，不使用跨进程 JavaScript 对象表模拟通信。
已启动端口转移时，尚未交付的消息继续留在 pipe 中，由新所有者启动后接收。
原 MessagePortMain 包装对象在成功转移后失效。
宿主 bootstrap 的 `TakePort` 同样清理 JS keepalive 和 finalizer；旧包装对象的
`close()` 不会关闭交给新所有者的端点。`AdoptPort` 创建包装对象中途失败时，
真实端点和已启动包装对象均按关闭流程释放。

转移列表在序列化前校验，并在执行可能包含 getter 的序列化后再次校验。
克隆失败、重复端口、失效端口或目标文档不可用时，不会提前交出其他端口。
如果应用 getter 自行转移或关闭某端口，这个应用操作本身仍然生效。

`close()` 关闭本端真实句柄，并异步触发一次 `close` 事件。
对端在排空其已启动接收队列后观察关闭；没有排队消息的未启动端口也可以观察关闭。
未启动且无人引用的包装对象允许回收并关闭其句柄；已启动端口保留到关闭或转移。
应用容器退出时统一关闭全部端口。

## 数据与范围

支持 V8 structured clone 的常用值，包括循环对象图、Map、Set、Date、RegExp、
BigInt、ArrayBuffer 和 typed array，以及显式转移的 MessagePort。
DOM 端口传来的已转移 ArrayBuffer 由接收端重建，支持有界的可调整大小缓冲区。

Electron IPC 的转移列表只接受消息端口，ArrayBuffer 作为消息数据时按值复制。
函数、SharedArrayBuffer、WebAssembly.Module 和需要 Blink 专用资源管理的对象
（例如 Blob、DOM 元素、ImageBitmap、文件系统句柄和流）不能跨主进程边界传递。
本端能够检测时抛出 `DataCloneError`；从 DOM 端口收到无法解码的消息时，
MessagePortMain 触发 `messageerror`。这些值不会被静默替换成空对象。
主进程中由 `worker_threads.markAsUncloneable` 标记的普通对象和 Error，
无论位于消息顶层还是嵌套位置，也会在序列化时拒绝；失败保留转移端口的
所有权。内建 ArrayBuffer/typed array 等继续遵守 Node/V8 的标记例外。

单条消息的编码数据、缓冲区内容和端口开销合计最多 64 MiB；每条消息最多
1024 个端口或 1024 个转移缓冲区。应用主进程尚未启动时，宿主转发队列另受
64 MiB 和 1024 条消息的总量限制。当前不提供 Service Worker 的 IPC 端口转移。

## 实现与验证入口

- `common/ipc/xenon_message_port_codec.*`：主进程与渲染进程共用的编解码和资源校验。
- `chrome/browser/ipc/xenon_message_port.*`：主进程真实端点与 V8 绑定。
- `resources/ipc/main/message_ports.js`：Electron 包装对象、事件与生命周期。
- `MessagePortRuntimeTest.*`、`MessagePortCodecTest.*`：原生端点、队列、克隆失败与重入回归。
- `resources/ipc/main_message_ports_unittest.cjs`：公开 JS 接口行为回归。

上述实现没有依赖 TH、PLE 或特定第三方应用名称，也没有 Windows 原生窗口依赖。
跨平台运行结果仍应由各目标平台的构建和实际运行验证。

2026-11-12 的 Windows 实际跨进程探针通过 12 项检查，包括主进程与渲染进程
双向通信、复杂值复制、ArrayBuffer 转移、嵌套端口转移与回传、两个渲染进程
通过主进程创建的端口直接通信、导航后对端关闭、退出时清理，以及 8 类失败转移
不会提前交出其他端口。该轮应用正常退出，未发现新增 crash dump 或残留进程。
证据保存在本机独立验证目录
`F:/electron-compat-audit/20261111/message-port-probe-02/verification.json`；
此路径是测试产物位置，不是运行时依赖。
