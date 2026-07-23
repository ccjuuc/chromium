# CorsOriginPatternSetter 与 CSP 对照

本文整理 Chromium 里 **跨源访问放行（CORS Origin Access List）** 与 **内容安全策略（CSP）** 两套机制的差异与用法，并以 XL Player WebUI（`chrome://xl-player`）中的实现为对照。

参考代码：

| 角色 | 路径 |
|------|------|
| XL 业务调用 | `xunlei/chrome/browser/ui/webui/xl_player_webui_controller.cc` → `AllowPlayerCrossOriginAccess()` |
| 公共 API | `content/public/browser/cors_origin_pattern_setter.{h,cc}` |
| 扩展同类用法 | `extensions/browser/network_permissions_updater.cc` |
| WebUI CSP API | `content/public/browser/web_ui_data_source.h` |

---

## 目录

1. [问题场景](#1-问题场景)
2. [CORS 与 CSP：两层不同的墙](#2-cors-与-csp两层不同的墙)
3. [`CorsOriginPatternSetter::Set`](#3-corsoriginpatternseterset)
4. [`CorsOriginPattern` 字段](#4-corsoriginpattern-字段)
5. [XL Player 实际写法](#5-xl-player-实际写法)
6. [WebUI 侧 CSP 配置](#6-webui-侧-csp-配置)
7. [与扩展权限模型的关系](#7-与扩展权限模型的关系)
8. [常见误区](#8-常见误区)
9. [检查清单](#9-检查清单)

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

## 2. CORS 与 CSP：两层不同的墙

| | **CORS（跨源资源共享）** | **CSP（内容安全策略）** |
|--|--|--|
| **管什么** | 页面 JS 发起的跨源 **网络请求** 能否读到响应 | 页面允许 **加载/执行** 哪些脚本、样式、连接目标等 |
| **失败表现** | Console：CORS / blocked by CORS policy；业务拿不到 response body | Console：Refused to load/execute … because it violates CSP |
| **配置位置** | Browser 进程 → NetworkContext 的 origin access list | 响应头 / WebUIDataSource 注入的 CSP |
| **本场景 API** | `CorsOriginPatternSetter::Set` | `WebUIDataSource::OverrideContentSecurityPolicy` / `DisableTrustedTypesCSP` |
| **是否互相替代** | **否** | **否** |

一句话：

- **CORS 白名单**：让 `chrome://xl-player` 能请求 `https://*.xunlei.com`。
- **CSP**：让页面能加载自己的 `script`、是否允许 Trusted Types、是否允许 `unsafe-inline` 等。

两者都要配对好，页面才能既「跑起来」又「打得通接口」。

---

## 3. `CorsOriginPatternSetter::Set`

### 3.1 签名

```cpp
// content/public/browser/cors_origin_pattern_setter.h
static void Set(
    content::BrowserContext* browser_context,
    const url::Origin& source_origin,
    std::vector<network::mojom::CorsOriginPatternPtr> allow_patterns,
    std::vector<network::mojom::CorsOriginPatternPtr> block_patterns,
    base::OnceClosure closure);
```

### 3.2 做了什么

`Set` 会把「源 Origin → 允许/禁止的目标模式」同时写入两处：

1. **`BrowserContext::GetSharedCorsOriginAccessList()`**  
   进程内共享列表；Network Service 重启后可据此恢复。
2. **当前已加载的每个 `StoragePartition` 的 `NetworkContext`**  
   调用 `SetCorsOriginAccessListsForOrigin`，真正影响线上请求判定。

`closure` 在两处都 ack 后调用（内部用 `BarrierClosure(2, ...)`）。不关心完成时机时可传 `base::DoNothing()`。

### 3.3 语义（直观理解）

对 **`source_origin` 发起的请求**，若目标匹配某个 `allow_patterns` 且不被 `block_patterns` 命中，则 Network Service 按 **CORS origin access allowlist** 处理——业务上等价于「对该目标放行跨源读响应」，无需服务端 CORS 头。

这不是把整个浏览器的 webSecurity 关掉，而是 **按「源 Origin + 目标 pattern」精细放行**。

---

## 4. `CorsOriginPattern` 字段

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

## 5. XL Player 实际写法

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

调用时机：`XlPlayerWebUIController` 构造时，在创建 `WebUIDataSource` **之前**调用，保证页面脚本发起请求前列表已登记。

效果对照：

| 项目 | 值 |
|------|----|
| source | `chrome://xl-player` |
| allow | `http(s)://*.xunlei.com`（含子域、任意端口） |
| block | 无 |

---

## 6. WebUI 侧 CSP 配置

同一文件里，CORS 之外还有 CSP（解决「脚本能不能跑」）：

```cpp
source->DisableTrustedTypesCSP();
source->OverrideContentSecurityPolicy(
    network::mojom::CSPDirectiveName::ScriptSrc,
    "script-src chrome://resources 'self' 'unsafe-inline';");
```

| API | 作用 |
|-----|------|
| `DisableTrustedTypesCSP()` | 关闭 Trusted Types 强制；Vue/Vite 会对 `innerHTML` 建 policy，不关则可能白屏 |
| `OverrideContentSecurityPolicy(ScriptSrc, ...)` | 覆盖默认 `script-src`，允许 `chrome://resources`、本源、以及（此处）`'unsafe-inline'` |

注意：

- 改 CSP **不会**让跨源 XHR 自动成功。
- 配 CORS 白名单 **不会**让违规脚本突然可执行。

---

## 7. 与扩展权限模型的关系

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

## 8. 常见误区

1. **「关掉 CSP 就等于放开 CORS」** — 错。CSP 管加载/执行，CORS 管跨源读响应。  
2. **「在页面里设 `Access-Control-Allow-Origin: *`」** — 无效。CORS 响应头必须由**目标服务器**返回；浏览器侧要用 Origin Access List 或代理。  
3. **只配了 https、漏了 http** — 开发/内网若走 http 仍会失败；XL 两边都加了。  
4. **source_origin 写错 host** — 必须以真实 WebUI host 为准（`kChromeUIXLPlayerHost`），和地址栏一致。  
5. **认为 `Set` 是同步立刻全局生效** — 会推到各 StoragePartition；一般构造期调用 + `DoNothing` 足够；若紧挨着发请求且偶发失败，再等 `closure`。  
6. **用 `DisableWebSecurity` 命令行代替** — 过宽、不安全、不适合产品路径；应用 Origin Access List。

---

## 9. 检查清单

接入类似「WebUI → 自有后端」跨源能力时：

- [ ] 明确 **source Origin**（`chrome://...` / `chrome-extension://...`）
- [ ] 列出最小必要的 **目标 domain + scheme**（优先子域模式，避免 `*` 全网）
- [ ] 调用 `CorsOriginPatternSetter::Set`（BrowserContext 非空）
- [ ] 单独检查 **CSP / Trusted Types** 是否挡住页面启动
- [ ] DevTools：Network 里确认不再是 CORS error；Console 无 CSP refuse
- [ ] 不把 CORS 放行当成通用「关闭浏览器安全」

---

## 延伸阅读

- `content/public/browser/cors_origin_pattern_setter.h` 注释（Shared list + NetworkContext 双写）
- `content/browser/loader/cors_origin_pattern_setter_browsertest.cc`（行为用例）
- `services/network/public/mojom/cors_origin_pattern.mojom`（pattern 定义）
- MDN：[CORS](https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS)、[CSP](https://developer.mozilla.org/en-US/docs/Web/HTTP/CSP)
