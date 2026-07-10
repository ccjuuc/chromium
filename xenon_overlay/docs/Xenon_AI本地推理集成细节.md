# Xenon AI：`xenon_ai_inferencer_burn` 本地推理集成 — 提交细节说明

本文档对应提交 **`e676bce455f83`**（`Xenon AI: Integrate local xenon_ai_inferencer_burn for menu rewrite`），按模块把**实现细节、协议、线程模型、构建路径与边界行为**写细，便于评审与后续维护。

---

## 1. 元信息与变更规模

| 项 | 内容 |
|----|------|
| Commit | `e676bce455f838cdecbe8f13c9e29efbcaca5559` |
| 变更文件数 | 15 |
| 行数说明 | `+6630` 主要来自 **`Cargo.lock`**（约 5.5k 行）；其余为 C++/Rust/Python/GN |

---

## 2. 端到端架构（从右键到写回）

```
用户：在可编辑区域选中文本 → 右键「Change Tone → Professionalize」
  ↓
XenonAiContextMenuObserver::ExecuteCommand
  ↓（条件：is_editable && PROFESSIONALIZE && 无进行中 UserData）
XenonAiService::RunInferencerRewriteAsync(instruction, selection_utf8, callback)
  ↓
ThreadPool（MayBlock + USER_VISIBLE）：XenonAiInferenceClient::RunRewrite
  ↓
常驻子进程 xenon_ai_inferencer_burn --stdio-server
  ├─ stdin：一行 JSON（含临时文件路径）
  ├─ 子进程：读文件 → Llama 推理 → stdout 多行 DELTA + 一行 DONE
  └─ Chrome：解析 DELTA 拼接 → 回到 UI 线程 callback
  ↓
ApplyInferencerRewriteOnWebContents(WebContents*, success, text)
  └─ WebContents::Replace(UTF-8→UTF-16) 写回选区
```

**关键点**：上下文菜单的 `Observer` 在菜单关闭后即销毁，因此**不能**用 `WeakPtr<XenonAiContextMenuObserver>` 承接异步结果；改为 **`WebContents::GetWeakPtr()`** + 挂在 `WebContents` 上的 **`SupportsUserData`**（`kXenonAiRewriteDataKey`）在成功路径上累积/替换文本。

---

## 3. 构建系统（GN / Cargo）

### 3.1 `xenon_overlay/buildflags/features.gni`

- 在**第二个** `declare_args()` 块中新增（GN 要求与上一块 `enable_xenon_service` 等分离）：
  - **`build_xenon_ai_inferencer_burn`**（默认 `true`）：为 `true` 时 GN 会调度 Cargo action，把 sidecar 产物放到 **`$root_out_dir`**。
- 注释说明：`cargo` 需在 PATH；设为 `false` 则跳过 GN action（仍可手工拷贝 sidecar 到输出目录旁）。

### 3.2 `//xenon_overlay/xenon_ai_inferencer_burn/BUILD.gn`

- 条件：`enable_xenon_ai && build_xenon_ai_inferencer_burn`。
- **`action("xenon_ai_inferencer_burn")`**：
  - **脚本**：`build_inferencer_burn.py`
  - **inputs**：脚本、`Cargo.toml`、`Cargo.lock`、`src/main.rs`（ninja 增量依据）
  - **outputs**：
    - Windows：`$root_out_dir/xenon_ai_inferencer_burn.exe`
    - 非 Windows：`$root_out_dir/xenon_ai_inferencer_burn`
  - **args**：`--crate-root`（crate 根目录）、`--out-dir`（`$root_out_dir`），均经 `rebase_path` 传给 Python。

### 3.3 `build_inferencer_burn.py`（行为细节）

1. 解析 `--crate-root`、`--out-dir`，规范为绝对路径。
2. **`cargo` 探测**：`shutil.which("cargo")`，失败则 stderr 报错并返回 1。
3. **Cargo 构建**：
   - `cargo build --release`
   - `--manifest-path=<crate>/Cargo.toml`
   - **`--target-dir=<out_dir>/obj/xenon_ai_inferencer_burn_cargo`**  
     把 Rust `target/` 放在 Chromium `out` 下，避免污染源码树中的 `target/`（若开发者本地在 crate 内 `cargo build` 仍会生成另一份 target，二者独立）。
4. 从 `target_dir/release/` 取二进制名（Win：`xenon_ai_inferencer_burn.exe`，否则无后缀），**`shutil.copy2`** 到 **`out_dir` 根**（与 `chrome.exe` 同目录，满足 `DIR_EXE` 查找）。

### 3.4 `chrome/BUILD.gn`

- 顶部 **`import("//xenon_overlay/buildflags/features.gni")`**，使 `chrome` 目标能读 `enable_xenon_ai` / `build_xenon_ai_inferencer_burn`。
- 在 **chrome 主可执行文件** 的 **`data_deps`** 段中增加：
  - `if (enable_xenon_ai && build_xenon_ai_inferencer_burn) { data_deps += [ "//xenon_overlay/xenon_ai_inferencer_burn:xenon_ai_inferencer_burn" ] }`  
  确保 **ninja chrome** 时会先跑 action，产物出现在 **`$root_out_dir`**，运行时与 `chrome.exe` 并列。

### 3.5 `xenon_overlay/chrome/browser/xenon_ai/BUILD.gn`

- `source_set("xenon_ai")` 的 **`sources`** 增加：
  - `xenon_ai_inference_client.cc`
  - `xenon_ai_inference_client.h`  
  与现有 `xenon_ai_service` 等编在同一 target，**不**引入对 `side_panel` 的依赖（避免与 webview 注册形成 GN 环，注释里已有说明）。

---

## 4. Rust sidecar：`xenon_ai_inferencer_burn`

### 4.1 `Cargo.toml` 要点

| 项 | 说明 |
|----|------|
| **bin** | `xenon_ai_inferencer_burn`，入口 `src/main.rs` |
| **features** | `default = ["backend_vulkan"]`；可选 `backend_ndarray`（CPU，更小更慢） |
| **burn** | `0.20.1`，`default-features = false`，`features = ["std"]` |
| **llama-burn** | git `tracel-ai/models`，包名 `llama-burn`，`llama3` + `pretrained` |
| **serde / serde_json** | stdio-server 一行一 JSON |

首次运行/加载时，预训练权重行为以 **llama-burn / Cargo 注释** 为准（文档中常见为首次拉取较大 `model.mpk` 等，**不在 ninja 阶段**完成）。

### 4.2 入口与两种模式

- **`--stdio-server`**（Chrome 使用）：
  1. `load_llama_once()` → `LlamaConfig::llama3_2_1b_pretrained`（`MAX_SEQ_LEN = 2048`）。
  2. 向 stdout 打印 **`READY`** 并 **`flush`**。
  3. **`loop { read_line(stdin) }`**：每行一个 JSON；`n==0` 退出循环（stdin EOF）。
- **无该 flag**：解析 `--input` / `--task` / `--instruction`，单次读文件、推理、`DELTA`+`DONE` 后退出（调试/命令行用）。

### 4.3 Windows 子系统

- `#![cfg_attr(windows, windows_subsystem = "windows")]`：Windows 下不弹控制台窗口（与 Chrome `start_hidden` 等配合）。

### 4.4 stdio-server 请求 JSON（`Request`）

| 字段 | 类型 | 说明 |
|------|------|------|
| `input` | string | **文件路径**（UTF-8 路径字符串）；子进程 `fs::read_to_string` 读正文 |
| `instruction` | string，默认 `""` | 用户/浏览器侧「改写指令」 |
| `task` | string，默认 `""` | 空则当作 `"rewrite"`；**仅** `rewrite` 会走 `generate_rewrite` |

### 4.5 响应协议（stdout）

- **`DELTA <text>`**：单行；`emit_delta` 会把生成文本里的 **换行替换为空格**，避免 Chrome 按行协议误解析。
- **`DONE <code>`**：整数退出码式状态；**每条请求末尾**必须有一条 `DONE`，且 Chrome 在读到 **`DONE ` 前缀的行** 后结束本次响应读取。
- **常见 `DONE` 含义**（sidecar 当前实现）：
  - `0`：成功（前有 `DELTA`）
  - `1`：空输入等
  - `2`：不支持的 `task`
  - `3`：推理/内部错误
  - `4`：JSON 解析失败（仍 `flush`，便于 Chrome 对齐流）

每次处理完请求后 **`stdout.flush()`**，降低管道缓冲导致父进程阻塞读的风险。

### 4.6 提示词与同语言偏好

- **`wrap_llama3_user_prompt`**：Llama 3 风格 `system` / `user` / `assistant` 头（代码中使用与 llama-burn 示例一致的 **redacted header** token 拼法）。
- **system**：明确要求「只输出改写结果、**与输入同语言**、非英文原文不要随意译成英文」。
- **`generate_rewrite`**：
  - 无 `instruction`：`Rewrite... Keep the same language...` + `body`。
  - 有 `instruction`：`{instruction}` + `Text (preserve this language...)` + `body`。
- **采样**：`Sampler::new_top_p(0.9, 42)`，`generate(..., max_tokens=256, temperature=0.6, ...)`（具体以 `main.rs` 为准）。

---

## 5. Chrome：`XenonAiInferenceClient`

### 5.1 可执行文件解析

- **`base::PathService::Get(base::DIR_EXE, &exe_dir)`**。
- **`ResolveInferencerExe`**：只认 **`xenon_ai_inferencer_burn`**（Windows 加 `.exe`）；**不存在则失败**（不再支持其他 stub 名）。

### 5.2 输入侧：选区 → 临时文件

- **`base::ScopedTempFile`** 创建临时文件，**`base::WriteFile`** 写入 **`selection_utf8`**（UTF-8 字节，与 JSON 里路径一起交给子进程读）。

### 5.3 常驻宿主与进程级单例

- **`BurnInferencerHost`**：持有一个子进程 + 父端 **stdin 写**、**stdout 读** `base::File`。
- **进程级**（匿名 namespace）：
  - **`ResidentHostLock()`**：`static base::NoDestructor<base::Lock>`（避免静态析构顺序问题，满足 **`-Wexit-time-destructors`** 等约束）。
  - **`ResidentHostInstance()`**：`static base::NoDestructor<std::unique_ptr<BurnInferencerHost>>`。
- **`XenonAiInferenceClient::RunRewrite`** 在整段流程持 **`base::AutoLock`**，保证多线程调用 `RunRewrite` 时**不会并发**写同一子进程管道（与 ThreadPool 上「一次一个任务」配合，仍防御未来复用）。

### 5.4 子进程启动（Windows 与 POSIX 差异）

**Windows**

- `CreatePipe` 两对：父写子 stdin、子写父 stdout。
- **`stderr`**：GUI Chrome 下 stderr 常为 NULL；若子进程继承无效句柄，Rust 侧 `eprintln!` 等可能崩。此处为子进程打开 **可继承的 `NUL` 写** 作为 stderr。
- **`LaunchOptions`**：`start_hidden = true`；`stdin_handle` / `stdout_handle` / `stderr_handle` + **`handles_to_inherit`** 显式列出需继承的句柄。
- 启动后父进程关闭「子进程端已接管」的句柄副本（如子 stdin 读端、子 stdout 写端），保留父用的 `stdin_write`、`stdout_read`。

**非 Windows**

- `base::CreatePipe` + **`options.fds_to_remap`** 映射到 `STDIN_FILENO` / `STDOUT_FILENO`。
- 未在代码中为子进程单独绑 stderr（与 Win 分支不对称，行为依赖默认继承/空设备，若后续在 POSIX 上遇到同类问题可再对齐 Win 行为）。

### 5.5 启动握手：`READY` 与前置 stdout 行

- 子进程加载模型时，依赖库可能先向 stdout 打印多行（如进度日志）。
- Chrome 在 **`EnsureStarted`** 里 **循环 `ReadLine`**，跳过所有非 `READY` 行（trim 后比较），上限 **`kMaxStdoutLinesBeforeReady = 65536`**，防止死循环。
- **`ReadLine`**：逐字节读直到 `\n`；行尾 **`\\r` 剥离**（兼容 CRLF），避免 `DONE` 行解析失败。

### 5.6 单次请求：`RunOneRequest`

1. 构造 **`base::Value::Dict`**：`input` = 临时文件 **AsUTF8Unsafe()** 路径字符串，`instruction`，`task` = `"rewrite"`。
2. **`base::WriteJson`** → UTF-8 JSON，末尾 **`\\n`** 作为一条记录结束符。
3. **`WriteAll`** 写入 stdin；**`Flush()`**（失败仅 VLOG，非致命）。
4. 循环 **`ReadLine`** 直到读到 **`DONE `** 开头的行为止，将之前所有行拼成 **`stdout_batch`**（含换行）。
5. **`ParseSidecarOutput`**：
   - 按行拆分（`TRIM_WHITESPACE`、`SPLIT_WANT_NONEMPTY`）；
   - 行首 **`DELTA `** → 拼接正文子串；
   - 行首 **`DONE `** → 解析整数，`code==0` 为成功；**`DONE` 行上整数部分 trim`**；
   - 必须至少见到一条 **`DONE`**，否则 `missing DONE line`。
6. 若 `DONE` 非 0 或解析失败：**`Shutdown()`** 子进程并失败返回，避免残留坏状态。

### 5.7 失败与复用策略

- **`EnsureStarted` / `RunOneRequest` 失败**：`host.reset()`，下次 `RunRewrite` 会新建 `BurnInferencerHost`。
- **不复用 `Process::IsRunning()`** 作为「是否健康」的唯一依据（注释写明在 Windows 上曾导致误杀健康子进程）；改为以 **句柄有效 + 读写错误** 驱动 **`Shutdown`/`relaunch`**。

### 5.8 日志前缀

- 常量 **`kLogTag = "[xenon_ai_inferencer] "`**（历史命名；实际只对接 burn 二进制）。

---

## 6. Chrome：`XenonAiService::RunInferencerRewriteAsync`

- **`content::GetUIThreadTaskRunner({})`** 保存为 `ui_runner`。
- **`base::ThreadPool::PostTaskAndReplyWithResult`**：
  - **任务特征**：`base::MayBlock()`（子进程/IO）、`base::TaskPriority::USER_VISIBLE`。
  - **Worker 返回值**：`std::pair<bool, std::string>` — `first` 成功/失败，`second` 成功为模型输出，失败为 **错误信息字符串**。
- **Reply 链**：在默认 reply runner 上再 **`ui_runner->PostTask`**，保证 **`callback` 最终在浏览器 UI 线程**执行（满足 `WebContents::Replace` 等线程约束）。
- **`PostTaskAndReplyWithResult` 返回 `false`**（如浏览器关闭、调度失败）：在 UI 线程 **`callback(false, "inferencer task was not scheduled (ThreadPool)")`**，并打 `LOG(WARNING)`。

---

## 7. Chrome：右键菜单 `XenonAiContextMenuObserver`

### 7.1 头文件 `xenon_ai_context_menu_observer.h` 的删减

- 移除 **`#include "base/memory/weak_ptr.h"`**（类本体不再持 `WeakPtrFactory`）。
- 删除未实现的流式接口声明：
  - `OnRewriteSuggestionDataReceived`
  - `OnRewriteSuggestionCompleted`
- 删除成员 **`base::WeakPtrFactory<XenonAiContextMenuObserver> weak_ptr_factory_`**。

### 7.2 `xenon_ai_context_menu_observer.cc` 新增与逻辑

- **匿名命名空间**内：
  - **`XenonAiRewriteData`**：`SupportsUserData::Data`，字段 **`accumulated_text`**（为将来流式/多次 delta 预留；当前单次推理也会走同一套 `Replace` 逻辑）。
  - **`ApplyInferencerRewriteOnWebContents`**：
    - `success`：**`RestoreFocus`**；若 `accumulated_text` 非空先 **`Undo`**（避免重复替换叠层）；**`StrAppend`** delta；**`Replace(UTF8ToUTF16)`**；**`RemoveUserData`**。
    - `!success`：若有累积则 **`Undo`**，再 **`RemoveUserData`**。
- **`ExecuteCommand`**：
  - **`IDC_XENON_AI_CONTEXT_PROFESSIONALIZE`（61004）** 且 **`params_->is_editable`** 且 **无** `kXenonAiRewriteDataKey` → **原地改写路径**。
  - 设置 UserData → **`RunInferencerRewriteAsync`**，指令为英文长句，明确要求 **professional + polished** 且 **保留原文语言**（与 burn 侧 system/user 规则一致）。
  - **`base::BindOnce` + `web_contents_->GetWeakPtr()`** 调用 `ApplyInferencerRewriteOnWebContents`。
  - 其他命令（如 Summarize）：仍走 **侧栏**（`SetPendingPrompt` + `SidePanelCoordinator::Show(kXenonAI)`）。

### 7.3 命令 ID 范围

- **`IsCommandIdSupported`**：`61000 <= id <= 61004`（与文件内 `constexpr` IDC 常量一致；后续若迁入 `chrome_command_ids.h` 需同步改范围与菜单定义）。

---

## 8. 线程、阻塞与 UX 注意点

| 话题 | 说明 |
|------|------|
| **阻塞点** | `RunRewrite` 全程同步：等子进程 READY、写 stdin、读 stdout 到 DONE、推理在子进程内完成；**必须在 ThreadPool 上调用**（当前由 `RunInferencerRewriteAsync` 保证）。 |
| **UI 线程** | 仅回调内触摸 `WebContents` / 选区替换。 |
| **单例 host** | 全局唯一 burn stdio-server；多次菜单操作共享同一子进程（模型常驻内存）。 |
| **并发** | `ResidentHostLock` 防止多 worker 同时 `RunRewrite` 交错写管道（若未来放宽为并行请求，需改为队列或每请求子进程）。 |

---

## 9. 运维与排错清单

1. **找不到二进制**：确认 **`$root_out_dir`** 下存在 `xenon_ai_inferencer_burn(.exe)`；`build_xenon_ai_inferencer_burn` 是否为 `true`；本机是否有 **`cargo`**。
2. **`READY` 等不到**：看子进程是否在 READY 前崩溃；Chrome 日志中带 `[xenon_ai_inferencer]` 的前缀行。
3. **`DONE` 非 0**：对照第 4.5 节码值；stderr 在 Win 上指向 NUL，严重错误需临时改子进程 stderr 或附加日志文件才能看到 Rust 侧 `writeln!(stderr)`。
4. **中文变英文**：已用 system/user 指令 + Chrome 侧长 instruction 约束「同语言」；若仍漂移，属于 **1B 模型能力/采样** 范畴，可调温度、max tokens 或换模型。

---

## 10. 相关路径速查

| 角色 | 路径 |
|------|------|
| GN buildflag | `xenon_overlay/buildflags/features.gni` |
| GN action | `xenon_overlay/xenon_ai_inferencer_burn/BUILD.gn` |
| Cargo 源码 | `xenon_overlay/xenon_ai_inferencer_burn/src/main.rs` |
| 推理客户端 | `xenon_overlay/chrome/browser/xenon_ai/xenon_ai_inference_client.{h,cc}` |
| 领域服务 | `xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service.{h,cc}` |
| 右键菜单 | `xenon_overlay/chrome/browser/xenon_ai/xenon_ai_context_menu_observer.{h,cc}` |
| Chrome data_deps | `chrome/BUILD.gn`（`enable_xenon_ai && build_xenon_ai_inferencer_burn`） |
| 忽略本地 target | `xenon_overlay/xenon_ai_inferencer_burn/.gitignore`（`/target/`） |

---

## 11. 与仓库其他文档的关系

- 侧栏、Mojo、菜单产品向说明：仍以 **`Xenon_AI集成参考.md`**、**`Xenon_AI侧栏与右键菜单改动说明.md`** 等为准。
- **本文档**聚焦 **`e676bce`** 引入的 **burn sidecar + 浏览器管道 + 右键写回** 的工程细节；若后续提交修改协议或路径，请同步更新本节或于文末追加「修订历史」。
