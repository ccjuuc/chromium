# IPC bootstrap 源码组织

`xenon_ipc_main_bootstrap.js` 和 `xenon_ipc_renderer_bootstrap.js` 是构建入口。
它们声明源码组合顺序，**不能直接注入 V8 或传给 `vm.runInContext`**。
`// @include "..."` 只由构建工具展开，运行时不加载源码片段。

## 目录与职责

| 目录 | 内容 |
| --- | --- |
| `common/` | 两端行为及实现相同的 zlib 工厂、readline 实现 |
| `main/` | app/窗口/session 生命周期、主进程 IPC、Node 模块及宿主调用 |
| `renderer/` | renderer IPC、Node 模块、原生对象桥、CommonJS 加载、preload/webview 安装 |

每个片段包含完整的声明或语句，按功能边界拆分，不按固定行数截断。
当前 main 有 14 个职责文件，renderer 有 15 个，两端共用 2 个文件。
文件系统、窗口和原生桥接各自集中维护；保留有独立契约测试价值的模块。
入口保留必要的安装顺序和模块注册状态。两端有差异的 Buffer、path、URL、
network 等实现继续分别维护；不能因为名字相同就覆盖其中一端。

## 构建与运行

`//xenon_overlay/resources:ipc_bootstrap` 使用 Chromium 自带 Node 执行
`xenon_overlay/tools/bundle_ipc_bootstrap.cjs`，静态展开入口并检查 JavaScript 语法。
输出为：

```text
out/<配置>/gen/xenon_overlay/resources/ipc/xenon_ipc_main_bootstrap.js
out/<配置>/gen/xenon_overlay/resources/ipc/xenon_ipc_renderer_bootstrap.js
```

GRIT 依赖该动作，把生成文件打入原有资源 ID。main 和 renderer 的 C++ 注入方式
保持不变；未初始化 ResourceBundle 的原生测试读取可执行文件目录下 `gen/` 中的
同一生成文件，该文件也列入测试运行数据。

新增或删除片段时，只修改对应入口中的 include。`bootstrap_sources.gni`
自动从入口读取构建输入，无需手工维护第二份清单。include 仅允许出现在入口中，
不形成多层引用。构建器另写依赖文件；输出未变化时不重写文件，避免无效的下游重建。
路径以入口所在目录为根，统一使用 `/`；不允许重复、循环、越界或缺失的 include。

## 作用域和模块身份

这些文件是**构建时组合的源码片段**，仍共享入口的同一个闭包，不是各自拥有
运行时作用域的 ES module。这样可以保留既有初始化次序、函数提升和对象身份。
本次拆分后的完整脚本与拆分前逐字一致（统一为 LF 换行）。

修改时需特别注意：

- `fs.js` 中流的类继承依赖后方 `builtins.js` 中的函数声明提升。
- main 的 app 退出流程、session 和 IPC 回调会访问后方声明的窗口及事件状态。
- renderer 的 `native_bridge.js` 统一管理句柄、代理和回调状态；`modules.js`
  中的 CommonJS 加载与内建模块注册共用缓存。
- 网络和 IPC 片段有安装监听器的副作用；preload 必须在原来的页面初始化阶段执行。

不要给每个片段额外包 IIFE、调整顺序，或改成运行时文件加载。
同一 context 中的 `require` 缓存和返回对象保持一致；不同 context 仍分别创建状态。
后续引入独立工厂时应显式传入依赖，并单独验证初始化时序和模块身份。

## 测试与诊断

完整集成测试和基准使用 `bootstrap_test_support.cjs` 的 `readBootstrap()`，
与生产打包调用同一构建器。独立契约测试可用 `readBootstrapPart()` 读取明确命名的
源码片段，避免按字符串位置截取整个 bootstrap。自定义 `XENON_TEST_BOOTSTRAP`
仍支持历史单文件脚本。

```text
node xenon_overlay/tools/validate_ipc_compat.cjs
```

构建器测试覆盖共享作用域、初始化顺序、单层 include、错误 include、源码覆盖、
特殊字符路径和增量输出。修改原生加载路径时还要构建并运行原生 IPC 回归。

V8 报错行号对应生成文件；先打开该配置的 `gen/` 文件，再定位到相应职责片段。
本次调整减少重复维护，不增加运行时 I/O、IPC 或加载器，也不据此宣称启动提速。
