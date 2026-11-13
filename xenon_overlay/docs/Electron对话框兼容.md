# Electron 对话框兼容

`dialog.showMessageBox` / `showMessageBoxSync` 与
`showSaveDialog` / `showSaveDialogSync`、`showOpenDialog` / `showOpenDialogSync`
使用 Browser UI 线程上的真实对话框。
JS 参数适配位于 `resources/ipc/main/dialog.js`，原生实现位于
`chrome/browser/ui/xenon_electron_dialog.{h,cc}`。

## 行为

- 支持省略父窗口、传 `null` 父窗口、或传同一应用存活的 `BrowserWindow`。
  Browser 端再次核对容器、运行代次和窗口归属，数字 ID 不构成访问其他应用窗口的授权。
- 确认框由 Chromium Views 显示任意按钮、消息、详情和复选框。返回原始按钮索引，
  不按按钮文案猜测业务动作；标题关闭和 Esc 返回 `cancelId`，显式 `-1` 保留。
  `defaultId: -1` 使用首按钮作为默认焦点。未指定取消按钮时遵循 Electron 默认推导。
- 保存框使用跨平台 `ui::SelectFileDialog::SELECT_SAVEAS_FILE`，支持标题、默认路径、
  名称与扩展名过滤器，保留原生覆盖确认。它只返回用户所选路径，实际写文件仍由应用负责。
- 打开框复用同一个文件选择器生命周期，保留 `title`、`defaultPath`、分组过滤器名称和
  扩展名。`openFile`、`multiSelections` 映射为单文件或多文件选择，`openDirectory`
  映射为单目录选择，`createDirectory` 控制目录选择器允许创建目录；未指定模式时选择文件。
  Windows/Linux 同时指定 `openFile` 和 `openDirectory` 时按 Electron 的目录优先规则处理。
- 异步接口返回 Promise，等待用户操作期间应用事件循环继续运行；同步接口通过现有
  Mojo 同步调用等待 UI 回包。Browser 适配层保留异步 callback，不创建自己的嵌套 `RunLoop`。
  系统选择器内部的线程和模态行为仍由 Chromium 各平台实现管理。
- 保存取消时异步返回 `{canceled: true, filePath: ''}`，同步返回 `undefined`。
  确认框异步返回 `{response, checkboxChecked}`，同步仅返回 `response`。
  原生错误和父窗口销毁以明确错误结束，不转换成保存成功或用户取消。
- 打开取消时异步返回 `{canceled: true, filePaths: []}`，同步返回 `undefined`；选择成功
  返回真实路径列表。异步打开使用异步桥接，不通过同步函数包装 Promise。
- 父窗口或容器代次结束时取消所属请求；旧请求不能回到重开的新应用。
  确认框同时关闭，文件选择器断开 listener，避免迟到结果访问已销毁运行时。
- 旧 WebUI PageHandler 的打开操作也使用此公共实现，直接传递标准 options，并根据当前
  WebUI 窗口和 PageHandler 代次取消请求。已删除 Windows 专用 picker、跨应用 owner 扫描、
  专用 `ShowElectronOpenDialog` 同步 Mojo 与扩展名中的目录魔术标记。

## 边界

此实现不宣称覆盖 Electron 对话框的全部平台外观或选项。

- 自定义 `icon`、AbortSignal、非零 `textWidth` 暂不支持。
- 保存框的自定义 `buttonLabel`、`message`、`nameFieldLabel`，以及启用
  `showsTagField`、`securityScopedBookmarks` 暂不支持。
- 保存 `properties` 仅接受原生选择器已有的 `createDirectory` 和
  `showOverwriteConfirmation`；其他显式属性返回 `ERR_NOT_SUPPORTED`。
- 打开框暂不支持多目录、macOS 文件和目录混选、`showHiddenFiles`、`promptToCreate`、
  `noResolveAliases`、`treatPackageAsDirectory`、`dontAddToRecent`、安全作用域书签及自定义
  按钮文字。Chromium 公共接口无法表达的显式选项返回 `ERR_NOT_SUPPORTED`，不默默忽略。
- `SelectFileDialog` 没有跨平台主动关闭接口。无父窗口的系统保存框在容器退出后
  可能仍需用户关闭；其 listener 已解绑，不会向旧应用或新代次交付路径。
- `showErrorBox` 保留既有实现。

## 验证

`xenon_dialog_unittest.cjs` 覆盖按钮、取消默认值与显式 `-1`、参数快照、Promise 等待、
父窗口校验、错误传播和文件选择取消；完整 bootstrap 合同测试检查六个接口的实际安装与桥接。
原生 `xenon_electron_dialog_unittest.cc` 检查真实 Views 选择与关闭、父窗口销毁、
打开/保存选择器参数、真实多选结果、目录模式、取消与终止的区别、迟到回调和生命周期。
Manager 回归保留失效 owner/generation 和权限同步调用栈拒绝弹框的检查。
选择器测试使用测试工厂，不代替系统文件框实测。

本轮 Windows 产品验证结果记录在 [Electron原包兼容性验证.md](Electron原包兼容性验证.md)。
跨平台使用 Chromium 抽象，macOS / Linux 尚未在本轮实际编译运行。
