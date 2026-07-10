# Chromium 内置 VLESS / VMess / Trojan 出站代理：修改步骤与实现细节

本文记录 Chromium 142 树内引入 **VLESS**、**VMess（AEAD）**、**Trojan** 三种 Project-V 系出站代理的完整改造路径。功能由 4 个提交分批落地，但都共享同一套骨架（统称 **Leaf 出站栈**），因此放在一篇里讲清楚。

涉及的提交（按时间顺序）：

| # | Commit | 主题 |
|---|---|---|
| ① | `00b4f982ccc41bc239953ce2926bc13f7cc1ab56` | 搭骨架：URI → ProxyServer → ProxyChain → ConnectJob → ClientSocket，先把 **VLESS（含 WebSocket 透传）** 走通 |
| ② | `9336869b4a20337d25c925e6dc959040c9b3221d` | vendor `plain_tcp_header.rs` / `sync_framing.rs` 到 `third_party/chromium_leaf`，cxx 桥暴露给 C++；新增 **VLESS Vision (`xtls-rprx-vision`)** |
| ③ | `144350fff58fd4362279e4aa9d925e1fca7c0ea7` | 新增 **VMess AEAD**（`LeafVmessStreamEngine`），WebSocket 升级用浏览器风默认头/合并 101+payload/分片 message；`security=tls` 时自动包 TLS；解析 `vmess://<base64(json)>` 分享链 |
| ④ | `d2c26c4d05f212c0fa85eb87b6ff5088eb61a2f4` | 新增 **Trojan**（修缺失的 command byte），默认内置代理改为 Trojan WS+TLS |

> **构建开关**：`net/features.gni` 的 `enable_chromium_leaf`（默认 `true`，生成 `BUILDFLAG(ENABLE_CHROMIUM_LEAF)`）与 `chromium_leaf_builtin_default_proxy`（默认 `true`，生成 `BUILDFLAG(CHROMIUM_LEAF_BUILTIN_DEFAULT_PROXY)`）。
>
> 关掉前者 = 不编 Leaf 相关代码；关掉后者 = 不种内置默认代理 URI（profile 默认仍是系统代理）。

---

## 一、整体架构

```
                  prefs::kProxy (fixed_servers + reverse_bypass)
                                    │
                                    ▼
        PrefProxyConfigTrackerImpl::PrefConfigToNetConfig
        + ApplyChromiumLeafBrowserPrefOverrides
                                    │
                  net::ProxyConfig.proxy_rules.single_proxies
                                    │
                       ProxyServer { scheme=VLESS|VMESS|TROJAN,
                                      host:port,
                                      credential,
                                      leaf_uri_query,
                                      leaf_uri_fragment }
                                    │
              HttpStreamFactory::Job 解析后选中 ProxyChain
                                    │
                CreateProxyParams (connect_job_params_factory.cc)
            ┌───────────────┴───────────────┐
            │   security=tls / xtls?         │ 否
            ▼                                ▼
   SSLSocketParams(LEAF_PROXY)    LeafSocketParams(transport=TCP)
            │                                │
   SSLConnectJob → DoLeafHandshake           │
            │                                │
            └──────► LeafConnectJob ◄────────┘
                              │
                  TransportConnectJob → TCP 到代理
                              │
                  (可选) SSLConnectJob → TLS 到代理 (ALPN=http/1.1 if type=ws)
                              │
                  LeafClientSocket::Connect()
                ┌────────────┼─────────────┐
                ▼            ▼             ▼
              VLESS        VMess         Trojan
            (Plain/Vision)  (AEAD)       (CRLF)
              + 可选 WebSocket binary 帧（masked）
                              │
                              ▼
                   StreamSocket → SSLClientSocket → HTTP/...
```

四个关键事实：

1. **Leaf 出站是单跳代理**（`proxy_chain.length() == 1`），多代理链不在范围内。
2. **TLS 由 Chromium 的 `SSLConnectJob` 提供**，不在 Rust 里造轮子。Leaf 只负责协议握手 + 可选的 WebSocket framing。
3. **ALPN**：当 URI 里 `type=ws` 时，传输层 TLS 强制 `kHttp11Only`，避免对端选 h2 后 WebSocket Upgrade 失败。
4. **Vision parser** 在 Rust（`third_party/chromium_leaf`）里，通过 cxx bridge 暴露；其它握手（VLESS plain/Trojan/VMess）都在 C++（`net/socket/`）里。

---

## 二、构建配置

### 2.1 `net/features.gni`

在 `declare_args()` 里追加：

```37:90:net/features.gni
  # Embeds //third_party/chromium_leaf (Rust FFI stub; full Leaf vendoring TBD).
  # Requires Rust toolchain in the build (see //docs/rust.md).
  enable_chromium_leaf = true

  # When true (and enable_chromium_leaf), the default profile/local-state proxy
  # pref is fixed_servers with kChromiumLeafDefaultProxyUris[0] (see
  # kChromiumLeafDefaultProxyUri) and reverse_bypass: only hosts in
  # kChromiumLeafDefaultProxyHostPatterns use the proxy; other traffic is direct.
  # Set false for "system proxy" default.
  chromium_leaf_builtin_default_proxy = true
```

### 2.2 `net/BUILD.gn`

在 `buildflag_header("buildflags")` 里追加：

```text
ENABLE_CHROMIUM_LEAF=$enable_chromium_leaf
CHROMIUM_LEAF_BUILTIN_DEFAULT_PROXY=$chromium_leaf_builtin_default_proxy
```

`component("net")` 的 `sources` 里**始终**追加（这些文件不依赖 Rust）：

```text
socket/chromium_leaf_vless_handshake.{cc,h}
socket/leaf_client_socket.{cc,h}
socket/leaf_connect_job.{cc,h}
socket/leaf_outbound_protocol.h
socket/leaf_vmess_engine.{cc,h}
```

仅当 `enable_chromium_leaf` 为真时再追加：

```text
socket/chromium_leaf_glue.{cc,h}
deps += [ "//third_party/chromium_leaf:chromium_leaf_ffi" ]
```

### 2.3 `DEPS` + `.gitmodules`

新增 Leaf 上游 submodule（VMess / VLESS / Trojan 的协议参考实现，目前**未直接链接**，留作 vendoring 蓝本）：

```text
# .gitmodules
[submodule "third_party/leaf"]
    path = third_party/leaf
    url = https://github.com/eycorsican/leaf.git

# DEPS（vars 部分）
'leaf_revision': 'a8971524d6e17366e12da0af08f74c2dac672cbd',

# DEPS（deps 部分）
'src/third_party/leaf':
    'https://github.com/eycorsican/leaf.git@' + Var('leaf_revision'),
```

### 2.4 `.gitignore` / `.cursorignore`

- `.gitignore`：忽略历史遗留路径 `/third_party/chromium_leaf_cargo_scratch/`（新代码已迁到 `third_party/chromium_leaf/cargo_scratch`）。
- `.cursorignore`：在 `out/*` 整体忽略中破例放行 `out/Debug_64/chrome_debug.log`，便于调试代理日志。

---

## 三、Rust shim：`third_party/chromium_leaf/`

### 3.1 GN 目标

```1:21:third_party/chromium_leaf/BUILD.gn
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import("//build/rust/rust_static_library.gni")

# Rust shim for Leaf-related ABI versioning and typed cxx FFI for VLESS framing.
# Sync framing sources live under src/ (aligned with upstream Leaf logic).

rust_static_library("chromium_leaf_ffi") {
  allow_unsafe = true  # cxx bridge expands unsafe shims.
  edition = "2024"
  crate_root = "src/lib.rs"
  sources = [
    "src/lib.rs",
    "src/plain_tcp_header.rs",
    "src/sync_framing.rs",
  ]
  cxx_bindings = [ "src/lib.rs" ]

  visibility = [ "//net:*" ]
}
```

### 3.2 cxx bridge 与导出函数

`third_party/chromium_leaf/src/lib.rs` 用 `#[cxx::bridge(namespace = "net::chromium_leaf")]` 暴露下列符号；C++ 通过 `#include "third_party/chromium_leaf/src/lib.rs.h"` 调用：

| 函数/类型 | 用途 |
|---|---|
| `chromium_leaf_ffi_abi_version() -> i32` | 当前 ABI 版本 = **6**；C++ 侧 `net::ChromiumLeafFfiAbiVersion()` 转发 |
| `chromium_leaf_vless_plain_tcp_header(uuid, dest_host, dest_port) -> Vec<u8>` | 构造**普通** VLESS TCP 请求头（无 add-on，命令=0x01） |
| `chromium_leaf_vless_vision_tcp_header(...)` | 构造 **Vision (`xtls-rprx-vision`)** 请求头（带 18 字节 add-on：长度 16 + flow 字符串） |
| `ChromiumLeafVisionParser` (Rust 类型，`Box<...>`) | Vision RX 增量解析器，匹配 Leaf `VisionParser`（无 Tokio） |
| `chromium_leaf_vision_parser_new(uuid)` | 构造 |
| `chromium_leaf_vision_parser_feed(parser, data)` | 喂入对端字节，返回明文 |
| `chromium_leaf_vision_parser_direct_copy(parser)` | 命中 `cmd=2` 后进入直拷贝模式 |
| `chromium_leaf_vision_parser_vision_done(parser)` | Vision 解析终结（后续即明文） |
| `chromium_leaf_outbound_handshake(...)` | C ABI 占位，目前直接返回 `ERR_NOT_IMPLEMENTED (-11)`，留给将来直接调用上游 leaf crate |

> 注意 ABI 版本：①里是 `4`、② 升到 `6`，并且把命名空间从 `net::chromium_leaf`（带斜杠）改成字符串字面量，避免 cxx 在某些 MSVC/Clang 组合下生成出错。

### 3.3 Vision RX 状态机要点

```122:191:third_party/chromium_leaf/src/sync_framing.rs
pub fn parse(&mut self, data: &[u8]) -> Vec<u8> {
    self.v_buffer.extend_from_slice(data);
    let mut to_client = Vec::new();
    let mut offset = 0;
    if !self.vless_response_header_parsed {
        if self.v_buffer.len() >= 2 { ... offset += 2; } else { return to_client; }
    }
    // ... 21 字节 frame：UUID(16) + cmd(1) + content_len(2) + padding_len(2) ...
}
```

关键不变量：

- **首次** 进入时先吞 2 字节 VLESS response prelude。
- 后续每个 Vision frame 由 21 字节头开始（包含 UUID 验证 + cmd + content_len + padding_len）；`cmd == 2` 进入 `direct_copy_rx`（后续全部按明文转发，不再解析）；`cmd != 0 && cmd != 2` 设 `vision_done`，剩余字节也按明文转发。
- C++ 侧通过 `LeafVlessVisionParser::Feed` 增量喂入，结果累积到 `vision_rx_queue_` 再交付给上层 Read。

### 3.4 可选的 Cargo 沙盒

`third_party/chromium_leaf/cargo_scratch/` 提供独立 `cargo check`/`cargo build` 入口，便于 IDE 智能提示与小规模实验，**target-dir** 输出到 `//out/cargo_chromium_leaf_target`，不污染源码树。

---

## 四、`ProxyServer` / URI 解析

### 4.1 新增三个 scheme

```42:48:net/base/proxy_server.h
    // Project V-style proxies handled by the Leaf outbound stack in net/socket.
    // URI/PAC use vless/vmess/trojan; implementation is shared (LeafConnectJob).
    SCHEME_VLESS = 1 << 7,
    SCHEME_VMESS = 1 << 8,
    SCHEME_TROJAN = 1 << 9,
```

`is_vless() / is_vmess() / is_trojan()` 与汇总判断 `is_leaf_outbound()` 一并提供。

### 4.2 `ProxyServer` 多了三个字段

```111:118:net/base/proxy_server.h
  bool is_vless() const { return scheme_ == SCHEME_VLESS; }
  bool is_vmess() const { return scheme_ == SCHEME_VMESS; }
  bool is_trojan() const { return scheme_ == SCHEME_TROJAN; }
  // True for proxy types implemented by LeafConnectJob / LeafClientSocket.
  bool is_leaf_outbound() const {
    return is_vless() || is_vmess() || is_trojan();
  }
```

构造函数新增三个 `std::string` 参数（`credential`、`leaf_uri_query`、`leaf_uri_fragment`），仅对 leaf scheme 生效；非 leaf scheme 在构造时被强制清空，保证比较和 Pickle 一致性。

`Persist` / `CreateFromPickle`、Mojom Traits 都同步追加这三个字段。

#### 4.2.1 三个新字段语义详解

权威定义在 `net/base/proxy_server.h`：

```56:61:net/base/proxy_server.h
  // |credential| holds percent-decoded URI userinfo bytes for Leaf outbounds
  // (see net/base/proxy_string_util.cc, matching net::GetIdentityFromURL
  // rules via UnescapeBinaryURLComponentSafe). Often UTF-8 text (Trojan
  // password, VLESS UUID); empty for non-Leaf schemes or PAC-only entries.
  // |leaf_uri_query| / |leaf_uri_fragment| preserve Xray-style vless://...?a=b#tag
  // pieces (raw query without '?', fragment without '#').
```

##### A. `credential` —— Xray URI 中 `userinfo` 解码后的明文

| Scheme | `credential` 内容 | 消费处 |
|---|---|---|
| `vless://<uuid>@host:port` | UUID 明文（带或不带破折号） | `leaf_client_socket.cc:165` `LeafVlessParseUuid(leaf_credential_, &uuid)` |
| `vmess://<uuid>@host:port` | 同 VLESS（VMess 也用 UUID 做 user id） | `leaf_client_socket.cc:165` |
| `trojan://<password>@host:port` | Trojan 密码原文（用于 `SHA224(password)`） | `leaf_client_socket.cc:153` `LeafTrojanBuildRelayHandshake(leaf_credential_, ...)` |
| 非 Leaf scheme | 永远为空（构造时强制 `clear()`） | `proxy_server.cc:70/75` |

**解码规则**：userinfo 用 `UnescapeBinaryURLComponentSafe` 做 `%xx` 解码，**不做** query 风格的 `+→空格` 转换，与 `net::GetIdentityFromURL` 完全一致：

```77:108:net/base/proxy_string_util.cc
// Decodes one userinfo subcomponent (username or password) the same way as
// net::GetIdentityFromURL()'s UnescapeIdentityString helper: use
// UnescapeBinaryURLComponentSafe so control bytes stay escaped, and do not
// apply query-style '+' decoding (see net/base/url_util.cc).
std::string UnescapeLeafUserinfoPiece(std::string_view escaped_piece) { ... }

std::string LeafCredentialFromAuthority(
    std::string_view authority,
    const url::Component& username_component,
    const url::Component& password_component) {
  std::string cred;
  if (username_component.is_valid()) {
    cred = UnescapeLeafUserinfoPiece(authority.substr(...));
  }
  if (password_component.is_valid()) {
    if (!cred.empty()) {
      cred.push_back(':');
    }
    cred += UnescapeLeafUserinfoPiece(authority.substr(...));
  }
  return cred;
}
```

如果 URI 同时给了用户名+密码（罕见的 `vless://name:pass@host`），用 `:` 拼成 `name:pass`。反向序列化用 `url::EncodeURIComponent`：

```112:127:net/base/proxy_string_util.cc
std::string LeafProxyUri(const ProxyServer& proxy_server, ...) {
  ...
  const std::string& cred = proxy_server.credential();
  if (cred.empty()) {
    base_uri = std::string(scheme_with_colon_slashslash) + hostport;
  } else {
    std::string encoded;
    url::StdStringCanonOutput o(&encoded);
    url::EncodeURIComponent(cred, &o);
    o.Complete();
    base_uri = base::StrCat({scheme_with_colon_slashslash, encoded, "@", hostport});
  }
  ...
}
```

`LeafClientSocket::Connect` 入口直接拒空：

```142:146:net/socket/leaf_client_socket.cc
  if (leaf_credential_.empty()) {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::LEAF_PROXY_CONNECT,
                                      ERR_INVALID_ARGUMENT);
    return ERR_INVALID_ARGUMENT;
  }
```

##### B. `leaf_uri_query` —— Xray 分享链接 `?…` 段（不含前导 `?`）

由 `LeafStripUriQueryAndFragment` 在 `url::ParseAuthority` **之前**剥出来（Chromium URL parser 不接受 authority 里出现 `?`/`#`）：

```141:145:net/base/proxy_string_util.cc
// Strips `#fragment` then `?query` from the leaf authority string (content
// after scheme://). Per URL ordering, fragment is removed first.
void LeafStripUriQueryAndFragment(std::string_view* authority_in_out,
                                  std::string* leaf_query,
                                  std::string* leaf_fragment) { ... }
```

`leaf_uri_query` 是 Leaf 客户端**所有传输/协议旋钮**的承载介质，等价于 Xray `streamSettings`。在两个关键位置被读取：

**(1) 决定要不要外裹 TLS / SNI / ALPN —— `connect_job_params_factory.cc`**

```265:310:net/socket/connect_job_params_factory.cc
  if (proxy_server.is_leaf_outbound() &&
      LeafOutboundQueryUsesTransportTls(proxy_server.leaf_uri_query())) {
    SSLConfig leaf_proxy_tls_config;
    ...
    HostPortPair leaf_tls_host_port = proxy_server.host_port_pair();
    std::string_view sni =
        LeafUriQueryLookup(proxy_server.leaf_uri_query(), "sni");
    if (!sni.empty()) {
      leaf_tls_host_port.set_host(std::string(sni));
    }

    const std::string_view leaf_transport_type =
        LeafUriQueryLookup(proxy_server.leaf_uri_query(), "type");
    const ConnectJobFactory::AlpnMode leaf_proxy_alpn_mode =
        base::EqualsCaseInsensitiveASCII(leaf_transport_type, "ws")
            ? ConnectJobFactory::AlpnMode::kHttp11Only
            : ConnectJobFactory::AlpnMode::kHttpAll;
    ...
    params = MakeSSLSocketParams(std::move(params), leaf_tls_host_port, ...);
  }
```

`LeafOutboundQueryUsesTransportTls` 仅看 `security=tls|xtls`：

```634:638:net/base/proxy_string_util.cc
bool LeafOutboundQueryUsesTransportTls(std::string_view leaf_uri_query) {
  std::string_view sec = LeafUriQueryLookup(leaf_uri_query, "security");
  return base::EqualsCaseInsensitiveASCII(sec, "tls") ||
         base::EqualsCaseInsensitiveASCII(sec, "xtls");
}
```

**(2) 协议 / 传输模式判定 —— `LeafClientSocket::Connect`**

```174:251:net/socket/leaf_client_socket.cc
// VMess 加密算法
std::string cipher = LeafVlessQueryLookup(leaf_uri_query_, "encryption");
if (cipher.empty()) cipher = LeafVlessQueryLookup(leaf_uri_query_, "cipher");
if (cipher.empty()) cipher = "chacha20-poly1305";

// VLESS Vision flow
const std::string flow =
    base::ToLowerASCII(LeafVlessQueryLookup(leaf_uri_query_, "flow"));
use_vision_ = (flow == "xtls-rprx-vision");

// 传输类型 (tcp / ws)
handshake_type_ =
    base::ToLowerASCII(LeafVlessQueryLookup(leaf_uri_query_, "type"));
...
// WS 路径下读取 host / path / useragent / origin
std::string ws_host = LeafVlessQueryLookup(leaf_uri_query_, "host");
std::string path = base::UnescapeURLComponent(
    LeafVlessQueryLookup(leaf_uri_query_, "path"), ...);
std::string ws_ua = LeafVlessQueryLookup(leaf_uri_query_, "useragent");
if (ws_ua.empty()) {
  ws_ua = LeafVlessQueryLookup(leaf_uri_query_, "userAgent");
}
std::string ws_origin = LeafVlessQueryLookup(leaf_uri_query_, "origin");
```

整理客户端识别的全部 query key（与 Xray `streamSettings` 对齐）：

| Key | 作用 | 适用 scheme |
|---|---|---|
| `security` | `tls` / `xtls` ⇒ 外层套 TLS；`none`/缺省 ⇒ 明文 | 三者通用 |
| `sni` | TLS 握手 SNI 与证书匹配主机名 | 三者通用（且 `security=tls`） |
| `type` | 传输：`tcp`（默认）或 `ws` | 三者通用 |
| `host` | WS `Host:` 头 | `type=ws` |
| `path` | WS HTTP 请求 path | `type=ws` |
| `useragent` / `userAgent` | WS GET 自定义 UA（Xray `streamSettings.headers`） | `type=ws` |
| `origin` | WS `Origin:` 头 | `type=ws` |
| `flow` | `xtls-rprx-vision` ⇒ 走 Vision 21B padding 协议 | VLESS |
| `encryption` / `cipher` | AEAD 算法（默认 `chacha20-poly1305`） | VMess |

**查询函数**：`LeafUriQueryLookup` 是手写的 `&` 切分查找，**保持原样字符串不解码**，调用方按需 `UnescapeURLComponent`：

```608:632:net/base/proxy_string_util.cc
std::string_view LeafUriQueryLookup(std::string_view query,
                                    std::string_view key) {
  while (!query.empty()) {
    ...
    size_t eq = pair.find('=');
    std::string_view k = eq == npos ? pair : pair.substr(0, eq);
    if (k == key) {
      return eq == npos ? std::string_view() : pair.substr(eq + 1);
    }
  }
  return {};
}
```

> 同名 C++ 包装 `LeafVlessQueryLookup`（在 `chromium_leaf_vless_handshake.cc:208`）则用 `GURL("https://local.invalid/?"+query)` + `GetValueForKeyInQuery` 做 `+`/`%xx` 解码后查找。两者择一使用：URL 通用键（`path`、`host`、`useragent`、`origin`）走解码版；纯 token 键（`type`、`flow`、`security`、`sni`、`encryption`）走原样版。

##### C. `leaf_uri_fragment` —— Xray 分享链接 `#…` 段（不含前导 `#`）

通过对 `net/` 下所有引用的盘点，可以确认 fragment **只用于回写 URI、不影响协议握手**：

| 引用位置 | 作用 |
|---|---|
| `net/base/proxy_server.h/cc` | 字段 + getter + Pickle 序列化 |
| `net/base/proxy_string_util.cc:129` | `LeafProxyUri` 反向拼回 `…#tag` |
| `net/base/proxy_string_util_unittest.cc:512,544` | 单测断言保留 fragment 原样 |
| `net/socket/connect_job_params_factory.cc:352` | 仅作为透传参数交给 `LeafSocketParams` |
| `net/socket/leaf_connect_job.cc:230` | 仅再透传给 `LeafClientSocket` |
| `net/socket/leaf_client_socket.cc:130` | 只在 VLOG 里打 `fragment_len` |

存在意义只有三件事：

1. 序列化时把 URI 完整 round-trip 回 `vless://uuid@host:port?type=ws…#kxinarvy` 形式；
2. 在调试日志、Mojo 跨进程传递、Pickle 持久化时不丢失节点别名；
3. 与 v2rayN / Clash 等客户端互导分享链接时人类可读的 tag 不会消失（如内置默认列表里的 `#tzw53key` / `#kxinarvy`，详见 §六）。

##### D. 端到端旅程

```
Xray 分享 URI / PAC string
        │
        ▼
ProxyUriToProxyServer (net/base/proxy_string_util.cc)
   ├─ LeafStripUriQueryAndFragment      → leaf_query / leaf_fragment
   └─ LeafCredentialFromAuthority       → credential
        │
        ▼
ProxyServer ctor (net/base/proxy_server.cc)
   ├─ credential_, leaf_uri_query_, leaf_uri_fragment_  // 仅 Leaf scheme 保留
   └─ Persist / CreateFromPickle  (Mojo 之外的旧持久化也支持)
        │
        ▼
network_param_mojom_traits.{h,cc}
   ├─ ToMojom → mojom::ProxyServer{ credential, leaf_uri_query, leaf_uri_fragment }
   └─ Read    → 反向构造 net::ProxyServer
        │
        ▼
ConfiguredProxyResolutionService → ProxyChain → CreateProxyParams
        │  (connect_job_params_factory.cc:265-353)
        │  ├─ 用 leaf_uri_query 决定是否裹 TLS / 设 SNI / ALPN
        │  └─ credential / leaf_uri_query / leaf_uri_fragment  → LeafSocketParams
        ▼
LeafConnectJob  →  LeafClientSocket
   ├─ credential   → UUID / 密码  (LeafVlessParseUuid / LeafTrojanBuildRelayHandshake)
   ├─ leaf_uri_query → encryption / flow / type / host / path / sni / useragent / origin
   └─ leaf_uri_fragment → 仅日志 + round-trip
        │
        ▼
对端 Xray / Trojan 服务器
```

### 4.3 Mojom

```52:74:services/network/public/mojom/network_param.mojom
enum ProxyScheme {
  ...
  kVless,
  kVmess,
  kTrojan,
};

struct ProxyServer {
  ProxyScheme scheme;
  HostPortPair? host_and_port;
  string? credential;
  string? leaf_uri_query;
  string? leaf_uri_fragment;
};
```

`network_param_mojom_traits.{h,cc}` 中的 EnumTraits / StructTraits `ToMojom` / `Read` 同步覆盖三个新枚举与三个新字段。

### 4.4 URI / PAC 解析

`net/base/proxy_string_util.cc` 新增的核心辅助：

| 函数 | 说明 |
|---|---|
| `GetSchemeFromUriScheme` / `GetSchemeFromPacTypeInternal` | 识别 `vless` / `vmess` / `trojan` |
| `LeafStripUriQueryAndFragment(authority, query_out, fragment_out)` | 先剥 `#fragment` 再剥 `?query`，把剩下的纯 authority 交给 `url::ParseAuthority` |
| `LeafCredentialFromAuthority(...)` | 用 `UnescapeBinaryURLComponentSafe` 解码 userinfo（与 `net::GetIdentityFromURL` 行为一致；不做 query 风格的 `+→空格` 转换） |
| `LeafProxyUri(server, "vless://"等)` | 反向序列化时用 `url::EncodeURIComponent` 编码 userinfo |
| `LeafUriQueryLookup(query, key)` | 在 `&` 拆分的 query 字串里查 key |
| `LeafOutboundQueryUsesTransportTls(query)` | 判 `security=tls` 或 `security=xtls`，决定要不要外裹 TLS |
| `ProxyServerFromVmessShareBase64Json(...)` | 解析 v2rayN/Clash 风格 `vmess://<base64(json)>` |
| `ProxyServerToPacResultElement` | 输出 `VLESS host:port` / `VMESS host:port` / `TROJAN host:port` |
| `ProxyServerToProxyUri` | 还原成 `vless://` / `vmess://` / `trojan://` URI |
| `kChromiumLeafDefaultProxyUris[]` | 内置默认 URI 列表（详见 §六） |

`ProxySchemeHostAndPortToProxyServer` 对 leaf scheme 走专用分支：**允许** userinfo 存在（普通 HTTP 代理是不允许 userinfo 的），且复用 `ParseAuthority` 解析剩下的 host:port。

### 4.5 `vmess://<base64(json)>` 兼容

`ProxyServerFromVmessShareBase64Json` 接受 v2rayN 风格 base64-JSON：

| JSON 字段 | 映射 |
|---|---|
| `add` | host |
| `port` | port（int 或 string，需 1–65535） |
| `id` | `credential`（VMess UUID） |
| `net` | `type=tcp` 或 `type=ws`（其它一律拒绝） |
| `path` | `path=`（自动补前导 `/`） |
| `host` | `host=`（HTTP Host / WS Host header） |
| `tls` | 解析 `"tls"` / `"1"` / bool true 时追加 `security=tls` |
| `sni` | `sni=` |
| `scy` | `encryption=`（`"auto"` 或缺省 → `aes-128-gcm`） |
| `alpn` | `alpn=` |
| `ps` | URI fragment（如果 fragment 为空） |

### 4.6 `proxy_chain.cc` / `proxy_resolution/proxy_config.cc`

- `ProxyChain::GetHistogramSuffix`：补 `"VLESS"` / `"VMESS"` / `"TROJAN"`。
- `ProxyConfig::ProxyRules::ParseFromString`：识别 `vless://` / `vmess://` / `trojan://` 段。**关键修复**：原始解析按 `=` 拆分 `http=...;https=...`，但 leaf URI 的 query 几乎一定带 `=`（`type=ws`、`encryption=none`），如果当成 per-scheme 会变成空 list 然后 fallback DIRECT。新增的 `IsLeafOutboundProxyUriToken` 把这种 token 直接作为 `PROXY_LIST` 处理。

```110:148:net/proxy_resolution/proxy_config.cc
  base::StringViewTokenizer proxy_server_list(proxy_rules, ";");
  while (proxy_server_list.GetNext()) {
    std::string_view trimmed_segment = base::TrimWhitespaceASCII(
        proxy_server_list.token(), base::TRIM_ALL);
    if (trimmed_segment.empty()) {
      continue;
    }
    if (IsLeafOutboundProxyUriToken(trimmed_segment)) {
      if (type == Type::PROXY_LIST_PER_SCHEME) {
        continue;
      }
      AddProxyURIListToProxyList(trimmed_segment, &single_proxies,
                                 ProxyServer::SCHEME_HTTP,
                                 allow_bracketed_proxy_chains, is_quic_allowed);
      type = Type::PROXY_LIST;
      continue;
    }
    ...
```

### 4.7 测试

- `net/base/proxy_string_util_unittest.cc`
  - `VlessXraySharingLinkRoundTrip`：`vless://uuid@host:port?...#tag` 解析出 credential / query / fragment 后再序列化等于原串。
  - `VmessShareLinkBase64Json`：覆盖 base64 JSON 解析。
  - `TrojanHandshakeMatchesXrayLayout`：`SHA224("pw")` hex (56) + CRLF + cmd(1) + atyp(3) + len + host + port(2) + CRLF。
- `net/proxy_resolution/proxy_config_unittest.cc::ParseFromString_LeafOutboundUriWithEqualsInQuery`：覆盖 §4.6 的 `=` 修复。
- `components/proxy_config/proxy_config_dictionary_unittest.cc::CreateFixedServersReverseBypass`：覆盖 §五的 `reverse_bypass` 字段。

---

## 五、Prefs 与 ProxyConfig

### 5.1 `ProxyConfigDictionary` 增加 `reverse_bypass`

```52:75:components/proxy_config/proxy_config_dictionary.h
  // When true, |bypass_list| hosts use the fixed proxy and all other hosts go
  // direct (Android "proxy override" semantics). When false (default), the
  // bypass list excludes hosts from the proxy.
  bool GetReverseBypass(bool* out) const;
  ...
  static base::Value::Dict CreateFixedServers(const std::string& proxy_server,
                                              const std::string& bypass_list,
                                              bool reverse_bypass = false);
```

`CreateDirect` / `CreateAutoDetect` / `CreatePacScript` / `CreateSystem` 也跟着补默认 `false` 参数；`CreateDictionary` 内部新增字段 `kProxyReverseBypass`（`"reverse_bypass"`）持久化到 dict。

### 5.2 三个新 Pref 名

```13:21:components/proxy_config/proxy_config_pref_names.h
inline constexpr char kProxy[] = "proxy";

// When ENABLE_CHROMIUM_LEAF: optional overrides applied after parsing |kProxy|.
// Non-empty values replace the fixed proxy URI and/or split-tunnel host list
// for the effective net::ProxyConfig (see PrefProxyConfigTrackerImpl).
inline constexpr char kChromiumLeafVlessUri[] = "proxy.chromium_leaf.vless_uri";
inline constexpr char kChromiumLeafProxyHostPatterns[] =
    "proxy.chromium_leaf.proxy_host_patterns";
```

### 5.3 默认 fixed_servers + reverse_bypass

`PrefProxyConfigTrackerImpl::RegisterPrefs` / `RegisterProfilePrefs` 在 `ENABLE_CHROMIUM_LEAF + CHROMIUM_LEAF_BUILTIN_DEFAULT_PROXY` 同时为真时，注册：

```637:670:components/proxy_config/pref_proxy_config_tracker_impl.cc
  registry->RegisterDictionaryPref(
      proxy_config::prefs::kProxy,
      ProxyConfigDictionary::CreateFixedServers(
          std::string(net::kChromiumLeafDefaultProxyUri),
          std::string(net::kChromiumLeafDefaultProxyHostPatterns),
          /*reverse_bypass=*/true));
  ...
  registry->RegisterStringPref(proxy_config::prefs::kChromiumLeafVlessUri, "");
  registry->RegisterStringPref(
      proxy_config::prefs::kChromiumLeafProxyHostPatterns, "");
```

否则注册 `CreateSystem()`（与上游一致）。

### 5.4 浏览器侧覆写：`ApplyChromiumLeafBrowserPrefOverrides`

```37:81:components/proxy_config/pref_proxy_config_tracker_impl.cc
void ApplyChromiumLeafBrowserPrefOverrides(
    const PrefService* pref_service,
    net::ProxyConfigWithAnnotation* config) {
  ...
  const std::string uri_override =
      pref_service->GetString(proxy_config::prefs::kChromiumLeafVlessUri);
  if (!uri_override.empty()) {
    ...
    parsed.ParseFromString(uri_override, allow_bracketed, allow_quic);
    if (parsed.type == net::ProxyConfig::ProxyRules::Type::PROXY_LIST &&
        !parsed.single_proxies.IsEmpty()) {
      pc.proxy_rules().single_proxies = parsed.single_proxies;
      pc.proxy_rules().type = net::ProxyConfig::ProxyRules::Type::PROXY_LIST;
    }
  }
  const std::string patterns = pref_service->GetString(
      proxy_config::prefs::kChromiumLeafProxyHostPatterns);
  if (!patterns.empty()) {
    pc.proxy_rules().bypass_rules.ParseFromString(patterns);
    // Inverted bypass: only matching hosts use the fixed proxy ...
    pc.proxy_rules().reverse_bypass = true;
  }
  *config = net::ProxyConfigWithAnnotation(pc, traffic_annotation);
}
```

被 `ReadPrefConfig` 在 `PrefConfigToNetConfig` 成功之后调用。这样 **运行时** 就能用：

```cpp
profile->GetPrefs()->SetString(proxy_config::prefs::kChromiumLeafVlessUri,
                                "trojan://pw@host:443?type=ws&security=tls&...");
profile->GetPrefs()->SetString(
    proxy_config::prefs::kChromiumLeafProxyHostPatterns, "*.youtube.com,*.googlevideo.com");
```

切换内置代理。两个 pref 也注册到 `PrefProxyConfigTrackerImpl::proxy_prefs_`，变化会触发 `OnProxyPrefChanged`，通知所有观察者重新计算 `ProxyConfig`。

### 5.5 `GetEffectiveProxyConfig` 让 `CONFIG_FALLBACK` 压过系统代理

默认注册的 fixed_servers 是 `CONFIG_FALLBACK` 状态（用户没动过），它**不**满足 `PrefPrecedes`，所以原版 Chromium 在 OS 报告系统代理时会忽略 prefs。新增的分支：

```575:587:components/proxy_config/pref_proxy_config_tracker_impl.cc
#if BUILDFLAG(ENABLE_CHROMIUM_LEAF) && BUILDFLAG(CHROMIUM_LEAF_BUILTIN_DEFAULT_PROXY)
  if (pref_state == ProxyPrefs::CONFIG_FALLBACK) {
    *effective_config = pref_config;
    VLOG(1) << "[LEAF_PROXY_DEBUG] GetEffectiveProxyConfig branch="
                    "BuiltinLeafFallbackOverridesSystem ...";
    return net::ProxyConfigService::CONFIG_VALID;
  }
#endif
```

效果：内置 Leaf 配置始终生效；用户在系统代理面板里设的内容会被忽略，除非通过 policy/extension（满足 `PrefPrecedes`）显式覆盖。

### 5.6 `PrefConfigToNetConfig` 强制全通道

profile 持久化 dict 里如果残留 `bypass_list` + `reverse_bypass`（例如老配置只代理 `www.youtube.com`），就会把 `googlevideo.com` / `ytimg.com` 等放过，媒体直接断流。当 PROXY_LIST 中任意一项是 leaf outbound 时，强制清空：

```697:725:components/proxy_config/pref_proxy_config_tracker_impl.cc (摘要)
if (any_leaf_outbound) {
  proxy_config.proxy_rules().bypass_rules.Clear();
  proxy_config.proxy_rules().reverse_bypass = false;
  ...
}
```

> 注意：这里清空只针对 **dict 持久化的** `bypass_list`，**不会**影响 §5.4 在覆写阶段重新写入的 patterns。

### 5.7 `ProxyConfigServiceImpl::UpdateProxyConfig` 的额外日志

打开 `ENABLE_CHROMIUM_LEAF` 时所有关键决策点都加了 `VLOG(1) << "[LEAF_PROXY_DEBUG] ..."`，配合 `--vmodule=pref_proxy_config_tracker_impl=1` 即可看到：

- `RegisterPrefs / RegisterProfilePrefs` 注册了哪条默认 URI；
- `ReadPrefConfig` 拿到的 raw dict 与最终 `net::ProxyConfig` JSON；
- `GetEffectiveProxyConfig` 走的是哪个分支（`PrefPrecedes` / `BuiltinLeafFallbackOverridesSystem` / `SystemUnset_*` / `UseSystemProxy` / `SystemPlusOverrideRules`）；
- `PrefConfigToNetConfig` 是否触发了全通道强制。

---

## 六、内置默认 URI 与示例

### 6.1 `net::kChromiumLeafDefaultProxyUris[]`

```648:672:net/base/proxy_string_util.cc
const char* const kChromiumLeafDefaultProxyUris[] = {
    // [0] Default: Trojan over WS+TLS.
    "trojan://Hw0hemotiQ@www.ettreasure.com:30508"
    "?type=ws&path=%2F&host=&security=tls&fp=chrome&alpn=h2%2Chttp%2F1.1"
    "#tzw53key",
    // [1] Alternate: VLESS over WS on 30507.
    "vless://85ad7b82-738b-44f7-91ce-64a1ff53a314@www.ettreasure.com:30507"
    "?encryption=none&security=none&type=ws&host=www.ettreasure.com&path=%2F30507"
    "#kxinarvy",
    // [2] Alternate: VMess (v2rayN Base64 JSON share).
    "vmess://ewogICJ2IjogIjIiLAogICJwcyI6ICJoN2M3bzE5dCIsCi...",
};
const char kChromiumLeafDefaultProxyHostPatterns[] = "*.baidu.com";
```

`kChromiumLeafDefaultProxyUri` 取 `[0]`，写进 `prefs::kProxy` dict 的 `proxy_server` 字段；`kChromiumLeafDefaultProxyHostPatterns` 写进 `bypass_list`，并把 `reverse_bypass=true`，所以默认 **只有** `*.baidu.com` 走 Trojan，其它直连。改方案的两种方式：

- 编译期：直接改 `kChromiumLeafDefaultProxyUris[0]` / `kChromiumLeafDefaultProxyHostPatterns`。
- 运行时：写 `kChromiumLeafVlessUri` / `kChromiumLeafProxyHostPatterns`（§5.4）；空字符串=不覆盖。

### 6.2 URI 关键字总表

| Key | 含义 / 取值 | 适用 |
|---|---|---|
| `type` | `tcp`（默认） / `ws` | VLESS / VMess / Trojan |
| `security` | `none`（默认） / `tls` / `xtls` | 决定是否在传输层包 TLS |
| `host` | WS Upgrade `Host:` 头；缺省回退到 `leaf_proxy_authority_host_` | type=ws |
| `path` | WS path；自动补前导 `/`；可百分号编码 | type=ws |
| `sni` | TLS SNI；缺省=代理 host | security=tls/xtls |
| `alpn` | 仅作为 URI 注释保留；实际 ALPN 由 §七 决定 | security=tls/xtls |
| `encryption` / `cipher` | VMess body 加密：`aes-128-gcm` / `chacha20-poly1305`（缺省） | VMess |
| `flow` | `xtls-rprx-vision` 启用 Vision；其它当成 plain | VLESS |
| `useragent` / `userAgent` | 覆盖 WS Upgrade `User-Agent:`；缺省 = 浏览器风固定串 | type=ws |
| `origin` | 设置 `Origin:` 头；缺省不发 | type=ws |
| `fp` | 仅作 URI 兼容字段（与 Xray streamSettings 对齐），未参与握手 | 任意 |

> 表里所有 query 值都会被 `LeafVlessQueryLookup` → `GetValueForKeyInQuery` 自动 URL-decode；`path=%2F30507` 拿出来就是 `"/30507"`。

---

## 七、ConnectJob 接入

### 7.1 `LeafSocketParams` 与 `ConnectJobParams` 变体

`net/socket/connect_job_params.{h,cc}` 在 variant 里追加 `scoped_refptr<LeafSocketParams>`，新增 `is_leaf_outbound() / leaf() / take_leaf()`。

`LeafSocketParams` 字段（`net/socket/leaf_connect_job.h`）：

| 字段 | 来源 |
|---|---|
| `nested_proxy_connect_params_` | `ConnectJobParams`：要么是 `TransportSocketParams`（裸 TCP 到代理），要么是 `SSLSocketParams`（外裹 TLS） |
| `destination_` | 真正要访问的最终目标 `host:port` |
| `network_anonymization_key_` | 标准 net 隔离键 |
| `traffic_annotation_` | 同上 |
| `protocol_` | `LeafOutboundProtocol::{kVless, kVmess, kTrojan}` |
| `leaf_credential_` | URI 的 userinfo（VLESS/VMess UUID、Trojan password） |
| `leaf_uri_query_` / `leaf_uri_fragment_` | URI 原样保留 |
| `leaf_proxy_authority_host_` | 代理本身的 host，作为 WS `Host:` 默认值 |

> ① 里 `nested_proxy_connect_params_` 还叫 `transport_params_`（只能裸 TCP）；③ 把它升成 `ConnectJobParams`，因此 `LeafConnectJob::DoTransportConnect` 既能起 `TransportConnectJob`、又能起 `SSLConnectJob`，对应 §7.2 的 `security=tls`。

### 7.2 `connect_job_params_factory.cc::CreateProxyParams`

```text
proxy_server.is_leaf_outbound()
   │
   │  if security=tls / xtls  →  先包一层 SSLSocketParams
   │     - SNI 用 query 的 sni= 覆盖 proxy host
   │     - ALPN：type=ws → kHttp11Only；否则 kHttpAll
   │     - SessionUsage = kProxy
   │     - disable_cert_verification_network_fetches = true
   ▼
LeafSocketParams(
    transport / ssl,
    final_dest,
    NAK,
    annotation,
    LeafOutboundProtocolForProxyScheme(scheme),
    proxy_server.credential(),
    proxy_server.leaf_uri_query(),
    proxy_server.leaf_uri_fragment(),
    proxy_server.host_port_pair().host()  // authority host
);
```

ALPN 强制 HTTP/1.1 的逻辑：

```283:302:net/socket/connect_job_params_factory.cc
const std::string_view leaf_transport_type =
    LeafUriQueryLookup(proxy_server.leaf_uri_query(), "type");
const ConnectJobFactory::AlpnMode leaf_proxy_alpn_mode =
    base::EqualsCaseInsensitiveASCII(leaf_transport_type, "ws")
        ? ConnectJobFactory::AlpnMode::kHttp11Only
        : ConnectJobFactory::AlpnMode::kHttpAll;
```

### 7.3 `ConnectJobFactory` 注入

`net/socket/connect_job_factory.{h,cc}` 构造函数追加 `std::unique_ptr<LeafConnectJob::Factory>`；`CreateConnectJob` 里：

```150:160:net/socket/connect_job_factory.cc
if (connect_job_params.is_leaf_outbound()) {
  return leaf_connect_job_factory_->Create(
      request_priority, socket_tag, common_connect_job_params,
      connect_job_params.take_leaf(), delegate, /*net_log=*/nullptr);
}
```

### 7.4 `SSLConnectJob` 多一个 `LEAF_PROXY` 分支

`SSLSocketParams::ConnectionType` 增加 `LEAF_PROXY`；`SSLConnectJob` 状态机加：

```text
STATE_LEAF_HANDSHAKE
STATE_LEAF_HANDSHAKE_COMPLETE
```

`DoLeafHandshake` 起一个 `LeafConnectJob` 跑握手，握手成功后把 `nested_socket_` 取出来，再继续 `STATE_SSL_CONNECT`。这条路径用于 **应用层 SSL 跑在 Leaf 之上**（最终目标是 HTTPS）：

```text
TCP → (代理 TLS, type=ws) → WS Upgrade → VLESS/VMess/Trojan 框架 → SSL → HTTP request
```

### 7.5 `client_socket_pool_manager_impl.cc`

把 leaf outbound 归到 `socks_socket_pool` bucket（pool 复用 SOCKS 一类，避免每个新协议都建独立 pool）：

```110:115:net/socket/client_socket_pool_manager_impl.cc
} else if (proxy_chain.Last().is_socks() ||
           proxy_chain.Last().is_leaf_outbound()) {
  type = "socks_socket_pool";
}
```

### 7.6 NetLog

`net/log/net_log_event_type_list.h` 新增 `EVENT_TYPE(LEAF_PROXY_CONNECT)` 与 `EVENT_TYPE(LEAF_CONNECT_JOB_CONNECT)`；`net/log/net_log_source_type_list.h` 新增 `SOURCE_TYPE(LEAF_CONNECT_JOB)`。

### 7.7 上游过滤白名单

凡是「过滤掉不支持的代理 scheme」的地方都要加上三个新 scheme：

- `net/http/http_stream_factory_job_controller.cc`：`supported_proxies` 与 `HistogramProxyUsed` 的 `max_scheme`。
- `services/network/proxy_resolving_client_socket.cc::DoProxyResolveComplete`：`RemoveProxiesWithoutScheme(...)` 加 VLESS/VMESS/TROJAN。
- `chrome/browser/extensions/api/proxy/proxy_api_helpers.cc::CreateProxyServerDict`：把 scheme 转字符串时补 `"vless"` / `"vmess"` / `"trojan"`。

### 7.8 `PlatformSocketDescriptor()`

`StreamSocket` 增加虚函数：

```149:154:net/socket/stream_socket.h
  // If this socket is (or wraps only) a connected platform TCP stream whose
  // handle is safe to use with synchronous send(), returns it; otherwise
  // kInvalidSocket. Used by Leaf outbound bootstrap without RTTI (Chromium
  // builds with /GR-).
  virtual SocketDescriptor PlatformSocketDescriptor() const;
```

`TCPClientSocket` 暴露 `socket_->SocketDescriptorForTesting()`，其它 Stream 默认返回 `kInvalidSocket`。这是为将来直接调用 `chromium_leaf_outbound_handshake`（基于 `RawFd/RawSocket` 的 Tokio 路径）预留的钩子。

---

## 八、`LeafClientSocket`：握手与 I/O

`LeafClientSocket` 在 `net/socket/leaf_client_socket.{h,cc}`，行为像 `SOCKS5ClientSocket`：自身是一个 `StreamSocket`，包住底层 `transport_socket_`，对上提供异步 `Connect / Read / Write`。

### 8.1 状态机

```text
HandshakeState
  kNone
  kTcpWriteVless / kTcpWriteVlessComplete
  kWsWriteHttp   / kWsWriteHttpComplete
  kWsReadHeaders / kWsReadHeadersComplete
  kWsWriteFrameComplete
  // (内部使用) kTcpReadPrefix*, kTcpReadAddon*, kWsReadFirstFrame*
```

`Connect()` 入口的关键流程（`net/socket/leaf_client_socket.cc`）：

1. 校验 `protocol_ ∈ {kVless, kVmess, kTrojan}`、`leaf_credential_` 非空。
2. 按协议构造 `handshake_vless_hdr_`：
   - **VLESS plain**：`LeafVlessBuildRequestHeader` → 走 Rust `chromium_leaf_vless_plain_tcp_header`（如开 `ENABLE_CHROMIUM_LEAF`），否则纯 C++ 实现。
   - **VLESS Vision**：`flow=xtls-rprx-vision` → `LeafVlessBuildVisionRequestHeader`（必须经过 Rust，附 18 字节 `add-on`）。
   - **VMess**：`LeafVmessBuildClientRequest`（C++，§九）。`encryption` / `cipher` 决定 body 算法（`aes-128-gcm` / `chacha20-poly1305`，默认 chacha20）。
   - **Trojan**：`LeafTrojanBuildRelayHandshake`（§十）。
3. 解析 `type=`：
   - 空或 `tcp` → `kTcpWriteVless`，先把握手头写出。
   - `ws` → 构造 `LeafVlessBuildWebSocketUpgradeRequest(path, host, sec_key, ua, origin)`，先做 WebSocket Upgrade，101 之后再发 masked binary 帧装握手头。

```text
TCP path:
  Write(handshake_hdr) → 立即返回 OK，把 need_strip_vless_response_ 置位
  （服务端只有等 destination 给数据时才会回 prelude，
    所以把"剥 2-byte 响应"延迟到第一次 Read。VMess 不剥；Vision 走 VisionParser。）

WS path:
  Write(GET ... HTTP/1.1 ...) → 读完 \r\n\r\n → 验证首行 101
       └─ TLS 经常把 101 头和首个 WS frame 合包，余下字节放进 ws_rx_accumulator_
  Build masked binary frame(handshake_hdr) → Write
       └─ 同样 need_strip_vless_response_ = true，第一次 ReadWithWsFraming() 时再剥
```

### 8.2 WebSocket Upgrade 头（浏览器风）

```203:236:net/socket/chromium_leaf_vless_handshake.cc
out.append("GET ").append(ws_path).append(" HTTP/1.1\r\n");
out.append("Host: ").append(ws_host).append("\r\n");
out.append("Connection: Upgrade\r\n");
out.append("Pragma: no-cache\r\n");
out.append("Cache-Control: no-cache\r\n");
out.append("User-Agent: ").append(ua).append("\r\n");
out.append("Upgrade: websocket\r\n");
out.append("Sec-WebSocket-Version: 13\r\n");
out.append("Sec-WebSocket-Key: ").append(sec_ws_key).append("\r\n");
if (!orig.empty()) out.append("Origin: ").append(orig).append("\r\n");
out.append("\r\n");
```

- `User-Agent` 缺省固定为 `Mozilla/5.0 ... Chrome/131.0.0.0 Safari/537.36`，与 CDN 抓取行为对齐；URI 给 `useragent=` / `userAgent=` 可覆盖。
- `Sec-WebSocket-Key` 由 `base::RandBytes(16)` + base64 现场生成。
- 所有外部输入字符串都过 `StripHeaderInjection`（删 `\r\n`），防 header 注入。

### 8.3 数据面

WS 帧解析在 `PopOneWebSocketFrame`：完整覆盖 7-bit / 16-bit / 64-bit 长度 + mask；只接 `0x2`（binary）作为 payload，丢弃 ping/pong（`0x9` / `0xA`）。`PullNextCompleteWsMessageIntoPending` 处理 WebSocket 消息分片（FIN=0 帧累积到 `ws_message_fragment_`，FIN=1 时合并）。

发送侧：

- **非 WS**：`Write` 直接转 `transport_socket_->Write`。
- **WS（VLESS/Trojan）**：每次 user `Write` 都包成一个 masked binary 帧。
- **WS + VMess**：先 `vmess_engine_->QueuePlainText(buf)`，循环 `ProduceCipherText` 拿 AEAD 包，再外裹 masked WS 帧；`FlushVmessCipherWrites` 处理 partial-write、异步回调。
- **TCP + VMess**：同上，但跳过 WS framing。

接收侧：

- **VLESS plain**：第一次 Read 把 2 字节 prelude + 2 字节 addon 全剥掉，再交付明文。
- **VLESS Vision**：`vision_parser_->Feed(...)`，结果累积到 `vision_rx_queue_`。
- **VMess**：`vmess_engine_->FeedCipherText(...)` 解 AEAD chunk，明文累积到 `vmess_rx_plain_`。
- **Trojan**：服务端不返回任何前缀，直接透传明文。

### 8.4 `ReadIfReady` 必须返回 `ERR_NOT_IMPLEMENTED`

```1153:1170:net/socket/leaf_client_socket.cc (摘要)
int LeafClientSocket::ReadIfReady(...) {
  ...
  if (use_vision_)   return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  if (vmess_engine_) return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  if (ws_framing_)   return ERR_READ_IF_READY_NOT_IMPLEMENTED;
  return transport_socket_->ReadIfReady(buf, buf_len, std::move(callback));
}
```

> 这是 ① 修过的关键 bug：`SocketBIOAdapter` 优先用 `ReadIfReady`，但 Leaf 侧需要把 prelude 剥掉才能交付明文，无法支持 ReadIfReady 语义。返回 `ERR_NOT_IMPLEMENTED` 后 BIO 会回退到 `Read()`，TLS over WS 才能跑通。

---

## 九、VMess AEAD 实现要点

代码位置：`net/socket/leaf_vmess_engine.{cc,h}`，命名空间 `net::chromium_leaf`。

### 9.1 KDF（v2fly/Xray 对齐）

```text
HMAC-SHA256 with base key "VMess AEAD KDF", recursively reseeded by `path` segments.
```

C++ 用一个 `VmessGoHmac` 包装 BoringSSL 的 SHA256_CTX 模拟 Go `crypto/hmac.New(parent.Create, key)` 的行为；`VmessAeadKdf(key, path, out[32])` 是入口。所有盐字面量都对上 v2fly 源码：

```text
"AES Auth ID Encryption"
"AEAD Resp Header Len Key" / "AEAD Resp Header Len IV"
"AEAD Resp Header Key"      / "AEAD Resp Header IV"
"VMess Header AEAD Key"     / "VMess Header AEAD Nonce"
"VMess Header AEAD Key_Length" / "VMess Header AEAD Nonce_Length"
```

### 9.2 客户端 hello (`LeafVmessBuildClientRequest`)

明文头 (`plain`)：

```text
ver(1) | iv(16) | key(16) | resp_header(1) | option(1)
       | (padding_len<<4 | security)(1)    | reserved(1) | cmd(TCP=1)(1)
       | port(2) | atyp(IPv4=1/Domain=2/IPv6=3) | addr...
       | random padding(padding_len)
       | fnv1a(plain_so_far)(4)
```

外层 AEAD 流程：

1. `auth_raw[16]`：`time(8)` + `rand(4)` + `crc32(auth_raw[0..12])`，再用 `AES-128-ECB` 用 `kdf16("AES Auth ID Encryption")` 加密。
2. `conn_nonce[8] = rand`。
3. `len_ct = AES-128-GCM(seal len_plain[2], k=kdf16("VMess Header AEAD Key_Length", auth_raw, conn_nonce), iv=kdf("Nonce_Length", ...)[0..12], aad=auth_raw)`.
4. `hdr_ct = AES-128-GCM(seal plain, k=kdf16("VMess Header AEAD Key", ...), iv=kdf("VMess Header AEAD Nonce", ...)[0..12], aad=auth_raw)`.
5. `wire = auth_raw || len_ct || conn_nonce || hdr_ct`.

`encryption` 选择 chacha20 时，`request_body_key` 走 `MD5(req_key) || MD5(MD5(req_key))` 派生 32 字节作为 chacha20 key（v2fly 兼容）。

### 9.3 chunk 框架（`LeafVmessStreamEngine`）

- **OPT** 默认 `kRequestOptionChunkStream | kRequestOptionChunkMasking | kRequestOptionGlobalPadding`。
- 每个 chunk：`length(2, 大端) | ciphertext(plain_len + tag) | padding`。
  - **`length` 加密**：`length_real ^ shake_xof(req/resp_iv) 16-bit mask`，外面再加 padding。
  - **`padding` 长度**：`shake.next() % 64`。**关键陷阱**：SHAKE 调用顺序必须是「先 padding 长度，再 length mask」（v2fly 源码顺序），写反了对端会按错误的 padding 跳。
- **nonce**：`request_body_iv[2..12]` 拼上 `chunk_nonce(2 bytes, 大端)`，`chunk_nonce` 从 `0xffff` 起逐 chunk +1（即 `0`、`1` …）。
- **响应头**：先解 `len_ct → resp_inner_len`，再解 `hdr_ct → resp_payload`，校验首字节等于 `request_body_key[0..1]`（约定的 response identifier）。

### 9.4 chunk 限额

```text
constexpr size_t kTagLen = 16;
constexpr size_t kMaxPayload = 0x4000;  // 16 KiB
```

`SealPayloadChunkWithPadding` 自动按 16 KiB 切块。

---

## 十、Trojan 实现要点

代码：`net/socket/chromium_leaf_vless_handshake.{cc,h}::LeafTrojanBuildRelayHandshake`。

```text
hex(SHA224(password)) (56)  CRLF (2)  CMD=1 (1)  ATYP+ADDR  PORT (2)  CRLF (2)
                                                ─────────────
ATYP=1: IPv4 (4)
ATYP=3: domain length (1) + domain (n)
ATYP=4: IPv6 (16)
```

实现里**关键 bug 修复**：原版漏写了 CMD 字节（v2fly/Xray 的 `proxy/trojan/protocol.go::ConnWriter.writeHeader` 必须写 1=TCP / 3=UDP），少这一字节会被对端按错位偏移解析整段 SOCKS-style 地址。`LeafClientSocket::Connect` 对 Trojan 的处理路径完全复用 VLESS 的 TCP / WS 状态机（不需要 Vision，不需要 AEAD body）。

测试覆盖：

```513:524:net/base/proxy_string_util_unittest.cc
TEST(ProxySpecificationUtilTest, TrojanHandshakeMatchesXrayLayout) {
  std::vector<uint8_t> h = LeafTrojanBuildRelayHandshake("pw", "example.com", 443);
  ASSERT_FALSE(h.empty());
  constexpr size_t kHostLen = 11;
  ASSERT_EQ(56u + 2u + 1u + 1u + 1u + kHostLen + 2u + 2u, h.size());
  EXPECT_EQ('\r', h[56]);
  EXPECT_EQ('\n', h[57]);
  EXPECT_EQ(1u, h[58]) << "TCP command byte (Xray trojan ConnWriter)";
  EXPECT_EQ(3u, h[59]) << "domain address type";
  EXPECT_EQ(static_cast<uint8_t>(kHostLen), h[60]);
}
```

---

## 十一、调试与排查

### 11.1 日志开关

所有诊断走 `[LEAF_PROXY_DEBUG]` 前缀；commit ③ 起，**非失败路径** 改成 `VLOG(1)`，**真错误**仍为 `LOG(ERROR)`。打开方式：

```text
--vmodule=pref_proxy_config_tracker_impl=1,
          proxy_config=1,
          connect_job_params_factory=1,
          leaf_connect_job=1,
          leaf_client_socket=1,
          chromium_leaf_vless_handshake=1
```

### 11.2 NetLog 事件

| 事件 / 源 | 来源 |
|---|---|
| `LEAF_PROXY_CONNECT` | `LeafClientSocket::Connect` BeginEvent / EndEventWithNetErrorCode |
| `LEAF_CONNECT_JOB_CONNECT` | `LeafConnectJob` 的 ConnectJob 父事件 |
| `LEAF_CONNECT_JOB`（source type） | NetLog source 标签 |

### 11.3 常见错误

| 现象 | 推断 |
|---|---|
| `Leaf WS not 101 header_prefix=...` | WS Upgrade 失败：检查 `host` / `path` / TLS SNI 是否对得上 CDN / Xray 配置 |
| `LeafVlessStripServerResponse failed` | 首响应不足 `2 + addon` 字节，通常是 TCP 半连接 / 握手被中间盒截断 |
| `VMess FeedCipherText failed` | AEAD 校验失败：UUID 错、`encryption` 与服务端不匹配、`option` 与服务端不一致（global padding / chunk masking） |
| `BuiltinLeafFallbackOverridesSystem` 没出现 | `chromium_leaf_builtin_default_proxy=false`，或者 prefs 已被 user/policy/extension 覆盖 |
| `fixed_servers prefs are IGNORED when OS reports a system proxy` | 走到 `UseSystemProxy` 分支，说明 `CONFIG_FALLBACK` 分支条件没满足；要么 prefs 已被 user 改过，要么没开 `CHROMIUM_LEAF_BUILTIN_DEFAULT_PROXY` |

### 11.4 抓包

- **WS upgrade 阶段** 直接 wireshark 看到（除非外裹 TLS）。
- **裹了 TLS** 时建议 `--ssl-key-log-file=...` 配 wireshark。
- VMess 的 chunk 解出后是明文 HTTP / TLS 流，通常你只关心 outer TLS。

---

## 十二、代码总览（按职责索引）

| 子系统 | 文件 |
|---|---|
| 构建开关 | `net/features.gni`、`net/BUILD.gn` |
| 第三方依赖 | `DEPS`、`.gitmodules`、`third_party/chromium_leaf/` |
| Rust shim + cxx | `third_party/chromium_leaf/{BUILD.gn,src/lib.rs,src/plain_tcp_header.rs,src/sync_framing.rs}` |
| C++ 包装 | `net/socket/chromium_leaf_glue.{h,cc}` |
| URI / ProxyServer | `net/base/proxy_server.{h,cc}`、`net/base/proxy_string_util.{h,cc}`、`net/base/proxy_chain.cc`、`net/proxy_resolution/proxy_config.cc` |
| Mojom | `services/network/public/mojom/network_param.mojom`、`services/network/public/cpp/network_param_mojom_traits.{h,cc}` |
| Prefs / Config | `components/proxy_config/proxy_config_dictionary.{h,cc}`、`components/proxy_config/proxy_config_pref_names.h`、`components/proxy_config/pref_proxy_config_tracker_impl.cc` |
| ConnectJob 接入 | `net/socket/connect_job_params.{h,cc}`、`net/socket/connect_job_factory.{h,cc}`、`net/socket/connect_job_params_factory.cc`、`net/socket/ssl_connect_job.{h,cc}`、`net/socket/client_socket_pool_manager_impl.cc` |
| LeafConnectJob | `net/socket/leaf_connect_job.{h,cc}`、`net/socket/leaf_outbound_protocol.h` |
| LeafClientSocket | `net/socket/leaf_client_socket.{h,cc}` |
| 协议握手 | `net/socket/chromium_leaf_vless_handshake.{h,cc}`、`net/socket/leaf_vmess_engine.{h,cc}` |
| StreamSocket 钩子 | `net/socket/stream_socket.{h,cc}`、`net/socket/tcp_client_socket.{h,cc}` |
| Network service 过滤白名单 | `services/network/proxy_resolving_client_socket.cc` |
| HTTP stack 过滤白名单 | `net/http/http_stream_factory_job_controller.cc`、`net/http/http_proxy_connect_job.cc` |
| Extensions Proxy API | `chrome/browser/extensions/api/proxy/proxy_api_helpers.cc` |
| NetLog | `net/log/net_log_event_type_list.h`、`net/log/net_log_source_type_list.h` |
| Xenon 探针 | `xenon_overlay/services/xenon_service_impl.cc`（探测目标改为 `https://example.com/`） |
| 测试 | `net/base/proxy_string_util_unittest.cc`、`net/base/proxy_server_unittest.cc`、`net/proxy_resolution/proxy_config_unittest.cc`、`net/socket/connect_job_factory_unittest.cc`、`components/proxy_config/proxy_config_dictionary_unittest.cc` |

---

## 十三、移植到其它 Chromium 分支的 Checklist

按依赖顺序执行（每步都可独立编过）：

1. **构建开关**：在 `net/features.gni` / `net/BUILD.gn` 加两个 buildflag。
2. **Rust shim**：拷贝整份 `third_party/chromium_leaf/`，`DEPS` 加 `leaf_revision`、`.gitmodules` 加 submodule。`gn check` 应能通过 `//third_party/chromium_leaf:chromium_leaf_ffi`。
3. **数据模型**：`ProxyServer` / `ProxyChain` / Mojom（`network_param.mojom` + `traits`）。这一步会让所有 `switch (scheme)` 编译失败，逐个加 case 即可（list 见 §七、§十二）。
4. **URI/PAC**：`proxy_string_util.{h,cc}` + `proxy_config.cc`。带上 `proxy_string_util_unittest.cc` 的三条测试。
5. **Prefs**：`proxy_config_dictionary` + `pref_proxy_config_tracker_impl` + `proxy_config_pref_names.h`，确认 `RegisterPrefs` / `RegisterProfilePrefs` 在 ENABLE_CHROMIUM_LEAF + CHROMIUM_LEAF_BUILTIN_DEFAULT_PROXY 时种 fixed_servers + reverse_bypass。
6. **NetLog**：加事件 / source。
7. **ConnectJob**：`leaf_outbound_protocol.h` → `leaf_connect_job.{h,cc}` → `connect_job_params*` → `connect_job_factory*` → `connect_job_params_factory.cc` → `ssl_connect_job.{h,cc}` 多 `LEAF_PROXY` 分支。
8. **LeafClientSocket**：`net/socket/leaf_client_socket.{h,cc}` + `chromium_leaf_vless_handshake.{h,cc}`。先验证 VLESS plain TCP / WS。
9. **VMess**：`leaf_vmess_engine.{h,cc}`，加测试覆盖 KDF。
10. **Trojan**：`LeafTrojanBuildRelayHandshake` + `LeafClientSocket::Connect` 的 `kTrojan` 分支。
11. **运行时**：`profile->GetPrefs()->SetString(kChromiumLeafVlessUri, "...")` 配合 `kChromiumLeafProxyHostPatterns` 做 split-tunnel；不写就走内置默认。
12. **CI**：建议至少跑 `net_unittests --gtest_filter='*Leaf*:*Vmess*:*Trojan*:*Vless*'` + `services_unittests --gtest_filter='*ProxyServer*'` + `components_unittests --gtest_filter='*ProxyConfig*'`。

---

## 附录 A：与 Brave 仓库的差异

`F:\brave_browser\src` 没有这些改动，所以如果将来要把 Leaf 出站搬过去：

- Brave 自己的 `brave/components/brave_proxy/` 里的 dict 创建函数也要补 `reverse_bypass=` 参数。
- Brave 渠道扩展（`extensions/api/brave_*`）若依赖 `ProxyServer::Scheme` 枚举，同样需要补三个 case。
- Brave 默认通过 `BraveProfileManager` 设置 `prefs::kProxy`；建议把内置 URI 走 `kChromiumLeafVlessUri` 浏览器 pref 通道，而**不**改 Brave 自带的默认 dict，避免与 Brave Sync 冲突。
