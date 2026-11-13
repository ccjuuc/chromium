# Node 文件追加

`fs.appendFileSync()`、`fs.appendFile()` 和 `fs.promises.appendFile()` 共用追加写入
原语；main、renderer 和 `original-fs` 使用相同的选项解析。未指定 `flag`、
`flag: undefined` 或 `flag: null` 时保持追加，不截断原文件。显式 `flag: 'w'`
改为覆盖，`writeFile` 的 `flag: 'a'` 改为追加。

字符串使用调用者指定的 Buffer 编码；Buffer、TypedArray 和 DataView 只传输
实际子视图的字节。异步桥在提交时快照字节。原生追加使用 `base::File` 的
`FLAG_OPEN_ALWAYS | FLAG_APPEND`，不存在的文件会创建，不采用先读后写。
renderer 的既有 `chrome://` 虚拟目录保持独立内存存储，不影响普通文件路径。

本次只支持路径形式和 `w` / `a` 两种 flag；其他 flag 明确报
`ERR_NOT_SUPPORTED`。自定义创建 mode 和 `flush: true` 也明确拒绝；文件描述符形式仍待补齐，
不能将本项验证视为完整 Node fs 兼容。

`xenon_fs_append_unittest.cjs` 在真实临时文件上检查四种运行时/模块组合的
内容保留、编码、子视图快照、异步回调与失败不写入。原生追加原语由
`XenonIpcMainContainerTest` 的现有文件桥回归覆盖。
