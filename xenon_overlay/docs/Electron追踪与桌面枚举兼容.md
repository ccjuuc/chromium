# contentTracing 与 desktopCapturer

这两个模块属于 Main 进程，使用真实 Chromium 后端，不提供假导出或固定成功返回值。

## contentTracing

- `getCategories()`、`startRecording()`、`getTraceBufferUsage()` 和 `stopRecording()` 经异步 Mojo 调用进入 Browser 的 `TracingController`。
- 每次记录绑定 Browser 校验过的容器/代次以及原生控制器的记录代次。同一时刻只能有一份由该接口发起的记录；已有 Chromium 记录时返回 `ERR_RESOURCE_BUSY`。即使记录被外部调用者停止并替换，旧应用也不能停止新记录。
- 输出在独立阻塞任务序列中分块写盘，检查打开、写入和刷新错误。待写数据最多 16 MiB，超出上限明确失败。没有指定路径时创建临时文件。请求被后端拒绝时不创建或截断目标文件；原生服务失败或输出为空不报告成功。
- 应用退出会停止自己的记录。启动期间后端失联没有 Chromium 错误回调，因此在 30 秒内明确失败，避免永远挂起 Promise。
- 追踪是浏览器级服务，包含宿主及其子进程的数据，并非单应用独享的进程集合。该接口不改变 Chromium/DevTools 本身的追踪实现。
- Chromium 控制器新增的记录代次和完成状态接口均受 `ENABLE_XENON_SERVICE` 保护；旧端点的回调和关闭宏后的执行路径保持原语义。

## desktopCapturer

- `getSources({types, thumbnailSize, fetchWindowIcons})` 实际枚举操作系统屏幕和窗口，使用 Chromium `NativeDesktopMediaList` 获取缩略图，并由各平台 `GetWindowIcon` 获取图标。
- 保留本进程窗口、PNG `NativeImage`、空缩略图和 `appIcon=null` 的语义。零尺寸请求跳过缩略图生成。单边最大 4096 像素，防止一次请求分配不受限图像。
- 原生列表的普通 `Update()` 不抓取缩略图；新增 `ENABLE_XENON_SERVICE` 保护的 `UpdateWithThumbnails()`，其余 Chromium 调用仍使用原方法。
- 使用系统选择器的后端等待实际选择和缩略图事件；取消、超时、平台不支持以及 macOS 权限拒绝明确失败。退出容器会取消请求，并销毁列表及捕获线程。
- `display_id` 只填有明确平台映射的值；当前 macOS 原生屏幕 ID 直接对应 display ID，其他平台暂为空字符串，不能拿源 ID 猜测显示器 ID。
- 此接口枚举来源和预览图；不代表 `getUserMedia` / `setDisplayMediaRequestHandler` 的整条捕获授权流程已经实现。

## 验证状态

独立 JS 合约测试验证输入、异步错误传播、快照语义和 NativeImage 转换。product09 的 Windows 真实宿主探针 `tracing-desktop-probe-01/verification.json` 已通过 5 项：查询 287 个真实类别，输出包含 18,363 条事件的 4,299,589 字节 JSON，枚举 1 个屏幕和 9 个窗口并生成实际 PNG 缩略图，验证零尺寸缩略图，以及带有活动记录时退出应用。宿主退出码 0，无残留测试进程或新增 dump；证据位于 `F:/electron-compat-audit/20261111/`。

5 个原生异步桥测试同时验证未完成请求不阻塞其他 IPC、内联完成延迟投递、原生错误码、应用退休后晚回复和 Error 原型 setter 隔离；已纳入 product09 的 538/538 原生回归。Windows 结果不能替代 macOS/Linux 实测，也不代表媒体捕获授权和完整视频流已通过验收。
