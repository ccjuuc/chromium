# `chrome.xenonPrivate` 扩展 API

面向 **Xenon 组件扩展**：在 **扩展上下文**（Service Worker、popup 等）里调用 **`chrome.xenonPrivate.*`**，由 **Browser 进程** 中的 **`ExtensionFunction`** 执行，与页面里的 **`window.xenon`（Mojo）**、**WebUI 的 `PageHandler`（Mojo）** 不是同一条通道。

---

## 1. 谁能用、安全边界

| 项 | 说明 |
|----|------|
| **调用方** | 仅 **扩展体系**（Manifest V3 的 Service Worker、`chrome-extension://` 下的 popup 等）。**普通网页、任意 WebUI 页面脚本不能直接调用** `chrome.xenonPrivate`。 |
| **权限** | Manifest 中声明 **`"xenonPrivate"`**（与 `_permission_features.json` 中的键一致）。 |
| **`location`** | 配置为 **`component`**：意图上 **仅随浏览器打包的组件扩展** 可使用；商店侧随意安装的扩展无法获得同等能力。 |
| **上下文** | `_api_features.json` 中为 **`privileged_extension`**。 |

若未声明权限、扩展不是组件加载路径，或 API 未编入构建，则 **`chrome.xenonPrivate` 可能为 `undefined`** 或方法不存在。

---

## 2. 怎么用（扩展侧）

### 2.1 在 `manifest.json` 里声明权限

在 **`permissions`**（或按需使用 **`optional_permissions`**）中加入 **`xenonPrivate`**：

```json
{
  "manifest_version": 3,
  "permissions": [
    "storage",
    "xenonPrivate"
  ]
}
```

本仓库内置扩展示例：`xenon_overlay/resources/extension/manifest.json`。

### 2.2 当前提供的方法：`ping`

- **签名（概念）**：`chrome.xenonPrivate.ping(callback: (response: string) => void)`
- **语义**：连通性探测；实现位于 Browser 侧，当前固定返回字符串 **`xenon-private-pong`**（见 `xenon_overlay/chrome/browser/extensions/api/xenon_private/xenon_private_api.cc`）。

**Service Worker（`background.js`）示例**——在已安装/启动时调用一次：

```javascript
chrome.runtime.onInstalled.addListener(() => {
  if (chrome.xenonPrivate && chrome.xenonPrivate.ping) {
    chrome.xenonPrivate.ping((response) => {
      console.log('chrome.xenonPrivate.ping =>', response);
    });
  }
});
```

**Popup / 扩展页面** 中同样使用回调形式；调用前建议做存在性检查，便于排查未编进构建或未授权的情况：

```javascript
if (!chrome?.xenonPrivate?.ping) {
  console.error('chrome.xenonPrivate.ping 不可用（检查 manifest permissions: xenonPrivate）');
} else {
  chrome.xenonPrivate.ping((response) => {
    console.log('xenonPrivate.ping OK:', response);
  });
}
```

更完整 UI 示例见：`xenon_overlay/resources/extension/main.js`、`index.html`。

### 2.3 与 `runtime.sendMessage` 的区别

- **`chrome.xenonPrivate.*`**：扩展 JS → **Browser** 原生代码（C++ `ExtensionFunction`）。
- **`chrome.runtime.sendMessage`**：扩展内 **不同上下文之间**（如 popup ↔ background）传消息，**不**等同于调用 Browser 私有 API。

---

## 3. 怎么在 Chromium / 本分支里「添加」这套 API（或新增方法）

### 3.1 已存在时的代码布局（本仓库）

| 角色 | 路径 |
|------|------|
| API Schema（JSON） | `chrome/common/extensions/api/xenon_private.json` |
| 列入 schema 编译列表 | `chrome/common/extensions/api/api_sources.gni` |
| Feature / Permission | `chrome/common/extensions/api/_api_features.json`、`_permission_features.json` |
| 权限 ID、直方图、安装文案等 | `extensions/common/mojom/api_permission_id.mojom`、`chrome/common/extensions/permissions/chrome_api_permissions.cc` 等（与 Chromium 其它 `*Private` API 同一套路） |
| **GN 注册** | `chrome/browser/extensions/api/BUILD.gn` 的 `api_implementations` 依赖 **`xenon_private`** |
| **Browser 实现（薄 shim，满足生成器 include 路径）** | `chrome/browser/extensions/api/xenon_private/xenon_private_api.h` → 包含 overlay 头文件 |
| **实际 C++ 实现** | `xenon_overlay/chrome/browser/extensions/api/xenon_private/xenon_private_api.{h,cc}`、`BUILD.gn` |

生成器要求实现头出现在 `//chrome/browser/extensions/api/<api_dir>/` 下，因此 **chrome 树里保留 shim**，逻辑放在 **`xenon_overlay`**。

### 3.2 在 `xenonPrivate` 下新增第二个方法（概要）

1. 编辑 **`xenon_private.json`**：在 **`functions`** 里增加新条目（参数、`returns_async` 等与现有 `ping` 一致风格）。
2. **全量/增量编译** 以刷新 `//chrome/common/extensions/api` 下生成头（如 `xenon_private.h`）。
3. 在 **`xenon_overlay/.../xenon_private_api.{h,cc}`** 中新增 **`ExtensionFunction` 子类**，**`DECLARE_EXTENSION_FUNCTION("xenonPrivate.新方法名", 直方图枚举)`**。
4. 在 **`extensions/browser/extension_function_histogram_value.h`** 与 **`tools/metrics/histograms/metadata/extensions/enums.xml`** 中为该函数增加 **新枚举项**（仅追加，不改序）。
5. **同一 permission** 下通常 **不必** 再改 `api_permission_id.mojom`；若引入全新 permission 字符串则需完整走 permission 注册链路。

逐步拆解（含 metrics 脚本、单测 `permission_set_unittest.cc` 等）见 **`xenon_overlay_architecture.md` §6.6–6.7**。

---

## 4. 排查清单

1. **`chrome.xenonPrivate` 为 undefined**  
   - `manifest.json` 是否包含 **`xenonPrivate`**。  
   - 当前构建是否包含 **`api_implementations` 中的 xenon_private** 目标。  
2. **权限报错 / 安装界面无该项**  
   - 核对 `_permission_features.json` 与 `chrome_api_permissions.cc` 中的字符串是否一致。  
3. **与 WebUI / 页面通信混淆**  
   - WebUI 应使用对应 **Mojo**；需要「扩展 ↔ Browser 原生」时用 **`chrome.xenonPrivate`**（或按 §6.6 新增其它 `chrome.*` API）。

---

## 5. 延伸阅读

- 架构上下文与文件索引：**`xenon_overlay_architecture.md`** → [§6 内置扩展](xenon_overlay_architecture.md#6-内置扩展component-extension)、[§6.6 添加自定义 Extension API](xenon_overlay_architecture.md#66-添加自定义-extension-api-的详细步骤)、[§6.7 示例文件表](xenon_overlay_architecture.md#67-示例chromexenonprivate-涉及文件)。
- **AI 与侧栏、与扩展 API 分工：** **`xenon_ai_integration.md`** → [总目录与分层](xenon_ai_integration.md#目录)。
