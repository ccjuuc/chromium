# Brave Farbling 反指纹移植说明

本文记录 `H:\chromium_142\src` 这份 Chromium 142 源码中，Brave 风格 farbling 反指纹能力的移植思路、当前修改点、行为边界和后续扩展方向。

## 目标

目标是移植 Brave 反指纹保护中最实用的一部分，但不直接搬入完整 Brave Shields 体系。

Brave 上游实现和下面这些 Brave 专属系统耦合很深：

- Brave Shields content settings
- 站点级 farbling 等级：off、balanced、maximum
- Brave 专属 Mojo settings struct
- profile prefs 和 UI 控制
- web compatibility 例外规则

当前本地 Chromium 移植版采用一个更小、更容易落地的设计：

- 只对普通 `http` 和 `https` 页面启用 farbling
- 不影响内部 scheme、扩展、WebUI、`file://` 和非 Web 上下文
- 使用“进程随机 token + 页面 origin”生成稳定的 per-origin 值
- 提供一个全局命令行关闭开关：`--disable-brave-farbling`
- 保持调用点足够小，方便后续继续接 WebGL、Audio、字体等指纹面

## 当前工作区状态

当前 git 可见的新增 helper 文件：

- `third_party/blink/renderer/core/canvas_interventions/brave_farbling.h`
- `third_party/blink/renderer/core/canvas_interventions/brave_farbling.cc`

之前规划或应用过的第一批接入点会在下文详细列出。如果工作区里相关接入点已经被 reset 或覆盖，可以按本文档的位置重新核对或恢复。

## 核心 Helper

### 文件

`third_party/blink/renderer/core/canvas_interventions/brave_farbling.h`

公开 API：

```cpp
CORE_EXPORT bool ShouldApplyBraveFarbling(ExecutionContext* context);

CORE_EXPORT uint64_t BraveFarblingKey(ExecutionContext* context,
                                      std::string_view surface);

CORE_EXPORT void ApplyBraveCanvasFarbling(ExecutionContext* context,
                                          base::span<uint8_t> data);
```

### 文件

`third_party/blink/renderer/core/canvas_interventions/brave_farbling.cc`

职责：

- 持有进程内 farbling token
- 判断某个 `ExecutionContext` 是否应该启用 farbling
- 派生稳定的 per-origin 随机 key
- 对 Canvas 像素读出结果应用 Brave 风格扰动

当前 helper 只在以下条件全部满足时启用 farbling：

- `ExecutionContext*` 非空
- 命令行没有 `--disable-brave-farbling`
- 当前 security origin 的协议是 `http` 或 `https`

这样做是为了避免影响浏览器内部页面、本地文件和非网页上下文。

## 随机性模型

当前移植版使用一个进程内 token：

```cpp
static const base::NoDestructor<base::Token> token(
    base::Token::CreateRandom());
```

这意味着：

- 同一个浏览器进程内结果稳定
- 浏览器重启后 token 变化，结果随之变化
- 不同 origin 会得到不同结果
- 不同指纹面可以通过传入不同 surface name 得到不同 key

普通指纹面的 key material：

```text
HMAC_SHA256(process_token, origin + "\0" + surface_name)
```

Canvas 的 key material 分两层：

```text
origin_key = HMAC_SHA256(process_token, origin + "\0")
canvas_key = HMAC_SHA256(origin_key, canvas_pixel_bytes)
```

Canvas key 依赖 canvas 内容。这和 Brave 的思路一致：同一个站点、同一份 canvas 内容会得到稳定扰动；不同 canvas 内容会得到不同扰动。

## Canvas Farbling

### 预期 GN 接入点

`third_party/blink/renderer/core/canvas_interventions/build.gni`

需要加入：

```gn
blink_core_sources_canvas_interventions = [
  "brave_farbling.cc",
  "brave_farbling.h",
  ...
]
```

### 预期读出接入点

`third_party/blink/renderer/modules/canvas/canvas2d/base_rendering_context_2d.cc`

在 `BaseRenderingContext2D::getImageDataInternal(...)` 里，`readPixels(...)` 成功后应用：

```cpp
ApplyBraveCanvasFarbling(GetTopExecutionContext(), image_data->RawByteSpan());
```

覆盖 JS API：

```js
ctx.getImageData(...)
```

`third_party/blink/renderer/core/html/canvas/html_canvas_element.cc`

在 `ImageDataBuffer::Create(image_bitmap)` 之后、`ToDataURL(...)` 之前应用：

```cpp
ApplyBraveCanvasFarbling(
    GetExecutionContext(),
    gfx::SkPixmapToWritableSpan(data_buffer->pixmap_multable()));
```

覆盖 JS API：

```js
canvas.toDataURL(...)
```

### 算法

Canvas farbling 只会改动极少量 RGB 通道的最低 bit：

```cpp
data[pixel_index] = data[pixel_index] ^ (bit & 0x1);
```

关键性质：

- 人眼基本看不出视觉变化
- canvas 字节级 hash 会改变
- 同一 origin、同一浏览器进程、同一 canvas 内容下重复读取结果稳定
- 不同 origin 之间不容易复用同一个 canvas 指纹
- 浏览器重启后 token 改变，输出也会改变

这和“每次调用都随机加噪声”不同。普通随机噪声很容易被脚本检测出来，例如连续两次调用 `toDataURL()` 得到不同结果。Farbling 的目标是在不明显破坏网页行为的前提下，让指纹长期复用和跨站关联变得不可靠。

## Hardware Concurrency Farbling

### 预期文件

`third_party/blink/renderer/core/execution_context/navigator_base.cc`

目标 JS API：

```js
navigator.hardwareConcurrency
```

建议行为：

- 如果真实值 `<= 2`，保持不变
- 否则返回 `2` 到 `min(real_value, 8)` 之间的 per-origin 稳定值
- 使用下面这个 surface key：

```cpp
BraveFarblingKey(execution_context, "navigator.hardwareConcurrency")
```

示例公式：

```cpp
static constexpr unsigned int kMinFarbledProcessors = 2;
static constexpr unsigned int kMaxBalancedProcessors = 8;
const unsigned int max_processors =
    std::min(hardware_concurrency, kMaxBalancedProcessors);
const uint64_t key = BraveFarblingKey(execution_context,
                                      "navigator.hardwareConcurrency");
return kMinFarbledProcessors + base::checked_cast<unsigned int>(
                                    key % (max_processors + 1 -
                                           kMinFarbledProcessors));
```

这样可以隐藏高核心数机器的精确信息，同时返回值仍然看起来合理。

## Device Memory Farbling

### 预期文件

`third_party/blink/renderer/core/frame/navigator_device_memory.h`

`third_party/blink/renderer/core/frame/navigator_device_memory.cc`

`third_party/blink/renderer/core/execution_context/navigator_base.h`

`third_party/blink/renderer/core/execution_context/navigator_base.cc`

目标 JS API：

```js
navigator.deviceMemory
```

因为 `NavigatorDeviceMemory` 本身没有直接保存 `ExecutionContext`，建议加一个 protected virtual hook：

```cpp
class CORE_EXPORT NavigatorDeviceMemory {
 public:
  virtual ~NavigatorDeviceMemory() = default;
  virtual float deviceMemory() const;

 protected:
  virtual ExecutionContext* GetDeviceMemoryExecutionContext() const;
};
```

然后在 `NavigatorBase` 里 override：

```cpp
ExecutionContext* GetDeviceMemoryExecutionContext() const override;
```

实现：

```cpp
ExecutionContext* NavigatorBase::GetDeviceMemoryExecutionContext() const {
  return GetExecutionContext();
}
```

建议 farbling 行为：

- 先取 Chromium 原本的 approximated device memory 值
- 如果 farbling 不适用，直接返回原值
- 否则从合理 bucket 中按 per-origin key 选择一个稳定值

可用值：

```cpp
static constexpr std::array<float, 5> kValidMemoryValues = {
    2.0, 4.0, 8.0, 16.0, 32.0};
```

Balanced 行为建议避免返回低于 `4.0` 的值，除非以后再增加更严格的 maximum 模式。本地公式可以使用：

```cpp
BraveFarblingKey(execution_context, "navigator.deviceMemory")
```

## 命令行控制

全局关闭开关：

```text
--disable-brave-farbling
```

当前 helper 在 `ShouldApplyBraveFarbling(...)` 里检查这个开关。

第一版先做成粗粒度开关。后续可以替换或补充为：

- Chromium pref
- enterprise policy
- per-site content setting
- `chrome://flags` 项
- 类似 Brave Shields 的 UI

## 和 Brave 上游的差异

这不是完整 Brave Shields 移植。

Brave 上游提供：

- farbling 等级：off、balanced、maximum
- 站点级设置和例外
- 分指纹面的 web compatibility bucket，例如 Canvas、WebGL、Audio、Font、Screen
- profile 级 farbling token 行为
- worker/frame settings 传播
- UI 和 policy 集成

当前本地移植版提供：

- 进程内 token
- per-origin 稳定值
- 只对 `http` 和 `https` 启用
- 一个全局关闭开关
- 可被 Blink 各调用点复用的 standalone helper

## 验证

已完成：

```text
git diff --check
```

当前环境限制：

`gn check` 没有跑通，因为 GN 尝试执行：

```text
C:/Users/Administrator/AppData/Local/Microsoft/WindowsApps/python3.exe
```

这是 WindowsApps 的 Python 占位符。GN 在运行 `build/toolchain/get_concurrent_links.py` 时返回 `9009`。

在真正依赖 patch 前，建议修好 Python/depot_tools PATH 后再跑：

```bat
gn check H:\chromium_142\src\out\Debug_64 //third_party/blink/renderer/core:core
```

或者构建 Blink target：

```bat
autoninja -C H:\chromium_142\src\out\Debug_64 blink_core
```

## 手动 Smoke Test

请用普通 `http` 或 `https` 页面测试，不要用 `file://`。

Canvas：

```js
const canvas = document.createElement('canvas')
canvas.width = 64
canvas.height = 64
const ctx = canvas.getContext('2d')
ctx.fillStyle = 'rgb(120, 80, 40)'
ctx.fillRect(0, 0, 64, 64)
console.log(canvas.toDataURL())
console.log([...ctx.getImageData(0, 0, 8, 8).data].join(','))
```

预期：

- 同一页面内重复调用结果稳定
- 不同 origin 输出不同
- 浏览器重启后输出变化
- 加 `--disable-brave-farbling` 启动时恢复未扰动行为

Hardware concurrency：

```js
console.log(navigator.hardwareConcurrency)
```

预期：

- 低核心设备可以保持原值
- 高核心设备返回稳定且合理的值，通常在 `2` 到 `8` 之间

Device memory：

```js
console.log(navigator.deviceMemory)
```

预期：

- 返回稳定且合理的 bucket
- 不暴露比 Chromium 原有 approximate bucket 更精细的真实内存信息

## 后续建议移植项

建议按下面顺序继续：

1. WebGL renderer/vendor/extension farbling
2. WebAudio buffer/analyser farbling
3. language list reduction/farbling
4. font visibility filtering
5. speech synthesis voices farbling
6. screen and pointer coordinate farbling
7. media device enumeration farbling

WebGL 和 WebAudio 的实际指纹收益最高，但它们会触及更多 Blink modules 代码，建议配合测试谨慎移植。
