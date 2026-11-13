# Web Streams 兼容

`stream/web` 和 `node:stream/web` 指向同一内置模块；CJS、ESM 与同一 main/Worker realm 的全局流构造函数共享身份。renderer 使用 Chromium 的真实 Blink 构造函数，避免替换后与浏览器 fetch/Blob 的 stream brand 不一致。

普通 V8 context 采用固定的 `web-streams-polyfill@4.5.1`。源码位于 `resources/ipc/third_party/web_streams_polyfill`，保留 MIT LICENSE、package.json、来源 URL、archive SHA-256 和 npm integrity。运行时无需下载安装依赖。UMD 代码仅在末尾增加打包器要求的换行，其余字节保持上游发布版本；封装向它提供局部 CommonJS exports，未修改第三方应用的文件或依赖。

本次覆盖 WHATWG ReadableStream/WritableStream/TransformStream、默认和 BYOB reader、byte stream/controller、writer、两种 queuing strategy，共 13 个核心构造函数。真实队列实现负责 backpressure、锁、reader/writer ready/closed、tee、cancel/abort、pipeTo/pipeThrough、async iterator/from 和错误传播。BYOB 必须拥有真正的 ArrayBuffer 转移能力；宿主同时缺少 ArrayBuffer.prototype.transfer 与 structuredClone 时明确拒绝安装，避免上游的降级路径保留已应被 detach 的 buffer。

main/Worker 的 Blob.stream 也接入 byte stream：每次有读取需求才复制最多 64 KiB 的不可变块，可用 BYOB 读取，取消后释放该 stream 保留的 chunk 引用。enqueue 转移的是新副本，不会 detach Blob 或共享 slice 的底层存储。renderer 保留 Blink Blob。

上游包不提供 TextEncoderStream/TextDecoderStream 和 CompressionStream/DecompressionStream。renderer 若有真实原生构造函数，模块直接导出；本批不向 main 注册空类。main 现有 TextDecoder fallback 未实现流式解码/fatal/ignoreBOM，TextEncoder fallback 对孤立 surrogate 的行为也尚未对齐，因此不能仅套 TransformStream 壳就声称编码流完成。

Node `stream` 的普通队列仍是单独能力。新增 `common/stream_web_adapters.js` 工厂提供 Readable/Writable/Duplex 的 fromWeb/toWeb；使用真实 reader/writer 与 Node 队列，按背压暂停/恢复、等待真实写入与 final，双向传播取消、错误和提前关闭。byte/BYOB 转换复制 Node Buffer，避免 detach Buffer 池。fromWeb 保持 Node 的转换锁所有权；双工获取第二个锁失败会回滚第一个锁。安装由 Node stream 基础模块统一完成。

状态读取是内部能力，不给 stream/web 增加非标准导出：先验证品牌，再读取固定 ponyfill 的真实 disturbed/state；锁定 reader 本身不会被标记为已读取，erroring 与 errored 区分。Blink 没有公开同步状态接口，必须经宿主绑定读取，缺少绑定时明确拒绝，不能以 locked 推导或固定返回 false。

验证：`xenon_web_streams_unittest.cjs` 15/15 与 `xenon_stream_web_adapters_unittest.cjs` 17/17 通过，包括与系统 Node 对照的 buffer 转移/BYOB/byte tee、实际 backpressure、pipe abort、transform 错误传播、iterator cancel、Blob 不可变性，以及 main/Worker/renderer bootstrap 身份。adapters 既验证真实系统 Node 流，也在 main/Worker/renderer 完整 bootstrap 中验证 vendored Node stream 的有限队列、双向转换、取消和真实状态。新增原生 `WebStreamsBuiltinUsesRealByteOwnershipAndQueues` 使用真实 ESM/CJS 加载及 V8 buffer detach，等待统一编译执行。
