# `network::mojom::NetworkService`：Browser 侧用法与接口说明

本文档说明 Chromium 中 **Browser 进程**如何通过 **`network::mojom::NetworkService`** 与 **网络服务进程**（或进程内网络线程上的同一套实现）交互：各**主要 Mojo 方法**的**用途**、**典型使用场景**，以及在 **Chrome / Content** 中的**示例落点**（路径以本仓库为准，升级版本后行号可能变化，请以符号搜索为准）。

- **Mojo 定义**：`services/network/public/mojom/network_service.mojom`
- **C++ 实现类**：`services/network/network_service.h` / `network_service.cc`
- **Browser 获取入口**：`content/public/browser/network_service_instance.h`（`GetNetworkService()`、`CreateNetworkContextInNetworkService()`）

---

## 1. 信任模型与线程

`NetworkService` 在 mojom 中注明为 **仅 Browser 进程应持有的 trusted 接口**，不得下发给 Renderer 等不可信进程；实现跑在带 **Network Sandbox** 的服务端。

- **`GetNetworkService()`**：须在 **UI 线程**调用（见 `network_service_instance.h` 注释）。
- **绝大多数「改全局网络行为」的调用**：同样约定在 Browser 主流程（多为 UI 线程）发起。

---

## 2. Browser 侧两条主路径

### 2.1 取 `NetworkService` 指针（Mojo Remote 的封装）

**用途**：确保网络服务已连接；对 **全局** API（DNS、QUIC、HTTP Auth、端口策略等）发消息。

```cpp
// content/public/browser/network_service_instance.h
network::mojom::NetworkService* network_service = content::GetNetworkService();
network_service->DisableQuic();  // 示例：见下文系统 NetworkContext 管理
```

### 2.2 创建 `NetworkContext`（真正承载 URL 栈的单位）

**用途**：为 Profile / StoragePartition / 系统 context 等创建**隔离**的一套网络环境（Cookie、缓存路径、证书、**代理管道**等均通过 `NetworkContextParams` 区分）。

**推荐 API**：`content::CreateNetworkContextInNetworkService(receiver, params)`  
内部会补全缓存路径等，再调用：

```cpp
// content/browser/network_service_instance_impl.cc（逻辑摘要）
GetNetworkService()->CreateNetworkContext(std::move(context), std::move(params));
```

**Chrome 示例**：系统级 context 创建（含 `SetMaxConnectionsPerProxyChain`、`CreateNetworkContextInNetworkService` 等一串启动配置）：

- `chrome/browser/net/system_network_context_manager.cc`（搜索 `CreateNetworkContextInNetworkService`）

**说明**：日常 **HTTP(S) 请求**不通过「每个请求调 `NetworkService`」完成，而是使用已创建的 **`NetworkContext::CreateURLLoaderFactory`** 等；`NetworkService` 更多承担 **全局开关** 与 **建新 Context**。

---

## 3. 接口分组：用途 · 场景 · 示例

下表以 **`network_service.mojom` 中 `interface NetworkService`** 为主线；带 `[EnableIf=...]` 的条目仅部分平台/构建存在。

### 3.1 初始化与生命周期

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **SetParams** | 传入 `NetworkServiceParams`，初始化连接类型、环境变量、默认 URLLoader 观察者优先级、FPS 开关、系统 DNS 远程等 | 网络进程启动后首次配置 | `SystemNetworkContextManager` 在 (Re)Start 流程中组装并调用（搜索 `SetParams`） |
| **CreateNetworkContext** | 绑定一个新的 `NetworkContext` + `NetworkContextParams` | 每个 Profile / 分区 / 系统 context | `content::CreateNetworkContextInNetworkService`；Chrome：`system_network_context_manager.cc` |

### 3.2 调试与观测（NetLog / SSL Key Log / 流量统计）

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **StartNetLog** | 将 NetLog 事件写入文件 | 用户/开发者打开 `chrome://net-export` 或诊断采集 | Chrome net 导出路径（搜索 `StartNetLog`） |
| **AttachNetLogProxy** | 外挂 NetLog 源与 sink | 多进程合并 NetLog、自定义观察者 | 少见，测试与特殊集成 |
| **SetSSLKeyLogFile** | TLS 密钥日志（须在 TLS 连接建立前） | Wireshark 解密 HTTPS 排障 | 开发者工具/实验开关相关路径 |
| **EnableDataUseUpdates** | 是否让 URLLoader 通过 observer 上报数据用量 | **任务管理器**需要网络读写字节 | `chrome/browser/task_manager/sampling/task_manager_impl.cc`：`StartUpdating()` / `StopUpdating()` |

**任务管理器示例**：

```cpp
// chrome/browser/task_manager/sampling/task_manager_impl.cc
content::GetNetworkService()->EnableDataUseUpdates(true);   // StartUpdating
content::GetNetworkService()->EnableDataUseUpdates(false);  // StopUpdating
```

### 3.3 DNS / 解析行为

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **ConfigureStubHostResolver** | 配置 stub 解析器、DoH、`SecureDnsMode`、Happy Eyeballs V3、额外记录类型、fallback DoH 等 | 用户在设置里改「使用安全 DNS」、策略下发、企业托管 | `chrome/browser/net/stub_resolver_config_reader.cc` |
| **GetDnsConfigChangeManager** | 取 `DnsConfigChangeManager`，可请求 DNS 配置变化通知 | 需要在 **系统 DNS 变更** 时重试探测/逻辑 | `chrome/browser/intranet_redirect_detector.cc`：`SetupDnsConfigClient()` |
| **SetIPv6ReachabilityOverride** | 覆盖 IPv6 可达性逻辑（是否仍查询 AAAA） | 策略或实验开关与 `net::features::kEnableIPv6ReachabilityOverride` | `system_network_context_manager.cc`：`UpdateIPv6ReachabilityOverrideEnabled()` |

**Secure DNS / Stub Resolver 示例**：

```cpp
// chrome/browser/net/stub_resolver_config_reader.cc
content::GetNetworkService()->ConfigureStubHostResolver(
    GetInsecureStubResolverEnabled(), GetHappyEyeballsV3Enabled(),
    secure_dns_mode, doh_config, additional_dns_query_types_enabled,
    fallback_doh_nameservers);
```

**DNS 配置变更通知示例**：

```cpp
// chrome/browser/intranet_redirect_detector.cc
mojo::Remote<network::mojom::DnsConfigChangeManager> manager_remote;
content::GetNetworkService()->GetDnsConfigChangeManager(
    manager_remote.BindNewPipeAndPassReceiver());
manager_remote->RequestNotifications(...);
```

### 3.4 传输与连接策略

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **DisableQuic** | **全局**关闭 QUIC，且 **不可恢复** | 企业策略、稳定性开关、排障对比 HTTP/2 | `system_network_context_manager.cc`：`SystemNetworkContextManager::DisableQuic()` |
| **SetMaxConnectionsPerProxyChain** | 限制单条代理链并发连接数（带饱和裁剪） | 与 `prefs::kMaxConnectionsPerProxy` 等 pref 对齐 | `system_network_context_manager.cc`：网络服务 (Re)Start 流程中 |
| **SetTLS13EarlyDataEnabled** | 全局 TLS 1.3 0-RTT（Early Data） | Feature + 托管 pref `prefs::kTLS13EarlyDataEnabled` | `system_network_context_manager.cc`：`UpdateTLS13EarlyDataEnabled()` |

**禁用 QUIC 示例**：

```cpp
// chrome/browser/net/system_network_context_manager.cc
content::GetNetworkService()->DisableQuic();
```

**Early Data 示例**：

```cpp
// chrome/browser/net/system_network_context_manager.cc
content::GetNetworkService()->SetTLS13EarlyDataEnabled(value);
```

### 3.5 HTTP 认证（全局）

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **SetUpHttpAuth** | **一次性的**静态 HTTP 认证参数（如 GSSAPI 库路径） | 网络服务创建 **任何** `NetworkContext` **之前**必须完成 | `system_network_context_manager` 启动序列（搜索 `SetUpHttpAuth`） |
| **ConfigureHttpAuthPrefs** | **可多次**更新的动态参数（允许的方案、NTLM/Kerberos 白名单等） | Local State / Policy 变更后同步到网络进程 | `system_network_context_manager.cc`：`OnAuthPrefsChanged` |

**动态 HTTP Auth 示例**：

```cpp
// chrome/browser/net/system_network_context_manager.cc
void OnAuthPrefsChanged(PrefService* local_state, const std::string& pref_name) {
  auto params = CreateHttpAuthDynamicParams(local_state);
  OnNewHttpAuthDynamicParams(params);
  content::GetNetworkService()->ConfigureHttpAuthPrefs(std::move(params));
}
```

### 3.6 网络状态、枚举、资源压力

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **GetNetworkChangeManager** | 网络在线/类型等变化 | 需要与 Network Service 侧 NQE/连接类型对齐的模块 | 测试与专用集成，如 `sandboxed_network_change_notifier_win_browsertest.cc` |
| **GetNetworkQualityEstimatorManager** | NQE 管理端点 | 网络质量估计、自适应 | 较少直接从 Browser 调，多通过封装类 |
| **GetNetworkList** | 枚举网卡等信息 | **WebRTC** 枚举设备、网络诊断 | `chrome/browser/media/webrtc/webrtc_text_log_handler.cc` 等 |
| **OnMemoryPressure** | 将 Browser 汇总的内存压力级别传给网络服务；**不**在单一函数里统一「删缓存」，而是广播给各 `MemoryPressureListener` | 系统/浏览器判定低内存时 | 见下文 **「OnMemoryPressure 与网络侧回收」**；Browser：`content/browser/network_service_client.cc`；测试可搜 `OnMemoryPressure` |
| **OnApplicationStateChange** (Android) | 应用前后台 | 省电、暂停/恢复网络活动 | Android Browser 集成 |

#### OnMemoryPressure 与网络侧回收

**压力从哪来**  
级别为 `base::MemoryPressureLevel`（`NONE` / `MODERATE` / `CRITICAL`），由 **Browser 进程**的内存压力监测（如 `base::MemoryPressureMonitor`，各平台信号不同）统一产生。`NetworkServiceClient` 继承 **`base::MemoryPressureListener`**，在回调里转发：

```cpp
// content/browser/network_service_client.cc
void NetworkServiceClient::OnMemoryPressure(
    base::MemoryPressureLevel memory_pressure_level) {
  GetNetworkService()->OnMemoryPressure(memory_pressure_level);
}
```

**NetworkService 做什么**  
只是把同一级别 **投递**到网络进程主线程上的 **`MemoryPressureListenerRegistry::NotifyMemoryPressure`**，由**已注册**的监听器分别响应；**没有**在此处逐类型写死「清 HTTP 磁盘缓存第几块」之类的逻辑：

```cpp
// services/network/network_service.cc
void NetworkService::OnMemoryPressure(
    base::MemoryPressureLevel memory_pressure_level) {
  base::SingleThreadTaskRunner::GetMainThreadDefault()->PostTask(
      FROM_HERE,
      base::BindOnce(
          &base::MemoryPressureListenerRegistry::NotifyMemoryPressure,
          memory_pressure_level));
}
```

**常见「收缩」行为（举例）**

| 监听方（举例） | 行为概要 |
|----------------|----------|
| **`net::HttpNetworkSession`**（`kHttpNetworkSession`） | 在 `MODERATE` / `CRITICAL` 下调用 **`CloseIdleConnections("Low memory")`**：关闭空闲 socket、减轻连接池占用；**不等于**整库删除磁盘 blockfile 缓存。 |
| **`disk_cache::MemBackendImpl`**（内存型缓存后端） | **`MODERATE`**：LRU 驱逐直到约 **`max_size_ / 2`**；**`CRITICAL`**：直到约 **`max_size_ / 10`**（对未使用条目 `Doom`）。 |
| **`services/network` 内其它注册项** | 如 Shared Dictionary、SlopBucket 等各自实现裁剪或回收（见对应源文件中的 `MemoryPressureListener`）。 |

**注意**：桌面 Chrome 常用的 **磁盘 blockfile HTTP 缓存**未必与上述 `MemBackendImpl` 同路径响应压力；不宜把「调用 `OnMemoryPressure`」理解为**必然**按比例缩小磁盘缓存目录。更准确说法是：**网络进程内所有订阅了内存压力的模块按自身策略释放内存或连接**。

#### 与 Renderer / 多标签内存的关系

- **`GetNetworkService()->OnMemoryPressure` 只驱动网络服务进程内的回收**（空闲连接、`MemBackendImpl` 等），**不会**直接释放 **Renderer 进程**里「10 个页面」的大头内存（DOM、V8 堆、排版树、大量 Blink 对象等）。
- **同一轮系统/浏览器内存紧张**时，Browser 里还会有 **其它**策略与监听器并行工作（例如 **标签丢弃**、**页面冻结**后的 Blink 内存清理、Android 上向子进程发压力信号等），那些才更直接作用于「多标签」。
- **对用户可见的影响**：网络侧回收可能导致 **空闲连接被关**、**网络进程内缓存被赶走**，以后续请求 **重连/重拉** 为主；**一般不会**只因这一项而让已渲染好的页面整页「空白」，除非你同时命中了丢弃标签、或其它 Browser/Renderer 策略。

更完整的 Renderer 侧机制见同目录 **`renderer_memory_reclaim.zh-CN.md`**。

### 3.7 证书与信任存储

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **OnTrustStoreChanged** | 用户/系统信任库变更 | 安装/删除根证书后轮询验证状态 | Chrome 证书/信任存储观察者路径 |
| **OnClientCertStoreChanged** | 客户端证书存储变更 | 智能卡、企业证书刷新 | 客户端证书相关模块 |
| **UpdateKeyPinsList** | 更新 HPKP/Key Pinning 列表及更新时间 | 组件或策略更新 pin 列表 | PKI 元数据组件路径 |
| **SetEncryptionKey** | 将 AES 密钥交给网络服务（与 OSCrypt 等配合） | **进程外**网络服务需与 Browser 加密状态一致 | `system_network_context_manager.cc`：非 Windows 下 `SetEncryptionKey(OSCrypt::GetRawEncryptionKey())` |

### 3.8 Web 平台与安全特性（部分为条件编译）

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **SetTrustTokenKeyCommitments** | Trust Token 发行方 commitments | 组件更新、测试 | `trust_token_key_commitments_component_installer.cc` |
| **SetFirstPartySets** | First-Party Sets 全局数据（通常仅首次生效） | FPS 企业策略/组件 | FPS 初始化路径 |
| **SetTpcdMetadataGrants** | TPCD metadata 与 Browser 同步 | 隐私分区相关豁免列表 | `chrome/browser/tpcd/metadata/manager_factory.cc` |
| **ConfigureSCTAuditing / ClearSCTAuditingCache / UpdateCtLogList / SetCtEnforcementEnabled** 等 | 证书透明度审计、日志列表 | 企业策略、组件更新 | `chrome/browser/ssl/sct_reporting_service.cc` 等 |

### 3.9 Browser 特权与工具类 API

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **SetRawHeadersAccess** | 按 **process_id** 授予在特定 origin 上读取**原始响应头** | **DevTools**、受控调试场景（提高被入侵 renderer 风险） | DevTools / 策略路径 |
| **ParseHeaders** | Browser 侧解析「不可信或非网络进程」来的响应头 | WebBundle、扩展、某些导航路径 | 导航与资源加载桥接 |
| **SetNetworkAnnotationMonitor** | 注册注解监控，用于审计带 `NetworkTrafficAnnotation` 的请求 | 企业合规、内部诊断 | 搜索 `SetNetworkAnnotationMonitor` |
| **SetExplicitlyAllowedPorts** | 显式允许通常禁用的端口 | 用户/策略扩展合法端口 | `system_network_context_manager.cc`：`UpdateExplicitlyAllowedNetworkPorts()` |

**显式允许端口示例**：

```cpp
// chrome/browser/net/system_network_context_manager.cc
content::GetNetworkService()->SetExplicitlyAllowedPorts(
    ConvertExplicitlyAllowedNetworkPortsPref(local_state_));
```

| **InterceptUrlLoaderForBodyDecoding** / **DecodeContentEncoding** | 在 Browser 侧对 body 做解码或桥接管道 | 下载、Signed Exchange、DevTools 需看解码后 body | 下载与 DevTools 路径 |
| **AddDurableMessageCollector** | DevTools「持久化消息体」收集 | 网络面板等 | DevTools durable message 相关 |

### 3.10 平台相关与测试

| 方法 | 用途 | 典型场景 | 示例 / 代码落点 |
|------|------|----------|-----------------|
| **SetGssapiLibraryLoadObserver** (Linux) | GSSAPI 库即将加载前通知 | Kerberos/协商与沙箱权衡 | `system_network_context_manager.cc`（Linux `gssapi_library_loader_observer_`） |
| **DumpWithoutCrashing** (Android) | 网络进程侧记录 dump | 定位卡死/挂起 | Android 调试（mojom 标注待移除） |
| **BindTestInterfaceForTesting** | 绑定 `NetworkServiceTest` | 单元测试 / 浏览器测试 | 大量 `*_browsertest.cc` |

---

## 4. 与代理（Proxy）的关系——为何很少直接动 `NetworkService`

用户侧代理主要走：

1. **Browser**：`PrefProxyConfigTrackerImpl` 等把 pref 变成 `net::ProxyConfig`；
2. **`ProxyConfigMonitor`**：持有各 `NetworkContext` 对应的 **`Remote<ProxyConfigClient>`**；
3. **网络进程**：`ProxyConfigServiceMojo` 实现 `ProxyConfigClient::OnProxyConfigUpdated`。

因此：**改代理**通常 **不**需要调用 `NetworkService` 上的某个「SetProxy」——而是更新 **Profile prefs** 或 **NetworkContextParams 里的管道**，由现有链路推送。全局方法里与代理「间接」相关的可能是 **QUIC**、**每链连接数** 等。

---

## 5. 小结

| 如果你想…… | 更可能用到的 API |
|------------|------------------|
| 让浏览器能上网、按 Profile 隔离 | **`CreateNetworkContextInNetworkService` + `NetworkContextParams`** |
| 关 QUIC / 改 DNS DoH / 改 HTTP Auth / 改端口与 TLS Early Data | **`GetNetworkService()->…`**（见上表） |
| 任务管理器看网络流量 | **`EnableDataUseUpdates`** |
| 监听系统 DNS 配置变化 | **`GetDnsConfigChangeManager`** |
| 用户改代理 / Leaf override | **Pref + `ProxyConfigMonitor`**，而非直接 `NetworkService` |

---

## 6. 维护说明

- 本文档依据 **`services/network/public/mojom/network_service.mojom`** 的方法列表整理；新增 Mojo 方法请以该文件为准。
- 文中 Chrome 路径均为 **引用示例**；若合并上游后行号偏移，请用 **符号搜索**（如 `ConfigureStubHostResolver`、`DisableQuic`）定位。

## 7. 相关文档

- **Renderer / 多标签内存回收**：`docs/renderer_memory_reclaim.zh-CN.md`
