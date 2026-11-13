# Electron 系统主题

`systemPreferences` 是 EventEmitter。`getAccentColor()` 从 Chromium 的平台
`OsSettingsProvider` 读取真实强调色，按 `RRGGBBAA` 返回；平台没有提供颜色时
明确报 `ERR_NOT_SUPPORTED`。`accent-color-changed` 来自系统设置变化，仅颜色
实际改变时发送事件对象与新颜色，不用定时器生成通知。

`nativeTheme.shouldUseDarkColors`、`shouldUseHighContrastColors`、
`shouldUseInvertedColorScheme` 和 `prefersReducedTransparency` 读取同一系统
来源；对应状态变化时发出 `updated`。宿主退出时取消原生订阅。
`isAeroGlassEnabled()` 在 Windows 查询 DWM，其余平台明确不支持。

`themeSource` 当前支持 `system`。`light` / `dark` 需要按应用覆盖窗口及网页
主题，当前明确报错，不修改整个 Chromium 宿主的全局主题。
macOS 的任意本地通知订阅、其他颜色和媒体权限接口不在本次范围内。

原生回归通过 Chromium 的 MockOsSettingsProvider 检查真实订阅通路、RGBA
转换、状态变化、去重和取消订阅；JavaScript 回归检查读取、事件参数和
`on` / `off`。Windows 真实发行包复验以兼容性验证报告为准；macOS/Linux
尚未实机验收。
