# Node 定时器兼容

主进程、Node Worker 和 Electron Utility 的全局定时器与 `require('timers')`
使用同一实现。`setTimeout` / `setInterval` 返回 `Timeout`，支持
`ref()`、`unref()`、`hasRef()`、`refresh()`、`close()`、数字转换和
`Symbol.dispose`；`setImmediate` 返回 `Immediate`，支持引用状态和释放。
`timers/promises` 使用这些真实句柄处理取消和 `ref: false`。

Renderer 的网页全局定时器保留 Web 平台的数字返回值；仅 Node 的
`require('timers')` 提供对象句柄，具有独立的取消表。取消接受本模块的
Timeout 对象或其数字/规范字符串标识，不尝试执行任意对象的数值转换。

引用状态写入原生定时器记录，Node Worker 判断是否仍有活动工作时只计入
被引用的定时器。取消会释放原生回调；刷新会撤销旧任务并重新安排，保留引用
状态和 AsyncLocalStorage 上下文。回调内刷新保留原标识，回调已经结束后重新
激活会分配新标识，已取消的定时器不能通过刷新复活。

Electron 主进程和 Renderer 的寿命由应用/页面决定；Electron Utility 也保持
官方常驻生命周期，不能将 `.unref()` 当作其自动退出请求。官方实测依据见
[ElectronUtilityProcess兼容.md](ElectronUtilityProcess兼容.md)。

产品调度使用 Chromium 的任务队列：主进程/Worker/Utility 的 Immediate 为
零延迟任务，Timeout 的有效延迟最少 1 毫秒；Renderer 的 Immediate 使用私有
MessageChannel，避免 DOM 嵌套定时器的最小延迟钳制，空闲即释放端口。
未移植完整 libuv timers/check 阶段，不承诺与 Node 在任意
混合 I/O 负载下完全相同的任务排序。回调中的异常仍通过该角色现有的未捕获
异常处理链报告。

`xenon_timers_unittest.cjs` 对照本机 Node 验证句柄、引用状态、刷新重入、
取消、参数和 Immediate 生命周期；原生回归检查引用状态确实影响活动工作。
实际发行包和 Worker 生命周期探针结果记录在原包兼容性报告中。
