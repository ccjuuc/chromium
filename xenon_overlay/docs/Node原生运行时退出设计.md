# Node 原生运行时退出设计

本记录描述已接线的退出方案。Chrome 产品已成功重链，N-API、主容器及 Session 原生回归通过；TH 的真实退出验证仍在进行，尚不表示产品退出验收完成。

## 已确认的危险路径

- `napi_queue_async_work` 的工作线程及主线程 completion 捕获裸 `napi_async_work`，work 又持有裸 env。当前 delete 不检查排队/执行状态。
- TSFN 的队列任务和 finalizer 捕获裸 TSFN，其 env 不受容器生命周期约束；停止 UV timer 无法取消这些 Chromium sequence 上的任务。
- `InitializeLoadedNodeAddon` 在调用注册入口之后才把 env 和 DLL 移入输出对象。入口排队工作后返回空值时，局部 env/DLL 提前释放。
- main `Shutdown` 清除 `loaded_node_addons_` 后立刻清 Context/Isolate。service 初始化失败局部容器、旧容器替换、service 析构都可进入此路径。
- shared `XenonNodeExecutor` 析构同样先清 addon map，再释放 isolate。它并非天然免疫。
- Worker 环境由 engine Finish 立即 reset；Worker 必须继续明确拒绝 addon，本轮不扩大其原生模块范围。

仅保留 env 或 isolate 不够：JavaScript 中的宿主函数和 Promise callback 还捕获 main container / executor 的 `this`。必须保留完整 runtime 对象与其成员，直到全部 native callback 退出。

## 最小接口与阶段

`NativeRuntimeShutdown` 提供 `BeginNativeShutdown()`、非阻塞 `PollNativeShutdown()` 和诊断描述。`RetireNativeRuntime` 接收整个对象的 unique ownership，从下一个任务开始执行。等待期间由连续的延时任务持有 owner，完成后先析构 runtime，再释放进程保活引用。不会在 native callback 自己的栈上销毁自己。

env 生命周期分为运行、关闭入口、等待异步资源、finalize、可释放：

1. 停止外部新请求和新 native work 注册。关闭 TSFN 入队，唤醒堵塞生产者；待发送数据通过 `call_js(nullptr, nullptr, ...)` 释放。不得在持有 TSFN mutex 时调用 addon 回调。
2. 每个 async work 使用受跟踪的状态和任务所有权，取消尚未开始的执行；已执行任务继续到 completion。completion 及其可能调用的 delete 结束之前保持 work/env/DLL 有效。不得靠 `PostTaskAndReply` 的裸指针保证生命周期。
3. 运行 cleanup hooks，并等待已执行工作和 TSFN producer/队列/finalizer 结束；等待期间整个 runtime 和 V8 realm 保持有效。TSFN finalizer 可能负责 join producer，关闭队列后必须允许在计数归零前调用；对象仍保持有效，直到 finalizer 返回且 producer/正在执行的 API 均结束。收到 `napi_closing` 的 call 本身消耗 producer 引用，不再要求随后 release。对象及 instance finalizer 在执行中的 native work 结束后运行，并允许完成资源清理所需的引用操作。
4. 最后继续非阻塞泵 UV close callbacks。只有所有 native 任务、回调深度和 producer 已归零、finalizer 已完成，且独立 loop 的 `uv_loop_close()` 成功，才释放 env、DLL、Context、Isolate。`uv_loop_alive()==0` 不是充分条件，未引用或未激活 handle 仍可借用 loop 内存。

清理回调可继续访问 env 完成释放；关闭后重新排队异步业务操作明确失败。不能为了获得“已退出”状态，丢弃 completion、直接删除 handle、替换 addon 的 close callback 或将资源永久存到泄漏列表。

初始化失败同样交付 env/DLL 给 runtime ownership，再返回错误。失败 addon 的 native 状态在 runtime 安全退出前保持有效，不能在注册入口返回的栈上销毁。

## Service 与进程生命周期

当前 `mojo::ServiceFactory` 在主 pipe 断开时直接删除 service，并随后触发 Utility 退出。因此，仅在 service 内保存 retired map 不够；service 析构必须把所有 runtime 转交独立 retirement owner。

Utility factory 注入进程保活获取/释放闭包。开始退役前增加进程引用，所有退役完成后释放。NAPI 与通用 retirement 层不依赖 content 内部类型。意外断连、初始化失败和替换旧 owner 都使用同一入口。ASAR 密钥和依赖执行器的所有权也必须覆盖需要它们的最后一个回调，不能先在 service 析构体中撤销。

超时只表示 addon 未履行关闭义务，绝不意味着可以释放仍忙的地址。独立 Utility 的超时处理由进程 owner 终止整个进程，让操作系统原子回收；retirement 自身既不强杀线程，也不返回虚假完成。若 owner 的超时回调返回，retirement 继续持有并等待，通知只发一次。进程内宿主没有安全的单个 native 线程强杀方案，不能把 Utility 的终止策略套到浏览器 UI 进程。

shared 兼容执行器可能通过 addon 自己的 `uv_default_loop()` 使用全局 loop，不能直接改成每 env 独立 loop。每进程最多一个执行器独占该 loop；第二个执行器明确返回资源忙，防止在不同 V8 realm 中泵同一组回调。租约在完整执行器释放后归还，标准模式使用每 runtime 独立 loop，不受该限制影响。

Mac Browser UI 的原生播放器宿主本来就是 `NoDestructor` 的进程级单例。仅在最终进程退出时开始非阻塞清理，完成则释放 executor；若 UI task runner 先停止，原有单例继续保有尚忙的 runtime，交给正在进行的进程退出回收。此路径不强杀 Browser，也不在正常关闭窗口时新建永久保留列表。通用 loader 不再通过 `RTLD_NODELETE` 永久固定 addon image。

UV 关闭回调执行后、关闭 loop 前再次排空 env finalizers 与 cleanup hooks，覆盖清理过程中继续注册清理项的情形。service window hooks 使用弱引用；容器的 executor hooks 则由同一个退役 owner 保持有效，先退役 main 再退役 executor。异步 DLL 准备任务也计入退出屏障。

## 必须验证的场景

- async work 仍在真实线程池运行时关闭容器，completion 恰好一次且 runtime 在之后才销毁。
- completion 内调用 delete，取消前后竞争、重复 queue、过早 delete 均不产生 UAF。
- TSFN 队列满时生产者阻塞；关闭后唤醒并返回 closing，已接收数据释放一次；finalizer 可以 join producer，最终销毁在队列、producer 与 finalizer 全部结束之后。
- addon 注册入口先创建异步工作再返回失败，失败退出依然完整回收。
- UV inactive/unref handle 阻止销毁，保留原 close callback；不同 runtime loop 不互相执行回调；重入 pump 被拒绝。
- service owner 提前析构，retirement 仍完整持有容器；下一个普通任务可以完成 native work，证明没有嵌套忙等；进程引用在 runtime 析构后释放。
- 退役超时报告一次且不提前 free；由 Utility 进程测试确认超时最终退出，无新增 crash dump 或残留进程。

Chrome 产品已成功重链。`napi_host_unittests` 全量 56/56 通过，覆盖通用退役 owner、async work、TSFN、注册失败和独立/默认 UV loop；主容器套件 465/465 通过，另新增的第 7 项 MessagePort 回归 1/1 通过；Session 套件 27/27 通过。旧 shared 执行器测试已改为验证默认 loop 独占租约及释放后的目录权限隔离，并完成重编译复验。TH 的真实退出验证仍在进行；产品退出与重开结果不能由单元测试通过替代。
