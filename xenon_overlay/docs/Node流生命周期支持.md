# Node 流生命周期支持

main 与 renderer 使用同一份 `readable-stream@4.8.0` 上游实现，提供
`Stream`、`Readable`、`Writable`、`Duplex`、`Transform`、`PassThrough` 的真实
读写队列、背压和销毁生命周期。`stream` 与 `stream.Stream` 保持相同身份；
`stream.promises`、`stream/promises` 及对应的 `node:` 名称使用相同模块实例。

上游源码、依赖与许可证保存在 `resources/ipc/third_party/readable_stream/`。
`tools/bundle_readable_stream.cjs` 在开发时生成静态 CommonJS 函数表，
`--check` 检查产物一致性。运行时不通过 eval、文件读取或动态编译加载它们。
`common/node_streams.js` 注入当前上下文的 Buffer、EventEmitter、调度器与
Web Stream 转换，原版上游源码不打补丁。

## 支持的调用

```js
const fs = require('fs');
const {Transform, pipeline, finished} = require('stream');
const promises = require('stream/promises');

pipeline(fs.createReadStream(source), new Transform({
  transform(chunk, encoding, done) { done(null, chunk); },
}), fs.createWriteStream(target), error => {
  if (error) console.error(error);
});

await promises.pipeline(
  [fs.createReadStream(source), fs.createWriteStream(target)],
  {signal: abortController.signal},
);

const cleanup = finished(stream, error => {
  cleanup();
  // 处理完成或失败。
});
await promises.finished(stream, {cleanup: true});
```

支持 `pipe`、`Readable.from`、异步迭代、对象模式、cork/uncork、异步 transform、
final/flush 钩子，以及同步/异步 iterable、generator/function 和 Web Stream
管线阶段。`Readable/Writable/Duplex.fromWeb/toWeb` 使用真正的读写锁和取消，
转换与状态查询边界见 [WebStreams兼容.md](WebStreams兼容.md)。未实现的抽象
`_read` / `_write` / `_transform` 保留 Node 错误，不吞掉数据或伪造成功。

每条连接在 `write()` 返回 `false` 时暂停上游，在 `drain` 后恢复。
Promise 管线支持 `{signal, end}`，取消错误保留 `signal.reason` 为 `cause`。
`end: false` 不结束目标；与 Node 一样，低于 highWaterMark 的排队写入可能仍未
完成，因此调用方需要最终 `end()` 并等待 `finished` 才能确认全部数据写出。

`finished` 支持 `{readable, writable, error, signal}`，取消观察不销毁被观察流。
回调形式返回 cleanup，Promise 形式支持 `{cleanup: true}`。`pipeline` 按 Node
约定保留部分终态错误监听器以接收延迟错误，不能依赖管线结束后监听器归零。

## 文件与压缩流边界

`common/file_streams.js` 将 `fs` / `original-fs` 接到真实流引擎的
`_construct`、`_read`、`_write`、`_destroy`。读范围保持 inclusive end，64 KiB
范围只请求该范围；但目前仍一次取得所请求的完整范围，在 JS 队列中按
highWaterMark 分发，**不是底层逐块文件读取**。不指定 end 时仍取得整个文件。
销毁会丢弃迟到读结果。写入按顺序提交，销毁等待已经提交且不能取消的写入结束，
不会在 close 后继续写文件。不支持的 fd、自定义 fs、写入偏移等选项明确报错。

`string_decoder` 使用上游增量解码实现，保留 UTF-8 跨块字符与不完整尾部。
zlib 流使用真实 Transform 与原生增量 codec，每次传输及输出有大小上限；
背压会暂停压缩泵，继续读取才恢复，不先收集整个输入再压缩。

## 验证

```powershell
node xenon_overlay/tools/bundle_readable_stream.cjs --check
node xenon_overlay/tools/validate_ipc_compat.cjs
```

定向覆盖慢速目标下的有界生产、异步 transform 串行、跨 realm 二进制数据、
cork/final、文件范围、UTF-8 边界、真实文件管线、取消时的在途写入和 Node 对照的
终态事件。JavaScript 契约测试不替代原生文件桥、压缩 codec 和完整应用运行验证。
