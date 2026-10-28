# TH macOS 的 Xenon 外挂构建入口

```sh
python3 xenon_overlay/tools/build_thunder_macos.py \
  --source /path/to/thunder_2025
```

默认输出到 `xenon_overlay/resources/thunder-2025-mac`，可用 `--output` 覆盖。其中包含 `Thunder.app`、相对路径描述符 `thunder_2025.xenon.json`、`COMPATIBILITY.md`、`build-report.json`、官方构建结果、日志和 source maps。构建同时在目录旁生成 `thunder-2025-mac.tar.xz` 和 `thunder-2025-mac.sha256`；压缩包根目录只包含 `Thunder.app/` 与描述符。

`Thunder.app` 是供 153 加载的 **BNDL 资源包，不能独立双击启动**。最终包不含 Electron Framework、Electron Helpers、独立 Electron 启动入口或其 Squirrel/Mantle/ReactiveObjC 运行依赖。保留 MacPlayer、MachXPC、Sparkle、数据库 Frameworks、TH 原生模块和浏览器扩展；移除前检查保留 Mach-O 的依赖。浏览器提供运行时，描述符不指定独立 executable，原有资源和工作目录分别解析。

压缩包和校验文件存在时，macOS 浏览器构建校验 SHA256 并解包，将 `Thunder.app` 和描述符一起放入外层 `Contents/Resources/xenon-apps/`。JSON 配置作为资源参与签名，不放在 `Contents/MacOS/` 代码目录；浏览器从资源目录发现配置，打包会清除旧位置的 TH 描述符。首次生成资源后重新执行 `gn gen out/<配置>`；之后正常构建 `chrome` 即可自动同步。仅这两个运行输入进入浏览器，顶层报告、日志与 source maps 不进入 app。

GN 开关 `xenon_bundle_thunder_macos` 控制是否打入 TH：在 macOS、`enable_xenon_service=true`，且默认位置同时存在 `thunder-2025-mac.tar.xz` 和 `thunder-2025-mac.sha256` 时默认开启，其余情况默认关闭。可在 `args.gn` 中显式设为 `false`，只构建宿主；显式设为 `true` 时，缺资源或平台条件不符会在 `gn gen` 阶段报错。归档或校验不合法会使资源打包失败。`--output` 自定义路径不会自动改变 GN 输入，随浏览器打包时应使用默认输出目录。

**解包目录、压缩包和 SHA256 均为 Git 忽略的本地产物，不提交 TH 资源或 ASAR 密钥。** 仓库只保存构建/校验脚本、兼容补丁、宿主代码和文档。新检出没有这些本地产物时，默认仅构建宿主；需要打入 TH 时，先准备获准使用的源码、SDK、依赖和本地构建配置，再运行以上构建入口。

已有成功的资源目录可单独生成归档，无需重新编译 TH：

```sh
python3 xenon_overlay/tools/archive_thunder_macos.py \
  --source xenon_overlay/resources/thunder-2025-mac \
  --archive xenon_overlay/resources/thunder-2025-mac.tar.xz \
  --checksum xenon_overlay/resources/thunder-2025-mac.sha256
```

归档保留文件内容、普通权限和 Framework 内部相对链接，包括 Mach-O 签名与 `CodeResources` 字节；不复制主机扩展属性。tar 成员排序固定，时间、uid/gid 归零，所以相同内容和权限产生相同归档；这不代表上游编译本身可重复。绝对/越界/悬空链接、硬链接、特殊文件和特殊权限位会导致失败。归档完成后重新读取校验全部成员并核对源目录未变，再发布标准格式的 SHA256 文件。

## 必须明确的兼容范围

**输出是带修复的“153 兼容版 TH”，不是未经修改的原版 TH 二进制。** 输入 TH 仓库不会被修改，构建使用独立临时快照；不会 reset、checkout、修改 Git index，也不会在原仓库安装依赖。临时快照中的源码修复与补丁哈希会完整记录在 `build-report.json`，不通过打包后替换 JavaScript 文本隐藏修改。

目前保留以下必要源码修复：

| 修复 | 原因 | 应用位置 |
| --- | --- | --- |
| 两个原生 helper 改用 N-API 弱引用、异步回调和异常边界；去掉直接 V8 内部 ABI 依赖 | TH 原 helper 使用旧版 Electron 的 V8 C++ 句柄布局，不能把它当成 153 的 N-API 包装句柄 | 临时快照，`patches/native-main.patch`、`patches/native-player.patch` |
| lkhb 在 macOS 内部定义 logger | 原模块存在未定义 `_g_xl_logger`，由模块自身持有真实状态 | 同上 |
| Mac 播放器通过版本化能力桥接 Browser 内的 SDK 与窗口 | TH main 位于 Utility，NSView 属于 Browser，真实视图指针不能跨进程使用 | 临时快照，`patches/app-js.patch`、`patches/player-js.patch` |
| Mac 播控初始化时序、标题和系统按钮 | 加载/焦点事件可能早于异步初始化完成，页面标题覆盖窗口标题 | 同上；新增窗口时序行为限 macOS |
| `SinglePromise` 传播失败并清理已完成请求 | 原实现遇到 AB 配置网络失败时，外层 Promise 永久等待，播放器无法进入初始化；同 key 后续请求也无法重试 | 临时快照，`patches/app-js.patch` |

`SinglePromise` 修复保留同 key 并发合并、同步启动和成功结果语义，将失败正确传给所有等待方，并在成功或失败后清理 key。它使用标准 `Map` 支持任意字符串 key，包含 `__proto__`，不吞掉网络错误或伪造配置。该修正本身与平台无关；本入口只在 Mac 临时快照中应用，不修改 Windows 源码仓库或产物。如果以后向上游共享代码移植，失败路径会从永久等待变为正常 rejection，需要在对应平台回归。

**不改 TH 的打包配置和图片代码。** 外挂资源装配器使用官方 ASAR API，将云盘插件的 `static/` 资源复制到原版 Mac URL 所指的相邻目录，补齐发布包缺失的目录；原插件 ASAR 和 source maps 保持不变。适配器声明 `Contents/Resources/xenon-native-player.json` 也由外挂装配器添加。

普通 Electron 仍走其原有播放器路径；Windows 条件分支保留原行为。本入口只在 macOS 执行，输出仅为本地 ad-hoc 签名包，不承诺 Windows 编译、正式 Developer ID 分发或公证。已知独立的 `dk.node` 网络线程退出问题不在本脚本修复范围内。

## 输入与工具链

- 需要包含 `app/scripts/mac/workflow.js` 的近期 `feature/mac_25.1.13` 源码、匹配子模块、Mac SDK 和已安装的锁定依赖。
- 当前验证基线：TH `3104cda9955185bc3538af79667ff19e017a1d96`；播放器 TS 子模块 `8a256318dd8cb688415459c5929613153664f05f`；播放器原生子模块 `3e9fb34273a7956a1de171b2d7f6c0634fafe7e5`。
- 按待修改文件的完整 SHA256 校验兼容性；无需整仓 commit 完全相同。每组文件必须全部匹配原版或全部匹配记录的修复版。未知版本、半应用补丁和本地冲突直接报错，绝不猜测套用或强制覆盖。
- Node **16.17.1**、pnpm **8.15.9**、CMake、Xcode。脚本优先使用显式参数/环境变量，其次检查 PATH 和常见工具链安装位置，不自动下载工具或更改全局版本。

可选参数：`--node /path/to/node`、`--pnpm-cli /path/to/pnpm.cjs`、`--cmake /path/to/cmake`、`--arch arm64|x64`。环境变量对应 `THUNDER_MAC_NODE`、`THUNDER_MAC_PNPM_CLI`、`THUNDER_MAC_CMAKE`。默认架构为当前机器架构；只构建 Release。

```sh
# 不构建、不写入源目录，检查补丁与工具版本。
python3 xenon_overlay/tools/build_thunder_macos.py --source /path/to/thunder_2025 --check
```

## 过程与失败处理

1. 检查所有补丁前后哈希及工具版本，复制独立源码和依赖快照。APFS 使用独立 inode 的文件克隆；不共享 hardlink 或可写 node_modules。拒绝跳出输入树的软链接，内部绝对链接重新指向快照。
2. 仅在快照应用已记录补丁，调用 TH 自带的官方 `export --config release --sign adhoc` 工作流。
3. 补齐静态资源和声明，移除 Electron 运行时，验证 helper 不再导入 V8/Node C++ 符号、lkhb 无未定义 logger。只签资源包自身，保留业务原生模块和扩展的签名/权限，执行 deep/strict 检查；最终签名后再记录哈希。
4. 对比构建前后的输入树指纹，不一致则拒绝发布。记录补丁、源码/子模块版本、工具版本及校验结果。
5. 生成并校验归档，再一起发布完整目录、归档和 SHA256。已存在的输出目录必须带本工具的成功记录；已有归档与校验文件必须成对且匹配，否则拒绝覆盖。发布任一项失败会恢复全部旧产物；若存储故障导致回滚也失败，明确报错并保留备份和恢复材料。构建失败保留独立工作目录和日志。`--keep-work-dir` 可保留成功快照供审查。单独归档入口也成对发布归档和 SHA256，两入口对同一默认产物共用构建锁。

调用方应保持输入稳定，避免构建时由编辑器、其他构建或 Git 操作同时修改源码。输入指纹不包含 Git 元数据、已知构建输出和废弃目录；SDK、源码、依赖及资源参与校验。

`SinglePromise` 回归使用输入仓库的 TypeScript 编译器，将真实补丁应用到临时 fixture，再验证并发结果、错误传播、清理重试、不同 key 和同步异常，不修改输入文件：

```sh
THUNDER_MAC_TEST_SOURCE=/path/to/thunder_2025 \
  /path/to/node-v16.17.1/bin/node --test \
  xenon_overlay/tools/thunder_macos/single_promise_patch_test.cjs
```
