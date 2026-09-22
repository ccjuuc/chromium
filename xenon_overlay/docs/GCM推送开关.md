# GCM 推送开关

启用 `ENABLE_XENON_SERVICE` 的桌面构建默认关闭 Google GCM 推送。
Windows、macOS 和 Linux 共用同一驱动开关，不依赖应用名称或 TH/PLE 配置。

关闭时，驱动不会启动 Google check-in、MCS 长连接或注册请求；恢复已有订阅
和新注册都经过同一检查。注册、发送和获取 token 使用现有的 `GCM_DISABLED`
结果，验证注册的回调返回 `false`，不会留下等待网络完成的回调。

应用需要 Google 推送时，在启动宿主前显式开启：

```text
--enable-features=XenonGCM
```

默认无需额外参数；也可以用 `--disable-features=XenonGCM` 显式关闭。
修改参数后需完全退出并重新启动宿主。此开关不修改用户的订阅数据，也不隐藏
其他错误日志。依赖 GCM 的 Web Push、扩展推送及相关通知在关闭时不可用。

普通网页请求、视频播放和 Xenon IPC 不经过 GCM。关闭本开关不等于禁止所有
Google 网络请求，其他 Google 服务有各自的启停逻辑。

未启用 `ENABLE_XENON_SERVICE` 时保留上游 GCM 行为。
