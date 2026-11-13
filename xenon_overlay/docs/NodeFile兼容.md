# Node File 兼容

main 与 Node Worker 的全局 `File` 和 `require('buffer').File` 使用同一构造函数，继承同 realm 的 Blob。renderer 保留 Chromium 原生 Blob/File，buffer 模块直接引用这两个构造函数。

File 保存不可变字节、只读 name/lastModified。构造时复制外部 BufferSource，嵌套 Blob/File 和 slice 可共享内部不可变块；arrayBuffer/bytes 暴露副本。slice 按规范返回 Blob。stream 使用已有真实字节流，每次按需求复制最多 64 KiB，并支持 BYOB 和取消。

输入与异常顺序以 Node buffer.File 为兼容目标：先转换 Blob 内容及其选项，再读取 lastModified，最后将 filename 转为 USVString，替换孤立代理项。lastModified 未给出时取当前时间，NaN 转 0，bigint/symbol 拒绝；当前 Node 保留小数和 Infinity，宿主也保持这一行为，不擅自套用浏览器的整数转换。name/lastModified 使用带品牌检查的 prototype getter，可继承，无可写 setter。

该能力不代表文件系统句柄，也不根据 name 读取磁盘。Blob/File 的跨 Worker structured clone 仍依赖宿主消息序列化能力，不能据此声明已支持。

验证：独立 File 的 5 项 Node 差分/真实字节流测试与已有 Blob 7 项测试通过；main、Worker、renderer 完整 bootstrap 测试验证 File/Blob 身份、内容和切片类型。
