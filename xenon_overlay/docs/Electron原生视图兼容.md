# Electron 原生视图

`View`、`WebContentsView` 与 `BrowserWindow.contentView` 使用 Chromium Views
原生视图树。`contentView` 是窗口客户区中覆盖在主网页上方的空视图，空白区域的
输入仍传给主网页。它不替换 BrowserWindow 自带的 WebContents。

视图支持添加、移除、调整顺序、跨父视图移动、边界、可见性与背景色。
JS 父视图持有子视图；原生端同步维护独立视图与已挂载视图的所有权，验证容器
代际和父子关系，拒绝循环或跨应用挂载。操作成功后才更新 JS 视图树。

WebContentsView 创建真实独立 WebContents，沿用 session、preload、Node 集成、
导航及键盘事件路径。既有独立 WebContents 可被接管，已挂载的 WebContents
拒绝重复接管。`getOwnerBrowserWindow()` 根据当前视图树查询，移除后返回 null。
窗口关闭仅移除包含的 WebContentsView；应用须显式调用 `webContents.close()`
销毁独立页面。容器退出会统一释放仍存活的视图与页面。

`close({waitForBeforeUnload: true})` 会执行网页的 beforeunload。网页阻止卸载时
默认保留页面并通知主进程 `will-prevent-unload`；监听器调用 `preventDefault()`
后明确关闭页面。默认 `close()` 跳过 beforeunload，按 Electron 的接口契约执行。

`setBorderRadius()` 使用平台原生视图的圆角能力，不支持的平台明确报错。
自定义 `setLayout()` 尚未实现，明确抛出 `ERR_NOT_SUPPORTED`，不以空操作成功
掩盖布局缺失。原生实现不直接使用 HWND，Chromium 键盘入口扩展由
`ENABLE_XENON_SERVICE` 宏保护。

独立 WebContents 的可见性由视图/祖先/Widget 生命周期同步到 Chromium 页面状态。
不能仅隐藏原生 NativeViewHost：这条路径不一定触发页面遮挡通知，网页可能继续
认为自己可见。默认 `backgroundThrottling: true` 时，隐藏、移除及隐藏祖先会令
`document.hidden` 为 true；设为 false 时仍遵循 Electron 的后台页面语义。

2026-11-12 的 Windows 实际产品测试分别以两种 backgroundThrottling 设置运行，
各 15 项通过（共 30 项），覆盖真实视口尺寸、键盘/拦截、隐藏/恢复、祖先隐藏、
窗口隐藏、移除重挂、原窗口销毁、独立页面保活、beforeunload 取消及显式关闭。
这验证了当前 Windows 产物；macOS/Linux 仍需在相应平台运行原生验证。

BrowserWindow 默认允许 `show: false` 的窗口在首次显示前绘制。
`paintWhenInitiallyHidden: false` 会阻止首次隐藏阶段的绘制，即使
`backgroundThrottling: false` 使页面报告可见，也要等首次显示后才产生帧。
显式隐藏、最小化、首次显示和关闭均结束初始隐藏绘制阶段；之后使用实际窗口
状态及 `backgroundThrottling` 同步页面可见性。

`ready-to-show` 来自 Blink 的首次有意义布局通知，与 Electron 的渲染入口一致。
它不等待隐藏原生窗口无法完成的屏幕呈现，也不由 DOMContentLoaded 或页面加载
完成事件模拟。已完成布局的空文档也可能产生该通知，这是 Blink/Electron 的
行为；同一 BrowserWindow 后续导航不会重复发送。通知经专用 Mojo 方法传递，
浏览器核验当前文档、主框架及应用容器代际，不依赖页面的 Node 集成或 preload。

已授权 Electron 主框架的渲染绑定通过作用域暂时解除 Viz 对“已提交但未显示”帧的
节流，确保初始隐藏窗口能持续绘制。该作用域不改变真实隐藏、最小化和页面冻结
的调度规则；文档销毁时释放，多个绑定及画中画设置按计数和原有策略组合。
Aura 的导航表面分配同样区分原生视图隐藏与渲染宿主隐藏，避免丢失初始绘制表面。
上述 Chromium 扩展均由 `ENABLE_XENON_SERVICE` 保护；普通浏览器页面不取得
解除节流的作用域。

2026-11-12 的最终 Windows 产品验证还通过了 58 项窗口检查，覆盖首次隐藏绘制
与后台节流的四种组合、连续动画帧、真实视频帧、隐藏/最小化/恢复及单次 ready
通知；不启用 Node、没有 preload 的两种 sandbox 配置通过 4 项；连续导航生命周期
通过 10 项。导航行为也以官方 Electron 44.10.2 运行同一探针进行了对照。
这三组探针均正常退出，未产生崩溃转储。
