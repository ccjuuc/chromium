# Thunder 2025 Electron 容器接入

## 1. 当前接入方式

侧栏 `TH` 使用容器 `thunder-2025` 运行 Thunder 的原始应用归档和原生 SDK。main 在独立 Utility Process 中执行，由 Thunder main 创建 BrowserWindow；运行页面保持归档内的真实 `file://` URL。`chrome://thunder-2025/` 是侧栏启动入口。

应用目录按发行布局保留，宿主配置和公钥放在目录之外。通用导入命令、外置配置字段和一致性校验见 [Electron 应用目录接入](./电子应用目录接入.md)，进程及 IPC 架构见 [Electron IPC 容器架构与接入](./Electron_IPC容器设计与接入.md)。

容器提供已实现的 Electron/Node API 和兼容 native 的加载能力，不修改 Thunder 的 main、renderer、登录或播放逻辑。

## 2. 版本与制品来源

| 内容 | 当前来源与约束 |
| --- | --- |
| 完整应用主包 | `F:/thunder_2025/bin/Release/resources/app/out.asar`，标准 ASAR；268 个文件与此前已验证构建逐一相同。 |
| Thunder executable、addon、播放器、SDK | `F:/thunder_2025/bin/Release`；当前 executable 版本为 `25.0.80.888`。 |
| 应用元数据 | 保留发布方 `resources/app/package.json`，其中版本仍为 `25.0.0.888`。宿主用 executable 版本作为实际应用版本。 |
| 插件 | 保留当前已验证的完整集合，包括 `XLLite/3.23.11`、游戏、播放器、网盘及 `VipPayCenter2025/2.0.7`。 |

原 `bin/Release` 插件配置声明的 `XLLite/3.23.5` 缺少实际文件；Submodule 的 Release native 也与当前 native 不同。不能直接覆盖为任意一个旧 Release 目录，或因取用 ProductRelease 插件而顺带升级其他插件。更新时应先获得完整、版本匹配的发行制品，再通过通用导入器原样导入。

原 `package.json` 的 `main` 是 `./out/main/index.js`，实际主脚本位于 `out.asar/main.js`。当前保留原 package 字节，通过外置宿主配置覆盖入口，不改写 vendor 源码或归档。

## 3. 当前布局与配置

仓库源资源和最终输出使用同一相对布局：

```text
resources/                         # 构建后对应宿主 exe 所在目录
├─ thunder_2025.xenon.json         # 宿主接入声明
├─ thunder_2025.asar-public-key.pem
└─ thunder_2025/
   ├─ Thunder.exe                 # 应用身份，不单独启动
   ├─ *.node / *.dll
   ├─ player/ / SDK/ / service/ / addins/
   └─ resources/app/
      ├─ package.json             # 保留原发布方内容
      ├─ out.asar                 # main、chunks、preload、renderer、static
      ├─ Release/node_sqlite3.node
      └─ plugins/                 # 包括 ASAR 和原有 companion/unpacked 文件
```

此前拆分的 `out/` 和 `renderer.asar` 已移除，preload 随完整归档保存。不存在 renderer URL 到另一套磁盘目录的产品映射，也不再把加密插件转换成普通 ASAR。

当前 [thunder_2025.xenon.json](../resources/thunder_2025.xenon.json) 声明：

```json
{
  "application": "thunder_2025",
  "executable": "thunder_2025/Thunder.exe",
  "name": "Thunder",
  "versionFromExecutable": true,
  "main": "out.asar/main.js",
  "archivePublicKey": "thunder_2025.asar-public-key.pem"
}
```

`application`、`executable`、公钥路径相对于 JSON 目录。解析后的应用根是 `thunder_2025/resources/app`，因此 `main` 相对于该目录。运行根为 `thunder_2025`，保持 `process.execPath`、`app.getPath('exe')`、原生 DLL 和 SDK 的发行路径语义。

## 4. 启动、关闭与重开

```text
Profile 就绪 → 读取外置配置并注册 thunder-2025
点击 TH → 初始化 Utility → 执行 out.asar/main.js
Thunder main → app.whenReady() → 创建 BrowserWindow
Browser → 创建真实窗口与 WebContents → 激活应用窗口
```

侧栏仅初始化或激活容器，不额外构造 TH 主窗口，也不预先创建播放窗口。`Thunder.exe` 提供应用身份和版本资源，执行 main 的进程仍是 Xenon Utility。

关闭遵循 BrowserWindow 的可取消关闭事件及应用退出语义，不以隐藏窗口替代销毁。正常退出时清理该容器的窗口、Utility 和 native 服务，保留配置以供下次明确打开。服务异常断开后等待用户重新打开，旧页面不能触发隐式重启或反复弹窗。

窗口标题、激活、bounds、最小尺寸、父子关系和原生消息桥均由通用窗口实现处理。页面标题经过可取消的 `page-title-updated` 事件同步至真实 delegate，TH 主窗口因而显示页面声明的“迅雷”。

## 5. 登录、文件来源与 UA

TH 服务身份由接入层声明，当前 UA 格式为：

```text
Thunder/<应用版本> XDASKernel/<应用版本> <Chromium User-Agent>
```

应用版本取上述 executable 版本；main 与 renderer 使用同一容器配置。该身份声明位于 TH 接入点，不写入通用 ASAR、IPC 或 Node 模块实现。

renderer 的应用路径、exe 路径和用户目录来自 Document 级运行配置。用户数据使用 Xenon 用户数据目录，不写入应用发行目录；更换应用文件不应清除登录状态。扫码、账密和自动登录均由 Thunder 自身回调及轮询处理。

片库宿主保持 `file://.../out.asar/...` 来源，相关本地文件访问规则只作用于托管 Electron 窗口及 guest。将该宿主换成 WebUI origin 会改变跨域行为：服务健康接口可以成功，但没有 CORS 许可的首页可能被阻止，表现为片库空白。

## 6. 原生 SDK 和打包

TH 与 PLE 各自使用同版本 addon、`player/`、`SDK/` 和 `containor.dll`，不能因文件同名而互相覆盖。当前 TH addon 位于发行根；原生模块按 executable 拼接的 DLL 路径，在对应应用运行根存在目标时由通用加载桥局部重定向。

播放器子进程仍使用 Xenon 宿主程序，通过继承的 `XENON_HOSTED_APP_DIR` 定位 `thunder_2025/player/containor.dll`；不启动 `Thunder.exe` 作为 player child。

构建和安装必须同时保留 `thunder_2025/`、同级的宿主 JSON 与公钥，不能仅复制归档或仅复制应用目录。用户配置、日志和下载缓存不属于发行输入。

## 7. 检查与回归

只读核对源资源和构建输出：

```powershell
python xenon_overlay/tools/import_electron_app.py `
  --src xenon_overlay/resources/thunder_2025 `
  --out out/Release_64/thunder_2025 --check
```

运行验收包括初始化、自动/扫码登录、片库、播放、游戏弹窗前台激活、拖动/最小尺寸，以及退出后从侧栏重开。定位时依次核对：

1. 外置配置及公钥是否存在，main override 是否指向 `out.asar/main.js`。
2. 日志是否记录 `thunder-2025` 容器成功初始化。
3. 页面是否使用正确 `file://` 归档 URL，renderer endpoint 是否有有效 `window_id`。
4. addon、SDK 和 player child 是否均来自 `thunder_2025`。
5. 正常退出后是否释放旧服务，重开是否建立新的容器生命周期。

旧验证记录保留在对应 `out/` 诊断目录；其中历史拆分路径和旧同步命令不再作为当前接入方式。
