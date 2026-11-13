# Node 文件权限

`fs.chmodSync(path, mode)`、`fs.chmod(path, mode, callback)` 和
`fs.promises.chmod(path, mode)` 使用同一真实文件系统操作，覆盖 main、Worker、
Utility、具备 Node 权限的 renderer 及 Electron `original-fs`。

路径接受字符串、UTF-8 Buffer 和 file URL；mode 接受 uint32 整数和八进制
字符串。参数错误在同步/回调 API 中同步抛出，Promise API 则拒绝。回调在
原生操作完成后异步调用一次，成功参数为 null；Promise 和同步调用成功均
返回 undefined。路径、权限和原生操作失败保留实际错误码、syscall 和 path。

原生层通过 libuv 的 `uv_fs_chmod` 调用平台文件系统，不再以“路径存在”冒充
成功。Windows 按 Node/libuv 语义改变真实只读属性，不伪造 Unix 用户/组权限；
POSIX 使用实际 permission bits 并遵循符号链接。异步调用仍由已有文件工作
序列执行，不在 renderer/UI 上做阻塞磁盘操作。

ASAR 的既有只读策略保持一致，逻辑归档成员不接受 chmod；`original-fs`
直接作用于物理文件，不切换全局 ASAR 状态。renderer 的 chrome 虚拟挂载没有
操作系统权限模型，明确报 ERR_NOT_SUPPORTED。现有主进程 fchmod/FileHandle
chmod 已使用 `uv_fs_fchmod`，本次没有新增 fd 接口或改变其所有权规则。

源码测试包含 main/renderer 的 fs/original-fs 共 13 项权限契约，并以真实
Node 文件系统作为边界验证只读属性、文件内容、URL/Buffer、参数错误及回调
时序。原生测试覆盖实际属性改变、无效请求不写盘、ASAR 与物理视图；POSIX
另有符号链接测试。源码 JavaScript 权限及相关回归 46 项通过；product16
原生权限新增 3 项通过。真实宿主四角色共 31 项通过，均实际验证只读属性
改变、内容保留、正确错误码和异步回调。宿主退出码为 0，无残留进程及新
dump，证据为 `F:/electron-compat-audit/20261111/fs-chmod-probe-product16-01/verification.json`。
未声明 macOS/Linux 实机已通过。
