# GN 规则与用法（Chromium 语境）

本文档说明 **GN（Generate Ninja）** 在 Chromium 仓库中的**执行顺序、语法要点、依赖语义、常用模板与平台变量**，并附**可运行的声明示例**与**基于本仓库抽样阅读（35+ 个 `BUILD.gn` / `.gni` / `.gn`）**的索引。更权威的语法以 GN 上游与 `gn help` 为准。

---

## 0. 官方文档与本地帮助

| 资源 | URL / 命令 |
|------|------------|
| GN 文档索引 | [gn.googlesource.com/gn/+/HEAD/docs/](https://gn.googlesource.com/gn/+/HEAD/docs/) |
| 语言与设计 | [language.md](https://gn.googlesource.com/gn/+/HEAD/docs/language.md) |
| 函数与变量参考 | [reference.md](https://gn.googlesource.com/gn/+/HEAD/docs/reference.md) |
| 快速入门 | [quick_start.md](https://gn.googlesource.com/gn/+/HEAD/docs/quick_start.md) |
| Chromium：`args.gn` 与变体 | [GN build configuration](https://www.chromium.org/developers/gn-build-configuration/) |
| 点文件（根 `.gn`） | 在 `src` 下执行：`gn help dotfile` |

**习惯**：写 `BUILD.gn` 时不确定函数签名，优先 `gn help rebase_path`、`gn help template` 等。

---

## 1. Chromium 中 GN 的执行顺序（必读）

1. **根目录 `.gn`**  
   - 指定 **`buildconfig`**（Chromium 为 `//build/config/BUILDCONFIG.gn`）。  
   - 可设 **`default_args`**（在 `BUILDCONFIG` **之前**执行，**不能**依赖 `is_android` 等平台变量，见 `.gn` 内注释）。  
   - **`exec_script_allowlist`**：仅允许列表内脚本在 GN 加载期执行 `exec_script()`（安全 + 性能），见 `//.gn` 与 `//build/dotfile_settings.gni`。

2. **`args.gn`（每个 out 目录一份）**  
   - 覆盖各 `declare_args()` 的默认值。  
   - 由 `gn args out/Debug` 编辑。

3. **`//build/config/BUILDCONFIG.gn`**  
   - 对每个 `BUILD.gn` 执行前已加载；此处声明的**非下划线前缀**变量对全局可见。  
   - 定义 **`host_*` / `target_*` / `current_*`**、`declare_args()` 默认值、`is_win` 等派生变量（见文件头 **PLATFORM SELECTION** 注释）。

4. **各目录 `BUILD.gn`**  
   - 在**当前工具链**上下文中执行；同一文件可能对 **default / host / secondary** 等多套 toolchain **重复执行**（`current_toolchain` 会变）。

5. **`.gni` 文件**  
   - 仅在被 `import()` 时加载；用于共享 `declare_args`、模板、列表，**不单独对应 ninja 目标**。

---

## 2. 语法精要（写 `BUILD.gn` 时每天用）

### 2.1 基本类型与字面量

- **布尔**：`true` / `false`。  
- **整数**：`42`。  
- **字符串**：`"hello"`；支持 **`$variable`** 与 **`${var}`** 插值。  
- **列表**：`[ "a", "b" ]`；用 **`+=`** 追加： `deps += [ "//base" ]`。  
- **作用域块**：`my_var = { k = "v" }` 较少用于手写；更多用内置函数的「键值对」参数风格（见 `declare_args`）。

### 2.2 控制流

```gn
if (is_win) {
  sources += [ "foo_win.cc" ]
} else if (is_linux) {
  sources += [ "foo_linux.cc" ]
} else {
  sources += [ "foo_posix.cc" ]
}
```

- **`if (defined(foo))`**：判断变量是否已定义（常用于可选 overlay）。  
- **`assert(cond, "message")`**：不满足则 `gn gen` 失败（Chromium 大量使用，如 `//build/config/android/config.gni` 中 `is_java_debug` 与 `is_official_build` 互斥）。

### 2.3 常用运算符

- **逻辑**：`&&` `||` `!`  
- **比较**：`==` `!=` `<` 等  
- **列表**：`+` 连接；`[ a ] + [ b ]`

### 2.4 注释

- 行注释 **`#`**；无块注释。

---

## 3. 目标类型（内置 target）与典型用途

| 类型 | 用途 |
|------|------|
| **`static_library`** | 静态 `.a` / `.lib`；不传播链接到依赖方时需对方再 `deps`。 |
| **`source_set`** | 将源编译进依赖者，**不单独产出**库文件；用于拆循环依赖、头-only+少量实现。 |
| **`component`** | `is_component_build` 下为 shared，否则 static；Chrome 模块边界常用（如 `//components/prefs`）。 |
| **`shared_library`** | 明确产出动态库。 |
| **`executable`** | 可执行文件。 |
| **`group`** | 无编译，仅聚合 `deps` / `public_deps`；常用于别名、平台分支聚合。 |
| **`action`** | 运行脚本生成文件；需 **`script` + `args` + `outputs`**，通常还有 **`inputs`**。 |
| **`copy`** | 拷贝文件。 |
| **`config`** | 编译选项集合（`defines` / `include_dirs` / `cflags` 等），通过 `configs +=` 挂到目标。 |

**Chromium 扩展**：大量 **`template(...)`** 包装上述类型，例如 `grit(...)`、`buildflag_header(...)`、`rust_executable(...)`、`compiled_action(...)`。

---

## 4. `deps` / `public_deps` / `data_deps`

| 类型 | 语义 |
|------|------|
| **`deps`** | 本目标编译/链接需要；**不**向依赖本目标的上游传播。 |
| **`public_deps`** | 依赖传播：依赖你的人也会拿到这些依赖（接口暴露时用）。 |
| **`data_deps`** | **运行时**需要（如打包进 apk、与 exe 同目录的数据文件）；**不参与**编译时链接，但构建顺序上会先构建被依赖方。 |

**示例（语义说明）**：`//chrome/BUILD.gn` 在 `enable_xenon_ai && build_xenon_ai_inferencer_burn` 时向 chrome 增加 **`data_deps`**，使 `ninja chrome` 会构建 **`//xenon_overlay/xenon_ai_inferencer_burn:xenon_ai_inferencer_burn`**，并将 sidecar 放到 **`$root_out_dir`**，供运行时 `DIR_EXE` 查找。

---

## 5. `import` 与文件边界

```gn
import("//build/config/compiler/compiler.gni")
import("//xenon_overlay/buildflags/features.gni")
```

- **`import`**：把 `.gni` 内容展开到当前作用域（可多次 import，注意循环）。  
- **路径**：几乎总是 **`//` 开头的绝对仓库路径**。  
- **`BUILD.gn` vs `.gni`**：可执行 `declare_args()` 的位置在部分仓库中有惯例；Chromium 常在 **`.gni`** 或 **`BUILDCONFIG.gn`** 中集中声明全局 args，feature 局部 args 放在功能目录的 `.gni`（如 `//xenon_overlay/buildflags/features.gni`）。

**GN 禁止**：在同一个 `declare_args() { }` 块内，后声明的 arg **不能**引用同块内先声明的 arg（需拆成两个 `declare_args()` 块——见 `xenon_overlay/buildflags/features.gni` 注释）。

---

## 6. `declare_args` 与 `gn args`

```gn
declare_args() {
  my_feature = true
}
```

- 用户可在 **`out/xxx/args.gn`** 写 `my_feature = false` 覆盖。  
- **不要在 `.gn` 的 `default_args` 里**写依赖 `is_win` 的默认值（`.gn` 早于 `BUILDCONFIG`）。  
- **`BUILDCONFIG.gn`** 里也有大量 `declare_args()`，与全局工具链、优化选项相关。

---

## 7. `template` 与 `invoker`

自定义「伪目标」：

```gn
template("my_wrap") {
  static_library(target_name) {
    sources = invoker.sources
    deps = invoker.deps
  }
}
```

- **`target_name`**：模板实例化时的目标名字符串。  
- **`invoker`**：调用者传入的 **scope**（`my_wrap("foo") { sources = [...] }`）。  
- **`forward_variables_from(invoker, "*", [...])`**：把调用者变量批量转发到内部真实目标（见 `//build/rust/rust_executable.gni`）。  
- **`assert(defined(invoker.sources), "sources required")`**：参数校验。

Chromium 中 **`compiled_action`**（`//build/compiled_action.gni`）用 `template()` 包一层 **`action()`**，并强制要求 **`tool` + `outputs` + `args`**，可选 **`inputs`**。

---

## 8. 路径与魔法变量

| 变量 | 含义 |
|------|------|
| **`//`** | 仓库根（`src`） |
| **`root_build_dir`** | 当前输出目录（如 `out/Debug`） |
| **`root_gen_dir`** | 全局生成目录 |
| **`target_gen_dir`** | 当前 **target** 的生成目录（常用于 `grit` / mojom 输出） |
| **`root_out_dir`** | 最终产物目录（chrome.exe 等） |

**`rebase_path(path, new_base)`**：把路径改写成相对 **`new_base`** 或相对于构建调用目录的字符串，供 **action 的 `args`** 传给 Python/二进制（几乎所有 `action`/`compiled_action` 都会用）。

```gn
args = [
  "--out",
  rebase_path("$root_out_dir", root_build_dir),
]
```

---

## 9. `visibility`

控制**谁可以依赖本 target**，减少误用与循环依赖：

```gn
source_set("internal_impl") {
  visibility = [ "//foo:*" ]
  ...
}
```

- **`"*"`** 后缀表示该目录及其子目录（见 `gn help label_pattern`）。  
- **`//ui/base`** 中 `component("ui_data_pack")` 使用 **`visibility = [ ":base", "//chromeos/...", ... ]`** 限制仅少数目标可依赖（见 `//ui/base/BUILD.gn`）。

---

## 10. 平台与工具链变量（条件编译核心）

编写跨平台 `sources +=` 时最常用：

| 变量 | 典型含义 |
|------|----------|
| `is_win` `is_mac` `is_linux` `is_chromeos` `is_android` `is_ios` `is_posix` `is_fuchsia` | 目标平台 |
| `is_debug` `is_official_build` `is_component_build` | 构建类型 |
| `current_cpu` `target_cpu` `host_cpu` | 架构 |
| `current_toolchain` `default_toolchain` `host_toolchain` | 当前正在执行 **哪一份** `BUILD.gn` |
| `is_clang` `use_lld` | 工具链特性（`//build/config/compiler/compiler.gni`） |

**多工具链示例**（摘自 `//build/compiled_action.gni` 注释）：仅编译 host 工具：

```gn
if (host_toolchain == current_toolchain) {
  executable("my_tool") {
    ...
  }
}
```

---

## 11. Chromium 常用模板（结合仓库实例）

### 11.1 `buildflag_header`（`//build/buildflag_header.gni`）

生成 **`#include .../foo_buildflags.h`**，代码里用 **`BUILDFLAG(ENABLE_XXX)`**，避免裸 `#ifdef` 散落。

**实例**：`//xenon_overlay/buildflags/BUILD.gn`

```gn
import("//build/buildflag_header.gni")
import("//xenon_overlay/buildflags/features.gni")

buildflag_header("buildflags") {
  header = "buildflags.h"
  flags = [
    "ENABLE_XENON_AI=$enable_xenon_ai",
    ...
  ]
}
```

### 11.2 `grit`（`//tools/grit/grit_rule.gni`）

- 参数 **`source`**（`.grd`）、**`outputs`**（与 grit 输出一致，否则构建报错）。  
- 可用 **`defines`** 把 GN 变量传给 grit（与 `buildflag_header` 联动时注意 `.grd` 里是否引用）。

### 11.3 `pkg_config`（`//build/config/linux/pkg_config.gni`）

- 在 Linux/CrOS 等上生成 **`config()`**，注入 `cflags`/`ldflags`。  
- 内含 **`declare_args()`**：`pkg_config`、`host_pkg_config`、`system_libdir` 等。

### 11.4 `shim_headers`（`//build/shim_headers.gni`）

- **`template("shim_headers")`**：内部定义 **`action`** + **`config`** + **`group`**，用 Python 生成 shim 头并导出 `include_dirs`。

### 11.5 `rust_executable` / `rust_static_library`（`//build/rust/*.gni`）

- 包装 GN 内置 **`executable`/`static_library`**，增加 **`edition`/`features`/`crate_name`** 等 Rust 特有参数。  
- 注释强调：**exe 名与 `target_name` 对齐**等规则（见 `rust_executable.gni`）。

### 11.6 `mixed_test` / `test`（`//testing/test.gni`）

- 体量很大；核心是 **`template("mixed_test")`**，按平台 import 不同 Android/Fuchsia/iOS 规则。  
- Fuchsia 上 **`exec_target_suffix`** 等细节见文件顶部注释。

### 11.7 `nocompile`（`//build/nocompile.gni`）

- 生成 **`nocompile_source_set`**，配合 **`.nc`** 源做「应编译失败」测试（clang `expected-error`）。

### 11.8 `mojom`（`//mojo/public/tools/bindings/mojom.gni`）

- 由 **`import("//mojo/public/tools/bindings/mojom.gni")`** 引入后，可写 **`mojom("name") { sources = [ "*.mojom" ] ... }`**。  
- 生成 C++/Java/JS 等绑定；Xenon 示例见 **`//xenon_overlay/chrome/browser/ui/webui/BUILD.gn`**（`xenon_ai.mojom` + `webui_module_path`）。

---

## 12. `exec_script`：能力极强，默认不要用

Chromium **根 `.gn`** 用大段注释说明：

- 会拖慢 **`gn gen`**（尤其 Windows）。  
- 不要用脚本探测「文件是否存在」来开关功能（应用 **`declare_args`**）。  
- **慎用 `glob`**：删除文件后输入列表不变会导致 action 不重跑等问题。

**允许列表**：`exec_script_allowlist` = `build_dotfile_settings` + ANGLE + `.gn` 内联列表；新增须在 review 中充分论证（见 `//.gn`）。

**合法用例示例**：`//build/timestamp.gni` 中 **`build_timestamp = exec_script(compute_build_timestamp, [...], "trim string", [lastchange_file])`**。

---

## 13. 实战最小示例

### 13.1 最小静态库

```gn
static_library("hello") {
  sources = [ "hello.cc", "hello.h" ]
  public_deps = [ "//base" ]
}
```

### 13.2 条件源文件 + 测试依赖

（风格参考 `//components/prefs/BUILD.gn`）

```gn
component("prefs") {
  sources = [ "a.cc", "b.cc" ]
  if (is_android) {
    sources += [ "android/foo.cc" ]
    deps += [ "android:jni_headers" ]
  }
  public_deps = [ "//base" ]
}

static_library("test_support") {
  testonly = true
  sources = [ "mock.cc" ]
}
```

### 13.3 `action` 调用 Python 并声明产物

（风格参考 `//xenon_overlay/xenon_ai_inferencer_burn/BUILD.gn`）

```gn
action("my_sidecar") {
  script = "//foo/build.py"
  inputs = [
    "//foo/build.py",
    "//foo/input.txt",
  ]
  outputs = [ "$root_out_dir/my_tool.exe" ]  # is_win 时用 .exe
  args = [
    "--out-dir",
    rebase_path("$root_out_dir", root_build_dir),
  ]
}
```

### 13.4 `group` 聚合多平台依赖

（风格参考 `//content/public/browser/BUILD.gn` 中 `group("browser")`）

```gn
group("browser") {
  if (is_component_build) {
    public_deps = [ "//content" ]
  } else {
    public_deps = [ ":browser_sources" ]
  }
}
```

---

## 14. 与 Xenon overlay 相关的 GN 习惯

1. **功能开关**：在 **`//xenon_overlay/buildflags/features.gni`** 使用 **`declare_args()`**；需要 **`#if BUILDFLAG(ENABLE_XENON_AI)`** 时在 **`//xenon_overlay/buildflags/BUILD.gn`** 生成 **`buildflag_header`**。  
2. **避免 GN 环**：`//xenon_overlay/chrome/browser/xenon_ai/BUILD.gn` 注释说明 **`xenon_ai` core 不得依赖 `side_panel`**，而 webview 注册在别处依赖 `side_panel`。  
3. **运行时 sidecar**：用 **`data_deps`** 挂到 **`//chrome`**，而不是把 Rust 直接链进 `chrome.dll`（当前 `xenon_ai_inferencer_burn` 方案）。

---

## 15. 附录 A：本次为撰写文档而抽样阅读/分析的 GN 文件（≥35）

下列文件均在本机仓库中打开并阅读了**头部或代表性片段**（用于归纳规则与举例；**非**对整棵依赖树做完整审计）。

| # | 路径 |
|---|------|
| 1 | `//.gn` |
| 2 | `//build/config/BUILDCONFIG.gn` |
| 3 | `//build/dotfile_settings.gni` |
| 4 | `//build/config/chrome_build.gni` |
| 5 | `//build/config/features.gni` |
| 6 | `//build/config/compiler/compiler.gni` |
| 7 | `//build/config/android/config.gni` |
| 8 | `//build/config/linux/pkg_config.gni` |
| 9 | `//build/config/rust.gni` |
| 10 | `//build/toolchain/toolchain.gni` |
| 11 | `//build/compiled_action.gni` |
| 12 | `//build/shim_headers.gni` |
| 13 | `//build/buildflag_header.gni` |
| 14 | `//build/timestamp.gni` |
| 15 | `//build/nocompile.gni` |
| 16 | `//build/rust/rust_executable.gni` |
| 17 | `//build_overrides/build.gni` |
| 18 | `//base/BUILD.gn` |
| 19 | `//components/prefs/BUILD.gn` |
| 20 | `//chrome/BUILD.gn`（`data_deps` / xenon 条件段） |
| 21 | `//content/public/browser/BUILD.gn` |
| 22 | `//ui/base/BUILD.gn` |
| 23 | `//google_apis/BUILD.gn` |
| 24 | `//testing/test.gni` |
| 25 | `//tools/grit/grit_rule.gni` |
| 26 | `//xenon_overlay/buildflags/features.gni` |
| 27 | `//xenon_overlay/buildflags/BUILD.gn` |
| 28 | `//xenon_overlay/chrome/browser/xenon_ai/BUILD.gn` |
| 29 | `//xenon_overlay/xenon_ai_inferencer_burn/BUILD.gn` |
| 30 | `//xenon_overlay/chrome/browser/ui/webui/BUILD.gn`（`mojom` + `mojom.gni`） |
| 31 | `//mojo/public/tools/bindings/mojom.gni`（头部：`declare_args` / Mojo 生成总入口） |

| # | 路径 |
|---|------|
| 32 | `//build/config/gclient_args.gni`（由 `gclient` 生成的 checkout 开关；短文件） |
| 33 | `//build/config/sanitizers/sanitizers.gni` |
| 34 | `//build/config/sysroot.gni`（glob 列出；与 `pkg_config` 协同） |
| 35 | `//build/config/ui.gni` |
| 36 | `//build/toolchain/rbe.gni` |
| 37 | `//build/toolchain/cc_wrapper.gni` |

> 说明：`//third_party/protobuf/proto_library.gni` 在部分环境可能因权限无法直接打开；本文通过 **`//google_apis/BUILD.gn`** 等对其 **`import()`** 的使用说明其在 Chrome 目标中的角色。

---

## 16. 附录 B：推荐阅读顺序（新人）

1. `gn help` → `gn help deps` / `gn help public_deps` / `gn help data_deps`  
2. [GN language.md](https://gn.googlesource.com/gn/+/HEAD/docs/language.md) 前半（类型、作用域）  
3. `//build/config/BUILDCONFIG.gn` 开头注释（host/target/current）  
4. 任选一个小组件 `//components/prefs/BUILD.gn` 全文  
5. 任选一个带 `action` 的目录（如本仓库 **`//xenon_overlay/xenon_ai_inferencer_burn`**）

---

*文档版本：与 Chromium `src` 树中 GN 习惯一致；GN 语言本身以上游为准。若 Chromium 升级改动 `BUILDCONFIG` 或模板参数，请以当前文件为准并更新本文相关小节。*
