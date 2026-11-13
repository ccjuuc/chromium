# Node execFile 与自定义 promisify

`child_process.execFile()` 在主进程、Utility 和有 Node 集成的 renderer
复用同一个实现，底层沿用已有 libuv `spawn`、管道与退出事件。参数以数组传输，
不拼接 shell 命令。回调返回分别收集的 stdout、stderr，默认 UTF-8，也支持
Buffer 输出及跨管道分块的字符解码。

支持超时终止、`killSignal`、`maxBuffer`、AbortSignal、cwd、env 和既有 spawn
选项。信号在创建子进程前由原生平台信号表校验，不在 JavaScript 硬编码数字。
超过限制时销毁输出流并终止所属子进程；非零退出、找不到程序、取消分别保留
退出码/信号、OS 错误、`AbortError` 与 cause。回调至多调用一次。

`maxBuffer` 以编码后的字节计数。与 Node 一致，字符串输出的截断使用已解码的
字符串切片；因此发生超限错误时返回的字符串编码后可能略大于字节上限，不能
把错误结果当成严格按字节截断的 Buffer。Buffer 模式按字节截断。

两端 `util.promisify` 共用标准 `Symbol.for('nodejs.util.promisify.custom')`，
支持自定义入口、调用者 this、函数属性和重复 promisify。execFile 的自定义
Promise 返回 `{stdout, stderr}`，保留 `.child`；失败时错误对象附带两路输出。

本次不增加 shell 执行、同步 execFile、Node fork/IPC、uid/gid 切换或额外 stdio
能力；不支持的请求仍明确失败。Worker 虽然可加载同一 JavaScript 模块，当前
未注入原生子进程 transport，实际调用仍报告不可用，不属于本次运行支持范围。

`xenon_exec_file_unittest.cjs` 使用真实 Node 子进程和独立 Node 对照，覆盖参数
字面传递、二进制/分块 UTF-8、双路输出、非零退出、Promise、字节上限、超时、
已取消/运行中取消、无效参数及异步 ENOENT。既有子进程测试与新增测试共 47 项
通过。完整 JavaScript 回归为 1286 通过、14 跳过。

第 15 版宿主的独立夹具在 main、Utility、renderer 各通过 8 项真实 OS 子进程
检查，覆盖上述输出、错误、超时与取消行为；宿主退出码为 0，无残留进程或
Crashpad dump。证据：`F:/electron-compat-audit/20261111/node-exec-file-product15-01/result.json`。
第三方原包的业务下载验收另行记录，不以接口夹具通过代替。
