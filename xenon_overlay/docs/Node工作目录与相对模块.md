# Node 工作目录与相对模块

## 工作目录

独立 Electron 主进程与 `utilityProcess.fork` 的 `process.cwd()` / `process.chdir()`
使用 libuv 的真实进程工作目录。启动时设置配置中的工作目录，此后相对文件读取、
文件描述符打开、原生 addon 和默认 cwd 的外部子进程使用同一个 OS 状态。
`process.argv`、应用资源根目录和模块父文件路径不会随 cwd 改变。

此能力仅在 Chromium 的 Utility 进程且携带 Browser 指定的独立应用标记时启用。
嵌入式运行时不能更改宿主进程 cwd，调用明确返回 `ERR_NOT_SUPPORTED`。
Worker 与主线程共享真实 cwd，已有 Worker 能观察主线程更改；Worker 调用
`process.chdir()` 返回 `ERR_WORKER_UNSUPPORTED_OPERATION`。非法类型、NUL、
不存在路径及普通文件保留相应错误，失败不会更新一份虚假的 JS 工作目录。

单实例锁请求也从原生运行时读取当前 cwd。同步和异步入口在转换应用参数后
覆盖私有目录快照；Browser 认证来源和启动代次后校验绝对路径并转发通知。
应用伪造运行时字段、修改 process.argv 或替换 process.cwd 都不能替代真实目录，
additionalData 仍原样作为应用数据保留。

## CommonJS 相对模块

Node 在查找实际文件前先消解 `.` / `..`。Chromium 的 `base::File` 直接拒绝含
父目录引用的路径，不能直接把 `require('../file.json')` 的拼接结果交给它。
通用模块规范化现在先按文件系统组件做词法归一化，再对真实文件 / ASAR 成员
解析；路径中的字面 `%`、`#`、Unicode、盘符与 UNC 共享根保持文件路径含义。
归一化后仍检查应用根目录，不能通过 `..` 或链接逃出模块加载范围。

## 验证状态

新增完整主 bootstrap JS 测试 3 项已通过，覆盖原生 cwd 读取、错误、输入类型与
Worker 禁止修改。原生集成测试覆盖嵌入宿主保护、真实 cwd / readFile / open 一致性、
ESM createRequire 与 CJS 父目录加载、缓存身份、缺失中间目录及越界拒绝。
这些原生测试已在 product10 通过。product11 的 Windows 独立应用探针
`cwd/native-02/result.json` 已通过：主进程、既有/新 Worker、同步/异步读取、
readstream 和 open 使用一致 cwd，旧 fd 保留已打开文件，Worker 修改被拒绝；
外部子进程的默认继承、显式 cwd 不污染父进程、Utility 继承和错误目录保持原状
也已验证。宿主退出码 0，无残留进程或 dump。此前 `cwd/native-01` 的标准流
初始化失败记录保留，不与本次通过结果混计。
单实例 cwd 新增两项原生回归，覆盖真实 chdir 后的同步/异步传输、伪造字段覆盖
和 Browser 端拒绝缺失/相对/NUL 路径；已在 product11 build05 原生回归通过。
