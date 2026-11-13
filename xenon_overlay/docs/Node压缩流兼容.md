# Node 压缩流实现

`require('zlib')` 和 `require('node:zlib')` 共享模块对象。主进程、页面 renderer 和 service worker 使用同一套模块工厂及原生 codec 引擎，没有应用名称、页面地址或业务 channel 分支。

## 实现

- gzip、zlib/deflate、raw deflate、Brotli、Zstd 的同步、回调式 convenience API，以及对应的 `Transform` 构造器/`create*` 工厂。
- 原生状态持有 Chromium 已依赖的 zlib、Brotli、Zstd codec；每个请求最多读取 64 KiB、输出 64 KiB。持续解压不会先收齐整个输入或整个输出。
- 普通流处理在 ThreadPool 运行；单个 codec 同时只有一个处理请求。JS 当前 write 消耗完成后才结束 write 回调，`push()` 返回 false 后暂停原生请求，直到 `_read()` 恢复。
- `flush()` 作为流内写入标记排队，保持与普通 write 的顺序；`_flush()` 执行 finish。分开的 gzip/Zstd frame 继续解码，损坏或不完整的数据通过 stream error 返回。
- `destroy()`/`close()`、renderer document 移除、主运行时退出都会取消并移除句柄。后台任务只持有可引用计数的 codec，不持有 V8 状态；最后一个任务释放后销毁 codec，无需阻塞退出等待。
- renderer/worker 经 IPC 使用 codec id。主进程从浏览器提供的真实 sender frame 或 worker endpoint 求出 owner；请求方不能通过伪造 owner 或猜测 id 操作其他文档的 codec。

## 边界

- convenience API 的输出最多 64 MiB，可用 `maxOutputLength` 进一步缩小；长流不设累计输出长度限制，依靠分块与背压控制缓冲。
- 单运行时最多 128 个活跃 codec、128 个待完成压缩工作请求。
- zlib 支持预置字典及标准 flush。Brotli 的大窗口扩展、Zstd 非零 `nbWorkers`、超过 26 位的显式窗口/表参数不支持。Zstd 实验参数不允许启用，其中稳定输入/输出缓冲模式与每次独立缓冲的生命周期不兼容。
- `reset()` 和运行中 `params()` 变更仍返回明确的 `ERR_NOT_SUPPORTED`；没有伪装成操作成功。
- 不实现 Node 私有 `_handle` 或 `_processChunk`，调用方应使用公开接口。

## 回归

`xenon_zlib_unittest.cjs` 使用 Node 的真实增量 codec handle 验证 JS pump：五格式互操作、2 MiB 解压背压、提前 flush、损坏输入、并发销毁、异步 callback 和字典。

`xenon_zlib_bridge_unittest.cc` 验证实际原生状态机，包括逐字节输入、127 字节输出、连接 frame、截断、取消、资源参数和输出限制；容器测试覆盖五格式异步 `pipe()` 与 document owner 销毁后句柄不可复用。
