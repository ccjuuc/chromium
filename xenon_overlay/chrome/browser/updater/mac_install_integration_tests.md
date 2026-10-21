# macOS 签名更新集成测试

`xenon_update_installer_mac_integration_unittest.mm` 在 macOS 上调用真实的
`PrepareMacFullUpdate` 和 `InstallMacUpdate`，执行仓库中 Chromium 安装脚本。
没有 mock 签名、包解压、DMG 挂载或安装步骤。普通单测不需要发布签名证书，
这四项集成测试在未设置 fixture 环境变量时明确显示 `SKIPPED`；这不代表
macOS 更新安装已验证。

## Fixtures

准备两个最终发布状态的 `.app`：

- 两者已用有效的 Developer ID Application 证书完成所有嵌套代码的签名，
  Team ID、bundle ID、designated requirement 和产品名兼容。
- `.app` 文件名相同，`CFBundleShortVersionString` 为数字版本，新版本严格
  大于旧版本；不要为了测试修改已签名的 Info.plist。
- 使用 Chromium 版本化 Framework 布局：
  `Contents/Frameworks/<CFBundleDisplayName> Framework.framework/Versions/<version>`，
  并有正常的 `Current` 链接及 Framework 可执行文件。
- 输入路径为绝对路径，指向实际目录，不能是 `.app` 目录的符号链接。
  运行用户能读取它们；输入可以放在只读目录。

测试仅把输入复制进自己创建的 `0700` 临时目录，然后生成 ZIP/DMG 并更新
临时目录中的旧版本副本。不改签输入，不写入 `/Applications`，不启动或
退出浏览器，也不需要 `sudo`。真实安装脚本会向 LaunchServices 注册临时
应用路径；临时文件在测试结束时删除。

磁盘需容纳两种包分别测试时的旧版、新版、解包副本与安装结果。保留数 GB
以上的可用空间；发布包较大时测试耗时可达数分钟。需使用 macOS 自带的
`codesign`、`ditto`、`hdiutil`、`bash` 和 `rsync`。

## 执行

在已配置的 macOS Chromium 构建环境运行：

```sh
autoninja -C out/Release xenon_updater_unittests
XENON_MAC_UPDATE_OLD_APP='/absolute/path/old/Xenon.app' \
XENON_MAC_UPDATE_NEW_APP='/absolute/path/new/Xenon.app' \
  out/Release/xenon_updater_unittests \
  --gtest_filter='SignedRelease/XenonMacInstallerIntegrationTest.*' \
  --test-launcher-jobs=1 \
  --test-launcher-timeout=1800000
```

只有两个变量均未设置时跳过。只设置一个变量、路径错误、签名无效或版本
不兼容会导致失败，以免 CI 把错误配置误判为已验证。运行前必须确认过滤器
实际匹配四项测试，并检查报告中 ZIP 和 DMG 都是 `PASSED`，不是 `SKIPPED`。

每种格式均验证包准备后的签名、安装后的 Developer ID 签名和版本、
`Current` 指向新版、旧 Framework 保留且二进制内容未改变、新 Framework
内容正确，以及再次安装相同版本被拒绝后已安装的应用仍然有效。测试安装副本
名称增加 `Renamed ` 前缀，发布包仍保留原文件名，以验证重命名的应用可以更新；
成功安装用例还将旧版本 Framework 目录设为 `0555`，验证保留旧版本不要求
该目录可写。测试退出前会恢复测试所改的目录权限。

另外两个恢复用例临时为测试副本的主程序设置 `UF_IMMUTABLE` 文件标志。
签名内容不变，真实安装脚本先复制新版 Framework，再在替换受保护的主程序时
失败。测试确认已经发生部分安装，清除该标志，并使用同一个
`MacUpdateInstallContext` 重新准备全量包和安装，要求最终签名、版本和旧
Framework 均正确；恢复成功后的同版本重装仍须拒绝。清除文件标志由 RAII
保证，提前断言失败也会执行；运行目录的文件系统必须支持用户 immutable 标志。
恢复用例还模拟其他进程已经写入更高版本号，验证保留的重试身份不会放行降级。

这些测试覆盖已有签名发布包的本地安装流程，不覆盖网络 feed、差分包生成、
浏览器进程实际重启、提权或断电恢复。Windows 上的编译和单测通过不能替代
这里的 macOS 原生执行。
