# KeyedService（ProfileKeyedService）详解

本文说明 Chromium/Chrome 中 **KeyedService** 的作用、生命周期与典型写法，并以本仓库 **`XenonAiService` + `XenonAiServiceFactory`** 为**可编译的对照实现**（含 `BrowserContext*` → `Profile*` 注入）。

---

## 目录

1. [KeyedService 是什么](#1-keyedservice-是什么)
2. [典型组成：Service + Factory + FactoriesBuilt](#2-典型组成service--factory--factoriesbuilt)
3. [`BrowserContext` 与 `Profile`（工厂入参怎么用）](#3-browsercontext-与-profile工厂入参怎么用)
4. [生命周期与 `Shutdown()`](#4-生命周期与-shutdown)
5. [Xenon AI：完整对照实现](#5-xenon-ai完整对照实现)
6. [在业务里如何获取服务](#6-在业务里如何获取服务)
7. [`DependsOn` 与依赖顺序](#7-dependson-与依赖顺序)
8. [何时不该用 KeyedService](#8-何时不该用-keyedservice)
9. [检查清单](#9-检查清单)
10. [延伸阅读](#10-延伸阅读)

---

## 1. KeyedService 是什么

### 1.1 定义

**KeyedService** 表示一类「按 **`content::BrowserContext` 为键**」托管的服务实例。在 Chrome 桌面端，这个 context **几乎总是 `Profile`（或与其成对的 OTR profile）**。

框架提供：

- **每个“键”最多一份服务实例**（由 `ProfileKeyedServiceFactory` 及 `ProfileSelections` 决定哪些 profile 会实例化）
- **两阶段销毁**：先 `Shutdown()`，再析构（见 §4）

基类（`components/keyed_service/core/keyed_service.h`）核心只有：

```cpp
class KeyedService {
 public:
  virtual ~KeyedService() = default;
  virtual void Shutdown() {}
};
```

### 1.2 为什么不用全局单例

- **多 Profile 隔离**：A 用户与 B 用户的会话、Prefs、凭证不能共享一个静态对象。
- **生命周期绑定 Profile**：Profile 退出时统一收口，减少 UAF。
- **依赖拓扑可见**：在 Factory 里 `DependsOn(OtherFactory::GetInstance())`，比隐式全局顺序更清晰。

---

## 2. 典型组成：Service + Factory + FactoriesBuilt

| 角色 | 职责 | Xenon AI 文件 |
|------|------|----------------|
| **Service** | 继承 `KeyedService`，持有 **与本 Profile 绑定的状态**（后续可加 Prefs、Mojo、限流器等） | `xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service.{h,cc}` |
| **Factory** | 继承 `ProfileKeyedServiceFactory`：`ProfileSelections`、**`BuildServiceInstanceForBrowserContext`**、`GetForProfile` / `DependsOn` | `xenon_overlay/chrome/browser/xenon_ai/xenon_ai_service_factory.{h,cc}` |
| **FactoriesBuilt（可选）** | 在 `EnsureBrowserContextKeyedServiceFactoriesBuilt()` 路径里 **`GetInstance()`**，尽早注册工厂、暴露依赖错误 | `xenon_overlay/chrome/browser/xenon_ai/browser_context_keyed_service_factories.{h,cc}` ← 由 `chrome/browser/profiles/chrome_browser_main_extra_parts_profiles.cc` 调用 |

---

## 3. `BrowserContext` 与 `Profile`（工厂入参怎么用）

`ProfileKeyedServiceFactory` 创建服务时的挂钩是：

```cpp
std::unique_ptr<KeyedService> BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const override;
```

**参数不是摆设**：它就是当前要服务的那个 profile 对应的 `BrowserContext*`。常规做法是：

```cpp
Profile* const profile = Profile::FromBrowserContext(context);
CHECK(profile);
return std::make_unique<MyService>(profile);
```

然后在 `MyService` 里用 **`raw_ptr<Profile>`**（或构造时拉 `PrefService` 等依赖）保存下来，供后续业务使用。

> **注意**：若 `ProfileSelections` 与调用方 profile 不一致，可能出现“拿不到服务 / factory 不创建”的现象；排查时先核对 selections 与 `GetForProfile` 传入的是否为 **同一 profile 家族**。

---

## 4. 生命周期与 `Shutdown()`

- 框架在销毁服务前会调用 **`Shutdown()`**，用于解绑观察者、停掉定时器、关闭 Mojo receiver 等。
- **不要在 `Shutdown()` 里再 `FooFactory::GetForProfile` 去拉别的 KeyedService**（对方可能已 Shutdown 或正在销毁）。
- 需要跨服务协作时：在 **Factory 构造**里 `DependsOn(...)`，或在 **运行期**缓存 `raw_ptr`（仍要谨慎 Shutdown 顺序）。

---

## 5. Xenon AI：完整对照实现

以下与仓库源码一致；摘取时若行号变化，以文件为准。

### 5.1 Service：持有 `Profile*`

```cpp
// xenon_ai_service.h（结构摘要）
class XenonAiService : public KeyedService {
 public:
  explicit XenonAiService(Profile* profile);
  Profile* profile() const { return profile_.get(); }
  std::string GetBuildStamp() const;

 private:
  const raw_ptr<Profile> profile_;
  const std::string build_stamp_;
};
```

```cpp
// xenon_ai_service.cc（构造摘要）
XenonAiService::XenonAiService(Profile* profile)
    : profile_(CHECK_DEREF(profile)), build_stamp_("xenon-ai-1") {}
```

要点：

- **`CHECK_DEREF(profile)`**：工厂保证非空时也可以写 `profile_(profile)`；这里明确“无 Profile 即逻辑错误”。
- **`raw_ptr<Profile>`**：符合 Chromium 内存安全习惯；不要在服务里长期裸存 `Profile*` 而不走 `raw_ptr`（除非有更强约束且团队有例外约定）。

### 5.2 Factory：把 `BrowserContext*` 转成 `Profile*` 再构造

```cpp
std::unique_ptr<KeyedService>
XenonAiServiceFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  Profile* const profile = Profile::FromBrowserContext(context);
  CHECK(profile);
  return std::make_unique<XenonAiService>(profile);
}
```

### 5.3 `GetForProfile`：懒创建

```cpp
XenonAiService* XenonAiServiceFactory::GetForProfile(Profile* profile) {
  return static_cast<XenonAiService*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}
```

### 5.4 ProfileSelections（当前策略）

```cpp
ProfileSelections::Builder()
    .WithRegular(ProfileSelection::kOriginalOnly)
    .Build()
```

含义：**Regular profile 按 `kOriginalOnly` 策略** 决定 OTR/incognito 等场景是否映射到“原始 profile 的服务”。调整此项会改变“无痕里能不能 Get 到同一份服务”，上线前要产品确认。

### 5.5 FactoriesBuilt 挂钩

```cpp
void EnsureXenonBrowserContextKeyedServiceFactoriesBuilt() {
#if BUILDFLAG(ENABLE_XENON_SERVICE) && BUILDFLAG(ENABLE_XENON_AI)
  XenonAiServiceFactory::GetInstance();
#endif
}
```

用于在 Chrome 启动流程中**尽早**确保工厂 singleton 存在（与 `DependsOn`、依赖解析时机相关）。

---

## 6. 在业务里如何获取服务

### 6.1 从 `Browser` / `Profile*`

```cpp
Profile* profile = browser->profile();
XenonAiService* service = xenon::XenonAiServiceFactory::GetForProfile(profile);
if (!service) {
  return;
}
// service->profile() 与传入的 profile 一致（同键同实例）
```

### 6.2 从 WebUI

```cpp
Profile* profile = Profile::FromBrowserContext(
    web_ui->GetWebContents()->GetBrowserContext());
XenonAiService* service = xenon::XenonAiServiceFactory::GetForProfile(profile);
```

业务逻辑（对话状态、模型路由）应优先进 **`XenonAiService`**，WebUI 只做展示与 Mojo 粘合。

---

## 7. `DependsOn` 与依赖顺序

若 `XenonAiService` 构造时需要另一个 KeyedService（例如某 `PrefService` 以外的业务服务），在 **`XenonAiServiceFactory` 构造函数**中声明：

```cpp
DependsOn(OtherServiceFactory::GetInstance());
```

并仍对 **`OtherServiceFactory::GetForProfile(profile)` 可能返回 null** 做运行时判断（feature / policy / selections 都可能导致）。

---

## 8. 何时不该用 KeyedService

| 场景 | 更合适的方式 |
|------|----------------|
| 进程级、与 Profile 无关的基础设施 | `BrowserProcess` 等进程级门面（仍要避免乱用 static） |
| 跟随某一帧 / 文档生命周期的能力 | `content::DocumentService<Interface>` 或帧级 Host |
| 单次导航逻辑 | `NavigationThrottle` 等 |

---

## 9. 检查清单

- [ ] `BuildServiceInstanceForBrowserContext` 是否 **使用** `context`（通常 `Profile::FromBrowserContext`）？
- [ ] Service 是否用 **`raw_ptr`** 持有 `Profile*` / 其它长寿命浏览器对象？
- [ ] `Shutdown()` 是否清理观察者、回调、Mojo？
- [ ] `ProfileSelections` 是否覆盖产品要的 **Regular / OTR** 行为？
- [ ] 是否需要 `DependsOn`？跨服务是否在 Shutdown 阶段乱拉 Factory？

---

## 10. 延伸阅读

- [`Chromium与Brave设计模式.md`](./Chromium与Brave设计模式.md) — KeyedService 模式速览  
- [`Xenon_AI集成参考.md`](./Xenon_AI集成参考.md) — Xenon AI 分层与侧栏边界  
- [`Xenon_AI与Brave组件流程对照.md`](./Xenon_AI与Brave组件流程对照.md) — 端到端流程（含 Profile 服务在流水线中的位置）  
