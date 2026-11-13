# Node Socket 流兼容

main 与 renderer 的 `net` 使用 `common/net.js` 的同一实现，入口仅提供平台、
IPC 发送函数和连接标识前缀。`net.Socket` 直接继承框架已使用的 upstream
`readable-stream` Duplex，与 `require('stream').Duplex` 保持类型身份一致。
没有另一套 readable/writable 长度、结束状态或读取队列。

`push()` 和 `_read()` 将读取背压接到真实 `uv_read_stop` / `uv_read_start`；
`_write()` 在收到 `uv_write` 完成通知后才完成，`_final()` 等待真实
`uv_shutdown` 完成。因而 `write()` 返回值、`drain`、`cork/uncork`、
`read/unshift` 和结束回调使用标准流状态机。进程内管道配对也遵循同一
Duplex 缓冲与背压；只有原生 bind 已成功，才允许使用本地配对。

`destroy()` 释放未完成写入和结束回调，忽略晚到完成通知。连接握手尚未
回复时，通过 client id、container 和 endpoint 三者共同定位并取消原生
句柄；不能仅凭另一页面的 id 取消连接。`setNoDelay()` 使用真实
`uv_tcp_nodelay`，支持连接前设置和连接后切换，消息仍经过所有者校验。
管道上的此 TCP 设置按 Node 约定无操作并返回 Socket。

连接、监听和关闭回调作为对应事件的 once 监听器注册，因此遵循注册顺序。
重连使用 upstream `_undestroy()`；与真实 Node 一致，保留监听器、解码器、
未消费的读取缓冲和显式暂停状态，同时解除前次结束/销毁标志。连接成功后
通过 `read(0)` 启动读取。`server.close()` 停止接受连接并等待现有连接排空，
包括已成功绑定后建立的本地管道配对；endpoint/容器销毁仍清理所有所属句柄。

TCP 与 Unix domain socket 使用平台的真实半关闭。Windows named pipe
遵循 libuv 的 EOF timer 行为，没有额外伪造 TCP 式半关闭。DNS、自定义
lookup、TCP keepAlive、TLS 和 Worker 独立 TCP 传输未在本次补齐；现有
`ref/unref` 尚未提供完整 Node 事件循环存活语义。

源码 net 回归 83 项通过，包含两个完整生产 bootstrap 的 18 项 Duplex
测试。真实 Node 基线的 5 项用例通过，覆盖 2 MiB 二进制及摘要校验、64 次
背压 drain、服务器关闭排空、管道、8 次连接取消以及 close 监听器内重连。
`net-reconnect-node-reference17.json` 另记录了真实 Node 对未消费缓冲与
暂停状态的处理。证据位于 `F:/electron-compat-audit/20261111/`。

product17 真实宿主主进程与 renderer 共 10 项通过，两个角色都完成上述
2 MiB 传输、重连、管道、连接取消和 close 回调内重连。宿主退出码为 0，
按 PID 与创建时间确认无所属残留进程，本次 profile 无新 dump。
证据为 `F:/electron-compat-audit/20261111/net-duplex-probe-product17-02/verification.json`。
首轮 probe 的重连前置条件误用了固定等待；第二轮改为 readable 事件确认
数据入缓冲，再执行暂停和重连，同一二进制即通过，没有为测试修改产品行为。

新增 4 项原生测试通过；两个旧测试按真实字节编码与 server.close 排空
契约更新后，完整 IPC 原生套件 591 项通过，见 `native-full17-02.json`。
测试调整没有改变产品二进制。未声明 macOS/Linux 实机验收通过。
