# Chromium 进程沙箱：官方文档拆解、实现细节与代码示例

本文按 **上游官方文档** 与 **`//sandbox` 源码** 对照展开：先讲清 **Broker 如何「生成 Target」在 Chromium 里的真实路径**，再补 **枚举默认值、初始化顺序、失败码** 等可核对细节。  
**V8 内存沙箱**（`v8/src/sandbox/`）与 **OS 进程沙箱**（`//sandbox`）不是同一概念。

---

## 官方文档索引

| 文档 | 路径 |
|------|------|
| 主设计（通用原则 + Windows 细节为主） | [`docs/design/sandbox.md`](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/design/sandbox.md) |
| FAQ | [`docs/design/sandbox_faq.md`](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/design/sandbox_faq.md) |
| 各进程在各平台是否沙箱（定级） | `docs/security/process-sandboxes-by-platform.md` |
| 顶层目录说明 | `sandbox/README.md` |
| Linux | `sandbox/linux/README.md`，`docs/linux/suid_sandbox_development.md` |
| Android | `docs/security/android-sandbox.md` |
| macOS Seatbelt | `sandbox/mac/seatbelt_sandbox_design.md` |

---

## 1. `docs/design/sandbox.md` — 文档要点 + 源码细化

### 1.1 目标（Overview）

- 沙箱目标是对「能执行的操作」给出 **硬边界**（不依赖穷举所有输入与状态）。
- 利用 **OS 安全机制**；具体保证 **随平台变化**。

### 1.2 设计原则（五条）

| 原则 | 含义 |
|------|------|
| 不重复造轮 | 优先用 OS 对内核对象的安全检查；可自建应用层抽象。 |
| 最小权限 | 沙箱内与编排沙箱的代码都最小权限。 |
| 假定恶意 | 越过 `main()` 早期初始化后，按恶意代码建模。 |
| 低开销 | 正常路径几乎无额外成本。 |
| 仿真不是安全 | 不依赖 VM/仿真作为安全边界。 |

### 1.3 Broker 职责（文档）与 Chromium 中的对应关系

官方文档列出的 Broker 职责：

1. 为每个 Target 指定策略  
2. 生成 Target 进程  
3. 承载策略引擎服务  
4. 承载拦截管理器  
5. 承载沙箱 IPC（与 Chromium Mojo 高层 IPC 不同）  
6. 代 Target 执行策略允许的操作  

在 Chromium 中，**浏览器进程**扮演 Broker；**「生成 Target」** 在 Windows 上不是泛泛调用 `CreateProcess`，而是走 **`SandboxWin::StartSandboxedProcess` → `BrokerServices::SpawnTargetAsync`**，并在回调里 **`ResumeThread`**。

---

### 1.4 Windows：从浏览器到子进程的真实调用链（细化）

**① content 子进程启动（Launcher 线程）**  
`ChildProcessLauncherHelper::LaunchProcessOnLauncherThread` 在非提升启动时调用 `StartSandboxedProcess`：

```138:165:content/browser/child_process_launcher_helper_win.cc
ChildProcessLauncherHelper::Process
ChildProcessLauncherHelper::LaunchProcessOnLauncherThread(
    const base::LaunchOptions* options,
    std::unique_ptr<FileMappedForLaunch> files_to_register,
    bool* is_synchronous_launch,
    int* launch_result) {
  ...
  *is_synchronous_launch = false;
  *launch_result = StartSandboxedProcess(
      delegate_.get(), *command_line(), options->handles_to_inherit,
      base::BindOnce(&ChildProcessLauncherHelper::
                         FinishStartSandboxedProcessOnLauncherThread,
                     this));
  return ChildProcessLauncherHelper::Process();
}
```

**② content 封装** → 直接进入 `sandbox/policy`：

```23:51:content/common/sandbox_init_win.cc
sandbox::ResultCode StartSandboxedProcess(
    SandboxedProcessLauncherDelegate* delegate,
    const base::CommandLine& target_command_line,
    const base::HandlesToInheritVector& handles_to_inherit,
    sandbox::StartSandboxedProcessCallback result_callback) {
  ...
  return sandbox::policy::SandboxWin::StartSandboxedProcess(
      full_command_line, handles_to_inherit, delegate,
      std::move(result_callback));
}
```

**③ `SandboxWin::StartSandboxedProcess`**（核心逻辑摘要）：

- 若 `IsUnsandboxedProcess`：走 **`LaunchWithoutSandbox`**（无策略对象）。  
- 否则：`g_broker_services->CreatePolicy(delegate->GetSandboxTag())`（同一 `tag` 可共享 **TargetConfig**，见 `sandbox.h` 中 `CreatePolicy(std::string_view tag)` 注释）。  
- **`GeneratePolicyForSandboxedProcess`**：把需继承的句柄写入策略、`GenerateConfigForSandboxedProcess`、调试下设置 stdout/stderr、`delegate->PreSpawnTarget`。  
- **`SpawnTargetAsync`**；回调 **`FinishStartSandboxedProcess`**：`delegate->PostSpawnTarget`、**`ResumeThread`**、把 `base::Process` 回传给上层。

```941:1017:sandbox/policy/win/sandbox_win.cc
ResultCode SandboxWin::StartSandboxedProcess(
    const base::CommandLine& cmd_line,
    const base::HandlesToInheritVector& handles_to_inherit,
    SandboxDelegate* delegate,
    StartSandboxedProcessCallback result_callback) {
  ...
  if (IsUnsandboxedProcess(delegate->GetSandboxType(), cmd_line,
                           *base::CommandLine::ForCurrentProcess())) {
    base::Process process;
    ResultCode result =
        LaunchWithoutSandbox(cmd_line, handles_to_inherit, delegate, &process);
    ...
  }

  auto policy = g_broker_services->CreatePolicy(delegate->GetSandboxTag());
  ...
  ResultCode result = GeneratePolicyForSandboxedProcess(
      cmd_line, handles_to_inherit, delegate, policy.get());
  ...
  g_broker_services->SpawnTargetAsync(
      cmd_line.GetProgram().value(), cmd_line.GetCommandLineString(),
      std::move(policy),
      base::BindOnce(&SandboxWin::FinishStartSandboxedProcess, delegate,
                     std::move(timer), std::move(result_callback)));
  return SBOX_ALL_OK;
}
...
void SandboxWin::FinishStartSandboxedProcess(
    ...
    base::win::ScopedProcessInformation target,
    DWORD last_error,
    ResultCode result) {
  ...
  delegate->PostSpawnTarget(target.process_handle());
  CHECK(ResumeThread(target.thread_handle()) != static_cast<DWORD>(-1));
  ...
  base::Process process(target.TakeProcessHandle());
  std::move(result_callback).Run(std::move(process), last_error, result);
}
```

**要点**：Target 进程常以 **挂起主线程** 的方式创建，完成 token/job/拦截器等注入后再 **恢复线程**，避免在策略未就绪时跑业务代码。

---

### 1.5 `SpawnTargetAsync` / `PreSpawnTarget` 内部阶段（Broker 侧）

`BrokerServicesBase::SpawnTargetAsync` 将 `TargetPolicy` 转为内部 `PolicyBase*`，进入 `SpawnTargetAsyncImpl`。在此之前，**`PreSpawnTarget`** 会顺序完成大量 OS 级准备（节选，见 `broker_services.cc`）：

- 校验 **必须从主 EXE 模块** 调用（否则 `SBOX_ERROR_INVALID_LINK_STATE`），防止沙箱编排代码跑在任意 DLL 里。  
- **`ConfigBase::Freeze()`**：冻结共享配置。  
- **单线程约束**：`SpawnTargetAsync` 全局不能多线程并发（`DCHECK` 同一线程），以便 launcher 线程缓解与全局状态一致。  
- 首次子进程创建前对 launcher 线程做 **ACG 相关** `ApplyMitigationsToCurrentThread(MITIGATION_DYNAMIC_CODE_OPT_OUT_THIS_THREAD)`（失败可软忽略）。  
- **`policy_base->MakeTokens()`**：构造将赋给 Target 的 token 集。  
- **`UpdateDesktopIntegrity`**：与独立桌面完整性级别相关。  
- **`policy_base->InitJob()`**：创建/配置 Job。  
- 组装 **`STARTUPINFOEX`**：桌面名、**进程缓解**（`SetMitigations`）、可选过滤环境变量、Win10 TH2+ 在 Job≤`kLimitedUser` 时 **`SetRestrictChildProcessCreation`**、AppContainer、继承句柄列表、**把 Job 关联到启动信息** 等。  
- **`BuildStartupInformation`** 失败则 `SBOX_ERROR_PROC_THREAD_ATTRIBUTES`。

```468:567:sandbox/win/src/broker_services.cc
base::expected<BrokerServicesBase::CreateTargetInfo, ResultCode>
BrokerServicesBase::PreSpawnTarget(std::wstring_view exe_path,
                                   std::wstring_view command_line,
                                   PolicyBase* policy_base) {
  ...
  if (CURRENT_MODULE() != exe_module) {
    return base::unexpected(SBOX_ERROR_INVALID_LINK_STATE);
  }
  ...
  ConfigBase* config_base = static_cast<ConfigBase*>(policy_base->GetConfig());
  if (!config_base->IsConfigured() && !config_base->Freeze()) {
    return base::unexpected(SBOX_ERROR_FAILED_TO_FREEZE_CONFIG);
  }
  ...
  static DWORD thread_id = ::GetCurrentThreadId();
  DCHECK(thread_id == ::GetCurrentThreadId());
  ...
  auto tokens = policy_base->MakeTokens();
  ...
  ResultCode result = UpdateDesktopIntegrity(config_base->desktop(),
                                             config_base->integrity_level());
  ...
  result = policy_base->InitJob();
  ...
  if (base::win::GetVersion() >= base::win::Version::WIN10_TH2 &&
      config_base->GetJobLevel() <= JobLevel::kLimitedUser) {
    startup_info->SetRestrictChildProcessCreation(true);
  }
  ...
  startup_info->AddJobToAssociate(policy_base->GetJobHandle());
  ...
  if (!startup_info->BuildStartupInformation()) {
    return base::unexpected(SBOX_ERROR_PROC_THREAD_ATTRIBUTES);
  }
  return CreateTargetInfo(std::move(startup_info), std::move(tokens.value()));
}
```

**与文档「2. 生成 Target 进程」的对应**：不是单纯 `CreateProcess`，而是 **Freeze 配置 → Token/桌面/Job/AppContainer/ProcThreadAttribute → 创建挂起进程 → 注入拦截与 IPC 共享段 → 恢复线程**（后续步骤在同文件 `SpawnTargetAsyncImpl` 继续，失败码见 `sandbox_types.h` 中 `SBOX_ERROR_*`）。

---

### 1.6 `TokenLevel` / `JobLevel` / `IntegrityLevel`（源码枚举，比文档更细）

定义见 `sandbox/win/src/security_level.h`。

**IntegrityLevel** 与 SID 注释（节选）：

```12:36:sandbox/win/src/security_level.h
// INTEGRITY_LEVEL_SYSTEM:      "S-1-16-16384" System Mandatory Level
// INTEGRITY_LEVEL_HIGH:        "S-1-16-12288" High Mandatory Level
// INTEGRITY_LEVEL_MEDIUM:      "S-1-16-8192"  Medium Mandatory Level
...
// INTEGRITY_LEVEL_UNTRUSTED:   "S-1-16-0"     Untrusted Mandatory Level
enum IntegrityLevel {
  INTEGRITY_LEVEL_SYSTEM,
  INTEGRITY_LEVEL_HIGH,
  INTEGRITY_LEVEL_MEDIUM,
  ...
  INTEGRITY_LEVEL_UNTRUSTED,
  INTEGRITY_LEVEL_LAST
};
```

**TokenLevel**：从 `USER_LOCKDOWN` 到 `USER_UNPROTECTED` 多档；头文件内 **表格** 说明每档的 Restricting SIDs / Deny-only / Privileges（文档 `sandbox.md` 的文字描述与此表一致，细节以源码表为准）。

**JobLevel**：`kLockdown`、`kLimitedUser`、`kInteractive`、`kUnprotected`；注释表格列出每档增加的 **UI 限制**（剪贴板、全局钩子、广播、User 句柄、单进程等）。

```91:123:sandbox/win/src/security_level.h
enum class JobLevel { kLockdown = 0, kLimitedUser, kInteractive, kUnprotected };
```

---

### 1.7 Chromium 为沙箱子进程配置的默认项（示例：`AddDefaultConfigForSandboxedProcess`）

策略细节在 `sandbox/policy/win/sandbox_win.cc` 的 **`AddDefaultConfigForSandboxedProcess`**，体现 **「低 IL + 延迟 Untrusted + 双 Token + 独立窗口站桌面」** 等组合（与文档「四类机制」对应，但是具体数值以代码为准）：

```214:231:sandbox/policy/win/sandbox_win.cc
ResultCode AddDefaultConfigForSandboxedProcess(TargetConfig* config) {
  // Prevents the renderers from manipulating low-integrity processes.
  config->SetDelayedIntegrityLevel(INTEGRITY_LEVEL_UNTRUSTED);
  ResultCode result = config->SetIntegrityLevel(INTEGRITY_LEVEL_LOW);
  ...
  result = config->SetTokenLevel(USER_RESTRICTED_SAME_ACCESS, USER_LOCKDOWN);
  ...
  config->SetLockdownDefaultDacl();
  config->AddKernelObjectToClose(HandleToClose::kDeviceApi);
  config->SetDesktop(Desktop::kAlternateWinstation);

  return SBOX_ALL_OK;
}
```

---

### 1.8 `TargetConfig::SetTokenLevel` 与 `LowerToken()`（双 Token 模型，源码注释）

`TargetConfig` 对接口语义的说明比设计文档更「可执行」：**initial** 在进程创建到 `LowerToken()`/`RevertToSelf()` 前用于加载；**lockdown** 在之后生效；**initial 不得比 lockdown 更宽松**。

```69:90:sandbox/win/src/sandbox_policy.h
  // initial: the security level for the initial token. This is the token that
  //   is used by the process from the creation of the process until the moment
  //   the process calls TargetServices::LowerToken() or the process calls
  //   win32's RevertToSelf(). Once this happens the initial token is no longer
  //   available and the lockdown token is in effect.
  ...
  // lockdown: the security level for the token that comes into force after the
  //   process calls TargetServices::LowerToken() or the process calls
  //   win32's RevertToSelf().
  [[nodiscard]] virtual ResultCode SetTokenLevel(TokenLevel initial,
                                                 TokenLevel lockdown) = 0;
```

**渲染器侧调用**（与 FAQ「不是启动即完全锁死」一致）：

```76:92:content/renderer/renderer_main_platform_delegate_win.cc
bool RendererMainPlatformDelegate::EnableSandbox() {
  sandbox::TargetServices* target_services =
      parameters_->sandbox_info->target_services;

  if (target_services) {
    sandbox::policy::WarmupRandomnessInfrastructure();
    ...
    target_services->LowerToken();
    return true;
  }
  return false;
}
```

**`LowerToken()` 内部**（失败直接 `TerminateProcess`，退出码见 `TerminationCodes`）：

```126:155:sandbox/win/src/target_services.cc
void TargetServicesBase::LowerToken() {
  if (!SetProcessIntegrityLevel(g_shared_delayed_integrity_level)) {
    ::TerminateProcess(::GetCurrentProcess(), SBOX_FATAL_INTEGRITY);
  }
  ...
  if (!::RevertToSelf())
    ::TerminateProcess(::GetCurrentProcess(), SBOX_FATAL_DROPTOKEN);
  ...
  if (g_shared_delayed_mitigations &&
      !LockDownSecurityMitigations(g_shared_delayed_mitigations)) {
    ::TerminateProcess(::GetCurrentProcess(), SBOX_FATAL_MITIGATION);
  }
  process_state_.SetInitCompleted();
}
```

---

### 1.9 官方文档里的 `AddRule(SUBSYS_FILES, ...)` 与当前 API

`docs/design/sandbox.md` 中的 `AddRule(SUBSYS_FILES, FILES_ALLOW_READONLY, ...)` 是 **概念性旧式子系统枚举示例**。当前对外配置主要在 **`TargetConfig`** 上，例如 **`AllowFileAccess(FileSemantics, pattern)`**（注释明确：**不建议新代码再增加文件通配规则**，优先通过 **Chrome IPC 传入句柄**）：

```142:157:sandbox/win/src/sandbox_policy.h
  // Adds a policy rule effective for processes spawned using this policy.
  // Files matching `pattern` can be opened following FileSemantics.
  ...
  // Note: Do not add new uses of this function - instead proxy file handles
  // into your process via normal Chrome IPC.
  [[nodiscard]] virtual ResultCode AllowFileAccess(FileSemantics semantics,
                                                   const wchar_t* pattern) = 0;
```

**低层 IPC 标签**（NT 桩与测试用）见 `sandbox/win/src/ipc_tags.h`（如 `NTCREATEFILE`、`NTOPENFILE` 等），与 **高层「文件通配策略」** 是不同层次。

---

### 1.10 失败与退出：`ResultCode` / `TerminationCodes`

- **`sandbox_types.h` `ResultCode`**：`SpawnTargetAsync`、策略配置失败时返回（如 `SBOX_ERROR_INITIALIZE_INTERCEPTIONS`、`SBOX_ERROR_CANNOT_CREATE_RESTRICTED_TOKEN` 等），部分错误需配合 `GetLastError()`。  
- **`TerminationCodes`**：`LowerToken` 等路径失败时子进程退出码（如 `SBOX_FATAL_INTEGRITY = 7006`、`SBOX_FATAL_MITIGATION = 7011`），与崩溃统计枚举对齐。

---

### 1.11 诊断（文档）

- `chrome://sandbox`  
- Trace：`disabled-by-default-sandbox`  
- Windows：`tools/win/trace-sandbox-viewer.py`  

---

## 2. `docs/design/sandbox_faq.md` — 要点

| 主题 | 要点 |
|------|------|
| 能否写盘 / 任意读 | 默认不能；由策略与 Broker 代劳。 |
| 内核 bug | 沙箱不解决内核漏洞。 |
| 管理员 / 驱动 | 不需要。 |
| 与 Site Isolation | FAQ 部分句子可能偏旧；**站点隔离**与 **OS 沙箱**互补。 |
| Lock-down 时机 | 见 §1.8：需 `LowerToken()` 后才进入严格状态。 |

---

## 3. Linux：`sandbox/linux/README.md` + 初始化顺序（源码）

### 3.1 机制

- **setuid helper** + **namespaces** + **seccomp-bpf**（详见目录内 README 与 `docs/linux/suid_sandbox_development.md`）。  
- **Landlock**：README 中仍为规划/评估性质，以实际提交为准。

### 3.2 `SandboxLinux::InitializeSandbox` 内 **确定顺序**（细读 `sandbox/policy/linux/sandbox_linux.cc`）

在单线程或允许的 GPU/ODML 多线程前提下，典型顺序包括：

1. **`PreinitializeSandbox`**（若尚未）  
2. 若 `options.engage_namespace_sandbox`：**`EngageNamespaceSandbox`**  
3. 若启用语义层：**检查是否过早打开目录**（否则 `CHECK` 失败，破坏 setuid 沙箱语义）  
4. **`InitLibcLocaltimeFunctions`**  
5. 非 ChromeOS 且非无沙箱类型：**`DiscourageGetaddrinfo()`**（避免 glibc 加载任意 NSS 插件）  
6. **`LimitAddressSpace`**（`setrlimit` 等，与进程类型相关）  
7. **`StartSeccompBPF`**  

对应片段（节选）：

```414:458:sandbox/policy/linux/sandbox_linux.cc
  if (!pre_initialized_)
    PreinitializeSandbox();

  if (options.engage_namespace_sandbox)
    EngageNamespaceSandbox(false /* from_zygote */);

  CHECK(!options.check_for_open_directories || !HasOpenDirectories())
      << "InitializeSandbox() called after unexpected directories have been "
      << "opened. This breaks the security of the setuid sandbox.";
  ...
#if !BUILDFLAG(IS_CHROMEOS)
  if (!IsUnsandboxedSandboxType(sandbox_type)) {
    DiscourageGetaddrinfo();
  }
#endif
  ...
  const bool limited_as = LimitAddressSpace(&error);
  ...
  return StartSeccompBPF(sandbox_type, std::move(hook), options);
```

### 3.3 地址空间限制与进程类型（`GetProcessDataSizeLimit`）

对 **Renderer / GPU / OnDeviceModelExecution** 等，在 64 位上会按物理内存分段设置 **RLIMIT_DATA** 上限（并有 Feature 可提高 Renderer 上限），见 `sandbox_linux.cc` 中 **`GetProcessDataSizeLimit`**（约 476–513 行）。

### 3.4 Seccomp 安装（同前文代码引用）

`StartSeccompBPF` 内 `PolicyForSandboxType` → `StartSandboxWithExternalPolicy` → `RunSandboxSanityChecks`。

---

## 4. Android — `docs/security/android-sandbox.md`（要点）

- 应用级 UID 隔离 + **`isolatedProcess`** + **Seccomp-BPF** 双层。  
- 进程创建经 **Zygote / Activity Manager**，与桌面 fork 模型不同；细节以该文档为准。

---

## 5. 策略类型与 Mojo

- 枚举：**`sandbox/policy/mojom/sandbox.mojom`**。  
- 服务声明：**`[ServiceSandbox=sandbox.mojom.Sandbox.xxx]`**。  
- 命令行映射：**`sandbox/policy/sandbox_type.cc`**（`SetCommandLineFlagsForSandboxType` 等）。  
- 定级表：**`docs/security/process-sandboxes-by-platform.md`**（可能落后于枚举）。

---

## 6. 文档与源码易不一致处

| 来源 | 说明 |
|------|------|
| `sandbox.md` 的 `AddRule(SUBSYS_*)` 示例 | 概念上仍成立；具体 API 以 **`TargetConfig`** / **`AllowFileAccess`** 等为准。 |
| `sandbox.md` M75 IL 示例 | 历史快照；以 **`AddDefaultConfigForSandboxedProcess`** 与当前策略为准。 |
| `process-sandboxes-by-platform.md` | 里程碑更新；新 `Sandbox` 值先出现在 **`sandbox.mojom`**。 |

---

*随分支演进请以当前树内实现为准。*
