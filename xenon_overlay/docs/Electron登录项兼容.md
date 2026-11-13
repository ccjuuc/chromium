# Electron 登录项兼容

Windows 的 `app.setLoginItemSettings` / `getLoginItemSettings` 操作当前用户的真实 Run 和 StartupApproved 注册表项。Browser 从已验证的容器配置提供应用入口、实际宿主程序和宿主用户数据目录；应用不能通过模拟的 `process.execPath` 或请求字段替换这些信息。

每项以规范化应用路径与宿主用户数据目录的 SHA-256 标识，后台 `NormalizeFilePath` 解析 junction 和实际路径拼写，路径大小写别名不会创建另一条登录项。启动命令保留 `--user-data-dir` 与 `--xenon-electron-app`，应用参数放在 `--` 后，使用 Chromium CommandLine 处理空格和引号。关闭登录启动只删除该应用、该宿主配置自己的条目；不存在的条目不会创建注册表键。

支持 `openAtLogin`、`args`、`enabled`；`openAsHidden: false` 可接受，启用隐藏启动明确不支持。`enabled` 缺省为 true，遵循 Electron v39.2.7 的实际实现：再次设置 `openAtLogin: true` 会重新启用该项。`path`、`name`、`type`、`serviceName` 替换和其他未实现选项返回 `ERR_NOT_SUPPORTED`。Windows Run 的启动命令超过 260 字符时仅拒绝启用；读取和移除仍可进行。

`openAtLogin` 表示当前查询业务参数匹配的条目存在；`executableWillLaunchAtLogin` 表示本应用已有匹配宿主路由且未禁用的条目，不受查询参数是否相同影响。`launchItems` 返回本应用条目的实际宿主程序、完整参数（含宿主路由和 `--`）及启用状态，可以还原注册命令，不包含共享宿主下其他应用的条目。

macOS 保留原有真实 getter，查询当前宿主 bundle 的实际登录项状态，未声称是每个被托管应用的独立注册。应用独立的 macOS 登录项注册与其他平台的此能力仍明确不支持。

验证包括实际注册表的临时 hive 隔离测试：启用、禁用、移除、参数引用、应用与 Profile 隔离、junction/大小写别名、参数不匹配、超长命令及拒绝选项。测试不会更改用户真实的启动项。另有 JS 契约验证 Browser 路由、禁止从应用提供宿主路径、错误代码与 macOS getter 行为。

语义参考：Electron v39.2.7 的 `shell/browser/browser.h`、`shell/browser/browser_win.cc` 与 `shell/common/gin_converters/login_item_settings_converter.cc`。
