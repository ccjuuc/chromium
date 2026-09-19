# 加密 ASAR 运行时支持评估

> 后续目录规范已更新：应用按完整发行目录原样导入，TH 使用单一 `out.asar`，
> PLE 保留最新加密 `out.asar`；宿主配置与公钥移到应用目录之外。
> 当前导入、启动及校验方式见 [Electron 应用目录接入](./电子应用目录接入.md)。
> 本文的旧路径、同步脚本行为和验证数据保留为当时的评估/实施记录，不能作为当前发布命令。

日期：2026-09-19。评估基线：`0c8f266aeeaed`。后续实现见文末。

## 结论

可以实现直接读取现有 TH/PLE 的加密发布包，同时保留普通 ASAR 支持和小范围读取
能力。建议在 `common/asar` 实现统一的归档成员读取接口及加密格式解析，
随后让文件系统和 URL loader 共用它。无需修改 TH/PLE 源码，也不需要再扩展
Chromium/Blink 接口。

这是一项中等规模的读取链路改造，不能仅删除打包时的转换步骤，或只给
`Archive::ReadFile()` 加一次解密。下文保留实施前的评估和真实原包只读验证，
文末记录随后完成的实现与验证范围。

## 实施前的转换做了什么

- `tools/sync_thunder_2025.py:103` 使用原厂 reader 解开插件包，再打成标准
  ASAR，替换同步目标副本；保留 unpacked 标记及额外 companion 文件。
  原厂输入包并未被这一函数就地覆盖。
- `tools/normalize_player_preloads.cjs:33` 处理 PLE 的历史加密 preload：
  原始字节必须与指定归档成员完全相同，才接受原厂解密结果并校验源码。
  当前 7.1.35 preload 使用正常源码构建，不能混用旧归档。
- `common/asar/archive.cc:169` 当前只解析标准 Pickle/JSON 索引。
  最近提交中 URL loader 的修改解决的是文件来源、CORS 和本地资源显示语义，
  没有添加加密归档读取能力。

因此，转换为普通 ASAR 是当时的构建期兼容方案；要保留加密发行产物，
应补全运行时能力后再切换同步方式。

## 实际加密格式

本机已检查的原包扩展名都是 `.asar`，没有发现 `.asara` 样本。其私有格式为：

```text
8 字节 Pickle 长度 | 256 字节 RSA 封装 | 加密 JSON 索引 | 成员内容
```

- RSA-2048、PKCS#1 v1.5，打包使用 privateEncrypt，读取使用 publicDecrypt。
  读取端只需公钥，不能把打包私钥带入运行时。
- RSA 封装包含索引密文长度、索引密文 MD5 及该归档的 AES key。
- 索引与 `encrypted:true` 成员使用 AES-128-ECB，无 IV，关闭自动 padding。
  原厂按 16 字节补空格；成员 `size` 为补齐后的长度，未记录原始长度，
  读取时不能擅自 trim。
- 一个包可以同时包含明文成员、加密成员和 unpacked 成员。
  成员物理位置为 `8 + headerSize + offset`，不能整包统一套 AES 解密。

格式依据为原工程
`F:/xl-player/xmp_7.1.35/app/asar/security-asar/lib/` 中的
`asarCrypto.js:22`、`disk.js:42/108`、`filesystem.js:91` 和 `crawlfs.js:9`。

这是兼容旧格式，不代表升级其安全等级：运行时公钥可以解出包内 AES key，
原格式也没有逐成员的完整性认证。

## 真实原包验证

| 发布包 | 包大小（字节） | 文件数 / 加密成员数 |
|---|---:|---:|
| PLE 7.1.35 `out.asar` | 90,720,472 | 52 / 12 |
| PLE `thunder-pan-plugin.asar` | 17,571,768 | 7 / 2 |
| TH `XLLite/3.23.11.asar` | 997,528 | 1 / 1 |
| TH `XLGame/plugin.asar` | 83,220 | 2 / 0 |
| TH `VipPayCenter2025/2.0.9.asar` | 21,451,032 | 113 / 0 |

PLE 样本来自 `F:/xl-player/xmp_7.1.35/setup/xmp_xdas_pack/Release/resources/app/`；
TH 样本来自 `F:/thunder_2025/Submodule/thunder_2025_bin/ProductRelease/resources/app/plugins/`。
后两个 TH 样本为加密索引、明文成员，说明不能用成员是否加密判断包格式。

在 PLE CSS 与 TH XLLite JavaScript 上，将按块解密结果与原厂完整解密后的
同一区间比较，均一致：

| 请求 | 实际读取密文 | 结果 |
|---|---:|---|
| 开头 1 字节 | 16 字节 | 一致 |
| 从偏移 3 读取 64 KiB | 65,552 字节 | 一致 |
| 尾部 19 字节 | 32 字节 | 一致 |

这是读取量及内容正确性验证，未测量浏览器启动耗时或解密吞吐。
格式允许按成员内 16 字节边界读取，连续范围最多额外读取 30 字节，
不需要为读取 64 KiB 解密整个成员，更不需要解开整个归档。

另已复现原厂 reader 的多包问题：`disk.js` 用 `global.aeskey` 保存密钥，
却按包缓存索引。先读 A，再读不同密钥的 B，随后从缓存读 A，会使用错误密钥。
运行时必须将 key 绑定到每个 Archive 实例。原厂工具还会打印密钥和索引，
且 MD5 比较未强制拒绝，因此不能直接引入该 JS 工具作为运行时实现。

## 必须统一的读取链路

| 链路 | 当前行为 | 建议 |
|---|---|---|
| `Archive::Init` / 元数据 | 标准索引；FileInfo 无加密信息 | 识别受支持格式，验证索引，保存每包解密上下文 |
| 普通 `fs.readFile` | 经 `Archive::ReadFile` | 使用统一成员 reader |
| FS 范围读取 | `xenon_file_system_bridge.cc:258` 直接使用原始句柄和 offset | 改为成员的逻辑范围读取，保持 EOF/end 语义 |
| `file://` 页面及资源 | URL loader 直接 DuplicateFile、MIME sniff、FileDataSource | sniff 与 DataPipe 流送都使用同一明文 reader |
| WebUI 资源 | 已经调用 `Archive::ReadFile` | 接入底层即可覆盖读取 |
| renderer require / preload | 通过 FS bridge 读源码 | 随底层读取统一覆盖，保留模块路径和缓存身份 |
| main CommonJS require | resolver 和源码读取仍直接访问 OS 文件 | 完整加密应用包支持还需统一 archive-aware stat/resolve/read |
| `.node` / 原生库 | 系统加载器需要真实文件 | 继续 unpacked/外置；packed 原生模块提取不是本次自动获得的能力 |

main CommonJS 和 packed native 加载是现有普通 ASAR 也存在的独立缺口。
不能把“插件/renderer 加密资源读取成功”等同于“完整加密 app.asar 已可启动”。

## 推荐实现边界

1. 在 `common/asar` 增加成员 `ReadRange` 或 `EntryReader`；`ReadFile` 复用它。
   普通成员保持直接读取；加密成员按块读取并解密，仅返回请求区间。
2. 公钥和允许的格式由可信接入配置提供。核心不得按 TH/PLE 名称、页面名、
   固定安装路径分支，也不得硬写产品密钥。没有正确配置或格式损坏时明确失败。
3. 每个 Archive 保存自己的不可变密钥、索引及成员布局。并发读取不共享有状态
   解密器。缓存需要区分解码配置；避免在进程级全局缓存锁下执行解密 I/O。
4. 校验 RSA 封装、长度/对齐、索引 MD5、JSON、成员边界和整数溢出。
   保持现有 CORS、路径和 unpacked 语义，不以支持加密为由放宽资源权限。
5. 统一 FS range、URL sniff 和流送后，才能关闭相应构建期明文化步骤，
   原包直接复制发布。期间继续保留标准包路径以便逐项比较和回归。
6. 第二阶段若要求直接使用完整发行 `out.asar/app.asar` 启动，补齐 main
   CommonJS 解析与真实原生文件路径映射，再切换 main/preload 的打包入口。

解密可使用仓库现有 BoringSSL，由平台无关 C++ 实现，不依赖 Windows API
或外部 Node 工具。先做好这条完整读取链，不需要引入通用插件加载框架。

## 实施后的验收要求

- 标准包与加密包同内容比较；混合明文/密文/unpacked、空成员、尾部和非对齐读。
- 两个不同 key 的归档交替、并发读取；缓存配置变化；错误公钥、损坏索引、截断内容。
- 64 KiB 范围物理读取量有界，不退回“整成员读取后切片”。
- file URL 的 sniff 和实际响应内容一致；原 CORS 与本地资源规则保持。
- renderer preload/require，以及纳入范围的 main require，均使用实际归档测试。
- 最后使用未转换的 TH/PLE 原包验证启动、登录、片库、播放、弹窗和关闭后重开。

本次原包只读探测记录放在 `out/asar-encryption-evaluation-20260919/`；
没有修改厂商产物或运行时源码，性能结论仅限上述读取量实测。

## 实现（2026-09-19）

- `common/asar` 统一支持标准包和上述加密包。`EntryReader` 返回逻辑明文，
  FS、范围读取、URL MIME sniff 和 DataPipe 使用同一接口；块对齐请求原地解密，
  非对齐请求最多额外读取 30 字节，仅首尾块使用 16 字节临时空间。
  单次物理 I/O 最多 1 MiB，避免超大 unpacked 读取触发 base::File 的 int 长度上限。
  没有完整成员明文缓存。
- `IpcMainConfig.archive_public_keys` 接收可信宿主提供的绝对根路径和公钥 PEM。
  浏览器和 main Utility 使用同份配置，renderer 配置不增加密钥字段。
  公钥不是秘密，但运行时严格拒绝私钥或多个 PEM 块。
- 每包独立解密上下文；按 owner 原子更新或撤销配置。相同配置保持缓存，
  冲突和非法替换保留旧配置；已经打开的 reader 保留原文件快照。
  正在运行的容器拒绝变更公钥，避免浏览器和 Utility 配置不一致。
- main CommonJS 的入口、package.json、JS/JSON 读取支持归档路径，保留模块缓存
  身份；文件、目录及多级链接先解析真实成员路径，再处理缓存和相对依赖。
  原生模块只支持 `.asar.unpacked`，不实现 packed 原生模块自动提取。
- TH/PLE 的接入代码从各自 `resources/app/asar-public-key.pem` 读取公钥，
  按应用资源根注册。产品名称、公钥和安装路径均未写入 ASAR 核心。
- TH 同步脚本默认保留原始插件包；`--asar-format standard` 显式保留旧转换方式。
  两个同步脚本均支持 `--asar-public-key`，在替换运行时前验证并发布公钥。
  PLE 独立 preload 的历史规范化仍保留，它与 ASAR 成员解密不同。
- TH 的 XLLite 3.23.11、XLGame 和 VipPayCenter 2.0.7 与当前包逐成员明文核对
  一致后，恢复原始加密归档。未升级会员插件到 2.0.9。
  当前 PLE 附带的两个 ASAR 原本就是加密包且与 7.1.35 新原包内容不同，保持原版本。
  厂商工程中的源码和发布产物均未修改。

### 验证记录

- Windows Release 浏览器和原生测试编译成功；URL loader 与 manager 测试对象编译成功。
- 原生回归 **233/233** 通过，覆盖 16 项归档测试，以及真实加密 CJS、FS range、
  两个不同密钥交替/并发读取、失败初始化撤销和服务析构撤销。
- JS 回归 **583 通过、2 跳过**。同步工具 Python **24/24** 通过，包含原厂转换
  兼容测试；preload 检查 **7 通过、1 可选跳过**。
- URL loader 新增测试目前只编译；真实浏览器 URL 通路验证另列，不将其算作已执行单测。
- 在新版 9222 浏览器中，7 个真实加密包的 13 个选定成员通过全量 SHA-256、
  偏移 3 的最多 64 KiB 范围以及末尾 19 字节核对，均与原厂解密结果一致。
  renderer FS 和 file URL 均通过，URL 的 MIME 正确；TH main Utility 的相应
  三个插件包读取也通过。两份 7.1.35 原包只复制到构建目录临时验证，已移除。
  提交审查修复后重新编译并执行上述 92 项内容校验，全部通过；新增测试覆盖
  跨物理 I/O 分块，以及从链接别名首次 require 时的相对依赖和缓存身份。
- TH 自动登录、片库 guest 加载正常；PLE 播放错误码为 0，原生统计确认渲染视频帧。
  TH 通过自身 QuitApp 正常退出，退出码 0。桌面工具无法绑定窗口后，用户通过
  原生侧栏操作确认“重开正常，片库正常”；资源页探针不作为侧栏重开的验证依据。
- 核心使用 base/BoringSSL，未新增 Chromium 源码改动或 Windows 专用解密 API。
  本机验证平台为 Windows，未执行 Linux/macOS 构建。

实施记录位于 `out/asar-encryption-implementation-20260919/`。
