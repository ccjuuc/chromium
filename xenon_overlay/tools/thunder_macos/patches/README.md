# TH macOS 原生兼容补丁

这些补丁由 Xenon 外挂打包入口应用到临时源码副本。传入的 TH 主仓、子模块和已有 app 都不修改。补丁修复原生加载和对象生命周期边界，不修改 TH 业务流程。

## 版本和应用规则

`native-compatibility.json` 记录补丁 SHA256、每个文件应用前后的 SHA256，以及审查时的 Git 基线：

| 补丁 | 相对 TH 根目录的仓库 | 基线提交 | 文件数 |
| --- | --- | --- | --- |
| `native-main.patch` | `.` | `3104cda9955185bc3538af79667ff19e017a1d96` | 5 |
| `native-player.patch` | `cppsrc/player_comp_native` | `3e9fb34273a7956a1de171b2d7f6c0634fafe7e5` | 4 |

Manifest 的 `path` 相对 manifest 所在目录，`files[].path` 相对对应的 `repository`。每组文件全部匹配 `before_sha256` 才应用补丁；全部匹配 `after_sha256` 时记录为已兼容。部分应用、未知内容或缺失文件均需报错并指出文件，不能靠模糊匹配覆盖调用方的修改。基线提交用于溯源；内容校验不能省略，也不能单凭提交号认定工作区文件相同。

先校验补丁 SHA256，再在临时副本执行 `git apply --check` 和 `git apply`，最后核对全部 `after_sha256`。补丁上下文包含原文件的非 UTF-8 注释，必须按字节读取和传递，不能重新编码整个补丁。Windows 原有 monitor 实现与声明按字节保留。

## 必要修改及边界

| 修改 | 必要原因 |
| --- | --- |
| 两个 helper 的 `object_life_monitor.cpp/.h` | 原版把 `napi_value` 的内存直接复制到 `v8::Local`，再用 V8 持久句柄和全局默认 libuv loop。改为标准 N-API finalizer/reference、每 env 的 loop 和 cleanup hook，保留 1000ms 延后回调、null 接收者和单一 userData 参数。 |
| 两个 helper 的 `mac/init.h` | `GetFileVersion` 中未使用的 V8 HandleScope 仍使原生模块依赖 V8 C++ ABI。移除该作用域及不再使用的 Node/V8 头文件，平台功能保持不变。 |
| 两个 helper 的入口 `.cpp` | 未注册的 `RequestGarbageCollectionForTesting` 仍产生 V8 导入。仅在非 Mac 分支保留原实现和头文件；这并非移除任何已导出的 JS API。 |
| 主仓 `LinkProtocol.cpp` | Mac 开启原生日志时，`InitAddon` 读写 `g_xl_logger`，原版却仅在 Windows 定义该变量。为 Mac 定义同模块的隐藏可见性 logger，避免依赖另一个 addon 或宿主碰巧导出的业务变量。 |

monitor 使用 `napi_async_init`、`napi_make_callback` 和 `napi_fatal_exception` 建立真实异步回调及异常边界。GC finalizer 只排队；env 清理停止计时器、释放引用，不再执行 JS。不能替换为空 finalizer、吞异常或人为成功的返回值。

## 原版二进制为何不能仅由宿主补符号

已核对原始导出包的 `nm -u`：两个 helper 各导入 12 个 V8 C++ 符号，包含 `v8::HandleScope`、`v8::api_internal::GlobalizeReference`、`MakeWeak` 和 `v8::Function::Call`。它们不是稳定 Node-API ABI。原 `ObjectLifeMonitor::BindTo1` 又明确通过 `memcpy` 假定 `napi_value` 等同 V8 Local 句柄；153 的 `napi_value__` 是持有 `v8::Global` 的宿主包装对象。即使导出同名 V8 函数，原模块对句柄布局的假定仍不成立。

原始 `lkhb.node` 导入 `_g_xl_logger`，此前实际 `dlopen` 也报告该符号不存在。这个变量是 TH 使用的 `std::shared_ptr<spdlog::logger>`，没有可由通用宿主承诺的稳定布局及所有权。把变量塞入 153 或用另一 addon 提供，都会把 TH 的 C++ 库版本和销毁顺序泄漏到宿主。

因此保留宿主通用 N-API 语义，并重新编译有 ABI 问题的 addon。不能通过改 Mach-O 符号、构造假 V8 入口或注入一个空 logger 来声称原版二进制兼容。这里的“输入源码不变”指对输入仓库不落盘修改；产物包含明确记录的源码级兼容补丁，并非原二进制逐字节复用。

## CMake 外挂方案取舍

CMake 的 `CMAKE_PROJECT_INCLUDE` 是官方源码外注入入口（3.15 起），配合 `cmake_language(DEFER)`（3.19 起）可在 target 创建后追加源文件或调整 `SOURCES`。lkhb 的变量定义可以迁成单独 `.cc` 并仅加入 Mac 的 lkhb target；也可使用原有 `THUNDER_CPP_CLOSE_LOG=ON` 避开日志变量，但这会主动关闭日志，不等价于保持当前包行为。

两个 helper 不能只追加一个 shim：原 object monitor 已定义相同方法，而且入口使用相对路径包含原 `mac/init.h` 和 monitor 头。完整替换仍需生成三份相互一致的源文件/头文件、维护 target 源列表和 include 路径。当前在临时副本应用小范围、带内容哈希的源码补丁，更容易审查和复现；不需要改上游 CMake，也不需要重建一套 V8 ABI 兼容层。

## 验证要求

本目录补丁已在仅含基线文件的临时目录中通过应用、逆向检查和应用后哈希核对；两份 Windows monitor 实现及声明与基线一致。提取补丁本身没有重新构建或运行 TH。

最终打包仍需实际重编译两个 helper 和 lkhb，并检查成品：helper 不得保留 V8/Node C++ 导入；lkhb 不得留下未解析的 `_g_xl_logger`。包签名、153 加载与播放测试，以及原 Node/Electron 的生命周期回归，都属于输出验证，不能用“补丁应用成功”替代。此前该补丁内容已通过 Node 16/24、Electron 22 及 153 N-API 回归；新一次打包必须记录自己的构建和验证结果。

PC 影响：无。Mac 专用修改不进入 Windows 编译分支，外挂入口仅生成 macOS 产物。
