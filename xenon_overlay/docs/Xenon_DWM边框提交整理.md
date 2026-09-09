# Xenon DWM 边框提交整理

本文整理 `feature/xenon-overlay` 中涉及 Windows DWM 边框（`DWMWA_BORDER_COLOR`）修改的实现，涵盖 `XenonWebDialog`、`XenonElectronWindowHost`、`XenonCommonDialog` 三处场景。

## 背景

Windows 11 起，DWM（Desktop Window Manager）会为所有顶层窗口绘制 1px 的边框（border）。该边框颜色由系统主题决定，不受应用程序 `WS_BORDER` 等经典样式控制。对于无边框（frameless）、自定义圆角、自绘阴影的 Xenon 浮层窗口，DWM 边框会破坏视觉效果——在窗口周围出现一条不期望的系统颜色细线。

解决方案是使用 `DwmSetWindowAttribute()` 将 `DWMWA_BORDER_COLOR` 设为 `DWMWA_COLOR_NONE`，告知 DWM 不绘制该边框。

## 核心 API

```cpp
// DWMWA_BORDER_COLOR 的数值常量（SDK 可能尚未定义）
constexpr DWORD kDwmwaBorderColor = 34;

// DWMWA_COLOR_NONE 表示不绘制边框
constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;

COLORREF border_color = kDwmColorNone;
::DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border_color,
                        sizeof(border_color));
```

关键常量说明：

| 常量 | 值 | 含义 |
|---|---|---|
| `kDwmwaBorderColor` | `34` | 对应 `DWMWA_BORDER_COLOR`，用数字避免旧 SDK 缺少该枚举 |
| `kDwmColorNone` | `0xFFFFFFFE` | 对应 `DWMWA_COLOR_NONE`，表示不绘制 DWM 边框 |

## 涉及文件

| 文件 | 函数 | 场景 |
|---|---|---|
| `xenon_overlay/chrome/browser/ui/xenon_web_dialog.cc` | `RemoveDwmBorder()` | WebDialog 无边框窗口 |
| `xenon_overlay/chrome/browser/ui/xenon_electron_window_host.cc` | `ConfigureFramelessDwmWindow()` | Electron BrowserWindow 无边框窗口 |
| `xenon_overlay/chrome/browser/ui/xenon_common_dialog.cc` | `DisableNativeWindowChrome()` | CommonDialog 自绘阴影窗口 |

---

## XenonWebDialog：`RemoveDwmBorder()`

### 代码

```cpp
void RemoveDwmBorder() {
#if BUILDFLAG(IS_WIN)
    views::Widget* widget = GetWidget();
    if (!widget) {
      return;
    }
    HWND hwnd = views::HWNDForNativeWindow(widget->GetNativeWindow());
    if (!hwnd) {
      return;
    }
    constexpr DWORD kDwmwaBorderColor = 34;
    constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;
    COLORREF border_color = kDwmColorNone;
    ::DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border_color,
                            sizeof(border_color));
#endif
}
```

### 调用时机

在 `XenonWebDialogView::FinishAddedToWidget()` 中，当窗口使用无边框（`!UseNativeFrame()`）且处于 Windows 平台时：

```cpp
if (!xenon_delegate_->UseNativeFrame()) {
    // ... 设置圆角等 ...
#if BUILDFLAG(IS_WIN)
    if (UseFramelessCompositorShadow(
            xenon_delegate_->UseDwm(),
            xenon_delegate_->ShouldShowShadow())) {
      SetupFramelessCompositorShadow();
    } else {
      RemoveDwmBorder();
    }
#endif
}
```

### 触发条件

当 frameless WebDialog **不需要**自绘 compositor shadow 时调用 `RemoveDwmBorder()`。具体条件：

| 条件 | 说明 |
|---|---|
| `!UseNativeFrame()` | 窗口已设置为无边框 |
| `!UseFramelessCompositorShadow(dwm, shadow)` | 不走自绘 compositor shadow 路径 |

即当 `UseDwm()` 为 `true` 且在 Win11+ 时，使用 DWM 圆角 + 移除 DWM 边框；不额外绘制 compositor shadow。

### 与 DWM 圆角的关系

`XenonWebDialog` 中 DWM 边框移除与 DWM 圆角紧密关联：

```cpp
// Widget 初始化时
if (!frame) {
    const bool use_system_rounded_corners =
        UseDwmRoundedCorners(dwm || system_rounded_corners);
    if (use_system_rounded_corners) {
      params.rounded_corners =
          gfx::RoundedCornersF(kDwmRoundedCornerHintRadius);
    }
}
```

决策逻辑：

| `UseDwm()` | Win11+ | 行为 |
|---|---|---|
| `true` | `true` | DWM 圆角 + `RemoveDwmBorder()` 移除 DWM 边框 |
| `true` | `false` | 退回 translucent + compositor shadow |
| `false` | 任意 | translucent + Views 自绘圆角 + compositor shadow |

---

## XenonElectronWindowHost：`ConfigureFramelessDwmWindow()`

### 代码

```cpp
void ConfigureFramelessDwmWindow(HWND hwnd) {
  if (!hwnd || base::win::GetVersion() < base::win::Version::WIN11) {
    return;
  }
  DWM_WINDOW_CORNER_PREFERENCE corner_preference = DWMWCP_ROUND;
  ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE,
                          &corner_preference, sizeof(corner_preference));

  constexpr DWORD kDwmwaBorderColor = 34;
  constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;
  COLORREF border_color = kDwmColorNone;
  ::DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border_color,
                          sizeof(border_color));
}
```

### 特点

- 同时设置 DWM 圆角（`DWMWCP_ROUND`）和移除 DWM 边框。
- 仅在 Win11+ 执行。
- 用于 Electron 风格的 BrowserWindow。

### DWM 状态重置问题

Electron BrowserWindow 还通过 `CanResetDwmAppearance()` 监控可能重置 DWM 外观的窗口消息：

```cpp
bool CanResetDwmAppearance(uint32_t message) {
  return message == WM_SHOWWINDOW || message == WM_NCACTIVATE ||
         message == WM_THEMECHANGED ||
         message == WM_DWMCOLORIZATIONCOLORCHANGED ||
         message == WM_SETTINGCHANGE || message == WM_STYLECHANGED;
}
```

这些消息可能导致 DWM 重新应用默认边框颜色，因此需要在消息处理后重新调用 `ConfigureFramelessDwmWindow()`。

---

## XenonCommonDialog：`DisableNativeWindowChrome()`

### 代码

```cpp
// Disable DWM round/shadow/border. Dialog uses compositor 24px corner +
// shadow.
void DisableNativeWindowChrome(views::Widget* widget) {
  if (!widget) {
    return;
  }
  HWND hwnd = views::HWNDForNativeWindow(widget->GetNativeWindow());
  if (!hwnd) {
    return;
  }
  DWM_WINDOW_CORNER_PREFERENCE corner_pref = DWMWCP_DONOTROUND;
  ::DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_pref,
                          sizeof(corner_pref));

  if (base::win::GetVersion() >= base::win::Version::WIN11) {
    constexpr DWORD kDwmwaBorderColor = 34;
    constexpr COLORREF kDwmColorNone = 0xFFFFFFFE;
    COLORREF border_color = kDwmColorNone;
    ::DwmSetWindowAttribute(hwnd, kDwmwaBorderColor, &border_color,
                            sizeof(border_color));
  }
}
```

### 特点

- 与 WebDialog / ElectronWindow 相反，此处**禁止** DWM 圆角（`DWMWCP_DONOTROUND`）。
- 原因：CommonDialog 使用 compositor 层自绘 24px 圆角和阴影，DWM 圆角会与之冲突。
- DWM 边框移除同样仅在 Win11+ 执行。

---

## 三处实现对比

| 维度 | XenonWebDialog | XenonElectronWindowHost | XenonCommonDialog |
|---|---|---|---|
| 函数名 | `RemoveDwmBorder()` | `ConfigureFramelessDwmWindow()` | `DisableNativeWindowChrome()` |
| 文件 | `xenon_web_dialog.cc` | `xenon_electron_window_host.cc` | `xenon_common_dialog.cc` |
| DWM 圆角 | Widget InitParams 中设置 | `DWMWCP_ROUND` | `DWMWCP_DONOTROUND` |
| DWM 边框 | 移除（`kDwmColorNone`） | 移除（`kDwmColorNone`） | 移除（`kDwmColorNone`） |
| Win11+ 检查 | 在外层 `UseDwmRoundedCorners()` 中 | 函数入口处 | `if` 分支中 |
| DWM 重置防护 | 无 | 有 `CanResetDwmAppearance()` | 无 |
| 自绘阴影 | compositor shadow 或无 | 无（Electron 自身处理） | compositor shadow（24px 圆角） |

## 关键约束与注意事项

1. **SDK 兼容**：`DWMWA_BORDER_COLOR` 在较旧的 Windows SDK 中可能未定义，因此代码使用数值常量 `34`，并附注释说明意图。
2. **Win11+ 限定**：`DWMWA_BORDER_COLOR` 属性仅 Windows 11（build 22000+）支持。Win10 上调用不会报错但无效果。
3. **DWM 重置**：某些窗口消息（`WM_NCACTIVATE`、`WM_THEMECHANGED`、`WM_DWMCOLORIZATIONCOLORCHANGED` 等）可能导致 DWM 重新应用默认边框。当前仅 `XenonElectronWindowHost` 做了防护。
4. **圆角与边框的配合**：使用 DWM 圆角（`DWMWCP_ROUND`）时**必须**同时移除 DWM 边框，否则圆角窗口周围会出现系统主题色的弧线边框。
5. **透明窗口的特殊性**：当 `WindowOpacity::kTranslucent` 时，Chromium 会移除 `WS_THICKFRAME`，此时 DWM 边框行为可能不同。WebDialog 的 DWM 路径保持 opaque 正是为了避免这个问题。

## 维护清单

新增需要移除 DWM 边框的窗口类型时，需要：

1. 确认目标窗口是否在 Win11+ 上运行。
2. 在 Widget 创建或显示后调用 `DwmSetWindowAttribute(hwnd, 34, &kDwmColorNone, ...)`。
3. 评估是否需要 DWM 重置防护（参考 `CanResetDwmAppearance()`）。
4. 确认 DWM 圆角策略：是使用 `DWMWCP_ROUND`（让 DWM 负责圆角）还是 `DWMWCP_DONOTROUND`（自绘圆角）。
5. 更新本文档。
