# 文件元数据与 BigInt Stats

`fs.stat/lstat/fstat` 的同步、回调和 Promise 入口，以及
`FileHandle.stat({bigint: true})`，通过统一的原生文件元数据转换返回结果。
只有 `options.bigint === true` 选择 BigInt Stats；默认仍返回 Number Stats。
renderer 的路径查询具有相同语义，其尚未开放的文件描述符 API 不在本轮范围。

## 精度与类型

- 物理路径使用 libuv `stat/lstat`，文件描述符使用 libuv `fstat`。两者共用
  `FileSystemStatValue`，直接从 `uv_stat_t` 取值，保留符号链接类型和真实
  `dev/ino/size/mtimeNs/birthtimeNs`，可用于打开前后的文件身份核对。
- BigInt 请求将原生整数及纳秒时间编码为十进制字符串，通过现有 IPC 传输。
  JavaScript 从这些字符串构造 BigInt；禁止先经 double/Number 再转换。
  纳秒字符串以整数十进制拼接计算，覆盖负时间，不依赖平台专属 128 位类型。
- BigInt Stats 的十个整数元数据字段、四个 `*Ns` 和四个 `*Ms` 使用 BigInt。
  毫秒通过纳秒除以 `1000000n` 向零截断；`Date` 字段按 Node 规则延迟构造。
  BigIntStats 是独立类型，不是公开 `fs.Stats` 的实例；类型判断支持 BigInt mode。
- ASAR 成员与 renderer 虚拟挂载仅转换其已知元数据。未知 inode、权限和时间
  保持 `undefined`，不以零伪装实际文件信息。`original-fs` 的物理归档视图
  仍沿用真实文件系统元数据。传输若提供已经舍入的 Number 作为 BigInt 字段，
  明确返回 `ERR_IPC_INVALID_RESPONSE`。

这项能力适用于所有调用方。它用于修复原版应用完成下载时的通用文件身份
校验，不包含按应用名称、包名或业务路径分支的处理。

## 验证

文件系统 JS 专项 149 项中 141 项通过、8 项既有条件跳过，零失败。新增用例
对照 Node 24 的真实 Stats 和 BigIntStats，覆盖主进程与 renderer 的各类入口、
FileHandle、超过 `2^53` 的整数、正负纳秒、毫秒截断、Date、虚拟元数据及
拒绝已舍入字段。日志为审计目录下 `fs-bigint-source20-tests.log`。

四项新增原生测试检查大整数与纳秒序列化、物理路径、ASAR/原始归档视图，
以及 `lstat` 与 `fstat` 的文件身份和时间一致性。product20 的四项均通过，
完整 IPC 原生测试 600/600 通过；证据为 `native-stats20-02.json` 和
`native-full20-01.json`。

相同文件的 Node 24 主线程及真实 Worker 基线 8/8 通过。product20 实际
主进程 4 项、Worker 4 项、独立 Utility 4 项、renderer 3 项，共 15/15 通过；
路径同步/回调/Promise、可用角色的 fstat 和 FileHandle.stat 全部核对
`dev/ino/size/mode/mtimeNs/birthtimeNs`，与 Node 提前记录的精确值一致。
本轮避免以会因读取变化的 atime 作稳定基线。宿主、Worker、Utility 均退出 0，
没有归属残留进程或专用 profile 的新增 dump，端口 9268 已释放。证据为：

- `fs-bigint-node-baseline20/verification.json`
- `fs-bigint-probe-product20-01/verification.json`

原版 Motrix 的实际下载提交也已通过：打开前 lstat 与打开后 FileHandle.stat
的文件身份校验成功，最终写出正式 `sample.bin`，大小 2,097,289 字节且 SHA-256
与受控下载源匹配。应用菜单退出后退出码 0、无归属残留进程。此项覆盖该下载
提交闭环，不表示所有应用功能已验收；证据为
`motrix/xenon-core-product20-01/result.json`。
