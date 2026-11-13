# Node 同步模块钩子

主进程 `module` / `node:module` 导出 `registerHooks({resolve, load})`。
它连接真实的 CommonJS 加载器和 V8 ESM 模块图，不转写应用入口，也不按应用名添加路径映射。
发行包可以用 resolve hook 改写 ESM 解析，也可以覆盖
`Module._resolveLookupPaths` 调整 CommonJS 搜索目录。两条机制分别生效；
归档目录不因叫作 `node_modules.asar` 而被自动加入路径。
默认文件读取复用现有目录、普通及加密 ASAR 实现。

## 加载行为

- 同步 resolve/load 链按最后注册优先执行；`nextResolve` / `nextLoad` 继续前一层并合并部分 context。
- 不调用 next 的钩子必须返回 `shortCircuit: true`。Promise 结果、错误字段或无效源码明确失败；应用抛出的对象/异常原样传播。
- 返回的注册对象包含 `resolve`、`load` 和 `deregister()`；重复注销无副作用。正在执行的链使用快照，后续注册/注销影响后续加载。
- CommonJS `require`、`require.resolve`、`createRequire` 和 ESM 静态/动态 import、`import.meta.resolve` 使用同一注册表。`require.resolve` 只解析，不执行 load。
- resolve context 传递 parentURL、require/import conditions 和 importAttributes。默认 package `exports` / `imports` 解析使用钩子传给 next 的条件集合。
- load 可提供 `module`、`commonjs`、`json` 源码，也可转发 `builtin` / `addon`。字符串和 ArrayBuffer/视图均可，视图只解码自身字节范围。CommonJS 命名导出从变换后的源码静态分析。
- 自定义 URL 可完全由钩子提供源码。默认文件或 addon 加载继续执行应用目录、ASAR 和原生模块策略；短路返回 URL 不会扩大默认文件访问范围。
- 默认 load 校验 JSON import attributes；钩子可以修改传给 nextLoad 的属性，或自行处理自定义类型。ESM 模块缓存使用 URL 与已解析的 `type`，保留同一模块身份；注销钩子不清除已执行模块。

## CommonJS 搜索目录

主进程、Utility、Worker 和 renderer 的 `module` 提供可覆盖的
`_nodeModulePaths(from)` 与 `_resolveLookupPaths(request, parent)`。
前者按平台路径规则生成祖先的 `node_modules` 目录；后者返回 builtin 的
`null`、相对请求的父目录，或父模块的 `paths` 副本。
运行时不配置隐式用户或全局模块目录，公开的 `globalPaths` 是空快照。

裸包 `require`、`require.resolve`、`createRequire` 及同步 resolve hook 的
CommonJS 默认解析调用当前的 `_resolveLookupPaths`，`this` 为 Module，
`parent` 为对应模块记录。`module.paths` 可修改，`require.resolve.paths`
查询同一路径入口。一个 `createRequire` 持有自身稳定的父记录。
ESM 默认包查找和 `imports` 映射的包目标继续按其标准规则解析。

默认查找保留已解析模块的热缓存；自定义查找每次执行，避免函数替换或
闭包状态改变后继续使用旧目录。已执行文件仍共享 CommonJS 导出缓存。
无论路径来自默认算法、用户覆盖还是 hook，最终加载都保留 canonical
路径检查、应用根目录限制及 ASAR/native 授权。当前自定义查找返回值
只接受不含 NUL 的绝对路径数组；相对查找根明确报错。

## 实现位置

`resources/ipc/main/module_hooks.js` 只维护注册和同步链规则。
`chrome/browser/ipc/xenon_ipc_main_container_modules.cc` 负责 V8 桥接、默认解析/读取与变换后源码缓存。
原生 `XenonModuleResolver` 保持文件及 package 解析职责，`XenonEsmModuleLoader` 保持模块图、实例化、执行与 Promise 职责。

没有提供异步 `module.register()` loader、TypeScript 擦除或 Wasm source-phase 模块。
Renderer 的 Node 加载器仍是 CommonJS，不导出一个无法生效的 registerHooks 占位接口。

## 回归

独立链测试 `module_hooks_unittest.cjs` 包含真实 Node 对照，已在 Windows 的 Node 上通过。
`xenon_module_contract_unittest.cjs` 比较 Node 的默认搜索顺序与父记录规则；
`renderer_module_paths_unittest.cjs` 覆盖任意依赖目录覆盖、父记录身份、
函数及闭包变化、`createRequire`、越界拒绝和默认热缓存无文件系统 IPC。
`XenonIpcEsmIntegrationTest.SynchronousHooks*` 使用真实临时发行目录、ASAR、CJS 缓存与 V8 ESM 图，覆盖条件解析、LIFO、注销、二进制子视图、虚拟模块、导入属性、模块身份和原始异常。
原生用例与正式发行包启动结果需要以实际编译执行记录为准；API 用例通过不能代替应用核心功能验收。
