# Electron 电源保持和加密存储

这两个 main 进程模块由 Browser 承担系统权限。请求所属应用和容器代次由原生 IPC 调用方认证，不接受 JavaScript 传入 owner。Worker 和 utility 不因此获得 Browser 权限。

## powerSaveBlocker

支持 `start('prevent-app-suspension' | 'prevent-display-sleep')`、`stop(id)` 和 `isStarted(id)`。Browser 通过 Chromium Device Service 的 WakeLockProvider 提交请求，最终由 Windows/macOS/Linux 的 PowerSaveBlocker 后端持有系统资源。

每个容器代次有独立 ID 集合和一个原生锁。显示保持优先于应用保持；停止显示保持后，如果仍有应用保持请求，会恢复为较低级别。最后一个请求停止、容器退出、宿主销毁或 Device Service 断连时释放资源。另一应用或重启后的应用不能停止旧代次的锁。没有真实后端的平台直接报不支持。

级别变化时先提交新类型的锁，再取消旧锁；每个原生管道始终保持创建时的类型。这样避免 Chromium 当前 context-free WakeLockProvider 在 `ChangeType` 后仍按旧类型保存对象、退出时无法正确移除对象的问题，无需修改 Device Service。

`start` 返回表示请求已提交；`isStarted` 表示该请求仍在当前代次中登记。这与 Electron 的请求语义一致，不表示操作系统、电源管理策略或硬件绝对不会休眠。

## safeStorage

兼容旧版同步方法 `isEncryptionAvailable`、`encryptString`、`decryptString`，以及新版异步方法 `isAsyncEncryptionAvailable`、`encryptStringAsync`、`decryptStringAsync`。异步解密返回 `{result, shouldReEncrypt}`。加密返回 Buffer，密文在 IPC 内只进行 Base64 编码，密钥留在 Browser 的 Chromium OSCrypt Encryptor 内。

Browser 使用宿主 `OSCryptAsync::GetInstance` 获取真实用户密钥配置。初始化完成前可用性为 false，同步加解密明确报未就绪；异步调用等待初始化完成，不运行嵌套消息循环。初始化失败、系统密钥不可用或原生解密失败时透传失败，不把明文当作密文返回。退出时取消所属代次的未完成请求。

Windows 使用宿主 OSCrypt 的 DPAPI/App-Bound 配置，macOS 使用宿主 Keychain 配置；能力由实际 Encryptor 判断，不按操作系统名称写死为 true。提供的是本宿主生成密文的加解密能力，不保证其他 Electron 应用、不同宿主数据目录或历史 Electron 密文可互通。

Linux 的 PosixKeyProvider 是固定密钥回退，不能默认当作用户系统密钥。通过 Encryptor 的只读实际选中 provider 信息识别该后端，默认不开放加解密；应用必须显式调用 `setUsePlainTextEncryption(true)`。该选择只对调用应用的当前代次生效，退出即失效。

`getSelectedStorageBackend()` 仅在 Linux 导出。未初始化时返回 `unknown`；确定选中 Posix 回退时返回 `basic_text`。Chromium 的 Freedesktop provider 不公开最终服务是 GNOME、KWallet 还是其他实现，因此其余情况返回 `unknown`，不通过环境变量或桌面名称猜测。返回 unknown 不会覆盖 Encryptor 的真实可用性。

## 验证范围

JavaScript 契约检查覆盖二进制密文、中文和 NUL、严格参数类型、同步错误透传、异步等待和取消、平台导出差异。原生测试使用真实 OSCrypt 测试密钥进行加解密、随机化和损坏检测，并覆盖初始化失败、代次隔离及退出取消。电源测试通过真实 Mojo 管道检查锁级别变化、释放和断连。

这些检查不能代替各平台的实际睡眠、电池策略、Keychain 解锁、Linux 密钥服务及应用重启持久化验证。只有相应平台运行记录才能证明这些行为已实际通过。

2026-11-12 的 Windows product09 实测通过：`powercfg /requests` 记录到本宿主的显示保持请求，停止显示保持后变为防挂起请求，应用带未停止的锁退出后该请求消失。其他应用原有请求仍保留。同步和异步加密均完成中文/NUL 往返与损坏密文拒绝；关闭宿主并使用同一 profile 重启后，两份密文仍可解密。两次宿主均以 0 退出，没有自有残留进程或 minidump。原生电源 6 项、加密存储 10 项通过。此次实测不覆盖 macOS/Linux 或真实休眠/唤醒策略。
