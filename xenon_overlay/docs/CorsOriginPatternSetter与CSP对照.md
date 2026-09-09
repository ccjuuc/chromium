# CorsOriginPatternSetter 与 CSP 对照

本文整理 Chromium 里 WebUI 发起外部网络请求所涉及的三层机制：**网络加载器白名单（Network Loader Allowlist）**、**跨源访问放行（CORS Origin Access List）** 与 **内容安全策略（CSP）**，并以 Xenon 系列 WebUI（`chrome://xenon-player-electron`、`chrome://thunder-2025`、`chrome://xl-player` 等）中的实现为对照。

参考代码：

| 角色 | 路径 |
|------|------|
| 网络加载器白名单 | `chrome/browser/ui/webui/chrome_web_ui_controller_factory.cc` → `IsWebUIAllowedToMakeNetworkRequests()` |
| Xenon 通用 CORS 放行 | `xenon_overlay/chrome/browser/ui/webui/xenon_player_electron_controller.cc` → `AllowHostedCrossOriginAccess()` |
| XL 业务调用（旧） | `xunlei/chrome/browser/ui/webui/xl_player_webui_controller.cc` → `AllowPlayerCrossOriginAccess()` |
| 公共 API | `content/public/browser/cors_origin_pattern_setter.{h,cc}` |
| 扩展同类用法 | `extensions/browser/network_permissions_updater.cc` |
| WebUI CSP API | `content/public/browser/web_ui_data_source.h` |
| 渲染进程网络访问入口 | `content/browser/renderer_host/render_frame_host_impl.cc` → `CommitNavigation` |

---

## 目录

1. [问题场景](#1-问题场景)
2. [三层不同的墙](#2-三层不同的墙)
3. [Network Loader Allowlist（网络加载器白名单）](#3-network-loader-allowlist网络加载器白名单)
4. [`CorsOriginPatternSetter::Set`](#4-corsoriginpatternseterset)
5. [`CorsOriginPattern` 字段](#5-corsoriginpattern-字段)
6. [Xenon 通用写法：`AllowHostedCrossOriginAccess`](#6-xenon-通用写法allowhostedcrossoriginaccess)
7. [XL Player 旧写法（参考）](#7-xl-player-旧写法参考)
8. [WebUI 侧 CSP 配置](#8-webui-侧-csp-配置)
9. [与扩展权限模型的关系](#9-与扩展权限模型的关系)
10. [常见误区](#10-常见误区)
11. [检查清单](#11-检查清单)

---

## 1. 问题场景

`chrome://xl-player` 页面里的业务 JS 会 `fetch` / `XHR` 访问迅雷后端，例如：

```text
https://rcv-xldc.xunlei.com/sync_data
```

此时：

- **页面 Origin**：`chrome://xl-player`
- **请求目标**：`https://*.xunlei.com`（跨源）
- 服务端通常**不返回**可用的 `Access-Control-Allow-Origin`

在标准 Web 安全模型下，浏览器会拦截响应（CORS 失败）。Electron 时代播放器往往通过放宽 webSecurity / 白名单绕过；迁移到 Chromium WebUI 后，应对方式是：

> 对「由 `chrome://xl-player` 发起、目标为 `*.xunlei.com`」的请求，在 **Network Service 的 CORS Origin Access List** 里登记为允许（等价于对该对端跳过 CORS 检查）。

这就是 `content::CorsOriginPatternSetter::Set(...)` 的用途。

---

## 2. 三层不同的墙

WebUI 发起外部网络请求需要同时通过三层检查：

| | **Network Loader Allowlist** | **CORS（跨源资源共享）** | **CSP（内容安全策略）** |
|--|--|--|--|
| **管什么** | WebUI 渲染进程**是否有权访问网络加载器** | 页面 JS 发起的跨源 **网络请求** 能否读到响应 | 页面允许 **加载/执行** 哪些脚本、样式、连接目标等 |
| **失败表现** | 请求直接无法发出（无 network URLLoaderFactory） | Console：CORS / blocked by CORS policy；拿不到 response body | Console：Refused to load/execute … because it violates CSP |
| **配置位置** | `ChromeWebUIControllerFactory::IsWebUIAllowedToMakeNetworkRequests` | Browser 进程 → NetworkContext 的 origin access list | 响应头 / WebUIDataSource 注入的 CSP |
| **本场景 API** | 修改 factory 的 host 白名单 | `CorsOriginPatternSetter::Set` | `WebUIDataSource::OverrideContentSecurityPolicy` / `DisableTrustedTypesCSP` |
| **是否互相替代** | **否** | **否** | **否** |

一句话：

- **Network Loader Allowlist**：让 `chrome://xenon-player-electron` 的渲染进程能触达网络。
- **CORS 白名单**：让 `chrome://xenon-player-electron` 能请求 `https://*.xunlei.com`。
- **CSP**：让页面能加载 `script`、`connect-src` 允许 `https:`/`http:`、是否允许 `unsafe-inline` 等。

三者都要配对好，页面才能既「跑起来」又「打得通接口」。

---

## 3. Network Loader Allowlist（网络加载器白名单）

### 3.1 背景

Chromium 默认**不给** WebUI 渲染进程访问 Network Service 的能力。有 WebUI bindings 的页面只能通过 `WebUIURLLoaderFactory` 加载 `chrome://` 资源。如果不放行，`fetch('https://...')` 会直接失败——连 CORS preflight 都发不出去。

### 3.2 检查入口

在 `RenderFrameHostImpl::CommitNavigation()` 中：

```cpp
// content/browser/renderer_host/render_frame_host_impl.cc
if ((enabled_bindings_.HasAny(kWebUIBindingsPolicySet)) &&
    !GetContentClient()->browser()->IsWebUIAllowedToMakeNetworkRequests(
        subresource_loader_factories_config.origin())) {
  // 不给网络加载器，只用 WebUIURLLoaderFactory
  pending_default_factory = std::move(factory_for_webui);
} else {
  // 放行：给网络加载器
}
```

### 3.3 白名单配置

`ChromeContentBrowserClient` 委托到 `ChromeWebUIControllerFactory`：

```cpp
// chrome/browser/ui/webui/chrome_web_ui_controller_factory.cc
bool ChromeWebUIControllerFactory::IsWebUIAllowedToMakeNetworkRequests(
    const url::Origin& origin) {
  return
      origin.host() == chrome::kChromeUISyncConfirmationHost ||
      origin.host() == chrome::kChromeUIInspectHost ||
      origin.host() == chrome::kChromeUIDownloadsHost ||
      origin.host() == chrome::kChromeUIExtensionsHost ||
      origin.host() == "xenon-login" ||
      origin.host() == "xenon-overlay" ||
      origin.host() == "xenon-player-electron" ||
      origin.host() == "thunder-2025" ||
      origin.host() == "xenon-player" ||
      origin.host() == "xenon-node" ||
      origin.host() == "xenon-ui" ||
      origin.host() == chrome::kChromeUIDrivePickerHostHost;
}
```

### 3.4 已注册的 Xenon hosts

| Host | 用途 |
|------|------|
| `xenon-login` | 登录页 |
| `xenon-overlay` | Overlay 浮层 |
| `xenon-player-electron` | 播放器（Electron 托管） |
| `thunder-2025` | 迅雷 2025 主界面 |
| `xenon-player` | 播放器（旧） |
| `xenon-node` | Node.js 运行时 |
| `xenon-ui` | 开发测试 UI |

### 3.5 新增 WebUI 时

如果新 WebUI 需要访问外部网络（`fetch` 任何 `https://` / `http://`），**必须**把 host 加到此白名单中，否则渲染进程没有网络加载器。

---

## 4. `CorsOriginPatternSetter::Set`

### 4.1 签名

```cpp
// content/public/browser/cors_origin_pattern_setter.h
static void Set(
    content::BrowserContext* browser_context,
    const url::Origin& source_origin,
    std::vector<network::mojom::CorsOriginPatternPtr> allow_patterns,
    std::vector<network::mojom::CorsOriginPatternPtr> block_patterns,
    base::OnceClosure closure);
```

### 4.2 做了什么

`Set` 会把「源 Origin → 允许/禁止的目标模式」同时写入两处：

1. **`BrowserContext::GetSharedCorsOriginAccessList()`**  
   进程内共享列表；Network Service 重启后可据此恢复。
2. **当前已加载的每个 `StoragePartition` 的 `NetworkContext`**  
   调用 `SetCorsOriginAccessListsForOrigin`，真正影响线上请求判定。

`closure` 在两处都 ack 后调用（内部用 `BarrierClosure(2, ...)`）。不关心完成时机时可传 `base::DoNothing()`。

### 4.3 语义（直观理解）

对 **`source_origin` 发起的请求**，若目标匹配某个 `allow_patterns` 且不被 `block_patterns` 命中，则 Network Service 按 **CORS origin access allowlist** 处理——业务上等价于「对该目标放行跨源读响应」，无需服务端 CORS 头。

这不是把整个浏览器的 webSecurity 关掉，而是 **按「源 Origin + 目标 pattern」精细放行**。

---

## 5. `CorsOriginPattern` 字段

构造（Mojo）：

```cpp
network::mojom::CorsOriginPattern::New(
    protocol,   // 如 "https" / "http"
    domain,     // 如 "xunlei.com"
    port,       // 0 表示配合 PortMatchMode
    CorsDomainMatchMode,   // 是否含子域
    CorsPortMatchMode,     // 是否任意端口
    CorsOriginAccessMatchPriority);
```

常用组合（XL Player）：

| 字段 | 取值 | 含义 |
|------|------|------|
| protocol | `"https"` / `"http"` | 协议 |
| domain | `"xunlei.com"` | 主域 |
| port | `0` | 配合 `kAllowAnyPort` |
| DomainMatchMode | `kAllowSubdomains` | 匹配 `*.xunlei.com` 与 `xunlei.com` |
| PortMatchMode | `kAllowAnyPort` | 任意端口 |
| Priority | `kDefaultPriority` | 默认优先级 |

`block_patterns` 为空即可（只允许、不额外拉黑）。

---

## 6. Xenon 通用写法：`AllowHostedCrossOriginAccess`

`XenonPlayerElectronController` 中将 CORS 放行抽象为通用函数，支持任意 `webui_host` + 任意 `cors_domains`：

```cpp
// xenon_player_electron_controller.cc
void AllowHostedCrossOriginAccess(
    content::BrowserContext* browser_context,
    std::string_view webui_host,
    const std::vector<std::string>& allowed_domains) {
  if (!browser_context) {
    return;
  }
  const GURL player_url(base::StringPrintf("%s://%s", content::kChromeUIScheme,
                                           webui_host.data()));
  const url::Origin source_origin = url::Origin::Create(player_url);

  auto make_pattern = [](const char* protocol, const char* domain) {
    return network::mojom::CorsOriginPattern::New(
        protocol, domain, /*port=*/0,
        network::mojom::CorsDomainMatchMode::kAllowSubdomains,
        network::mojom::CorsPortMatchMode::kAllowAnyPort,
        network::mojom::CorsOriginAccessMatchPriority::kDefaultPriority);
  };

  std::vector<network::mojom::CorsOriginPatternPtr> allow_patterns;
  for (const std::string& domain : allowed_domains) {
    allow_patterns.push_back(make_pattern("https", domain.c_str()));
    allow_patterns.push_back(make_pattern("http", domain.c_str()));
  }

  content::CorsOriginPatternSetter::Set(
      browser_context, source_origin, std::move(allow_patterns),
      /*block_patterns=*/{}, base::DoNothing());
}
```

调用时机：`XenonPlayerElectronController` 构造函数中，在创建 `WebUIDataSource` **之前**调用：

```cpp
// xenon_player_electron_controller.cc — 构造函数
content::BrowserContext* const browser_context =
    web_ui->GetWebContents()->GetBrowserContext();
AllowHostedCrossOriginAccess(browser_context, webui_host, cors_domains);
```

### 多产品复用

`XenonPlayerElectronController` 的构造函数接受 `webui_host` 和 `cors_domains` 参数，不同产品通过不同的 `WebUIConfig` 传入：

| 产品 | Host | `cors_domains` | Config 入口 |
|------|------|----------------|-------------|
| 播放器 | `xenon-player-electron` | `{"xunlei.com"}` | `XenonPlayerElectronController` 默认构造 |
| 迅雷 2025 | `thunder-2025` | `{"xunlei.com"}` | `XenonThunder2025Config::CreateWebUIController()` |

`XenonThunder2025Config` 的复用方式：

```cpp
// xenon_thunder_2025_controller.cc
std::unique_ptr<content::WebUIController>
XenonThunder2025Config::CreateWebUIController(content::WebUI* web_ui,
                                              const GURL& url) {
  return std::make_unique<XenonPlayerElectronController>(
      web_ui, kThunder2025Host, kThunderFrontendDirSwitch,
      "thunder_2025/resources/app/renderer.asar/main-renderer",
      std::vector<std::string>{"xunlei.com"});
}
```

效果对照：

| 项目 | 播放器 | 迅雷 2025 |
|------|--------|-----------|
| source | `chrome://xenon-player-electron` | `chrome://thunder-2025` |
| allow | `http(s)://*.xunlei.com`（含子域、任意端口） | `http(s)://*.xunlei.com`（含子域、任意端口） |
| block | 无 | 无 |

---

## 7. XL Player 旧写法（参考）

`AllowPlayerCrossOriginAccess()` 是早期 XL Player 的写法，host 和 domain 写死：

```cpp
// xl_player_webui_controller.cc
void AllowPlayerCrossOriginAccess(content::BrowserContext* browser_context) {
  const GURL player_url(base::StringPrintf(
      "%s://%s", content::kChromeUIScheme, chrome::kChromeUIXLPlayerHost));
  const url::Origin source_origin = url::Origin::Create(player_url);

  auto make_pattern = [](const char* protocol, const char* domain) {
    return network::mojom::CorsOriginPattern::New(
        protocol, domain, /*port=*/0,
        network::mojom::CorsDomainMatchMode::kAllowSubdomains,
        network::mojom::CorsPortMatchMode::kAllowAnyPort,
        network::mojom::CorsOriginAccessMatchPriority::kDefaultPriority);
  };

  std::vector<network::mojom::CorsOriginPatternPtr> allow_patterns;
  allow_patterns.push_back(make_pattern("https", "xunlei.com"));
  allow_patterns.push_back(make_pattern("http", "xunlei.com"));

  content::CorsOriginPatternSetter::Set(
      browser_context, source_origin, std::move(allow_patterns),
      /*block_patterns=*/{}, base::DoNothing());
}
```

与新写法的区别：新的 `AllowHostedCrossOriginAccess` 将 `webui_host` 和 `allowed_domains` 参数化，可被多个产品复用。

---

## 8. WebUI 侧 CSP 配置

同一文件里，CORS 之外还有 CSP（解决「脚本能不能跑」「能连谁」）。

`XenonPlayerElectronController` 的完整 CSP 配置：

```cpp
// xenon_player_electron_controller.cc — 构造函数
source->DisableTrustedTypesCSP();
source->OverrideContentSecurityPolicy(
    network::mojom::CSPDirectiveName::ScriptSrc,
    "script-src 'self' chrome://resources 'unsafe-eval' 'unsafe-inline';");
source->OverrideContentSecurityPolicy(
    network::mojom::CSPDirectiveName::StyleSrc,
    "style-src 'self' chrome://resources 'unsafe-inline';");
source->OverrideContentSecurityPolicy(
    network::mojom::CSPDirectiveName::ConnectSrc,
    "connect-src 'self' https: http: data: blob:;");
source->OverrideContentSecurityPolicy(
    network::mojom::CSPDirectiveName::ImgSrc,
    "img-src 'self' https: http: data: blob:;");
source->OverrideContentSecurityPolicy(
    network::mojom::CSPDirectiveName::MediaSrc,
    "media-src 'self' https: http: data: blob: file:;");
source->OverrideContentSecurityPolicy(
    network::mojom::CSPDirectiveName::FontSrc,
    "font-src 'self' data:;");
```

### 各指令说明

| CSP 指令 | 配置值 | 作用 |
|----------|--------|------|
| `DisableTrustedTypesCSP()` | — | 关闭 Trusted Types 强制；Vue/Vite 会对 `innerHTML` 建 policy，不关则可能白屏 |
| `script-src` | `'self' chrome://resources 'unsafe-eval' 'unsafe-inline'` | 允许本源脚本、chrome://resources、eval（Node.js 兼容）、inline script |
| `style-src` | `'self' chrome://resources 'unsafe-inline'` | 允许本源和 inline 样式 |
| `connect-src` | `'self' https: http: data: blob:` | **关键**：允许 fetch/XHR 连接到任意 https/http 目标 |
| `img-src` | `'self' https: http: data: blob:` | 允许加载外部图片 |
| `media-src` | `'self' https: http: data: blob: file:` | 允许加载外部媒体（含本地 file:） |
| `font-src` | `'self' data:` | 允许 data URI 字体（内嵌字体） |

### 三层配合

```text
请求 chrome://xenon-player-electron → https://rcv-xldc.xunlei.com/sync_data

  ① IsWebUIAllowedToMakeNetworkRequests("xenon-player-electron") → true
     → 渲染进程获得 Network Service URLLoaderFactory

  ② CorsOriginPatternSetter::Set(..., "xunlei.com", ...)
     → Network Service 跳过 CORS 检查

  ③ CSP connect-src: 'self' https: http: ...
     → 浏览器允许发起此连接

  → 请求成功 ✓
```

注意：

- 只改 CSP **不会**让跨源 XHR 自动成功（仍需 CORS 放行）。
- 只配 CORS 白名单 **不会**让违规脚本突然可执行（CSP 独立检查）。
- 不加 Network Loader Allowlist，前两者都无从谈起（没有网络加载器）。

---

## 9. 与扩展权限模型的关系

扩展更新 host permissions 时，同样走 `CorsOriginPatternSetter::Set`：

```cpp
// extensions/browser/network_permissions_updater.cc
content::CorsOriginPatternSetter::Set(
    browser_context, extension.origin(), mojo::Clone(allow_patterns),
    mojo::Clone(block_patterns), barrier_closure);
```

| | 扩展 | XL Player WebUI |
|--|--|--|
| source_origin | `chrome-extension://<id>/` | `chrome://xl-player` |
| pattern 来源 | manifest / 用户授权的 host permissions | 代码写死的 `*.xunlei.com` |
| 目的 | 扩展跨站请求合法化 | WebUI 业务请求合法化 |

同一套 Network Service 机制，不同产品入口。

---

## 10. 常见误区

1. **「关掉 CSP 就等于放开 CORS」** — 错。CSP 管加载/执行，CORS 管跨源读响应。  
2. **「配了 CORS 白名单就够了」** — 错。如果 host 不在 `IsWebUIAllowedToMakeNetworkRequests` 白名单中，渲染进程根本没有网络加载器。  
3. **「在页面里设 `Access-Control-Allow-Origin: *`」** — 无效。CORS 响应头必须由**目标服务器**返回；浏览器侧要用 Origin Access List 或代理。  
4. **只配了 https、漏了 http** — 开发/内网若走 http 仍会失败；Xenon 两边都加了。  
5. **source_origin 写错 host** — 必须以真实 WebUI host 为准，和地址栏一致。  
6. **认为 `Set` 是同步立刻全局生效** — 会推到各 StoragePartition；一般构造期调用 + `DoNothing` 足够；若紧挨着发请求且偶发失败，再等 `closure`。  
7. **用 `DisableWebSecurity` 命令行代替** — 过宽、不安全、不适合产品路径；应用 Origin Access List。  
8. **CSP `connect-src` 漏了 `https:` / `http:`** — 即使 CORS 白名单已配，CSP 仍会拦截 `fetch`。

---

## 11. 检查清单

接入类似「WebUI → 自有后端」跨源能力时：

- [ ] 明确 **source Origin**（`chrome://...` / `chrome-extension://...`）
- [ ] 在 `ChromeWebUIControllerFactory::IsWebUIAllowedToMakeNetworkRequests` 中**注册 host**
- [ ] 列出最小必要的 **目标 domain + scheme**（优先子域模式，避免 `*` 全网）
- [ ] 调用 `AllowHostedCrossOriginAccess` 或 `CorsOriginPatternSetter::Set`（BrowserContext 非空）
- [ ] CSP `connect-src` 包含 `https:` / `http:`（按需）
- [ ] 单独检查 **CSP / Trusted Types** 是否挡住页面启动
- [ ] DevTools：Network 里确认不再是 CORS error；Console 无 CSP refuse
- [ ] 不把 CORS 放行当成通用「关闭浏览器安全」

---

## 延伸阅读

- `content/public/browser/cors_origin_pattern_setter.h` 注释（Shared list + NetworkContext 双写）
- `content/browser/loader/cors_origin_pattern_setter_browsertest.cc`（行为用例）
- `content/browser/renderer_host/render_frame_host_impl.cc` → `CommitNavigation`（网络加载器分配逻辑）
- `chrome/browser/ui/webui/chrome_web_ui_controller_factory.cc` → `IsWebUIAllowedToMakeNetworkRequests`（host 白名单）
- `services/network/public/mojom/cors_origin_pattern.mojom`（pattern 定义）
- MDN：[CORS](https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS)、[CSP](https://developer.mozilla.org/en-US/docs/Web/HTTP/CSP)
