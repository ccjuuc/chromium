# Chromium 接入 Rust 库：流程说明与 `rust.md` / `ffi.md` / `rust.gni` 详解

本文面向需要在 Chromium 树内 **引入或编写 Rust 代码、并与 C++ 互操作** 的开发者：先给出 **可执行的端到端流程**，再逐份解释官方文档 **`docs/rust.md`**、**`docs/rust/ffi.md`** 与构建配置 **`build/config/rust.gni`** 的职责与要点；**从 crates.io 正式导入依赖** 的细则见 **[附录 A](#appendix-a-readme-importing)**。

---

## 一、为什么要走这套流程？

- **安全**：特权进程（Browser / GPU 等）若用 C++ 处理 **不可信数据**，易触发内存安全类漏洞；官方政策见 [Rule of 2](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/security/rule-of-2.md)。**Rust 被定位为可在这些进程内直接处理不可信数据、又不必再拆 Utility 进程** 的方案之一（见 `rust.md` 的 *Why?*）。
- **构建统一**：Rust 必须通过 **GN + Ninja** 与 C/C++ 同一套工具链、LTO、sanitize 等策略对齐；**不能**在正式产品路径上只靠本机随意 `cargo build`。

---

## 二、端到端流程（按场景分支）

### 2.1 先决条件

1. 仓库使用 Chromium 的 **标准构建**（`gn gen` + `ninja`），且 **`enable_rust`** 为真（默认在 `build_with_chromium` 为真时开启，见下文 `rust.gni`）。
2. 阅读 **`docs/rust.md`**（总纲）、**`docs/rust/ffi.md`**（与 C++ 互操作）；若需从 crates.io 导入 crate，见上游 **`third_party/rust/README-importing-new-crates.md`** 或本文 **[附录 A](#appendix-a-readme-importing)**。
3. 代码评审需 **OWNERS** 同意；无单独的「申请用 Rust」流程（见 `rust.md` *Status*）。

### 2.2 场景 A：自有（first-party）Rust 代码

| 步骤 | 动作 |
|------|------|
| 1 | 在合适的 `BUILD.gn` 中使用 **`rust_static_library`**（**不要**用 GN 内置的 `rust_library`），见 `build/rust/rust_static_library.gni`。 |
| 2 | 多个 **first-party** crate 之间依赖时，用 **`chromium::import!`**（来自 `//build/rust/chromium_prelude`），**不要**直接 `use other_crate::...`，以避免 crate 名全局不唯一问题。**依赖标准库** 或 **`//third_party/rust` 里的 crate** 时仍可用普通 `use`。 |
| 3 | 需要被 C++ 调用或与 C++ 双向调用：按 **`docs/rust/ffi.md`**，优先使用 **`cxx`** 写 `#[cxx::bridge]`；C++ 侧包含生成的头文件并链接对应目标。 |
| 4 | 单元测试：使用 **`//testing/rust_gtest_interop`**（见 `rust.md`）。 |
| 5 | 日志：可使用集成进 `//base` 的 **`log`** crate，替代部分 `LOG(...)` 场景（component build 下可能有局限，见 `rust.md` TODO）。 |

### 2.3 场景 B：第三方 — crates.io

| 步骤 | 动作 |
|------|------|
| 1 | 按 **[附录 A](#appendix-a-readme-importing)** 使用 **`run_gnrt.py`** 把依赖写入 **`chromium_crates_io`**、**vendor**、**生成 `BUILD.gn`**。 |
| 2 | 在业务 `BUILD.gn` 中用 **`cargo_crate`** / 对生成的 **`//third_party/rust/<crate>/vX_Y:lib`** 加 **`deps`**（附录 A 第 6–7 步）。 |
| 3 | 版本更新通常走 **`tools/crates/create_update_cl.md`** 描述的半自动流程。 |

### 2.4 场景 C：第三方 — 非 crates.io

- 放在 **`//third_party/rust` 之外**（例如自有 Git 子树）。
- 仍可依赖 **`//third_party/rust`** 中的 crate，并使用 **`//build/rust/*.gni`**；**没有**与 crates.io 相同的自动更新工具链。
- 官方举例：`//third_party/crabbyavif`、`//third_party/cloud_authenticator`（见 `rust.md`）。

### 2.5 互操作与不稳定特性

- **FFI**：必读 **`docs/rust/ffi.md`**（下文第三节展开）。
- **Rust nightly / 不稳定特性**：默认 **不支持**；需与 Rust 工具链团队达成一致，见 **`tools/rust/unstable_rust_feature_usage.md`**。

### 2.6 仅做实验、想临时用 cargo

- 可用 **`tools/crates/run_cargo.py`**（使用 Chromium 自带的 `//third_party/rust-toolchain` 里的 cargo），并配置 **`.cargo/config.toml`** 指向已 vendored 的依赖（见 `rust.md` *Using cargo*）。

---

## 三、`docs/rust.md` 详解（总纲文档在说什么）

**路径**：`docs/rust.md`  

**定位**：Rust 在 Chromium 中的 **政策、状态、first-party / third-party 用法入口、测试与日志、不稳定特性、临时 cargo** 的 **单一入口说明**。

### 3.1 章节对应关系

| 章节 | 内容摘要 |
|------|----------|
| **Why?** | 不可信数据处理 + Rule of 2；Rust 用于在特权进程内安全处理，而不强制再拆进程。 |
| **Status** | 全平台支持；**M119** 起生产级；仓库内任意位置可用（受互操作能力限制）；无额外审批流程，仅需常规 OWNERS；联系方式（邮件、Slack 等）。 |
| **First-party Rust libraries** | 必须用 **`rust_static_library`**；crate 间用 **`chromium::import!`**；链接到 **`docs/rust/ffi.md`**。 |
| **Mapping of Chromium APIs** | **gtest**：`//testing/rust_gtest_interop`；**日志**：`log` crate 与 `//base` 集成。 |
| **Third-party — crates.io** | 见 `README-importing-new-crates.md`；用 **`cargo_crate`**；更新流程见 `tools/crates/create_update_cl.md`。 |
| **Third-party — other** | 非 crates.io 放 `//third_party/rust` 外；无自动导入工具。 |
| **Unstable features** | 默认禁止；见 `unstable_rust_feature_usage.md`。 |
| **VSCode / cargo** | 分别指向 `docs/rust/dev_experience_tips_and_tricks.md` 与 `run_cargo.py` + vendored crates。 |

### 3.2 你需要记住的三句话

1. **自有 Rust = `rust_static_library` +（crate 间）`chromium::import!`**。  
2. **crates.io = `cargo_crate` + 官方 README 流程**。  
3. **与 C++ 接线 = 先看 `ffi.md`，优先 `cxx`**。

---

## 四、`docs/rust/ffi.md` 详解（FFI 与 C++ 如何对接）

**路径**：`docs/rust/ffi.md`  

**定位**：说明 **C++/Rust 边界** 上 **推荐工具、不支持的工具、以及 FFI 层常用 Rust 写法**，**不是** Rust 语言教程。

### 4.1 支持的 FFI 工具

| 工具 | 说明 |
|------|------|
| **`cxx`**（推荐） | 见 [cxx.rs](https://cxx.rs/)；教学补充：[Comprehensive Rust / Chromium interoperability](https://google.github.io/comprehensive-rust/chromium/interoperability-with-cpp.html)。 |
| **`bindgen`** | 从头文件生成 Rust 绑定；用法见 **`//build/rust/rust_bindgen.gni`**。 |

**当前 `//build/rust/*.gni` 不正式支持**：`cbindgen`、`crubit`（文档明确列出）。

### 4.2 `cxx` 最佳实践（摘自 `ffi.md`）

- C++ 生成代码应落在 **明确的 `namespace`**，例如：`#[cxx::bridge(namespace = "my_project")]`.
- **尽量在一个** `#[cxx::bridge]` **块内** 维护绑定声明；多 `bridge` 虽可共享类型，但有坑。

### 4.3 FFI 层 Rust 惯用法（文档建议）

- 用 **`From` / `TryFrom`**（或 **`Into` / `TryInto`** 以绕过 orphan rule）在 **FFI 类型** 与 **第三方 crate 类型** 之间转换（文中有 Skia `rust_png` 等链接示例）。
- 用 **`?`** 做错误传递，再在 **最外层** 转成 C++ 友好的状态码（文中有 `zip_ffi_glue.rs` 等示例链接）。
- 使用 **`let Ok(x) = ... else { ... }`** 等模式简化错误分支。

### 4.4 与 `rust.md` 的关系

- **`rust.md`** 告诉你「要写 Rust、用哪个 GN 模板」。  
- **`ffi.md`** 告诉你「C++ 和 Rust 边界怎么画、优先用什么库、错误怎么传出」。

---

## 五、`build/config/rust.gni` 详解（构建开关与工具链从哪来）

**路径**：`build/config/rust.gni`  

**定位**：被 **`import("//build/config/rust.gni")`** 的全局 Rust 构建配置：  
**是否启用 Rust**、**Chromium prelude**、**工具链路径**、**目标三元组 `rust_abi_target`**、**ThinLTO**、**bindgen 路径**、**与自定义工具链的关系** 等。  
**注意**：官方约定 **`enable_rust` 只能在 `//build` 与 `//testing` 里查询**；**不要**在 `//base`、`//net` 等目录用 `enable_rust` 做条件分支（见文件内 `declare_args` 注释）。

### 5.1 `declare_args()` 中与「接库」最相关的变量

| 变量 | 含义 |
|------|------|
| **`enable_rust`** | 是否参与 Rust 构建。默认 **`build_with_chromium`**；纯引用 `//build` 的第三方工程可关掉，以免依赖 Rust 工具链。 |
| **`enable_chromium_prelude`** | 是否启用 **`chromium::import!`** 所需的 prelude；依赖 `//third_party/rust` 的 syn/quote 等。Chromium 内 **不支持关闭**；下游若关闭则需自行保证 **`crate_name` 全局唯一**（强烈不推荐）。 |
| **`rust_sysroot_absolute`** | 非空时使用 **自定义** Rust sysroot 绝对路径；为空则使用 **`//third_party/rust-toolchain`**。自定义路径为 **社区支持**，CQ 不保证。 |
| **`rust_bindgen_root`** | **`bindgen`** 可执行文件所在根目录，默认 `//third_party/rust-toolchain`。 |
| **`rustc_version`** | 自定义工具链时填 **`rustc -V` 输出**，用于在工具链变更时触发 **全量重编** Rust 目标。 |
| **`toolchain_supports_rust_thin_lto`** | Rust 产物是否参与 **ThinLTO**；需 **链接器与 rustc 所用 LLVM** 能消费对方产生的 IR（文档引用若干 bug）。 |
| **`enable_rust_cxx`** | 使用 **CXX** 相关能力时；注释说明 **带分配器的 Chromium shim 与 cxx 有关**；默认随 **`enable_rust`**。 |

### 5.2 派生逻辑（理解构建行为）

- **`use_chromium_rust_toolchain`**：`rust_sysroot_absolute == ""` 时为真 → 使用树内 **`//third_party/rust-toolchain`**。  
- **`rustc_nightly_capability`**：与是否用 Chromium 自带工具链相关；为 false 时 **避免 nightly 特性**（无 bot 保证，但接受补丁）。  
- **`toolchain_has_rust`**：当前 GN 工具链是否具备 Rust 支持（如 **wasm 不支持** `chromium_toolchain_supports_platform`）。  
- **`rust_sysroot`**：最终传给 rustc 的 sysroot 路径（树内或绝对路径）。  
- **`rust_abi_target`**： lengthy 的 **`if (is_linux)` / `is_android` / `is_win` …** 分支，把 **Chromium 的 cpu/os** 映射到 **Rust target triple**（如 `x86_64-pc-windows-msvc`、`x86_64-unknown-linux-gnu`）；若启用 Rust 且 triple 不在 **`//build/rust/known-target-triples.txt`** 会 **assert**（需先登记）。  
- **`rust_macro_toolchain`**：过程宏需在 **host**、且无 sanitize 等限制时使用单独 toolchain（注释说明 proc macro 与预编译 rustc 加载动态库的限制）。  
- **`can_build_rust_unit_tests`**：例如 **Android** 上单元测试形态特殊，当前为 **false**（见文件内 TODO）。  

### 5.3 读 `rust.gni` 的实际用途

- **排查「为什么我这台配置不编 Rust」**：看 **`enable_rust`**、**`toolchain_has_rust`**、**`rust_abi_target`** 是否为空。  
- **排查链接/LTO 问题**：看 **`toolchain_supports_rust_thin_lto`** 与 LLVM 版本一致性问题。  
- **下游项目**：可关 **`enable_rust`** / **`enable_chromium_prelude`**，但需承担 **用 C/C++ 重写** 或 **自行维护 crate 名** 的成本（见注释）。

---

## 六、关键 GN 模板与文件索引

| 用途 | 文件 |
|------|------|
| 自有 Rust 静态库 | `build/rust/rust_static_library.gni` |
| crates.io 包装 | `build/rust/cargo_crate.gni` |
| 通用 Rust 目标 / prelude | `build/rust/gni_impl/rust_target.gni`、`build/rust/chromium_prelude/` |
| bindgen | `build/rust/rust_bindgen.gni` |
| CXX 集成 | `third_party/rust/cxx/...`、`build/rust/` 下与 `enable_rust_cxx` 相关的 gni（见 `rust_target.gni` 头部） |

---

## 七、官方文档路径速查

| 文档 | 路径 |
|------|------|
| Rust 总纲 | `docs/rust.md` |
| FFI | `docs/rust/ffi.md` |
| 构建配置 | `build/config/rust.gni` |
| 从 crates.io 导入 | `third_party/rust/README-importing-new-crates.md` |
| `//third_party/rust` 评审清单 | `third_party/rust/OWNERS-review-checklist.md` |
| 通用第三方入库 | `docs/adding_to_third_party.md` |
| 构建错误（含 GN 未列出资源） | `docs/rust/build_errors_guide.md` |
| 不稳定特性 | `tools/rust/unstable_rust_feature_usage.md` |
| 开发体验（VSCode 等） | `docs/rust/dev_experience_tips_and_tricks.md` |

---

<a id="appendix-a-readme-importing"></a>

## 附录 A：`README-importing-new-crates` 规则与操作步骤

以下内容合并自上游 **`//third_party/rust/README-importing-new-crates.md`** 与 **`//third_party/rust/OWNERS-review-checklist.md`** 的要点，便于离线查阅；**以源码树内原文为准**。

[Web 版 README](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/third_party/rust/README-importing-new-crates.md)

### A.1 适用范围

把 **[crates.io](https://crates.io/)** 上的库导入 Chromium 后，下列代码路径才能 **正式依赖** 它：

- Chromium 自有代码（如 `//chrome`、`//components`）
- 复用同一套 `//third_party/rust` 的工程（文档举例 Pdfium、V8）
- 其它第三方目录中依赖 Rust 的组件（如 `//third_party/cloud_authenticator/cbor`）

**评审**：须满足 **`OWNERS-review-checklist`**，且所有第三方库还须遵守 **`docs/adding_to_third_party.md`** 的通用第三方流程。

### A.2 两个「真相来源」文件

| 文件 | 作用 |
|------|------|
| **`//third_party/rust/chromium_crates_io/Cargo.toml`** | 声明 **直接** 依赖的 crate 及版本、features。这是 **标准 Cargo.toml**，但 **根 crate 本身不会被编译**，只用于 **解析依赖与 feature**。**传递依赖不必手写**，由 **gnrt** 等工具自动展开。 |
| **`//third_party/rust/chromium_crates_io/gnrt_config.toml`** | Chromium 专用元数据（与纯 cargo 无关）：**`allow_unsafe`**、**`allow_unstable_features`**、**`extra_src_roots`**、**`group`**（如 `test` / `sandbox`）、**许可证文件**（无法自动推断时的 **`license_files`**）等。 |

补丁目录：**`//third_party/rust/chromium_crates_io/patches`**（说明见同目录 **`patches/README.md`**）。

### A.3 导入步骤（在 checkout **根目录**执行）

1. **登记依赖**（任选其一）  
   - `vpython3 ./tools/crates/run_gnrt.py add foo` → 添加 **foo** 最新版  
   - `vpython3 ./tools/crates/run_gnrt.py add foo@1.2.3` → 指定版本  
   - 或 **手改** `chromium_crates_io/Cargo.toml`，版本从 crates.io 查询  

2. **下载 / vendor 源码**  
   - `./tools/crates/run_gnrt.py vendor`  
   - 会应用 `patches/` 下补丁  

3. **（可选）仅测试/工具使用**  
   在 **`gnrt_config.toml`** 中：  
   `[crate.foo]`  
   `group = "test"`  

4. **生成各 crate 的 `BUILD.gn`**  
   - `vpython3 ./tools/crates/run_gnrt.py gen`  

5. **加入 git**  
   - `git add -f third_party/rust/chromium_crates_io/vendor`（**`-f` 重要**，vendor 内文件可能被 `.gitignore`）  
   - `git add third_party/rust`  

6. **纳入 GN/Ninja/CQ（必须）**  
   仅 vendor **不会** 被 `gn` 自动全量编进产品。须在 **某个已有 target** 上增加 **`deps`**，指向例如：  
   `//third_party/rust/some_crate/v123:lib`  
   之后可验证：  
   `autoninja -C out/Default third_party/rust/some_crate/v123:lib`  
   这样 **CQ** 才能在 rustc 升级或新告警时覆盖该 crate。

7. **业务侧引用**  
   在自有 `BUILD.gn` 里 **`deps += [ "//third_party/rust/..." ]`**，并在需要处使用 **`cargo_crate`** 模板（`build/rust/cargo_crate.gni`）— 与生成出的目标名保持一致即可。

**版本批量更新**：见 **`tools/crates/create_update_cl.md`**。

### A.4 安全与 `group`（Security 节）

- 若 **发布用** 库经评审后认为 **不满足** [rule of 2](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/security/rule-of-2.md)，应标为 **`sandbox`**，表示 **不可在特权进程使用**：  
  `[crate.foo]`  
  `group = "sandbox"`  

- 若某 **传递依赖** 从 **`safe` 变为 `sandbox`**，导致依赖链 **跨 group**，**`gnrt vendor` 会失败**。需 **修复 unsafe 评审问题**，或 **同步调整** 链上其它 crate 的 **`group`**（在 `gnrt_config.toml` 中显式设置）。

### A.5 评审清单摘要（`OWNERS-review-checklist.md`）

- **新 crate**：**ATL / 通用第三方** 审批与 **`//third_party/rust/OWNERS`**；纯新增传递依赖有时需 **FYI ATL**（见 checklist 正文）。  
- **`unsafe`**：需有 reviewer 过目；期望 **块小、封装好、有说明**（与 rule of 2 一致）。  
- **proc macro / `build.rs`**：构建期执行代码须关注 **确定性、封闭性**（hermetic）。  
- **已高度信任的作者**（如 rust-lang、`windows-*`、Google/Chromium 相关项目）可 **减轻** 评审负担；可用 **`ban_features`** 限制 feature。  
- **许可证**：多由 **gnrt** 生成 **`README.chromium`** 时检查；新许可证类型可能需改 gnrt（见 checklist 指向的 readme 流程）。  
- **显式不审**：tests、benchmarks、examples 可不按同等级审。

### A.6 故障排除

若出现类似 **`Rust source file or input not in GN sources`**（例如 **`include_str!`** 引用了未列入 GN 的 `README.md`）：

- 在 **`gnrt_config.toml`** 中声明缺失文件为额外输入，再执行 **`run_gnrt.py gen`**。  
- 详见 **`docs/rust/build_errors_guide.md`**（搜索 “Rust source file or input not in GN inputs”）。

### A.7 与 §2.6「临时 cargo」的关系

- **附录 A** 描述的是 **正式纳入 `//third_party/rust` + gn + ninja + CQ** 的路径。  
- **`run_cargo.py` + vendored 依赖** 用于 **实验脚本**，**不能替代** `add → vendor → gen → deps`；若独立工程 **永不** 进入 `chromium_crates_io`，则 **不适用** 附录 A，但应在项目 README 中 **单独说明** 构建方式。

---

*本文是对 Chromium 源码树内上述文件的整理与释义；若与上游不一致，以当前分支及 [rust.md](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/rust.md) 为准。*
