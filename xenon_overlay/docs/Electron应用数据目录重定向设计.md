# Electron 应用数据目录

本文说明路径、目录锁、专属 Storage Service、Cookie 密钥与退出顺序的设计边界。
实际读写、命名分区和跨宿主重启的验收状态统一记录在
[Electron原包兼容性验证.md](Electron原包兼容性验证.md)，避免沿用中间排查状态。
macOS/Linux 没有本轮实机结果。

## 路径契约

`app.getPath/setPath('userData' | 'sessionData')` 由 Browser 保存权威状态。
Utility 通过认证的 owner/generation 请求读取或修改，成功结果直接来自 Browser；
不会只改 JavaScript 返回值，也不会覆盖宿主进程全局 PathService。
默认 logs 路径继续使用这份 userData 状态。

新接入应用默认使用宿主 `User Data/ElectronApps/<身份 SHA-256>/UserData` 存放
应用普通文件，避免同名 `config.json` 互相覆盖。身份优先采用 descriptor 的
`storageId`，其次使用宿主声明的稳定非 `default` 容器 ID。裸命令行应用没有固定
容器 ID 时回退到规范化安装路径；显示名和版本不参与。安装目录随版本改变时应
显式声明固定 `storageId`。哈希仅用于安全、可跨平台的目录组件，不声称提供访问
控制；允许 Node 文件系统的应用仍具有其原有文件访问能力。

`userDataPolicy: "legacy-host"` 显式保留原宿主 userData 根目录。现有 TH、PLE、
draw.io、MarkText、Motrix 接入配置采用此策略；行为由配置决定，不按应用名分支。
切换策略或身份不移动、复制、删除已有文件，也不扫描旧配置推测所属应用。
宿主自己的 `--user-data-dir`/登录启动身份独立保存，不被应用 userData 替换。

启动主 JavaScript 前，Browser 在 `MayBlock` 线程创建默认 userData。期间先将
初始化和后续请求排入同一 Mojo pipe，目录准备成功后才绑定 Utility receiver，
保持初始化与 renderer 注册顺序。失败明确终止启动；关闭或替换的 owner 不会被
异步完成回调重新拉起。此过程不创建或冻结应用会话存储。

未显式重定向会话的应用仍沿用原宿主 Profile 和原有应用分区布局。默认 userData
隔离不改变 Cookie、IndexedDB 等现有路径。getPath('sessionData') 返回原默认
应用分区的真实目录，因此可以与 userData 不同。

在首次使用会话前设置新的 userData，未显式设置的 sessionData 跟随它。
显式设置 sessionData 后，两者独立。存储已经初始化时，不允许改变实际会话
目录；同一目录可重复设置。已有显式 sessionData 时，仍可独立更新 userData。

读取路径和构造 JavaScript Session/普通 View 对象不会实例化或冻结存储。
content::GetStoragePartitionPath 的只读查询 使用 Chromium 原有目录算法，
不创建 StoragePartition、不访问磁盘；内存分区返回空路径。该 Chromium 扩展
由 ENABLE_XENON_SERVICE 宏保护。

同步权限检查回调可以读取 app 路径；读取由 Browser 在当前调用栈返回，不依赖
UI 任务队列。此回调内修改数据路径或首次初始化存储会立即收到
ERR_INVALID_STATE，避免相互等待和超时后仍修改路径。已经初始化的存储访问
继续使用现有上下文。

## 校验与首次使用

路径设置在工作线程验证绝对路径、文件/目录类型和已有父目录，解析已有符号
链接并提交规范路径。不创建目标目录；NUL 在构造 FilePath 前拒绝。失败时
原有路径保持不变。原宿主 Profile 的目录及其父/子目录不能另开独立 Profile。
其他应用保留的相同或重叠会话目录会被拒绝。
工作线程同时规范化宿主 Profile、初始 userData 和原 sessionData 的已有祖先，
使用 NormalizeFilePath 解析 Windows junction 和 POSIX 符号链接。宿主本身
通过别名启动也不能绕过重叠检查；默认目录的真实等价路径保持幂等，不另建
Profile。成功提交后保存默认路径身份，读取路径不执行文件系统操作。
Windows/macOS 对尚不存在的路径后缀采用不区分大小写的保守预留规则，
相同和父子路径使用同一规则。它可能在区分大小写的卷上拒绝本可独立的目录，
用于避免大小写别名并发打开同一 Profile；不声称与所有卷的规则精确等价。

首次真实存储操作通过统一 EnsureElectronAppStorageContext 入口。未重定向
时冻结原分区配置；重定向时在工作线程创建目标目录并取得目录内的操作系统
文件锁，随后用 Profile::CreateProfile 的异步模式初始化独立 Profile。调用方
保留 Mojo 回调，等待完成后继续，不嵌套运行 UI 消息循环，也不增加 UI 磁盘
阻塞豁免。Profile 创建失败时允许修正路径后重试。

独立 Profile 的默认会话直接使用指定的 sessionData 根目录，命名持久分区使用
Chromium 分区路径 `Storage/ext/electron/<分区哈希>`，内存分区保持不落盘。
独立 Profile 已隔离应用，命名分区不再重复追加容器哈希，避免不必要的长路径
导致 Windows IndexedDB 数据库创建失败。未重定向应用仍保留原容器哈希路径。
Cookie、网络缓存、LocalStorage、
IndexedDB、CacheStorage、Service Worker 和代码缓存由同一 BrowserContext
原生管理，避免只改网络缓存而使其他存储仍在旧目录。

独立 Profile 在 OnProfileCreationStarted（prefs 和 keyed services 初始化前）
登记专属 Storage Service。DOMStorage 的 LocalStorage/SessionStorage 控制
接口根据所属 BrowserContext 路由；专属服务仍使用 Chromium 原沙箱，文件
系统 broker 只开放该 Profile 根目录。普通浏览器 Profile 沿用全局服务。
Android、single-process 和测试强制进程内模式沿用已有进程内实现。

## Cookie 加密状态

重定向后的 Cookie 加密状态存放在 `sessionData/Local State`，与应用的数据
目录一起保留。不能继续使用宿主全局 Local State 的密钥：同一 Cookie 数据库
在不同 `--user-data-dir` 下打开时，宿主密钥不同，会被当作不可解密数据删除。
普通浏览器 Profile 和未重定向的应用仍使用原宿主 provider。

创建独立 Profile 前，后台严格读取 Local State，使用 PrefService 承载原生
OSCrypt provider 所需偏好。Windows 使用 DPAPI，macOS 使用 Keychain，
Linux 沿用 Chromium 的 Secret Portal/Freedesktop provider、功能开关和
basic 回退策略；本实现不另造密码算法。自定义应用目录不满足 Windows
App-Bound 的默认浏览器数据目录约束，因此不使用宿主 App-Bound 密钥。

取得 Encryptor 后，后台以只读 SQLite 连接验证默认会话及命名持久分区的
已有加密 Cookie，再用 ImportantFileWriter 原子写入 Local State。写入成功
才创建 Profile，并在 keyed services 初始化前安装对应的 Cookie provider。
检查只遍历 Chromium 分区目录层级，不递归扫描整个磁盘缓存。

损坏的 Local State、不正确的偏好类型、系统密钥不可用、缺失或不匹配的
密钥、已有 Cookie 无法解密、状态文件写入失败均使初始化明确失败；不会
重置、改名或回退宿主密钥，也不会把数据库交给会删除解密失败记录的网络
服务。已有加密 Cookie 而 Local State/key 缺失时，新 key 只在内存候选态，
校验失败后丢弃，原数据保留。

保证范围是同一应用 sessionData 在同一操作系统用户下跨宿主目录重启。
这不是任意 Chrome/Electron 旧 Profile 的迁移工具，也不承诺其他可执行文件
生成的 App-Bound v20 密文能迁移；无法用当前原生 provider 解密时明确拒绝。
Local State 与数据文件需要一起保留，跨系统用户或操作系统迁移还受原生
密钥保护机制约束。

## 生命周期与模块边界

`xenon_electron_app_storage` 管理路径、owner 状态、目录预留和文件锁，依赖
base/content 和会话 shutdown notifier。`xenon_electron_app_storage_profile`
单独适配 Chrome Profile 的创建/销毁，通过工厂回调传给注册入口，避免
Profile 与权限/会话模块循环依赖。测试可替换上下文创建器，仍执行真实路径
校验和目录锁状态机。

BrowserWindow、WebContentsView/Guest、session 操作、自定义协议、webRequest
和会话网络工厂使用同一有效上下文。原权限 handler/同步检查管道在上下文
切换时保留，只更新所属上下文，不能重建 owner 丢掉已注册的策略。
窗口和 Guest 使用同一 SiteInstance 创建入口：默认分区走普通 CreateForURL，
命名持久/内存分区走 CreateForFixedStoragePartition，后者不接受默认分区。
renderer 配置读取当前 userData；后续 Worker/Utility 继承快照，不能通过路径
API 获得主进程窗口权限。

退出先撤销 owner 路由、请求和窗口/子进程，再退休存储。目录预留与文件锁
保持到 ProfileDestroyer 等待依赖 renderer 完成、StoragePartition 关闭、专属
Storage Service 的正常退出或崩溃通知以及 IO 队列越过关闭操作。尚未完成的
服务启动也持有退出屏障，直到启动成功或失败；使用 ServiceProcessHost 的
跨平台生命周期通知，不在 UI 阻塞等待进程。旧 generation 的排队成功回调在交付前再次验证有效性，
不能把已退休上下文交给新请求。初始化中退出也走同一路径。文件锁在后台
阻塞序列释放，跨宿主进程不能同时打开相同目录。文件锁依赖操作系统/文件
系统的原生锁语义，不对不支持可靠锁的网络文件系统作额外承诺。

`crashDumps` 仍属于宿主 Crashpad 数据库；本实现没有伪造每应用独立 Crashpad
路径。已有数据没有自动搬迁，指定新目录意味着改用该目录的数据。

## 验证

原生测试覆盖只读路径计算与实际分区路径一致、读取不实例化、旧布局保留、
首次使用冻结、userData/sessionData 独立设置、冲突与失败原子性、创建失败
重试、初始化中退休、排队成功取消以及宿主关闭。
Cookie 测试覆盖独立密钥持久化与隔离、缺失/损坏/错误 key 保留原数据、
默认与命名分区只读检查、不可用 provider、写入失败、普通 Profile 不受影响，
以及 Windows 原生 DPAPI 重建后的实际解密。

实际回归使用外置应用 fixture，在不同宿主 user-data-dir 下共享应用指定目录，
分次关闭/重启验证 Cookie、LocalStorage、IndexedDB、CacheStorage、Service
Worker，覆盖默认/命名持久/内存分区、首次使用后改路径拒绝及独立 userData。
还应确认旧目录没有新会话数据，原发行包无需修改即可跨过 setPath 启动点。
源码编译和原生模拟测试通过，不能替代这些真实持久化及退出重开验证。

product14 的真实 Cookie 专项已完成：继承 userData 与显式 sessionData 各自
写入、换宿主 profile 后读取，共四阶段通过；包含默认、命名持久与内存分区。
另将既有数据复制后分别设置错误密钥、缺失密钥、缺失 Local State、无效密钥、
损坏 Local State，五项均明确失败且默认/命名 Cookie 数据库和 Local State
的前后 SHA-256 未变。九次宿主均退出 0，无归属残留进程或新增 dump。
证据为 `F:/electron-compat-audit/20261111/cookie-storage-probe-product14-01/`
及 `cookie-storage-negative-product14-neg01.json`；负例仅操作专项副本。
