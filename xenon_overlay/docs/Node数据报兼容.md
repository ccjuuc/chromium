# Node 数据报

主进程、独立 Utility 和 Node Worker 的 `require('dgram')`、`require('node:dgram')`
及对应 ESM 导入使用同一个模块对象。Socket 通过当前运行实例的 libuv loop
操作真实 UDP 套接字，不用 TCP、HTTP 或进程内消息模拟 UDP。

已实现 IPv4/IPv6、显式及 send 时自动 bind、hostname 解析、自定义 lookup、
二进制和空报文、消息来源信息、send 完成回调、connect/disconnect、address/
remoteAddress、close、ref/unref、组播成员与源指定成员、组播接口/TTL/loopback、
广播、普通 TTL、收发缓冲区大小和发送队列查询。系统拒绝的操作保留原生错误；
未提供套接字的运行环境明确报错。

每个实例只保存自己的套接字和解析任务。关闭取消尚未完成的域名解析，发送缓冲
保留到 libuv 的完成回调，关闭回调释放套接字。退出时先关闭所有数据报资源，再
经过既有原生运行时排空屏障释放 loop、V8 和模块。

Chromium 显式驱动原生 loop 时，临时启用框架自己的 idle guard，使仅有 unref
套接字时仍可处理到达的消息。guard 在 Pump 外 inactive/unref，不改变用户
handle 的引用状态。退出要等待所有应用 handle/request 和 guard 的关闭回调。
Worker 的空闲判断使用 libuv 的真实存活状态，包含待完成 DNS/send 请求，
同时排除 guard。强制终止和父应用退出经过同一原生排空屏障，再销毁运行时。

边界：当前 libuv 版本不具备 UDP `reusePort`，请求该选项明确报 ENOTSUP；
导入现有 fd/handle 未实现。当前组播成员操作要求先 bind，尚未补齐 Node
`addMembership` 在未 bind 时的隐式内核绑定；未 bind 查询的部分错误码仍需对齐。
普通网页和 renderer 未开放该模块。

验证：JS 契约覆盖 9 项，包含真实 bootstrap 的二进制参数传输；原生测试覆盖真实 IPv4/IPv6 回环、空报文、域名解析、
关闭释放端口、组播/缓冲设置和完整 V8 模块调用。新增原生和产品探针的运行结果
以 [Electron 原包兼容性验证](Electron原包兼容性验证.md) 的对应版本记录为准，
不能用仅通过 JS 契约测试代表网络传输已经验证。

product15 的真实 Worker 专项已通过自然收尾 4/4、带 pending 工作的强制
终止 3/3，以及 app.quit 同时终止 bound/send/DNS 三个 Worker。Utility 在
Browser.close 前真实退出，三轮宿主退出 0、无残留进程和新增 dump。详情与
证据见 [NodeWorker线程兼容](NodeWorker线程兼容.md)。
