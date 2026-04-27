# Chromium 命令行开关全览（桌面 / Windows 视角）

> 自动从 Chromium 源码 (`H:\chromium_142\src`) 扫描生成。已过滤掉 Android / iOS / ChromeOS / Fuchsia / Cast 仅限平台的开关。

> 共收录 **1185** 个开关（去重后），扫描了 所有 `*switches*.cc/.h` 以及若干 `command_line_switches` 文件。

> 每个开关下方的英文引用来自源码原注释（最权威准确的用途说明），分类标题与概述为中文。Buildflag 一列表明该开关只在特定编译条件下生效，写着 “全平台” 则没有条件编译限制。

> 使用方式：`chrome.exe --switch-name[=value] ...`。


## 目录

- [一、核心 / 启动 / 浏览器主流程](#一、核心-启动-浏览器主流程)
  - [Apps (apps/switches.cc) (1)](#apps-appsswitchescc)
  - [Base 基础库 (base/base_switches.cc) (28)](#base-基础库-basebase_switchescc)
  - [Base i18n (base/i18n/base_i18n_switches.cc) (4)](#base-i18n-basei18nbase_i18n_switchescc)
  - [Chrome 浏览器 (chrome/common/chrome_switches.cc) (215)](#chrome-浏览器-chromecommonchrome_switchescc)
  - [Embedder Support (components/embedder_support/switches.cc) (8)](#embedder-support-componentsembedder_supportswitchescc)
  - [Content 层 (content/public/common/content_switches.cc) (213)](#content-层-contentpubliccommoncontent_switchescc)
- [二、渲染 / 图形 / 合成 / Blink](#二、渲染-图形-合成-blink)
  - [合成器 cc (cc/base/switches.cc) (33)](#合成器-cc-ccbaseswitchescc)
  - [Viz (components/viz/common/switches.cc) (10)](#viz-componentsvizcommonswitchescc)
  - [Viz Demo (components/viz/demo/common/switches.cc) (1)](#viz-demo-componentsvizdemocommonswitchescc)
  - [GPU Command Buffer Client (gpu/command_buffer/client/gpu_switches.cc) (2)](#gpu-command-buffer-client-gpucommand_bufferclientgpu_switchescc)
  - [GPU Command Buffer Service (gpu/command_buffer/service/gpu_switches.cc) (18)](#gpu-command-buffer-service-gpucommand_bufferservicegpu_switchescc)
  - [GPU 配置 (gpu/config/gpu_switches.cc) (51)](#gpu-配置-gpuconfiggpu_switchescc)
  - [Skia (skia/ext/switches.cc) (2)](#skia-skiaextswitchescc)
  - [Blink (third_party/blink/common/switches.cc) (39)](#blink-third_partyblinkcommonswitchescc)
  - [UI Base (ui/base/ui_base_switches.h) (20)](#ui-base-uibaseui_base_switchesh)
  - [UI Compositor (ui/compositor/compositor_switches.cc) (7)](#ui-compositor-uicompositorcompositor_switchescc)
  - [Display (ui/display/display_switches.cc) (9)](#display-uidisplaydisplay_switchescc)
  - [gfx (ui/gfx/switches.cc) (7)](#gfx-uigfxswitchescc)
  - [OpenGL / ANGLE (ui/gl/gl_switches.cc) (51)](#opengl-angle-uiglgl_switchescc)
- [三、媒体 / 音视频 / WebRTC / MIDI](#三、媒体-音视频-webrtc-midi)
  - [Cast Streaming (components/cast_streaming/browser/cast_streaming_switches.h) (1)](#cast-streaming-componentscast_streamingbrowsercast_streaming_switchesh)
  - [Media (media/base/media_switches.cc) (45)](#media-mediabasemedia_switchescc)
  - [Media Capture (media/capture/capture_switches.cc) (2)](#media-capture-mediacapturecapture_switchescc)
- [四、网络 / 安全浏览 / 鉴权](#四、网络-安全浏览-鉴权)
  - [网络会话配置 (components/network_session_configurator/common/network_switches.cc) (2)](#网络会话配置-componentsnetwork_session_configuratorcommonnetwork_switchescc)
  - [Safe Browsing (components/safe_browsing/core/common/safebrowsing_switches.cc) (22)](#safe-browsing-componentssafe_browsingcorecommonsafebrowsing_switchescc)
  - [GAIA (google_apis/gaia/gaia_switches.cc) (9)](#gaia-google_apisgaiagaia_switchescc)
  - [GCM (google_apis/gcm/engine/gservices_switches.cc) (3)](#gcm-google_apisgcmenginegservices_switchescc)
  - [Network Service (services/network/public/cpp/network_switches.cc) (18)](#network-service-servicesnetworkpubliccppnetwork_switchescc)
- [五、多进程 / 沙箱 / 崩溃 / Tracing / Metrics](#五、多进程-沙箱-崩溃-tracing-metrics)
  - [Crash (components/crash/core/app/crash_switches.cc) (2)](#crash-componentscrashcoreappcrash_switchescc)
  - [Heap Profiling (components/heap_profiling/in_process/switches.cc) (1)](#heap-profiling-componentsheap_profilingin_processswitchescc)
  - [Metrics (components/metrics/metrics_switches.cc) (9)](#metrics-componentsmetricsmetrics_switchescc)
  - [Heap Profiling 服务 (components/services/heap_profiling/public/cpp/switches.cc) (23)](#heap-profiling-服务-componentsservicesheap_profilingpubliccppswitchescc)
  - [Tracing (components/tracing/common/tracing_switches.cc) (19)](#tracing-componentstracingcommontracing_switchescc)
  - [Mojo Test (mojo/core/test/test_switches.cc) (2)](#mojo-test-mojocoretesttest_switchescc)
  - [沙箱 (sandbox/policy/switches.cc) (15)](#沙箱-sandboxpolicyswitchescc)
  - [内存检测 (services/resource_coordinator/memory_instrumentation/switches.cc) (2)](#内存检测-servicesresource_coordinatormemory_instrumentationswitchescc)
  - [Service 可执行 (services/service_manager/public/cpp/service_executable/switches.cc) (2)](#service-可执行-servicesservice_managerpubliccppservice_executableswitchescc)
  - [Service Manager (services/service_manager/switches.cc) (1)](#service-manager-servicesservice_managerswitchescc)
- [六、扩展 / WebUI / 应用](#六、扩展-webui-应用)
  - [扩展更新 (chrome/browser/extensions/updater/extension_updater_switches.cc) (2)](#扩展更新-chromebrowserextensionsupdaterextension_updater_switchescc)
  - [flags UI (components/webui/flags/flags_ui_switches.cc) (2)](#flags-ui-componentswebuiflagsflags_ui_switchescc)
  - [Extensions (extensions/common/switches.cc) (31)](#extensions-extensionscommonswitchescc)
- [七、隐私 / 安全 / Policy / Sync / Signin](#七、隐私-安全-policy-sync-signin)
  - [Bound Session (chrome/browser/signin/bound_session_credentials/bound_session_switches.cc) (2)](#bound-session-chromebrowsersigninbound_session_credentialsbound_session_switchescc)
  - [WebAuthn (chrome/browser/webauthn/webauthn_switches.cc) (3)](#webauthn-chromebrowserwebauthnwebauthn_switchescc)
  - [自动填充 (components/autofill/core/common/autofill_switches.cc) (7)](#自动填充-componentsautofillcorecommonautofill_switchescc)
  - [Browser Sync (components/browser_sync/browser_sync_switches.cc) (3)](#browser-sync-componentsbrowser_syncbrowser_sync_switchescc)
  - [OS Crypt (components/os_crypt/common/os_crypt_switches.h) (1)](#os-crypt-componentsos_cryptcommonos_crypt_switchesh)
  - [密码管理器 (components/password_manager/core/browser/password_manager_switches.cc) (4)](#密码管理器-componentspassword_managercorebrowserpassword_manager_switchescc)
  - [企业策略 (components/policy/core/common/policy_switches.cc) (7)](#企业策略-componentspolicycorecommonpolicy_switchescc)
  - [Signin (components/signin/public/base/signin_switches.cc) (2)](#signin-componentssigninpublicbasesignin_switchescc)
  - [Sync (components/sync/base/command_line_switches.cc) (7)](#sync-componentssyncbasecommand_line_switchescc)
  - [Trusted Vault (components/trusted_vault/command_line_switches.cc) (2)](#trusted-vault-componentstrusted_vaultcommand_line_switchescc)
- [八、搜索 / 翻译 / NTP / Dom Distiller](#八、搜索-翻译-ntp-dom-distiller)
  - [Chrome Google (chrome/browser/google/switches.cc) (2)](#chrome-google-chromebrowsergoogleswitchescc)
  - [NTP Modules (chrome/browser/new_tab_page/modules/modules_switches.cc) (1)](#ntp-modules-chromebrowsernew_tab_pagemodulesmodules_switchescc)
  - [DOM Distiller (components/dom_distiller/core/dom_distiller_switches.cc) (11)](#dom-distiller-componentsdom_distillercoredom_distiller_switchescc)
  - [Google Base URL (components/google/core/common/google_switches.cc) (2)](#google-base-url-componentsgooglecorecommongoogle_switchescc)
  - [NTP Tiles (components/ntp_tiles/switches.cc) (1)](#ntp-tiles-componentsntp_tilesswitchescc)
  - [Regional (components/regional_capabilities/regional_capabilities_switches.cc) (4)](#regional-componentsregional_capabilitiesregional_capabilities_switchescc)
  - [搜索引擎 (components/search_engines/search_engines_switches.cc) (5)](#搜索引擎-componentssearch_enginessearch_engines_switchescc)
  - [Doodle Logos (components/search_provider_logos/switches.cc) (3)](#doodle-logos-componentssearch_provider_logosswitchescc)
  - [翻译 (components/translate/core/common/translate_switches.cc) (3)](#翻译-componentstranslatecorecommontranslate_switchescc)
- [九、Variations / 优化 / 组件更新 / 反馈](#九、variations-优化-组件更新-反馈)
  - [Client Hints (components/client_hints/common/switches.cc) (1)](#client-hints-componentsclient_hintscommonswitchescc)
  - [组件更新 (components/component_updater/component_updater_switches.cc) (4)](#组件更新-componentscomponent_updatercomponent_updater_switchescc)
  - [Data Sharing (components/data_sharing/public/switches.h) (1)](#data-sharing-componentsdata_sharingpublicswitchesh)
  - [错误页 (components/error_page/common/error_page_switches.cc) (2)](#错误页-componentserror_pagecommonerror_page_switchescc)
  - [反馈 (components/feedback/feedback_switches.cc) (1)](#反馈-componentsfeedbackfeedback_switchescc)
  - [Infobars (components/infobars/core/infobars_switches.h) (1)](#infobars-componentsinfobarscoreinfobars_switchesh)
  - [Optimization Guide (components/optimization_guide/core/optimization_guide_switches.cc) (27)](#optimization-guide-componentsoptimization_guidecoreoptimization_guide_switchescc)
  - [页面内容注解 (components/page_content_annotations/core/page_content_annotations_switches.cc) (6)](#页面内容注解-componentspage_content_annotationscorepage_content_annotations_switchescc)
  - [权限 (components/permissions/switches.cc) (1)](#权限-componentspermissionsswitchescc)
  - [Variations (components/variations/variations_switches.cc) (21)](#variations-componentsvariationsvariations_switchescc)
- [十、设备 / 输入 / 窗口系统 / 无障碍](#十、设备-输入-窗口系统-无障碍)
  - [输入 (components/input/switches.cc) (3)](#输入-componentsinputswitchescc)
  - [UI DevTools (components/ui_devtools/switches.cc) (1)](#ui-devtools-componentsui_devtoolsswitchescc)
  - [Gamepad (device/gamepad/public/cpp/gamepad_switches.cc) (1)](#gamepad-devicegamepadpubliccppgamepad_switchescc)
  - [VR (device/vr/public/cpp/switches.cc) (3)](#vr-devicevrpubliccppswitchescc)
  - [HID (services/device/public/cpp/hid/hid_switches.cc) (1)](#hid-servicesdevicepubliccpphidhid_switchescc)
  - [无障碍 (ui/accessibility/accessibility_switches.cc) (11)](#无障碍-uiaccessibilityaccessibility_switchescc)
  - [UI Events (ui/events/event_switches.cc) (7)](#ui-events-uieventsevent_switchescc)
  - [Ozone (ui/ozone/public/ozone_switches.cc) (9)](#ozone-uiozonepublicozone_switchescc)
  - [Views (ui/views/views_switches.h) (3)](#views-uiviewsviews_switchesh)
  - [Window Manager (ui/wm/core/wm_core_switches.cc) (1)](#window-manager-uiwmcorewm_core_switchescc)
- [十一、Headless / 自动化 / 测试](#十一、headless-自动化-测试)
  - [Chrome Headless Mode (chrome/browser/headless/headless_mode_switches.h) (2)](#chrome-headless-mode-chromebrowserheadlessheadless_mode_switchesh)
  - [Chrome Test (chrome/test/base/test_switches.cc) (3)](#chrome-test-chrometestbasetest_switchescc)
  - [Headless 命令处理 (components/headless/command_handler/headless_command_switches.cc) (9)](#headless-命令处理-componentsheadlesscommand_handlerheadless_command_switchescc)
  - [Components Test (components/test/test_switches.cc) (1)](#components-test-componentstesttest_switchescc)
  - [Content Shell (content/shell/common/shell_switches.h) (9)](#content-shell-contentshellcommonshell_switchesh)
  - [Web Tests (content/web_test/common/web_test_switches.h) (13)](#web-tests-contentweb_testcommonweb_test_switchesh)
  - [Headless Public (headless/public/switches.h) (12)](#headless-public-headlesspublicswitchesh)
- [十二、其它 / 杂项](#十二、其它-杂项)
  - [Actor (chrome/browser/actor/actor_switches.cc) (2)](#actor-chromebrowseractoractor_switchescc)
  - [Device Trust Attestation (attestation_switches.cc) (1)](#device-trust-attestation-attestation_switchescc)
  - [Nearby Share (chrome/browser/nearby_sharing/common/nearby_share_switches.cc) (5)](#nearby-share-chromebrowsernearby_sharingcommonnearby_share_switchescc)
  - [Predictors (chrome/browser/predictors/predictors_switches.cc) (1)](#predictors-chromebrowserpredictorspredictors_switchescc)
  - [Windows 服务 (chrome/windows_services/service_program/switches.h) (3)](#windows-服务-chromewindows_servicesservice_programswitchesh)
  - [Mojo Proxy (mojo/proxy/switches.cc) (5)](#mojo-proxy-mojoproxyswitchescc)
  - [WebNN (services/webnn/webnn_switches.h) (12)](#webnn-serviceswebnnwebnn_switchesh)

## 常用开关速查（中文）

以下是最常用的一组桌面端开关，按场景分组，其余 1000+ 条在下文按模块给出官方注释。

### 启动与基础

| 开关 | 用途 |
| --- | --- |
| `--user-data-dir=<dir>` | 指定用户数据目录，隔离配置/缓存/扩展，调试多实例常用。 |
| `--profile-directory=<name>` | 在 user-data-dir 下启动指定 Profile。 |
| `--no-first-run` | 跳过首次运行向导，自动化常用。 |
| `--no-default-browser-check` | 不弹出“设为默认浏览器”提示。 |
| `--disable-features=<F1,F2>` | 关闭指定 Feature（chrome://flags 里的实验项）。 |
| `--enable-features=<F1,F2>` | 启用指定 Feature，可追加 `<F>:param/value` 形式指定参数。 |
| `--lang=<code>` | 指定 UI 语言（底层走 base_i18n_switches）。 |
| `--log-level=0..3` | 提高日志等级（0=INFO，3=ERROR）。 |
| `--enable-logging=stderr` | 将日志输出到 stderr（配合 `--v=<n>` 打开 VLOG）。 |
| `--v=<n>` | VLOG 详细级别。 |
| `--vmodule=pattern=level,...` | 按文件/模块开启 VLOG。 |

### 渲染 / GPU 调试

| 开关 | 用途 |
| --- | --- |
| `--disable-gpu` | 关闭 GPU 加速，所有内容走软件渲染。 |
| `--disable-gpu-compositing` | 关闭合成 GPU，只保留 2D 软件合成。 |
| `--use-angle=<backend>` | 指定 ANGLE 后端：d3d11 / d3d9 / gl / gles / vulkan / metal 等。 |
| `--use-gl=<backend>` | 指定 GL 后端：desktop / egl / swiftshader 等。 |
| `--enable-gpu-rasterization` | 启用 GPU 光栅化。 |
| `--disable-gpu-vsync` | 解除 GPU 垂直同步，调试帧率上限。 |
| `--show-paint-rects` | 在网页上画出每帧重绘区域。 |
| `--show-layer-animation-bounds` | 高亮动画中图层边界。 |
| `--enable-gpu-benchmarking` | 开启 JS `chrome.gpuBenchmarking` 测试接口。 |
| `--disable-accelerated-video-decode` | 禁用硬件视频解码。 |
| `--disable-accelerated-2d-canvas` | 禁用 2D Canvas GPU 加速。 |

### 网络 / 证书 / 代理

| 开关 | 用途 |
| --- | --- |
| `--proxy-server=<scheme://host:port>` | 指定代理服务器。 |
| `--proxy-bypass-list="<list>"` | 绕过代理的地址列表。 |
| `--host-resolver-rules="MAP * 127.0.0.1"` | 自定义 DNS 解析规则。 |
| `--ignore-certificate-errors` | 忽略所有 SSL/TLS 错误（调试用，有安全风险）。 |
| `--test-type` | 内部测试模式标志，常与上面证书绕过一起使用。 |
| `--disable-quic` | 禁用 QUIC。 |
| `--enable-quic` | 强制启用 QUIC。 |
| `--log-net-log=<file>` | 输出 NetLog JSON 到指定文件。 |
| `--ssl-key-log-file=<file>` | 导出 TLS 密钥（配合 Wireshark 解密 TLS）。 |
| `--disable-background-networking` | 关闭后台网络活动（升级检查、Field Trial 等）。 |

### 站点隔离 / 安全

| 开关 | 用途 |
| --- | --- |
| `--site-per-process` | 强制每个站点独立渲染进程（严格站点隔离）。 |
| `--disable-site-isolation-trials` | 禁用站点隔离 Finch 试验。 |
| `--disable-web-security` | 关闭同源策略（仅调试，切勿用于日常浏览）。 |
| `--allow-insecure-localhost` | localhost 自签 HTTPS 不报错。 |
| `--allow-running-insecure-content` | 允许 HTTPS 页面加载 HTTP 子资源。 |
| `--unsafely-treat-insecure-origin-as-secure=<url>` | 把 http 源当作 secure context（用于本地 PWA 调试）。 |

### 扩展 / 应用

| 开关 | 用途 |
| --- | --- |
| `--disable-extensions` | 禁用所有扩展。 |
| `--load-extension=<path>` | 加载未打包扩展。 |
| `--disable-extensions-except=<paths>` | 保留指定扩展，禁用其他。 |
| `--allowlisted-extension-id=<id>` | 将扩展 ID 加入解锁白名单（访问受限 API）。 |
| `--whitelisted-extension-id=<id>` | 旧名；同上。 |
| `--pack-extension=<path>` | 打包 CRX。 |
| `--pack-extension-key=<pem>` | 指定 CRX 签名私钥。 |

### 沙箱 / 进程

| 开关 | 用途 |
| --- | --- |
| `--no-sandbox` | 完全关闭沙箱（极其不安全，仅用于排错）。 |
| `--disable-gpu-sandbox` | 仅关闭 GPU 沙箱。 |
| `--renderer-startup-dialog` | Renderer 启动时弹对话框，便于附加调试器。 |
| `--gpu-startup-dialog` | GPU 进程启动时弹对话框。 |
| `--wait-for-debugger-children=<proc>` | 指定子进程类型启动时等待调试器。 |
| `--single-process` | 所有组件跑在一个进程（仅调试，崩溃模型不同）。 |

### Headless / 自动化

| 开关 | 用途 |
| --- | --- |
| `--headless[=new|old]` | Headless 模式；`new` 是新版 Headless，默认在 133 之后。 |
| `--remote-debugging-port=<port>` | 开启 DevTools 协议端口，支持 Puppeteer/CDP。 |
| `--remote-allow-origins=<origins>` | 允许指定 origin 访问 DevTools WebSocket。 |
| `--window-size=W,H` | 初始窗口大小。 |
| `--virtual-time-budget=<ms>` | Headless 下加速虚拟时间。 |
| `--screenshot[=path]` | Headless 模式截屏。 |
| `--dump-dom` | Headless 下 dump 最终 DOM。 |
| `--print-to-pdf[=file]` | Headless 下打印成 PDF。 |

### Tracing / Profiling

| 开关 | 用途 |
| --- | --- |
| `--trace-startup=<categories>` | 启动即开始 Tracing，到 trace-startup-duration 秒为止。 |
| `--trace-startup-file=<path>` | Tracing 输出文件。 |
| `--trace-shutdown` | 进程退出时自动写出 Trace。 |
| `--enable-heap-profiling` | 启用进程内堆采样。 |
| `--memlog=<mode>` | 启动内存日志服务。 |

### 变体 (Variations) / Field Trials

| 开关 | 用途 |
| --- | --- |
| `--force-fieldtrials=<name/group/...>` | 强制 Field Trial 分组（测试用）。 |
| `--variations-server-url=<url>` | 覆盖 Finch 种子下载地址。 |
| `--fake-variations-channel=<channel>` | 伪造 Finch 判定的发布通道。 |



## 一、核心 / 启动 / 浏览器主流程

Chrome 浏览器主进程、多进程启动、用户资料（Profile）、打印、WebUI、应用模式（App Mode）等最常用的开关集中在这里。大部分桌面端日常调试和功能开关都在 `chrome_switches` 和 `content_switches`。


### Apps (apps/switches.cc)

Chrome Apps（打包应用）相关开关。

_开关数：1_


#### `--load-and-launch-app`

- **符号**: `switches::kLoadAndLaunchApp` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：从指定目录加载一个未打包 App 并启动它。
> Loads an app from the specified directory and launches it.


### Base 基础库 (base/base_switches.cc)

最底层的通用开关：Feature 开关、Field Trial、崩溃报告、低端设备模式、共享内存句柄、Profiling 等。

_开关数：28_


#### `--background-thread-pool-field-trial`

- **符号**: `switches::kBackgroundThreadPoolFieldTrial` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：配置后台 ThreadPool 线程池的 Field Trial（分组实验）。
> Configure the background threadpool field trial.

#### `--disable-best-effort-tasks`

- **符号**: `switches::kDisableBestEffortTasks` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把所有 BEST_EFFORT 优先级的任务推迟到进程关闭前才执行（调试后台任务用）。
> Delays execution of TaskPriority::BEST_EFFORT tasks until shutdown.

#### `--disable-breakpad`

- **符号**: `switches::kDisableBreakpad` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：完全关闭 Breakpad 崩溃上报。
> Disables the crash reporting.

#### `--disable-dev-shm-usage`

- **符号**: `switches::kDisableDevShmUsage` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX)
- **用途**：不使用 /dev/shm（Linux/VM 下若该分区太小会导致 Chrome 崩溃，此开关改用临时目录）。
> The /dev/shm partition is too small in certain VM environments, causing Chrome to fail or crash (see http://crbug.com/715363). Use this flag to work-around this issue (a temporary directory will always be used to create anonymous shared memory files).

#### `--disable-features`

- **符号**: `switches::kDisableFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按逗号分隔名单关闭 Feature；与 --enable-features 搭配。
> Comma-separated list of feature names to disable. See also kEnableFeatures.

#### `--disable-highres-timer`

- **符号**: `switches::kDisableHighResTimer` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows）关闭高精度计时器。
> Disable high-resolution timer on Windows.

#### `--disable-low-end-device-mode`

- **符号**: `switches::kDisableLowEndDeviceMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制关闭低端设备模式（IsLowEndDeviceMode 永为 false）。
> Force disabling of low-end device mode when set.

#### `--disable-usb-keyboard-detect`

- **符号**: `switches::kDisableUsbKeyboardDetect` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows）关闭 USB 键盘检测（触控屏弹屏幕键盘时会用）。
> Disables the USB keyboard detection for blocking the OSK on Windows.

#### `--enable-crash-reporter`

- **符号**: `switches::kEnableCrashReporter` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：启用崩溃上报；子进程在无法自决时由父进程内部注入。
> Indicates that crash reporting should be enabled. On platforms where helper processes cannot access to files needed to make this decision, this flag is generated internally.

#### `--enable-crash-reporter-for-testing`

- **符号**: `switches::kEnableCrashReporterForTesting` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_POSIX)
- **用途**：（POSIX）在调试版里强制打开 Breakpad 上报，仅测试用。
> Used for turning on Breakpad crash reporting in a debug environment where crash reporting is typically compiled but disabled.

#### `--enable-features`

- **符号**: `switches::kEnableFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按逗号分隔名单启用 Feature，可用 `<Feature>:<param>/<value>/...` 形式带参数。
> Comma-separated list of feature names to enable. See also kDisableFeatures.

#### `--enable-low-end-device-mode`

- **符号**: `switches::kEnableLowEndDeviceMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制开启低端设备模式。
> Force low-end device mode when set.

#### `--field-trial-handle`

- **符号**: `switches::kFieldTrialHandle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：内部用：把携带 Field Trial 状态的共享内存句柄传给子进程（用户一般不用直接指定）。
> Handle to the shared memory segment containing field trial state that is to be shared between processes. The argument to this switch is made of segments separated by commas: - The platform-specific handle id for the shared memory as a string. - (Windows only) i=inherited by duplication or p=child must open parent. - The high 64 bits of the shared memory block GUID. - The low 64 bits of the shared memory block GUID. - The size of the shared memory segment as a string.

#### `--force-fieldtrials`

- **符号**: `switches::kForceFieldTrials` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制指定 Field Trial 分组，格式 `Name/Group/...`，分组名前加 `*` 立即激活。
> This option can be used to force field trials when testing changes locally. The argument is a list of name and value pairs, separated by slashes. If a trial name is prefixed with an asterisk, that trial will start activated. For example, the following argument defines two trials, with the second one activated: "GoogleNow/Enable/*MaterialDesignNTP/Default/" This option can also be used by the browser process to send the list of trials to a non-browser process, using the same format.
> See FieldTrialList::CreateTrialsFromString() in field_trial.h for details.

#### `--force-high-res-timeticks`

- **符号**: `switches::kForceHighResTimeTicks` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：即便 CPU 没报告不变 TSC 也强制用 QPC 作为 TimeTicks 源。
> Forces the use of QPC for TimeTicks even if cpuid doesn't report the presence of an invariant TSC.

#### `--full-memory-crash-report`

- **符号**: `switches::kFullMemoryCrashReport` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：生成包含整个进程内存的大型崩溃 dump。
> Generates full memory crash dump.

#### `--log-best-effort-tasks`

- **符号**: `switches::kLogBestEffortTasks` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把每个 BEST_EFFORT 任务的投递记入日志，用于诊断调度被执行栅栏阻塞的问题。
> Logs information about all tasks posted with TaskPriority::BEST_EFFORT. Use this to diagnose issues that are thought to be caused by TaskPriority::BEST_EFFORT execution fences. Note: Tasks posted to a non-BEST_EFFORT UpdateableSequencedTaskRunner whose priority is later lowered to BEST_EFFORT are not logged.

#### `--metrics-shmem-handle`

- **符号**: `switches::kMetricsSharedMemoryHandle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：内部用：把直方图（UMA）共享内存句柄从浏览器进程传给子进程。
> Handle to the shared memory segment a child process should use to transmit histograms back to the browser process.

#### `--noerrdialogs`

- **符号**: `switches::kNoErrorDialogs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：屏蔽所有错误对话框。
> Suppresses all error dialogs when present.

#### `--profiling-at-start`

- **符号**: `switches::kProfilingAtStart` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：浏览器进程启动时就启动采样 profiler（需 gn `enable_profiling=true`）。
> Starts the sampling based profiler for the browser process at startup. This will only work if chrome has been built with the gn arg enable_profiling = true. The output will go to the value of kProfilingFile.

#### `--profiling-file`

- **符号**: `switches::kProfilingFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：采样 profiler 的输出文件路径，可用 `{pid}`/`{count}` 占位符。
> Specifies a location for profiling output. This will only work if chrome has been built with the gyp variable profiling=1 or gn arg enable_profiling=true. {pid} if present will be replaced by the pid of the process. {count} if present will be incremented each time a profile is generated for this process. The default is chrome-profile-{pid} for the browser and test-profile-{pid} for tests.

#### `--profiling-flush`

- **符号**: `switches::kProfilingFlush` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：周期性地将 profile 数据 flush 到文件（值为秒数），防止异常退出丢数据。
> Controls whether profile data is periodically flushed to a file. Normally the data gets written on exit but cases exist where chromium doesn't exit cleanly (especially when using single-process). A time in seconds can be specified.

#### `--test-child-process`

- **符号**: `switches::kTestChildProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：提示测试框架：当前进程是由测试 spawn 出来的子进程。
> When running certain tests that spawn child processes, this switch indicates to the test framework that the current process is a child process.

#### `--trace-to-file`

- **符号**: `switches::kTraceToFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把指定类别（或默认）的 trace 事件直接写到文件。
> Sends trace events from these categories to a file. --trace-to-file on its own sends to default categories.

#### `--trace-to-file-name`

- **符号**: `switches::kTraceToFileName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：配合 --trace-to-file 指定输出文件名。
> Specifies the file name for --trace-to-file. If unspecified, it will go to a default file name.

#### `--v`

- **符号**: `switches::kV` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：设置 VLOG 的最大活动级别（默认 0，正数越大越详细）。
> Gives the default maximal active V-logging level; 0 is the default. Normally positive values are used for V-logging levels.

#### `--vmodule`

- **符号**: `switches::kVModule` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按模块/路径设置 VLOG 级别，如 `my_module=2,foo*=3` 或 `*/foo/bar/*=2`。
> Gives the per-module maximal V-logging levels to override the value given by --v.  E.g. "my_module=2,foo*=3" would change the logging level for all code in source files "my_module.*" and "foo*.*" ("-inl" suffixes are also disregarded for this matching). Any pattern containing a forward or backward slash will be tested against the whole pathname and not just the module.  E.g., "*/foo/bar/*=2" would change the logging level for all code in source files under a "foo/bar" directory.

#### `--wait-for-debugger`

- **符号**: `switches::kWaitForDebugger` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：进程启动后最多等 60 秒给调试器附加。
> Will wait for 60 seconds for a debugger to come to attach to the process.


### Base i18n (base/i18n/base_i18n_switches.cc)

国际化/区域 (locale) 覆盖相关开关。

_开关数：4_


#### `--ltr`

- **符号**: `switches::kForceDirectionLTR` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--force-text-direction / --force-ui-direction 取值：从左到右。

#### `--rtl`

- **符号**: `switches::kForceDirectionRTL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--force-text-direction / --force-ui-direction 取值：从右到左。

#### `--force-text-direction`

- **符号**: `switches::kForceTextDirection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制文本渲染方向，取值 `ltr`/`rtl`（主要用于 RTL 测试）。
> Force the text rendering to a specific direction. Valid values are "ltr" (left-to-right) and "rtl" (right-to-left). Only tested meaningfully with RTL.

#### `--force-ui-direction`

- **符号**: `switches::kForceUIDirection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制 UI 方向，取值 `ltr`/`rtl`。
> Force the UI to a specific direction. Valid values are "ltr" (left-to-right) and "rtl" (right-to-left).


### Chrome 浏览器 (chrome/common/chrome_switches.cc)

Chrome 桌面浏览器主进程和产品逻辑使用最多的开关集合：应用模式、Profile、启动 URL、打印、诊断、WebUI、安装/升级等。

_开关数：215_


#### `--accept-lang`

- **符号**: `switches::kAcceptLang` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：指定发给服务器的 Accept-Language 并暴露给 JS 的 `navigator.language`，格式 `lang[-country]`。
> Can't find the switch you are looking for? Try looking in: ash/constants/ash_switches.cc base/base_switches.cc etc. When commenting your switch, please use the same voice as surrounding comments. Imagine "This switch..." at the beginning of the phrase, and it'll all work out. Specifies Accept-Language to send to servers and expose to JavaScript via the navigator.language DOM property. language[-country] where language is the 2 letter code from ISO-639.

#### `--allow-appshim-signature-mismatch-for-tests`

- **符号**: `switches::kAllowAppShimSignatureMismatchForTests` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：（Mac 未签名构建）允许签名不匹配的 app shim 连到 Chrome，仅测试用。
> Only if we're running in an unsigned build, passing this flag will allow app shims whose code signature does not match what chrome is expecting to still connect to chrome. This is used by some tests to allow the test to pretend to be a valid app shim.

#### `--allow-cross-origin-auth-prompt`

- **符号**: `switches::kAllowCrossOriginAuthPrompt` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许第三方内容弹出 HTTP Basic Auth 用户名/密码框。
> Allows third-party content included on a page to prompt for a HTTP basic auth username/password pair.

#### `--allow-http-screen-capture`

- **符号**: `switches::kAllowHttpScreenCapture` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许非安全源（HTTP）使用屏幕捕获 / desktopCapture API。
> Allow non-secure origins to use the screen capture API and the desktopCapture extension API.

#### `--allow-running-insecure-content`

- **符号**: `switches::kAllowRunningInsecureContent` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许 HTTPS 页面加载 HTTP 的 JS/CSS/插件等混合内容。
> By default, an https page cannot run JavaScript, CSS or plugins from http URLs. This provides an override to get the old insecure behavior.

#### `--allow-silent-push`

- **符号**: `switches::kAllowSilentPush` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许 Web Push 不弹出通知的静默推送。
> Allows Web Push notifications that do not show a notification.

#### `--app`

- **符号**: `switches::kApp` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：以 App（应用）模式启动，值为目标 URL。
> Specifies that the associated value should be launched in "application" mode.

#### `--app-id`

- **符号**: `switches::kAppId` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按 ID 启动扩展应用。
> Specifies that the extension-app with the specified id should be launched according to its configuration.

#### `--app-launch-url-for-shortcuts-menu-item`

- **符号**: `switches::kAppLaunchUrlForShortcutsMenuItem` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：与 --app-id 搭配，指定 App 的 shortcuts 菜单项 URL。
> Overrides the launch url of an app with the specified url. This is used along with kAppId to launch a given app with the url corresponding to an item in the app's shortcuts menu.

#### `--app-mode-auth-code`

- **符号**: `switches::kAppModeAuthCode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：配合 --force-app-mode 使用的 GAIA auth code。
> Value of GAIA auth code for --force-app-mode.

#### `--app-mode-oauth-token`

- **符号**: `switches::kAppModeOAuth2Token` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：配合 --force-app-mode 使用的 OAuth2 refresh token。
> Value of OAuth2 refresh token for --force-app-mode.

#### `--app-run-on-os-login-mode`

- **符号**: `switches::kAppRunOnOsLoginMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：配合 --app-id，标记 App 是随 OS 登录启动的，以及以何种模式启动。
> This is used along with kAppId to indicate an app was launched during OS login, and which mode the app was launched in.

#### `--apps-gallery-download-url`

- **符号**: `switches::kAppsGalleryDownloadURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 webstore 下载扩展的 URL（需包含一个 `%s` 占位扩展 ID）。
> Overrides the URL that the webstore APIs download extensions from. Note: the URL must contain one '%s' for the extension ID.

#### `--apps-gallery-url`

- **符号**: `switches::kAppsGalleryURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 webstore URL 前缀，被视作 webstore 以获得专属 API/保护。
> Overrides the url that the browser treats as the webstore, granting it the webstore APIs and giving it some special protections.

#### `--apps-gallery-update-url`

- **符号**: `switches::kAppsGalleryUpdateURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 webstore 扩展的更新 URL。
> Overrides the update url used by webstore extensions.

#### `--apps-keep-chrome-alive-in-tests`

- **符号**: `switches::kAppsKeepChromeAliveInTests` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：有 Chrome Apps 打开时 Chrome 不退出（测试用）。
> Prevents Chrome from quitting when Chrome Apps are open.

#### `--auth-server-allowlist`

- **符号**: `switches::kAuthServerAllowlist` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：Negotiate（Kerberos/NTLM）认证服务器白名单。
> Allowlist for Negotiate Auth servers

#### `--auto-open-devtools-for-tabs`

- **符号**: `switches::kAutoOpenDevToolsForTabs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：每个新标签页自动打开 DevTools（自动化/调试用）。
> This flag makes Chrome auto-open DevTools window for each tab. It is intended to be used by developers and automation to not require user interaction for opening DevTools.

#### `--auto-select-desktop-capture-source`

- **符号**: `switches::kAutoSelectDesktopCaptureSource` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展请求桌面捕获时按名字子串自动选源（测试用），例如 `--auto-select-desktop-capture-source="Entire screen"`。
> This flag makes Chrome auto-select the provided choice when an extension asks permission to start desktop capture. Should only be used for tests. For instance, --auto-select-desktop-capture-source="Entire screen" will automatically select sharing the entire screen in English locales. The switch value only needs to be substring of the capture source name, i.e. "display" would match "Built-in display" and "External display", whichever comes first.

#### `--auto-select-screen-capture-source`

- **符号**: `switches::kAutoSelectScreenCaptureSource` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展请求桌面捕获时自动随意选一个屏幕（测试用，无需知道屏幕名）。
> This flag makes Chrome auto-select any screen when an extension asks permission to start desktop capture. Should only be used for tests. kAutoSelectDesktopCaptureSource (see above) can be also be used to auto-select screens. But it have the problem that you need to know the name of a screen to auto-select it. The name of screens can't be set, are different for different platforms, and are different if you have one or several screens. So it's hard to use for auto-selecting screens.
> This flag does not care what the screen name is, but it also gives no control. Any screen could be chosen. It is useful in tests where we don't care which screen is auto-selected.

#### `--auto-select-tab-capture-source-by-title`

- **符号**: `switches::kAutoSelectTabCaptureSourceByTitle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自动选择指定标题的标签页作为捕获源（测试用）。
> This flag makes Chrome auto-select a tab with the provided title when the media-picker should otherwise be displayed to the user. This switch is very similar to kAutoSelectDesktopCaptureSource, but limits selection to tabs. This solves the issue of kAutoSelectDesktopCaptureSource being liable to accidentally capturing the Chromium window instead of the tab, as both have the same title if the tab is focused.

#### `--auto-select-window-capture-source-by-title`

- **符号**: `switches::kAutoSelectWindowCaptureSourceByTitle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自动选择指定标题的窗口作为捕获源（测试用）。
> This flag makes Chrome auto-select a window with the provided title when the media-picker should otherwise be displayed to the user. This switch is very similar to kAutoSelectDesktopCaptureSource, but limits selection to the window.

#### `--auto-accept-browser-signin-for-tests`

- **符号**: `switches::kBrowserSigninAutoAccept` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用户登录 Google 网站时自动同步登录到 Chrome（自动化用）。
> Automatically signs the user into Chrome when signing in to other Google services on the web. This makes it easier for automated browsers to sign in.

#### `--bypass-account-already-used-by-another-profile-check`

- **符号**: `switches::kBypassAccountAlreadyUsedByAnotherProfileCheck` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许多 Profile 同步到同一账号（多客户端 E2E 测试用）。
> If specified, allows syncing multiple profiles to the same account. Used for multi-client E2E tests.

#### `--auto-reject-capture`

- **符号**: `switches::kCaptureAutoReject` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自动拒绝标签页/窗口/屏幕捕获请求（测试用）。
> This flag makes Chrome auto-reject requests capture a tab/window/screen.

#### `--check-for-update-interval`

- **符号**: `switches::kCheckForUpdateIntervalSec` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：检查更新的间隔秒数（仅测试用）。
> How often (in seconds) to check for updates. Should only be used for testing purposes.

#### `--cipher-suite-blacklist`

- **符号**: `switches::kCipherSuiteBlacklist` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：逗号分隔的 SSL Cipher Suite 黑名单。
> Comma-separated list of SSL cipher suites to disable.

#### `--code-sign-clone-cleanup`

- **符号**: `switches::kCodeSignCloneCleanupProcess` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **同名定义**: `sandbox/policy/switches.cc`
- **用途**：子进程类型：负责清理浏览器代码签名临时副本。
> A process type (switches::kProcessType) that cleans up the browser's temporary code sign clone.

#### `--crash-on-hang-threads`

- **符号**: `switches::kCrashOnHangThreads` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定若 UI/IO 线程在 N 秒内无响应就崩溃，如 `UI:18,IO:18`。
> Comma-separated list of BrowserThreads that cause browser process to crash if the given browser thread is not responsive. UI/IO are the BrowserThreads that are supported. For example: --crash-on-hang-threads=UI:18,IO:18 --> Crash the browser if UI or IO is not responsive for 18 seconds while the other browser thread is responsive.

#### `--create-browser-on-startup-for-tests`

- **符号**: `switches::kCreateBrowserOnStartupForTests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动时至少创建一个 browser 实例（面向默认空桌面的平台的测试用）。
> Some platforms like ChromeOS default to empty desktop. Browser tests may need to add this switch so that at least one browser instance is created on startup. TODO(nkostylev): Investigate if this switch could be removed. (http://crbug.com/148675)

#### `--create-profile-email-if-not-exists`

- **符号**: `switches::kCreateProfileEmailIfNotExists` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：配合 --profile-email：如果现有 Profile 里都没有该邮箱，则新建 Profile。
> If provided with kProfileEmail, prompts the user to create a new profile with kProfileEmail as the email address if that email is not found in any existing profile.

#### `--credits`

- **符号**: `switches::kCredits` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打印 about:credits 许可证信息后退出。
> Prints licensing information (same content as found in about:credits) and quits.

#### `--custom-devtools-frontend`

- **符号**: `switches::kCustomDevtoolsFrontend` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：DevTools 前端 URL 覆盖，可用 http:// 或 file:// 指定自定义前端来源。
> Specifies the http:// endpoint which will be used to serve devtools://devtools/custom/<path> Or a file:// URL to specify a custom file path to load from for devtools://devtools/bundled/<path>

#### `--debug-packed-apps`

- **符号**: `switches::kDebugPackedApps` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在打包 App 的右键菜单里加入调试项（如检查元素）。
> Adds debugging entries such as Inspect Element to context menus of packed apps.

#### `--debug-print`

- **符号**: `switches::kDebugPrint` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_PRINT_PREVIEW) && !defined(OFFICIAL_BUILD)
- **用途**：启用打印子系统的调试日志/功能。
> Enables support to debug printing subsystem.

#### `--devtools-flags`

- **符号**: `switches::kDevToolsFlags` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把额外参数传给 DevTools 前端。
> Passes command line parameters to the DevTools front-end.

#### `--diagnostics`

- **符号**: `switches::kDiagnostics` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：触发一组诊断模式。
> Triggers a plethora of diagnostic modes.

#### `--diagnostics-format`

- **符号**: `switches::kDiagnosticsFormat` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定 --diagnostics 输出格式。
> Sets the output format for diagnostic modes enabled by diagnostics flag.

#### `--diagnostics-recovery`

- **符号**: `switches::kDiagnosticsRecovery` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：要求诊断模式执行对应的修复步骤。
> Tells the diagnostics mode to do the requested recovery step(s).

#### `--disable-auto-reload`

- **符号**: `switches::kDisableAutoReload` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：出错页面不自动重载。
> Disable auto-reload of pages on top-level error.

#### `--disable-background-networking`

- **符号**: `switches::kDisableBackgroundNetworking` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭后台网络活动（升级检查、组件更新、Field Trial 拉取等），常用于网络性能测试。
> Disable several subsystems which run network requests in the background. This is for use when doing network performance testing to avoid noise in the measurements.

#### `--disable-component-extensions-with-background-pages`

- **符号**: `switches::kDisableComponentExtensionsWithBackgroundPages` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用带后台页的默认组件扩展，避免干扰性能测试。
> Disable default component extensions with background pages - useful for performance tests where these pages may interfere with perf results.

#### `--disable-component-update`

- **符号**: `switches::kDisableComponentUpdate` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭组件更新器（不再下发/更新各组件）。

#### `--disable-crashpad-for-testing`

- **符号**: `switches::kDisableCrashpadForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不初始化 Crashpad，不再捕获和符号化崩溃（测试用）。
> Disables crashpad initialization for testing. The crashpad binary will not run, and thus will not detect and symbolize crashes.

#### `--disable-default-apps`

- **符号**: `switches::kDisableDefaultApps` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：首次运行不安装默认 App（自动化用）。
> Disables installation of default apps on first run. This is used during automated testing.

#### `--disable-domain-reliability`

- **符号**: `switches::kDisableDomainReliability` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 Domain Reliability 监测。
> Disables Domain Reliability Monitoring.

#### `--disable-lazy-loading`

- **符号**: `switches::kDisableLazyLoading` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：禁用图片和 iframe 的懒加载。
> Disables lazy loading of images and frames.

#### `--disable-print-preview`

- **符号**: `switches::kDisablePrintPreview` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用打印预览。
> Disables print preview (For testing, and for users who don't like us. :[ )

#### `--disable-prompt-on-repost`

- **符号**: `switches::kDisablePromptOnRepost` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：POST 导航后刷新/回退不再弹“是否重新提交”确认框（测试用）。
> Normally when the user attempts to navigate to a page that was the result of a post we prompt to make sure they want to. This switch may be used to disable that check. This switch is used during automated testing.

#### `--disable-stack-profiler`

- **符号**: `switches::kDisableStackProfiler` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭栈采样 profiler（做性能对比基线用）。
> Disable stack profiling. Stack profiling may change performance. Disabling stack profiling is beneficial when comparing performance metrics with a build that has it disabled by default.

#### `--disable-zero-browsers-open-for-tests`

- **符号**: `switches::kDisableZeroBrowsersOpenForTests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：最后一个浏览器窗口关闭时也不退出进程（ChromeOS Aura 测试用）。
> Some tests seem to require the application to close when the last browser window is closed. Thus, we need a switch to force this behavior for ChromeOS Aura, disable "zero window mode". TODO(pkotwicz): Investigate if this bug can be removed. (http://crbug.com/119175)

#### `--disk-cache-dir`

- **符号**: `switches::kDiskCacheDir` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：自定义磁盘缓存目录（默认基于 user-data-dir）。
> Use a specific disk cache location, rather than one derived from the UserDatadir.

#### `--disk-cache-size`

- **符号**: `switches::kDiskCacheSize` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：磁盘缓存最大字节数。
> Forces the maximum disk space to be used by the disk cache, in bytes.

#### `--do-not-create-nsapp-for-tests`

- **符号**: `switches::kDoNotCreateNSAppForTests` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：（Mac 测试）ChromeTestSuite 中不初始化共享 NSApplication 实例。
> Skips initializing the shares NSApplication instance in ChromeTestSuite.

#### `--do-not-de-elevate`

- **符号**: `switches::kDoNotDeElevateOnLaunch` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动时不做提权降权（已降权后防止循环用）。
> Do not de-elevate the browser on launch. Used after de-elevating to prevent infinite loops.

#### `--dump-browser-histograms`

- **符号**: `switches::kDumpBrowserHistograms` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：请求正在运行的浏览器把直方图写到指定文件（若已存在则覆盖）。
> Requests that a running browser process dump its collected histograms to a given file. The file is overwritten if it exists.

#### `--enable-audio-debug-recordings-from-extension`

- **符号**: `switches::kEnableAudioDebugRecordingsFromExtension` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：若 WebRTC logging private API 正在激活，则开启音频调试录制。
> If the WebRTC logging private API is active, enables audio debug recordings.

#### `--enable-auto-reload`

- **符号**: `switches::kEnableAutoReload` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：出错页面自动重载。
> Enable auto-reload of pages on top-level error.

#### `--enable-bookmark-undo`

- **符号**: `switches::kEnableBookmarkUndo` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用书签多级撤销。
> Enables the multi-level undo system for bookmarks.

#### `--enable-cloud-print-proxy`

- **符号**: `switches::kEnableCloudPrintProxy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（service 进程下）启用 Cloud Print Proxy 组件。
> This applies only when the process type is "service". Enables the Cloud Print Proxy component within the service process.

#### `--devtools-greendev-ui`

- **符号**: `switches::kEnableDevToolsGreenDevUi` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 DevTools 实验版 GreenDev UI。
> Enables the experimental GreenDev UI in DevTools.

#### `--enable-domain-reliability`

- **符号**: `switches::kEnableDomainReliability` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Domain Reliability 监测。
> Enables Domain Reliability Monitoring.

#### `--enable-extension-activity-log-testing`

- **符号**: `switches::kEnableExtensionActivityLogTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用扩展活动日志的测试模式。

#### `--enable-extension-activity-logging`

- **符号**: `switches::kEnableExtensionActivityLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用扩展活动日志。
> Enables logging for extension activity.

#### `--enable-hangout-services-extension-for-testing`

- **符号**: `switches::kEnableHangoutServicesExtensionForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制启用 HangoutServicesExtension（测试用）。
> Force enabling HangoutServicesExtension.

#### `--enable-net-benchmarking`

- **符号**: `switches::kEnableNetBenchmarking` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 network-benchmarking 扩展 API。
> Enables the network-related benchmarking extensions.

#### `--enable-new-app-menu-icon`

- **符号**: `switches::kEnableNewAppMenuIcon` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS) && BUILDFLAG(IS_MAC) && \
- **用途**：启用新版应用菜单图标。

#### `--enable-potentially-annoying-security-features`

- **符号**: `switches::kEnablePotentiallyAnnoyingSecurityFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用一组较激进的安全特性（严格混合内容、强力特性受限等）。
> Enables a number of potentially annoying security features (strict mixed content mode, powerful feature restrictions, etc.)

#### `--enable-profile-shortcut-manager`

- **符号**: `switches::kEnableProfileShortcutManager` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：强制启用 Profile 快捷方式管理器（用 --user-data-dir 的测试需要）。
> Force-enables the profile shortcut manager. This is needed for tests since they use a custom-user-data-dir which disables this.

#### `--enable-unsafe-extension-debugging`

- **符号**: `switches::kEnableUnsafeExtensionDebugging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过 CDP（--remote-debugging-pipe）动态安装/卸载扩展（不安全，仅调试）。
> Enables installing/uninstalling extensions at runtime via Chrome DevTools Protocol if the protocol client is connected over --remote-debugging-pipe.

#### `--enable-user-metrics`

- **符号**: `switches::kEnableUserMetrics` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：在安装器中启用用户指标上报。
> Enable user metrics from within the installer.

#### `--experimental-ai-stable-channel`

- **符号**: `switches::kExperimentalAiStableChannel` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在稳定版里允许实验性 AI 扩展 API；设置后会禁用 Chrome 登录。
> Allows experimental ai extension APIs to be used in stable channel. This disables chrome sign-in if set, regardless of channel.

#### `--explicitly-allowed-ports`

- **符号**: `switches::kExplicitlyAllowedPorts` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：覆盖受限端口列表，按逗号分隔的端口号。
> Allows overriding the list of restricted ports by passing a comma-separated list of port numbers.

#### `--enable-extension-ai-data-collection`

- **符号**: `switches::kExtensionAiDataCollection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许扩展使用 AI 数据采集 API。
> Name of the command line flag to allow the ai data collection extension API.

#### `--extension-content-verification`

- **符号**: `switches::kExtensionContentVerification` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制开启扩展内容校验，取值见下面几个（bootstrap/enforce/enforce_strict）。
> Name of the command line flag to force content verification to be on in one of various modes.

#### `--bootstrap`

- **符号**: `switches::kExtensionContentVerificationBootstrap` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--extension-content-verification 取值：bootstrap（首次下发校验资源）。
> Values for the kExtensionContentVerification flag. See ContentVerifierDelegate::Mode for more explanation.

#### `--enforce`

- **符号**: `switches::kExtensionContentVerificationEnforce` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--extension-content-verification 取值：enforce（校验但不强制）。

#### `--enforce_strict`

- **符号**: `switches::kExtensionContentVerificationEnforceStrict` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--extension-content-verification 取值：enforce_strict（严格强制校验）。

#### `--enable-extension-actor-api`

- **符号**: `switches::kExtensionExperimentalActor` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许扩展使用实验性 Actor API。
> Name of the command line flag to allow the experimental actor API.

#### `--extensions-toolbar-zero-state-explore-extensions-by-category`

- **符号**: `switches::kExtensionsToolbarZeroStateExploreExtensionsByCategory` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展工具栏零态推荐的一个变体：按类别推荐扩展。
> This variation of the Zero State extensions toolbar recommendation suggests extension categories the user can explore in the Chrome Web Store. (e.g. find coupons, increase productivity)

#### `--extensions-toolbar-zero-state-single-web-store-link`

- **符号**: `switches::kExtensionsToolbarZeroStateSingleWebStoreLink` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展工具栏零态推荐的一个变体：给出一个指向 Chrome 网上应用店首页的链接。
> This variation of the Zero State extensions toolbar recommendation presents the user with a single link to the Chrome Web Store home page.

#### `--extensions-toolbar-zero-state-variation`

- **符号**: `switches::kExtensionsToolbarZeroStateVariation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定扩展工具栏零态推荐的变体。
> Specifies the variation of Zero State extensions toolbar recommendation to show. When a user with zero extensions installed clicks on the extensions puzzle piece in the Chrome toolbar, Chrome displays a submenu suggesting the user to explore the Chrome Web Store.

#### `--focus`

- **符号**: `switches::kFocus` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按 URL 或 app id 激活已有 Tab/App 窗口（格式为逗号分隔选择器，URL 末尾 `*` 表前缀匹配，`app:<id>` 选 PWA）。
> Activates an existing tab or app window by URL or app id before creating anything new. Syntax: comma-ordered selectors. Bare URLs are exact. Add a trailing * for prefix. app:<app-id> targets PWAs. Example: --focus=https://meet.google.com/*,app:abc123

#### `--focus-result-file`

- **符号**: `switches::kFocusResultFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 --focus 的结果以 JSON 形式写入文件。
> Specifies a file path to write JSON focus result information.

#### `--force-app-mode`

- **符号**: `switches::kForceAppMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制以 App 模式启动，隐藏部分 UI，若未安装会强制安装。
> Forces application mode. This hides certain system UI elements and forces the app to be installed if it hasn't been already.

#### `--force-first-run`

- **符号**: `switches::kForceFirstRun` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制走首次运行体验，忽略 --no-first-run。
> Displays the First Run experience when the browser is started, regardless of whether or not it's actually the First Run (this overrides kNoFirstRun).

#### `--force-ntp-mobile-promo`

- **符号**: `switches::kForceNtpMobilePromo` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS) && BUILDFLAG(IS_MAC) && \
- **用途**：强制显示 NTP 手机推广（忽略前置条件）。
> Forces the NTP mobile promo to appear without any preconditions.

#### `--force-whats-new`

- **符号**: `switches::kForceWhatsNew` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：若当前里程碑尚未显示过 What's New 则强制显示（覆盖 --no-first-run）。
> Displays the What's New experience when the browser is started if it has not yet been shown for the current milestone (this overrides kNoFirstRun, without showing the First Run experience).

#### `--from-browser-switcher`

- **符号**: `switches::kFromBrowserSwitcher` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：标记：本次启动来自 Legacy Browser Support for Edge 扩展的 native host；仅用于 UMA。
> Indicates that this launch of the browser originated from the Legacy Browser Support for Edge extension's native host. This is recorded in UMA.

#### `--from-installer`

- **符号**: `switches::kFromInstaller` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：标记：本次启动来自安装器（新装或覆盖安装），触发欢迎/无障碍公告等行为。
> Indicates that this launch of the browser originated from the installer (i.e., following a successful new install or over-install). This triggers browser behaviors for this specific launch, such as a welcome announcement for accessibility software (see https://crbug.com/1072735).

#### `--glic-admin-redirect-patterns`

- **符号**: `switches::kGlicAdminRedirectPatterns` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：glic WebView 里要重定向到管理员屏蔽面板的 URL 模式列表（空格分隔）。
> List of URL patterns in the glic webview to redirect to an admin blocked panel, as a space-separated list.

#### `--glic-webui-allowed-origins`

- **符号**: `switches::kGlicAllowedOrigins` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：glic WebView 允许的 origin 列表（空格分隔）。
> List of allowed origins in the glic webview, as a space-separated list.

#### `--glic-always-open-fre`

- **符号**: `switches::kGlicAlwaysOpenFre` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：glic 总是打开 FRE（首次运行体验）。

#### `--glic-always-show-web-actuation-toggle`

- **符号**: `switches::kGlicAlwaysShowWebActuationToggle` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：在 AI 设置页里总是显示 Web actuation 开关。
> Whether to show web actuation toggle in the Chrome AI settings page.

#### `--glic-automation`

- **符号**: `switches::kGlicAutomation` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：glic 自动化模式，配合 --glic-dev 进一步禁用部分功能以便基础测试。
> Automation is intended to be passed in addition to glic-dev. It further disables functionality to make basic testing easier.

#### `--glic-dev`

- **符号**: `switches::kGlicDev` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：glic 的开发者模式，仅通过命令行暴露。
> Dev mode for glic only exposed via command line flag.

#### `--glic-force-g1-for-mi`

- **符号**: `switches::kGlicForceG1StatusForMultiInstance` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：覆盖 AI 订阅等级为 G1（多实例启用测试用）。
> Override actual AI subscription tier by forcing G1 status, specifically for multi-instance enablement. Intended for manual testing only.

#### `--glic-fre-url`

- **符号**: `switches::kGlicFreURL` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：覆盖 glic FRE 的 URL。

#### `--glic-guest-url`

- **符号**: `switches::kGlicGuestURL` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：覆盖 glic guest URL。
> Overrides the glic guest URL.

#### `--glic-host-logging`

- **符号**: `switches::kGlicHostLogging` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：启用 glic api host 的额外日志。
> Whether additional logging is enabled in the glic api host.

#### `--glic-open-on-startup`

- **符号**: `switches::kGlicOpenOnStartup` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：启动时打开 glic，值可为 `attached` 或 `detached`。
> Use --glic-open-on-startup=attached or --glic-open-on-startup=detached.

#### `--glic-reset-mi-enabled-by-tier`

- **符号**: `switches::kGlicResetMultiInstanceEnabledByTier` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：把 kGlicMultiInstanceEnabledBySubscriptionTier 本地偏好复位为 false（手动测试用）。
> Reset local state pref kGlicMultiInstanceEnabledBySubscriptionTier to false. Intended for manual testing only.

#### `--glic-shortcuts-learn-more-url`

- **符号**: `switches::kGlicShortcutsLearnMoreURL` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：glic 快捷方式“了解更多”链接。

#### `--glic-skip-reload-after-navigation`

- **符号**: `switches::kGlicSkipReloadAfterNavigation` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_GLIC) && BUILDFLAG(ENABLE_GLIC_ANDROID)
- **用途**：导航后不触发重载。
> If this flag is set, then the page navigating will not trigger a reload.

#### `--guest`

- **符号**: `switches::kGuest` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS) && BUILDFLAG(IS_MAC) && \
- **用途**：以访客模式直接启动浏览器。
> Causes the browser to launch directly in guest mode.

#### `--help`

- **符号**: `switches::kHelp` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_CHROMEOS)
- **用途**：显示 man page（等价别名：`--h`、`-h`）。
> These flags show the man page on Linux. They are equivalent to each other.

#### `--h`

- **符号**: `switches::kHelpShort` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_CHROMEOS)
- **用途**：--help 的别名。

#### `--hide-crash-restore-bubble`

- **符号**: `switches::kHideCrashRestoreBubble` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动阶段不显示崩溃恢复气泡（配合 ChromeOS Full Restore）。
> Does not show the crash restore bubble when the browser is started during the system startup phase in ChromeOS, if the ChromeOS full restore feature is enabled, because the ChromeOS full restore notification is shown for the user to select restore or not.

#### `--hide-icons`

- **符号**: `switches::kHideIcons` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows）给“设置默认程序”对话框看的占位开关，实际仅弹错误框。
> Makes Windows happy by allowing it to show "Enable access to this program" checkbox in Add/Remove Programs->Set Program Access and Defaults. This only shows an error box because the only way to hide Chrome is by uninstalling it.

#### `--homepage`

- **符号**: `switches::kHomePage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定新开标签页显示的页面（主要用于测试）。
> Specifies which page will be displayed in newly-opened tabs. We need this for testing purposes so that the UI tests don't depend on what comes up for http://google.com.

#### `--ignore-profile-directory-if-not-exists`

- **符号**: `switches::kIgnoreProfileDirectoryIfNotExists` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：配合 --profile-directory：若目录不存在则不创建 Profile。
> If provided with kProfileDirectory, does not create the profile if the profile directory doesn't exist.

#### `--incognito`

- **符号**: `switches::kIncognito` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：以无痕模式打开初始窗口；后续窗口不一定无痕。
> Causes the initial browser opened to be in incognito mode. Further browsers may or may not be in incognito mode; see `IncognitoModePrefs`.

#### `--init-isolate-as-foreground`

- **符号**: `switches::kInitIsolateAsForeground` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：主线程 Isolate 以前台模式初始化（扩展进程默认后台，其他默认前台）。
> Specifies that the main-thread Isolate should initialize in foreground mode. If not specified, the the Isolate will start in background mode for extension processes and foreground mode otherwise.

#### `--install-autogenerated-theme`

- **符号**: `switches::kInstallAutogeneratedTheme` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按 `r,g,b` 安装一个自动生成的主题。
> Installs an autogenerated theme based on the given RGB value. The format is "r,g,b", where r, g, b are a numeric values from 0 to 255.

#### `--install-chrome-app`

- **符号**: `switches::kInstallChromeApp` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动安装给定 Chrome App 的流程。
> Causes Chrome to initiate an installation flow for the given app.

#### `--install-isolated-web-app-from-file`

- **符号**: `switches::kInstallIsolatedWebAppFromFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：以开发者模式安装本地未签名 Web Bundle 为 Isolated Web App。
> Causes Chrome to install the unsigned Web Bundle at the given path as a developer mode Isolated Web App.

#### `--install-isolated-web-app-from-url`

- **符号**: `switches::kInstallIsolatedWebAppFromUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：以开发者模式安装托管在 HTTP(S) 的 Isolated Web App。
> Causes Chrome to install a developer mode Isolated Web App whose contents are hosted at the given HTTP(S) URL.

#### `--instant-process`

- **符号**: `switches::kInstantProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 Renderer 标记为 Instant 进程。
> Marks a renderer as an Instant process.

#### `--keep-alive-for-test`

- **符号**: `switches::kKeepAliveForTest` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：最后一个浏览器窗口关闭后仍保持进程存活（测试用）。
> Used for testing - keeps browser alive after last browser window closes.

#### `--kiosk`

- **符号**: `switches::kKioskMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：开启 kiosk 模式（注意：不是 Chrome OS 的 kiosk）。
> Enable kiosk mode. Please note this is not Chrome OS kiosk mode.

#### `--kiosk-printing`

- **符号**: `switches::kKioskModePrinting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打印预览自动按下打印按钮。
> Enable automatically pressing the print button in print preview.

#### `--list-apps`

- **符号**: `switches::kListApps` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_MAC) && BUILDFLAG(IS_WIN)
- **用途**：把每个 Profile 打开/安装的 Web App 写入指定文件，不开窗口；可配合 --profile-base-name。
> Writes open and installed web apps for each profile to the specified file without launching a new browser window or tab. Pass a absolute file path to specify where to output the information. Can be used together with optional --profile-base-name switch to only write information for a given profile.

#### `--make-chrome-default`

- **符号**: `switches::kMakeChromeDefault` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：安装期间将 Chrome 设为默认浏览器。
> Indicates whether Chrome should be set as the default browser during installation.

#### `--make-default-browser`

- **符号**: `switches::kMakeDefaultBrowser` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：将 Chrome 设为默认浏览器。
> Makes Chrome default browser

#### `--metrics-client-id`

- **符号**: `switches::kMetricsClientID` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：将 metrics client id 从浏览器进程传到子进程（与 crash client id 不同）。
> This is how the metrics client ID is passed from the browser process to its children. With Crashpad, the metrics client ID is distinct from the crash client ID.

#### `--monitoring-destination-id`

- **符号**: `switches::kMonitoringDestinationID` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：连接监控类 GCM 消息的目标 ID（在非生产管理服务器下有用）。
> Allows setting a different destination ID for connection-monitoring GCM messages. Useful when running against a non-prod management server.

#### `--native-messaging-connect-extension`

- **符号**: `switches::kNativeMessagingConnectExtension` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：请求与指定扩展 ID 建立 native messaging 连接，对端由 --native-messaging-connect-host 指定。
> Requests a native messaging connection be established between the extension with ID specified by this switch and the native messaging host named by the kNativeMessagingConnectHost switch.

#### `--native-messaging-connect-host`

- **符号**: `switches::kNativeMessagingConnectHost` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：请求与指定 native messaging host 建立连接，对端扩展由 --native-messaging-connect-extension 指定。
> Requests a native messaging connection be established between the native messaging host named by this switch and the extension with ID specified by kNativeMessagingConnectExtension.

#### `--native-messaging-connect-id`

- **符号**: `switches::kNativeMessagingConnectId` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：当以上两个开关都设了时，这个值作为命令行参数转发给 native messaging host。
> If set when kNativeMessagingConnectHost and kNativeMessagingConnectExtension are specified, is reflected to the native messaging host as a command line parameter.

#### `--no-default-browser-check`

- **符号**: `switches::kNoDefaultBrowserCheck` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不弹默认浏览器检查气泡（测试用）。
> Disables the default browser check. Useful for UI/browser tests where we want to avoid having the default browser info-bar displayed.

#### `--no-experiments`

- **符号**: `switches::kNoExperiments` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 about:flags 里所有实验；about:flags 本身仍可用，适合排查崩溃。
> Disables all experiments set on about:flags. Does not disable about:flags itself. Useful if an experiment makes chrome crash at startup: One can start chrome with --no-experiments, disable the problematic lab at about:flags and then restart chrome without this switch again.

#### `--no-first-run`

- **符号**: `switches::kNoFirstRun` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：跳过首次运行任务以及额外的对话/气泡/提示（自动化/性能基准测试用）。
> Skip First Run tasks as well as not showing additional dialogs, prompts or bubbles. Suppressing dialogs, prompts, and bubbles is important as this switch is used by automation (including performance benchmarks) where it's important only a browser window is shown. This may not actually be the first run or the What's New page.
> Its effect can be partially ignored by adding kForceFirstRun (for FRE), kForceWhatsNew (for What's New) and/or kIgnoreNoFirstRunForSearchEngineChoiceScreen (for the DSE choice screen). This does not drop the First Run sentinel and thus doesn't prevent first run from occurring the next time chrome is launched without this flag. It also does not update the last What's New milestone, so does not prevent What's New from occurring the next time chrome is launched without this flag.

#### `--no-network-profile-warning`

- **符号**: `switches::kNoNetworkProfileWarning` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows）不要因 Profile 在网络共享盘上而弹警告。
> Whether or not the browser should warn if the profile is on a network share. This flag is only relevant for Windows currently.

#### `--no-pings`

- **符号**: `switches::kNoPings` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不发送超链接审计（ping）。
> Don't send hyperlink auditing pings

#### `--no-pre-read-main-dll`

- **符号**: `switches::kNoPreReadMainDll` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows）不预读 Chrome.dll 的内存页（预读可加速启动，但在部分子进程里不一定需要）。
> Whether this process should PrefetchVirtualMemory on the contents of Chrome.dll. This warms up the pages in memory to speed up startup but might not be required in later renderers and/or GPU. For experiment info see crbug.com/1350257.

#### `--no-proxy-server`

- **符号**: `switches::kNoProxyServer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制不使用代理（覆盖其他代理开关）。
> Don't use a proxy server, always make direct connections. Overrides any other proxy server flags that are passed.

#### `--no-service-autorun`

- **符号**: `switches::kNoServiceAutorun` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Service 进程不再自注册为开机启动项（不会删除已有的）。
> Disables the service process from adding itself as an autorun process. This does not delete existing autorun registrations, it just prevents the service from registering a new one.

#### `--no-startup-window`

- **符号**: `switches::kNoStartupWindow` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动时不自动打开浏览器窗口（用于仅承载后台 App 的场景）。
> Does not automatically open a browser window on startup (used when launching Chrome for the purpose of hosting background apps).

#### `--notification-inline-reply`

- **符号**: `switches::kNotificationInlineReply` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows Action Center）配合 --notification-launch-id，传入用户在 toast 中填写的 inline 回复。
> Used in combination with kNotificationLaunchId to specify the inline reply entered in the toast in the Windows Action Center.

#### `--notification-launch-id`

- **符号**: `switches::kNotificationLaunchId` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows Action Center）当用户点击 toast 启动 Chrome 时，这里是 Chrome 编码的 launch id。
> Used for launching Chrome when a toast displayed in the Windows Action Center has been activated. Should contain the launch ID encoded by Chrome.

#### `--on-the-fly-mhtml-hash-computation`

- **符号**: `switches::kOnTheFlyMhtmlHashComputation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 MHTML 从 Renderer 通过 Mojo 数据管道流回浏览器时边写边算哈希。
> Calculate the hash of an MHTML file as it is being saved. The browser process will write the serialized MHTML contents to a file and calculate its hash as it is streamed back from the renderer via a Mojo data pipe.

#### `--new-window`

- **符号**: `switches::kOpenInNewWindow` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在新浏览器窗口中打开 URL。
> Launches URL in new browser window.

#### `--pack-extension`

- **符号**: `switches::kPackExtension` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把指定目录打包成 .crx 扩展。
> Packages an extension to a .crx installable file from a given directory.

#### `--pack-extension-key`

- **符号**: `switches::kPackExtensionKey` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打包 .crx 时使用的 PEM 私钥。
> Optional PEM private key to use in signing packaged .crx.

#### `--pre-crashpad-crash-test`

- **符号**: `switches::kPreCrashpadCrashTest` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动极早期（crashpad/breakpad 初始化前）就崩溃，用于测试。
> Causes the browser process to crash very early in startup, just before crashpad (or breakpad) is initialized.

#### `--prediction-service-mock-likelihood`

- **符号**: `switches::kPredictionServiceMockLikelihood` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：模拟 Web Permission Prediction Service 的返回（测试用）。
> Used to mock the response received from the Web Permission Prediction Service. Used for testing.

#### `--preinstalled-web-apps-dir`

- **符号**: `switches::kPreinstalledWebAppsDir` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖查找默认/预装 Web App 配置 JSON 的目录。
> A directory where Chrome looks for json files describing default/preinstalled web apps. This overrides any default directory to load preinstalled web apps from.

#### `--privet-ipv6-only`

- **符号**: `switches::kPrivetIPv6Only` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：privet HTTP 仅使用 IPv6。
> Use IPv6 only for privet HTTP.

#### `--product-version`

- **符号**: `switches::kProductVersion` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打印产品版本并退出（Linux 上用于探测已装 Chrome 版本）。
> Outputs the product version information and quit. Used as an internal api to detect the installed version of Chrome on Linux.

#### `--profile-base-name`

- **符号**: `switches::kProfileBaseName` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_MAC) && BUILDFLAG(IS_WIN)
- **用途**：指定要读信息的 Profile 目录名；仅与 --list-apps 搭配有意义。
> Pass the basename of the profile directory to specify which profile to get information. Only relevant when used with --list-apps switch.

#### `--profile-directory`

- **符号**: `switches::kProfileDirectory` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为首个启动的浏览器选择要用的 Profile 目录。
> Selects directory of profile to associate with the first browser launched.

#### `--profile-email`

- **符号**: `switches::kProfileEmail` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按邮箱选择 Profile；若找不到则此开关无效；与 --profile-directory 同存在时后者优先。
> Like kProfileDirectory, but selects the profile by email address. If the email is not found in any existing profile, this switch has no effect. If both kProfileDirectory and kProfileEmail are specified, kProfileDirectory takes priority.

#### `--profile-management-attributes`

- **符号**: `switches::kProfileManagementAttributes` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_MAC) && BUILDFLAG(IS_WIN)
- **用途**：（Linux/Mac/Win）为指定域名启用第三方 Profile 管理的 SAML 属性（JSON 格式）。
> Domains and associated SAML attributes for which third-party profile management should be enabled. Input should be in JSON format.

#### `--proxy-auto-detect`

- **符号**: `switches::kProxyAutoDetect` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制代理自动探测。
> Forces proxy auto-detection.

#### `--proxy-bypass-list`

- **符号**: `switches::kProxyBypassList` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：配合 --proxy-server 的绕过规则列表，逗号分隔；与 --proxy-auto-detect/--no-proxy-server 同用时忽略。
> Specifies a list of hosts for whom we bypass proxy settings and use direct connections. Ignored if --proxy-auto-detect or --no-proxy-server are also specified. This is a comma-separated list of bypass rules. See: "net/proxy_resolution/proxy_host_matching_rules.h" for the format of these rules.

#### `--proxy-pac-url`

- **符号**: `switches::kProxyPacUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：使用给定 URL 的 PAC 脚本。
> Uses the pac script at the given URL

#### `--proxy-server`

- **符号**: `switches::kProxyServer` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：指定代理服务器，覆盖系统设置。
> Uses a specified proxy server, overrides system settings.

#### `--pwa-launcher-version`

- **符号**: `switches::kPwaLauncherVersion` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：由 PWA 启动器传入其版本，用于决定是否要把所有启动器更新到新方案。
> Specifies the version of the Progressive-Web-App launcher that launched Chrome, used to determine whether to update all launchers. NOTE: changing this switch requires adding legacy handling for the previous method, as older PWA launchers still using this switch will rely on Chrome to update them to use the new method.

#### `--refresh-platform-policy`

- **符号**: `switches::kRefreshPlatformPolicy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在已运行的 Chrome 上立刻刷新本机平台策略（不弹新窗口，不触发 cloud policy 刷新）。
> Forces immediate platform policy refresh (not cloud policy) when Chrome is already running. The switch prevents a new browser window from opening and only triggers the policy refresh. Useful for testing and automation to avoid waiting for the next scheduled refresh interval. No-op if Chrome is not already running.

#### `--relauncher`

- **符号**: `switches::kRelauncherProcess` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **同名定义**: `sandbox/policy/switches.cc`
- **用途**：子进程类型：负责重启浏览器（见 chrome/browser/mac/relauncher.h）。
> A process type (switches::kProcessType) that relaunches the browser. See chrome/browser/mac/relauncher.h.

#### `--dmg-device`

- **符号**: `switches::kRelauncherProcessDMGDevice` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：在 --relauncher 下指定要 unmount/弹出的 DMG BSD 设备名（`diskN` 或 `diskNsM`），随后移到废纸篓。
> When switches::kProcessType is switches::kRelauncherProcess, if this switch is also present, the relauncher process will unmount and eject a mounted disk image and move its disk image file to the trash.  The argument's value must be a BSD device name of the form "diskN" or "diskNsM".

#### `--remote-debugging-targets`

- **符号**: `switches::kRemoteDebuggingTargets` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：提供一组 `<host>:<port>` 用于发现 DevTools 远程调试目标。
> Provides a list of addresses to discover DevTools remote debugging targets. The format is <host>:<port>,...,<host>:port.

#### `--repair-all-valid-extensions`

- **符号**: `switches::kRepairAllValidExtensions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：所有被策略启用但损坏的扩展都尝试修复（通常在用户数据降级后用）。
> Indicates that all corrupted extensions should be repaired if they are are enabled by policy. This is mainly used after a user data downgrade.

#### `--restart`

- **符号**: `switches::kRestart` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：标记本次是重启（如改 flag 后），记录 Launch.Mode2 指标时忽略此次启动。
> Indicates that Chrome was restarted (e.g., after a flag change). This is used to ignore the launch when recording the Launch.Mode2 metric.

#### `--restore-last-session`

- **符号**: `switches::kRestoreLastSession` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动时恢复上次会话（覆盖偏好；不启用崩溃后的自动恢复以避免崩溃循环）。
> Indicates the last session should be restored on startup. This overrides the preferences value. Note that this does not force automatic session restore following a crash, so as to prevent a crash loop. This switch is used to implement support for OS-specific "continue where you left off" functionality on OS X and Windows.

#### `--ssl-version-max`

- **符号**: `switches::kSSLVersionMax` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许的最高 SSL/TLS 版本，取值 `tls1.2` 或 `tls1.3`。
> Specifies the maximum SSL/TLS version ("tls1.2" or "tls1.3").

#### `--ssl-version-min`

- **符号**: `switches::kSSLVersionMin` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许的最低 SSL/TLS 版本，取值 `tls1.2` 或 `tls1.3`。
> Specifies the minimum SSL/TLS version ("tls1.2" or "tls1.3").

#### `--tls1.2`

- **符号**: `switches::kSSLVersionTLSv12` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--ssl-version-min/max 的取值：TLS 1.2。
> TLS 1.2 mode for |kSSLVersionMax| and |kSSLVersionMin| switches.

#### `--tls1.3`

- **符号**: `switches::kSSLVersionTLSv13` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--ssl-version-min/max 的取值：TLS 1.3。
> TLS 1.3 mode for |kSSLVersionMax| and |kSSLVersionMin| switches.

#### `--same-tab`

- **符号**: `switches::kSameTab` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：命令行里的 URL 在活动标签打开而不是新标签；多个 URL 时首个替换活动标签。
> Indicates that the URL in the command line should open in the active tab instead of a new tab. In case of multiple URLS given as arguments, the first one will replace the active tab.

#### `--show-icons`

- **符号**: `switches::kShowIcons` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：与 --hide-icons 对应，Windows 上的占位开关。
> See kHideIcons.

#### `--silent-debugger-extension-api`

- **符号**: `switches::kSilentDebuggerExtensionAPI` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展用 chrome.debugger 附加页面时不显示 InfoBar（用于附加到扩展后台页）。
> Does not show an infobar when an extension attaches to a page using chrome.debugger page. Required to attach to extension background pages.

#### `--silent-launch`

- **符号**: `switches::kSilentLaunch` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动时默认不开任何窗口（想用 Chrome 做 ash server 时有用）。
> Causes Chrome to launch without opening any windows by default. Useful if one wishes to use Chrome as an ash server.

#### `--simulate-browsing-data-lifetime`

- **符号**: `switches::kSimulateBrowsingDataLifetime` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 BrowsingDataLifetime 策略设成极短值（比正常允许的更短，测试用）。
> Sets the BrowsingDataLifetime policy to a very short value (shorter than normally possible) for testing purposes.

#### `--simulate-critical-update`

- **符号**: `switches::kSimulateCriticalUpdate` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：模拟有关键更新可用。
> Simulates a critical update being available.

#### `--simulate-elevated-recovery`

- **符号**: `switches::kSimulateElevatedRecovery` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：模拟恢复升级通道需要提权。
> Simulates that elevation is needed to recover upgrade channel.

#### `--simulate-idle-timeout`

- **符号**: `switches::kSimulateIdleTimeout` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 IdleTimeout 策略设成极短值（测试用）。
> Sets the IdleTimeout policy to a very short value (shorter than normally possible) for testing purposes.

#### `--simulate-outdated`

- **符号**: `switches::kSimulateOutdated` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：模拟当前版本已过时。
> Simulates that current version is outdated.

#### `--simulate-outdated-no-au`

- **符号**: `switches::kSimulateOutdatedNoAU` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：模拟当前版本已过时且自动更新被关闭。
> Simulates that current version is outdated and auto-update is off.

#### `--simulate-upgrade`

- **符号**: `switches::kSimulateUpgrade` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：模拟有新版本可升级。
> Simulates an update being available.

#### `--source-app-id`

- **符号**: `switches::kSourceAppId` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：与已有进程 rendezvous 时，标记新进程的 StartupInfoW 含 STARTF_TITLEISAPPID（用于启动指标）。
> When rendezvousing with an existing process, used to indicate that the StartupInfoW of the new Chrome process had dwFlags == STARTF_TITLEISAPPID. This is used to record launch metrics.

#### `--source-shortcut`

- **符号**: `switches::kSourceShortcut` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：与已有进程 rendezvous 时，传入用于启动的快捷方式路径（用于启动指标）。
> When rendezvousing with an existing process, used to pass the path of the shortcut that launched the new Chrome process. This is used to record launch metrics.

#### `--start-maximized`

- **符号**: `switches::kStartMaximized` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动即最大化窗口（无视之前的窗口设置）。
> Starts the browser maximized, regardless of any previous settings.

#### `--start-stack-profiler`

- **符号**: `switches::kStartStackProfiler` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在子进程启动栈采样 profiler。
> Starts the stack sampling profiler in the child process.

#### `--browser-test`

- **符号**: `switches::kStartStackProfilerBrowserTest` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `content/public/common/content_switches.cc`
- **用途**：配合 --start-stack-profiler，在 browser test 模式下限制采样时长明显小于测试超时；ChromeOS 下强制所有进程都采样。
> Browser test mode for the |kStartStackProfiler| switch. Limits the profile durations to be significantly less than the test timeout. On ChromeOS, forces the stack sampling profiler to run on all processes as well.

#### `--storage-pressure-notification-interval`

- **符号**: `switches::kStoragePressureNotificationInterval` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：存储压力通知节流的间隔分钟数（开发测试用）。
> Interval, in minutes, used for storage pressure notification throttling. Useful for developers testing applications that might use non-trivial amounts of disk space.

#### `--system-audio-capture-default_checked`

- **符号**: `switches::kSystemAudioCaptureDefaultChecked` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：窗口/屏幕共享时“共享系统音频”默认勾选（主要用于测试）。
> This flag sets the checkboxes for sharing system audio during window or screen capture to on by default. It is primarily intended to be used for tests.

#### `--system-log-upload-frequency`

- **符号**: `switches::kSystemLogUploadFrequency` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：系统日志上传频率（毫秒，测试用）。
> Frequency in Milliseconds for system log uploads. Should only be used for testing purposes.

#### `--tab-capture-audio-default-unchecked`

- **符号**: `switches::kTabCaptureAudioDefaultUnchecked` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：标签页共享时“音频”默认不勾选（主要用于测试）。
> This flag sets the checkboxes for sharing audio during tab capture to off by default. It is primarily intended to be used for tests.

#### `--test-memory-log-delay-in-minutes`

- **符号**: `switches::kTestMemoryLogDelayInMinutes` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：内存日志的自定义延迟（分钟，测试用）。
> Custom delay for memory log. This should be used only for testing purpose.

#### `--test-name`

- **符号**: `switches::kTestName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把当前自动化测试名传给 Chrome。
> Passes the name of the current running automated test to Chrome.

#### `--auto-accept-this-tab-capture`

- **符号**: `switches::kThisTabCaptureAutoAccept` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自动接受当前标签页的捕获请求（测试用）。
> These flags make Chrome auto-accept/reject requests to capture the current tab. It should only be used for tests.

#### `--auto-reject-this-tab-capture`

- **符号**: `switches::kThisTabCaptureAutoReject` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自动拒绝当前标签页的捕获请求（测试用）。

#### `--trusted-download-sources`

- **符号**: `switches::kTrustedDownloadSources` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把一组下载源标为可信，前提是相应的组策略生效。
> Identifies a list of download sources as trusted, but only if proper group policy is set.

#### `--uninstall`

- **符号**: `switches::kUninstall` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：执行卸载清理步骤（Chrome first-run 相关）。
> Runs un-installation steps that were done by chrome first-run.

#### `--uninstall-app-id`

- **符号**: `switches::kUninstallAppId` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：卸载指定 ID 的 Web App。
> Specifies that the WebApp with the specified id should be uninstalled.

#### `--unique-temp-dir-suffix`

- **符号**: `switches::kUniqueTempDirSuffix` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：配合 kCodeSignCloneCleanupProcess：用唯一后缀重建临时目录路径。
> When switches::kProcessType is switches::kCodeSignCloneCleanupProcess this switch is required. The value must be the unique suffix portion of the temporary directory that contains the clone. The full path will be reconstructed by the cleanup process.

#### `--unlimited-storage`

- **符号**: `switches::kUnlimitedStorage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为所有 app/origin 覆盖配额为不限，仅测试用。
> Overrides per-origin quota settings to unlimited storage for any apps/origins.  This should be used only for testing purpose.

#### `--unsafely-disable-devtools-self-xss-warnings`

- **符号**: `switches::kUnsafelyDisableDevToolsSelfXssWarnings` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 DevTools 控制台粘贴时不再弹 self-XSS 警告。
> Disables warnings about self-XSS attacks when pasting into the DevTools console.

#### `--use-system-proxy-resolver`

- **符号**: `switches::kUseSystemProxyResolver` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）用 WinHttp 解析代理而非 Chromium 的代理解析。
> Uses WinHttp to resolve proxies instead of using Chromium's normal proxy resolution logic. This is only supported in Windows. TODO(crbug.com/40111093): Only use WinHttp whenever Chrome is exclusively using system proxy configs.

#### `--user-data-dir`

- **符号**: `switches::kUserDataDir` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `components/password_manager/core/browser/password_manager_switches.h`, `content/shell/common/shell_switches.h`, `headless/public/switches.h`
- **用途**：指定用户数据目录（浏览器所有状态的根目录）。
> Specifies the user data directory, which is where the browser will look for all of its state.

#### `--user-data-migrated`

- **符号**: `switches::kUserDataMigrated` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_DOWNGRADE_PROCESSING)
- **用途**：标记：本进程是 User Data 迁移后重启出来的。
> Indicates that this process is the product of a relaunch following migration of User Data.

#### `--validate-crx`

- **符号**: `switches::kValidateCrx` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：校验 .crx 的有效性并打印结果。
> Examines a .crx for validity and prints the result.

#### `--version`

- **符号**: `switches::kVersion` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：打印版本信息并退出。
> Prints version information and quits.

#### `--webrtc-ip-handling-policy`

- **符号**: `switches::kWebRtcIPHandlingPolicy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 WebRTC IP 处理策略（对应 Preferences 中的设置）。
> Override WebRTC IP handling policy to mimic the behavior when WebRTC IP handling policy is specified in Preferences.

#### `--webrtc-event-log-proactive-pruning-delta`

- **符号**: `switches::kWebRtcRemoteEventLogProactivePruningDelta` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：主动清理待上传的远端 WebRTC 事件日志的间隔秒数；正数为时长，0 表示不主动清。
> Sets the delay (in seconds) between proactive prunings of remote-bound WebRTC event logs which are pending upload. All positive values are legal. All negative values are illegal, and ignored. If set to 0, the meaning is "no proactive pruning".

#### `--webrtc-event-log-upload-delay-ms`

- **符号**: `switches::kWebRtcRemoteEventLogUploadDelayMs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：WebRTC 事件日志满足上传条件后需持续的毫秒数才真正上传。
> WebRTC event logs will only be uploaded if the conditions hold for this many milliseconds.

#### `--webrtc-event-log-upload-no-suppression`

- **符号**: `switches::kWebRtcRemoteEventLogUploadNoSuppression` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：即使有 active peer connection 也不抑制事件日志上传。
> Normally, remote-bound WebRTC event logs are uploaded only when no peer connections are active. With this flag, the upload is never suppressed.

#### `--winhttp-proxy-resolver`

- **符号**: `switches::kWinHttpProxyResolver` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）用 WinHTTP 拉取并解析 PAC 脚本，而不是 Chromium 的网络栈 + V8。
> Uses WinHTTP to fetch and evaluate PAC scripts. Otherwise the default is to use Chromium's network stack to fetch, and V8 to evaluate.

#### `--win-jumplist-action`

- **符号**: `switches::kWinJumplistAction` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）记录用户点击的 Jumplist 类别，用于启动指标。
> Specifies which category option was clicked in the Windows Jumplist that resulted in a browser startup.

#### `--window-name`

- **符号**: `switches::kWindowName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定初始窗口标题：`--window-name="My title"`。
> Specify the initial window user title: --window-name="My custom title"

#### `--window-position`

- **符号**: `switches::kWindowPosition` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定初始窗口位置：`--window-position=x,y`。
> Specify the initial window position: --window-position=x,y

#### `--window-size`

- **符号**: `switches::kWindowSize` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：指定初始窗口大小：`--window-size=w,h`。
> Specify the initial window size: --window-size=w,h

#### `--window-workspace`

- **符号**: `switches::kWindowWorkspace` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定初始窗口所在工作区：`--window-workspace=id`。
> Specify the initial window workspace: --window-workspace=id

#### `--class`

- **符号**: `switches::kWmClass` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_POSIX) && !BUILDFLAG(IS_MAC) && !BUILDFLAG(IS_CHROMEOS)
- **用途**：（X11）设置窗口的 WM_CLASS 属性。
> The same as the --class argument in X applications.  Overrides the WM_CLASS window property with the given value.


### Embedder Support (components/embedder_support/switches.cc)

embedder_support 组件暴露给浏览器壳的通用开关，如 User-Agent 覆盖等。

_开关数：8_


#### `--disable-popup-blocking`

- **符号**: `switches::kDisablePopupBlocking` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用弹窗拦截。
> Disables pop-up blocking.

#### `--headless`

- **符号**: `switches::kHeadless` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `ui/gfx/switches.cc`
- **用途**：启用 headless 模式。
> Enable headless mode.

#### `--origin-trial-disabled-features`

- **符号**: `switches::kOriginTrialDisabledFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用指定 Origin Trial 特性，用 `|` 分隔多个名称。
> Contains a list of feature names for which origin trial experiments should be disabled. Names should be separated by "|" characters.

#### `--origin-trial-disabled-tokens`

- **符号**: `switches::kOriginTrialDisabledTokens` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用指定 Origin Trial token（按签名），用 `|` 分隔。
> Contains a list of token signatures for which origin trial experiments should be disabled. Tokens should be separated by "|" characters.

#### `--origin-trial-public-key`

- **符号**: `switches::kOriginTrialPublicKey` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖校验 Origin Trial token 的默认公钥列表（逗号分隔）。
> Comma-separated list of keys which will override the default public keys for checking origin trial tokens.

#### `--short-reporting-delay`

- **符号**: `switches::kShortReportingDelay` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 Reporting API 的上报延迟缩短到 1 秒以内，便于更快看到 report。
> Sets the Reporting API delay to under a second to allow much quicker reports.

#### `--use-mobile-user-agent`

- **符号**: `switches::kUseMobileUserAgent` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：标记 Chromium 应使用移动版 UA。
> Set when Chromium should use a mobile user agent.

#### `--user-agent`

- **符号**: `switches::kUserAgent` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：用自定义 UA 覆盖默认 User-Agent。
> A string used to override the default user agent with a custom one.


### Content 层 (content/public/common/content_switches.cc)

内容层 (content) 暴露给 Chrome 与嵌入者的跨进程通用开关：Renderer/GPU/Utility 子进程命令行、站点隔离、IPC、可访问性等。

_开关数：213_


#### `--allow-command-line-plugins`

- **符号**: `switches::kAllowCommandLinePlugins` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许在命令行加载插件（测试用）。
> Allows plugins to be loaded in the command line for testing.

#### `--allow-file-access-from-files`

- **符号**: `switches::kAllowFileAccessFromFiles` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：默认 `file://` 不能读其他 `file://`，此开关恢复旧的不安全行为（开发测试用）。
> By default, file:// URIs cannot read other file:// URIs. This is an override for developers who need the old behavior for testing.

#### `--allow-insecure-localhost`

- **符号**: `switches::kAllowInsecureLocalhost` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：localhost 上的 TLS/SSL 错误不拦截、不拦请求。
> Enables TLS/SSL errors on localhost to be ignored (no interstitial, no blocking of requests).

#### `--allow-loopback-in-peer-connection`

- **符号**: `switches::kAllowLoopbackInPeerConnection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许 WebRTC PeerConnection 把 loopback 接口加入网络列表。
> Allows loopback interface to be added in network list for peer connection.

#### `--attribution-reporting-debug-mode`

- **符号**: `switches::kAttributionReportingDebugMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Attribution Reporting API 去掉延迟与噪声（调试）。
> Causes the Attribution Report API to run without delays or noise.

#### `--audio-process-high-priority`

- **符号**: `switches::kAudioProcessHighPriority` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：音频进程使用高优先级。
> Use high priority for the audio process.

#### `--auto-accept-camera-and-microphone-capture`

- **符号**: `switches::kAutoAcceptCameraAndMicrophoneCapture` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自动接受摄像头/麦克风捕获请求（视频会议网页自动化测试用；不影响屏幕捕获）。
> Bypasses the dialog prompting the user for permission to capture cameras and microphones. Useful in automatic tests of video-conferencing Web applications. This is nearly identical to kUseFakeUIForMediaStream, with the exception being that this flag does NOT affect screen-capture.

#### `--crash-test`

- **符号**: `switches::kBrowserCrashTest` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：浏览器进程启动时立即崩溃（测试用）。
> Causes the browser process to crash on startup.

#### `--browser-startup-dialog`

- **符号**: `switches::kBrowserStartupDialog` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：浏览器进程启动时弹对话框，便于附加调试器。
> Causes the browser process to display a dialog on launch.

#### `--browser-subprocess-path`

- **符号**: `switches::kBrowserSubprocessPath` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Renderer/Plugin 等子进程所用可执行程序路径。
> Path to the exe to run for the renderer and plugin subprocesses.

#### `--change-stack-guard-on-fork`

- **符号**: `switches::kChangeStackGuardOnFork` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Zygote fork 出新进程后更改栈 canary，避免共享秘密；取值 `enable`/`disable`。
> After a zygote forks a new process, change the stack canary. This switch is useful so not all forked processes use the same canary (a secret value), which can be vulnerable to information leaks and brute force attacks. See https://crbug.com/1206626. This requires that all functions on the stack at the time content::RunZygote() is called be compiled without stack canaries. Valid values are "enable" or "disable".

#### `--disable`

- **符号**: `switches::kChangeStackGuardOnForkDisabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--change-stack-guard-on-fork 取值：禁用。

#### `--enable`

- **符号**: `switches::kChangeStackGuardOnForkEnabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--change-stack-guard-on-fork 取值：启用。

#### `--device-scale-factor`

- **符号**: `switches::kDeviceScaleFactor` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：传给 Renderer 等子进程的设备像素比（DPR）。
> Device scale factor passed to certain processes like renderers, etc.

#### `--disable-canvas-aa`

- **符号**: `switches::kDisable2dCanvasAntialiasing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 2D Canvas 的抗锯齿。
> Disable antialiasing on 2d canvas.

#### `--disable-2d-canvas-clip-aa`

- **符号**: `switches::kDisable2dCanvasClipAntialiasing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 2D Canvas 裁剪的抗锯齿。
> Disable antialiasing on 2d canvas clips

#### `--disable-2d-canvas-image-chromium`

- **符号**: `switches::kDisable2dCanvasImageChromium` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不让 2D Canvas 渲染到 scanout buffer 以支持 overlay。
> Disables Canvas2D rendering into a scanout buffer for overlay support.

#### `--disable-3d-apis`

- **符号**: `switches::kDisable3DAPIs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭所有对页面可见的 3D API（主要是 WebGL）；用于企业策略。
> Disables client-visible 3D APIs, in particular WebGL. This is controlled by policy and is kept separate from the other enable/disable switches to avoid accidentally regressing the policy support for controlling access to these APIs.

#### `--disable-accelerated-2d-canvas`

- **符号**: `switches::kDisableAccelerated2dCanvas` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 2D Canvas 的 GPU 加速。
> Disable gpu-accelerated 2d canvas.

#### `--disable-accelerated-video-decode`

- **符号**: `switches::kDisableAcceleratedVideoDecode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用硬件视频解码（ChromeOS 策略依赖该开关，勿重命名）。
> Disables hardware acceleration of video decode, where available. Warning: do not remove or rename this flag, as it is used inside ChromeOS code to implement the DeviceHardwareVideoDecodingEnabled policy.

#### `--disable-accelerated-video-encode`

- **符号**: `switches::kDisableAcceleratedVideoEncode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用硬件视频编码。
> Disables hardware acceleration of video encode, where available.

#### `--disable-back-forward-cache`

- **符号**: `switches::kDisableBackForwardCache` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用前进/后退缓存 BFCache。
> Disables the BackForwardCache feature.

#### `--disable-background-timer-throttling`

- **符号**: `switches::kDisableBackgroundTimerThrottling` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不对后台页定时器任务做节流。
> Disable task throttling of timer tasks from background pages.

#### `--disable-backgrounding-occluded-windows`

- **符号**: `switches::kDisableBackgroundingOccludedWindowsForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：被其他窗口遮挡的 Renderer 不进入后台状态（测试确定性）。
> Disable backgrounding renders for occluded windows. Done for tests to avoid nondeterministic behavior.

#### `--disable-backing-store-limit`

- **符号**: `switches::kDisableBackingStoreLimit` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不限制 backing store 数量（防止多窗口多标签闪屏）。
> Disable limits on the number of backing stores. Can prevent blinking for users with many windows/tabs and lots of memory.

#### `--disable-blink-features`

- **符号**: `switches::kDisableBlinkFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按名字禁用 Blink runtime-enabled features（逗号分隔）；在 --enable-blink-features 之后应用。
> Disable one or more Blink runtime-enabled features. Use names from runtime_enabled_features.json5, separated by commas. Applied after kEnableBlinkFeatures, and after other flags that change these features.

#### `--disable-domain-blocking-for-3d-apis`

- **符号**: `switches::kDisableDomainBlockingFor3DAPIs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU reset 后不按域名封禁 3D API（测试用）。
> Disable the per-domain blocking for 3D APIs after GPU reset. This switch is intended only for tests.

#### `--disable-file-system`

- **符号**: `switches::kDisableFileSystem` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 FileSystem API。
> Disable FileSystem API.

#### `--disable-gesture-requirement-for-presentation`

- **符号**: `switches::kDisableGestureRequirementForPresentation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Presentation API 不要求用户手势。
> Disable user gesture requirement for presentation.

#### `--disable-gpu`

- **符号**: `switches::kDisableGpu` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 GPU 硬件加速；若没有软件渲染器，GPU 进程不会启动。
> Disables GPU hardware acceleration.  If software renderer is not in place, then the GPU process won't launch.

#### `--disable-gpu-compositing`

- **符号**: `switches::kDisableGpuCompositing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：合成器不再使用其 GPU 实现（走软件合成）。
> Prevent the compositor from using its GPU implementation.

#### `--disable-gpu-early-init`

- **符号**: `switches::kDisableGpuEarlyInit` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 GPU 进程的预先初始化。
> Disable proactive early init of GPU process.

#### `--disable-gpu-memory-buffer-compositor-resources`

- **符号**: `switches::kDisableGpuMemoryBufferCompositorResources` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不强制合成器资源由 GPU memory buffer 承载。
> Do not force that all compositor resources be backed by GPU memory buffers.

#### `--disable-gpu-memory-buffer-video-frames`

- **符号**: `switches::kDisableGpuMemoryBufferVideoFrames` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用由 GPU memory buffer 承载的 VideoFrame。
> Disable GpuMemoryBuffer backed VideoFrames.

#### `--disable-gpu-process-crash-limit`

- **符号**: `switches::kDisableGpuProcessCrashLimit` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：去掉 GPU 进程重启次数上限（测试用）。
> For tests, to disable the limit on the number of times the GPU process may be restarted.

#### `--disable-gpu-watchdog`

- **符号**: `switches::kDisableGpuWatchdog` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 GPU 进程无响应时用于杀进程的看门狗线程。
> Disable the thread that crashes the GPU process if it stops responding to messages.

#### `--disable-histogram-customizer`

- **符号**: `switches::kDisableHistogramCustomizer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 RenderThread 的 HistogramCustomizer。
> Disable the RenderThread's HistogramCustomizer.

#### `--disable-ignore-duplicate-navs-for-testing`

- **符号**: `switches::kDisableIgnoreDuplicateNavsForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 IgnoreDuplicateNavs 特性（避免测试里导航被意外忽略）。
> Disables the IgnoreDuplicateNavs feature. This prevent navigations from being unintentionally ignored in tests.

#### `--disable-in-process-stack-traces`

- **符号**: `switches::kDisableInProcessStackTraces` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭进程内栈回溯。
> Disables the in-process stack traces.

#### `--disable-ipc-flooding-protection`

- **符号**: `switches::kDisableIpcFloodingProtection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 IPC flooding 保护（默认开启，防止 JS 刷爆浏览器进程）。
> Disables the IPC flooding protection. It is activated by default. Some javascript functions can be used to flood the browser process with IPC. This protection limits the rate at which they can be used.

#### `--disable-javascript-harmony-shipping`

- **符号**: `switches::kDisableJavaScriptHarmonyShipping` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭最新已发布的 ECMAScript 6 特性。
> Disable latest shipping ECMAScript 6 features.

#### `--disable-kill-after-bad-ipc`

- **符号**: `switches::kDisableKillAfterBadIPC` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：子进程发送非法 IPC 时不杀之（测试用，平时不要开）。
> Don't kill a child process when it sends a bad IPC message.  Apart from testing, it is a bad idea from a security perspective to enable this switch.

#### `--disable-lcd-text`

- **符号**: `switches::kDisableLCDText` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 LCD 子像素抗锯齿文字。
> Disables LCD text.

#### `--disable-legacy-window`

- **符号**: `switches::kDisableLegacyIntermediateWindow` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：关闭与 WebContents 尺寸对应的 Legacy Window。
> Disable the Legacy Window which corresponds to the size of the WebContents.

#### `--disable-local-storage`

- **符号**: `switches::kDisableLocalStorage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 LocalStorage。
> Disable LocalStorage.

#### `--disable-logging`

- **符号**: `switches::kDisableLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制关闭日志（debug build 默认开）。
> Force logging to be disabled.  Logging is enabled by default in debug builds.

#### `--disable-low-latency-dxva`

- **符号**: `switches::kDisableLowLatencyDxva` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：创建 DXVA 解码器时不使用 CODECAPI_AVLowLatencyMode。
> Disables using CODECAPI_AVLowLatencyMode when creating DXVA decoders.

#### `--disable-mojo-broker`

- **符号**: `switches::kDisableMojoBroker` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 Mojo 初始化时禁用浏览器里的 Mojo broker 能力。
> Disables Mojo broker capabilities in the browser during Mojo initialization.

#### `--disable-new-content-rendering-timeout`

- **符号**: `switches::kDisableNewContentRenderingTimeout` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：主框架导航后长时间没提交新内容时不清空 Renderer 渲染输出。
> Disables clearing the rendering output of a renderer when it didn't commit new output for a while after a top-frame navigation.

#### `--disable-notifications`

- **符号**: `switches::kDisableNotifications` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 Web Notification 与 Push API。
> Disables the Web Notification and the Push APIs.

#### `--disable-nv12-dxgi-video`

- **符号**: `switches::kDisableNv12DxgiVideo` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：视频解码器不使用 NV12 纹理，改用 ARGB。
> Disables the video decoder from drawing to an NV12 textures instead of ARGB.

#### `--disable-origin-trial-controlled-blink-features`

- **符号**: `switches::kDisableOriginTrialControlledBlinkFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用所有可被 Origin Trial 启用的 Blink 特性。
> Disables all RuntimeEnabledFeatures that can be enabled via OriginTrials.

#### `--disable-platform-accessibility-integration`

- **符号**: `switches::kDisablePlatformAccessibilityIntegration` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用与平台无障碍层（读屏器等）的激活集成。
> Disables the activation of browser and web accessibility via interactions with the platform's accessibility integration (i.e., a screenreader will not be able to function with the browser).

#### `--disable-presentation-api`

- **符号**: `switches::kDisablePresentationAPI` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 Presentation API。
> Disables the Presentation API.

#### `--disable-pushstate-throttle`

- **符号**: `switches::kDisablePushStateThrottle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭对 history.pushState/replaceState 的节流。
> Disables throttling of history.pushState/replaceState calls.

#### `--disable-reading-from-canvas`

- **符号**: `switches::kDisableReadingFromCanvas` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：无论同源与否，所有 `<canvas>` 都标记为已污染。
> Taints all <canvas> elements, regardless of origin.

#### `--disable-remote-fonts`

- **符号**: `switches::kDisableRemoteFonts` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用远程 Web 字体（SVG 字体不受影响）。
> Disables remote web font support. SVG font should always work whether this option is specified or not.

#### `--disable-remote-playback-api`

- **符号**: `switches::kDisableRemotePlaybackAPI` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 RemotePlayback API。
> Disables the RemotePlayback API.

#### `--disable-renderer-backgrounding`

- **符号**: `switches::kDisableRendererBackgrounding` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Renderer 进程不进入后台状态。
> Prevent renderer process backgrounding when set.

#### `--disable-resource-scheduler`

- **符号**: `switches::kDisableResourceScheduler` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 ResourceScheduler（给想自己实现调度的 C++ Headless embedder 用）。
> Whether the ResourceScheduler is disabled.  Note this is only useful for C++ Headless embedders who need to implement their own resource scheduling.

#### `--disable-scroll-to-text-fragment`

- **符号**: `switches::kDisableScrollToTextFragment` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 ScrollToTextFragment（URL #:~:text= 锚点滚动）。
> This switch disables the ScrollToTextFragment feature.

#### `--disable-shared-workers`

- **符号**: `switches::kDisableSharedWorkers` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 SharedWorker。
> Disable shared workers.

#### `--disable-site-isolation-trials`

- **符号**: `switches::kDisableSiteIsolation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭站点隔离（同时也关闭桌面 M67 起默认启用的站点隔离；但策略/命令行显式 opt-in 仍会生效）。
> Disables site isolation. Note that the opt-in (to site-per-process, isolate-origins, etc.) via enterprise policy and/or cmdline takes precedence over the kDisableSiteIsolation switch (i.e. the opt-in takes effect despite potential presence of kDisableSiteIsolation switch). Note that for historic reasons the name of the switch misleadingly mentions "trials", but the switch also disables the default site isolation that ships on desktop since M67.
> The name of the switch is preserved for backcompatibility of chrome://flags.

#### `--disable-skia-runtime-opts`

- **符号**: `switches::kDisableSkiaRuntimeOpts` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Skia 不做运行时检测的高端 CPU 优化（用于 web test 走基线码路径）。
> Do not use runtime-detected high-end CPU optimizations in Skia.  This is useful for forcing a baseline code path for e.g. web tests.

#### `--disable-smooth-scrolling`

- **符号**: `switches::kDisableSmoothScrolling` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭平滑滚动（测试用）。
> Disable smooth scrolling for testing.

#### `--disable-software-compositing-fallback`

- **符号**: `switches::kDisableSoftwareCompositingFallback` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 达到崩溃上限后不再回退到软件合成（测试用）。
> For tests, to disable falling back to software compositing if the GPU Process has crashed, and reached the GPU Process crash limit.

#### `--disable-software-rasterizer`

- **符号**: `switches::kDisableSoftwareRasterizer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用软件光栅化（3D 软件光栅器）。
> Disables the use of a 3D software rasterizer.

#### `--disable-speech-api`

- **符号**: `switches::kDisableSpeechAPI` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 Web Speech API（识别+合成）。
> Disables the Web Speech API (both speech recognition and synthesis).

#### `--disable-speech-synthesis-api`

- **符号**: `switches::kDisableSpeechSynthesisAPI` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：仅禁用 Web Speech API 的合成部分。
> Disables the speech synthesis part of Web Speech API.

#### `--disable-threaded-compositing`

- **符号**: `switches::kDisableThreadedCompositing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用多线程 GPU 合成。
> Disable multithreaded GPU compositing of web content.

#### `--disable-v8-idle-tasks`

- **符号**: `switches::kDisableV8IdleTasks` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 V8 idle tasks。
> Disable V8 idle tasks.

#### `--disable-webgl`

- **符号**: `switches::kDisableWebGL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用所有版本的 WebGL。
> Disable all versions of WebGL.

#### `--disable-webgl2`

- **符号**: `switches::kDisableWebGL2` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 WebGL 2。
> Disable WebGL2.

#### `--disable-webgl-image-chromium`

- **符号**: `switches::kDisableWebGLImageChromium` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不让 WebGL 渲染到 scanout buffer 以支持 overlay。
> Disables WebGL rendering into a scanout buffer for overlay support.

#### `--disable-webrtc-encryption`

- **符号**: `switches::kDisableWebRtcEncryption` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 WebRTC 的 RTP 媒体加密（Chrome stable/beta 通道会忽略此开关）。
> Disables encryption of RTP Media for WebRTC. When Chrome embeds Content, it ignores this switch on its stable and beta channels.

#### `--disable-web-security`

- **符号**: `switches::kDisableWebSecurity` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭同源策略；需同时提供 --user-data-dir 才生效，仅网站测试用。
> Don't enforce the same-origin policy; meant for website testing only. This switch has no effect unless --user-data-dir (as defined by the content embedder) is also present.

#### `--disable-yuv-image-decoding`

- **符号**: `switches::kDisableYUVImageDecoding` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 YUV 图像解码（需 GPU rasterization 才有意义）。
> Disable YUV image decoding for those formats and cases where it's supported. Has no effect unless GPU rasterization is enabled.

#### `--disable-zero-copy-dxgi-video`

- **符号**: `switches::kDisableZeroCopyDxgiVideo` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：视频解码器不直接渲染到纹理。
> Disable the video decoder from drawing directly to a texture.

#### `--disallow-v8-feature-flag-overrides`

- **符号**: `switches::kDisallowV8FeatureFlagOverrides` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁止覆盖 V8 feature flags。
> Disallows overriding of v8 feature flags.

#### `--dom-automation`

- **符号**: `switches::kDomAutomationController` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 Renderer 里绑定 DOMAutomationController（按帧绑定，有性能开销，仅自动化 DOM 测试用）。
> Specifies if the |DOMAutomationController| needs to be bound in the renderer. This binding happens on per-frame basis and hence can potentially be a performance bottleneck. One should only enable it when automating dom based tests.

#### `--enable-aggressive-domstorage-flushing`

- **符号**: `switches::kEnableAggressiveDOMStorageFlushing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：激进地把 DOM Storage flush 到磁盘以降低数据丢失风险。
> Enable the aggressive flushing of DOM Storage to minimize data loss.

#### `--enable-automation`

- **符号**: `switches::kEnableAutomation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：标记浏览器由自动化控制（对应 `navigator.webdriver = true` 等）。
> Enable indication that browser is controlled by automation.

#### `--enable-blink-features`

- **符号**: `switches::kEnableBlinkFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按名字启用 Blink runtime-enabled features（逗号分隔），在 --disable-blink-features 之前应用。
> Enable one or more Blink runtime-enabled features. Use names from runtime_enabled_features.json5, separated by commas. Applied before kDisableBlinkFeatures, and after other flags that change these features.

#### `--enable-blink-test-features`

- **符号**: `switches::kEnableBlinkTestFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 status=test/experimental 的 Blink 特性（跑 web tests 时会打开）。
> Enables blink runtime enabled features with status:"test" or status:"experimental", which are enabled when running web tests.

#### `--canvas-2d-layers`

- **符号**: `switches::kEnableCanvas2DLayers` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用进行中的 2D Canvas API：BeginLayer/EndLayer。
> Enable in-progress canvas 2d API methods BeginLayer and EndLayer.

#### `--enable-caret-browsing`

- **符号**: `switches::kEnableCaretBrowsing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用原生插入符浏览（键盘移动光标阅读网页）。
> Enable native caret browsing, in which a moveable cursor is placed on a web page, allowing a user to select and navigate through non-editable text using just a keyboard. See https://crbug.com/977390 for links to i2i.

#### `--enable-experimental-cookie-features`

- **符号**: `switches::kEnableExperimentalCookieFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：一次性打开一组 Cookie 实验特性（SameSite/端口绑定/方案绑定/同链重定向等）。
> Flag that turns on a group of experimental/newly added cookie-related features together, as a convenience for e.g. testing, to avoid having to set multiple switches individually which may be error-prone (not to mention tedious). There is not a corresponding switch to disable all these features, because that is discouraged, and for testing purposes you'd need to switch them off individually to identify the problematic feature anyway.
> At present this turns on: net::features::kSameSiteDefaultChecksMethodRigorously net::features::kCookieSameSiteConsidersRedirectChain net::features::kEnablePortBoundCookies net::features::kEnableSchemeBoundCookies

#### `--enable-experimental-webassembly-features`

- **符号**: `switches::kEnableExperimentalWebAssemblyFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用实验性 WebAssembly 特性。
> Enables experimental WebAssembly features.

#### `--enable-experimental-web-platform-features`

- **符号**: `switches::kEnableExperimentalWebPlatformFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用开发中的 Web 平台特性。
> Enables Web Platform features that are in development.

#### `--enable-gpu-memory-buffer-video-frames`

- **符号**: `switches::kEnableGpuMemoryBufferVideoFrames` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用由 GPU memory buffer 承载的 VideoFrame。
> Enable GpuMemoryBuffer backed VideoFrames.

#### `--enable-isolated-web-apps-in-renderer`

- **符号**: `switches::kEnableIsolatedWebAppsInRenderer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（内部）把 IWA 启用状态传给 Renderer；不要用命令行直接加，请用 Feature `IsolatedWebApps`。
> Enables Isolated Web Apps (IWAs) in a renderer process. There are two ways to enable the IWAs: by feature flag and by enterprise policy. If IWAs are enabled by any of the mentioned above ways then this flag is passed to the renderer process. This flag should not be used from command line. To enable IWAs from command line one should use kIsolatedWebApps feature flag.

#### `--enable-lcd-text`

- **符号**: `switches::kEnableLCDText` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 LCD 子像素抗锯齿文字。
> Enables LCD text.

#### `--enable-logging`

- **符号**: `switches::kEnableLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制开启日志（release build 默认关）；值可接 `stderr`。
> Force logging to be enabled.  Logging is disabled by default in release builds.

#### `--enable-network-information-downlink-max`

- **符号**: `switches::kEnableNetworkInformationDownlinkMax` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 NetInfo API 的 type/downlinkMax 属性，并在连接类型变化时触发 change 事件。
> Enables the type, downlinkMax attributes of the NetInfo API. Also, enables triggering of change attribute of the NetInfo API when there is a change in the connection type.

#### `--enable-plugin-placeholder-testing`

- **符号**: `switches::kEnablePluginPlaceholderTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Plugin Placeholder 的测试特性（内部用）。
> Enables testing features of the Plugin Placeholder. For internal use only.

#### `--enable-precise-memory-info`

- **符号**: `switches::kEnablePreciseMemoryInfo` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：让 `window.performance.memory` 返回更精细、更及时的值（也作用于 Worker）。
> Make the values returned to window.performance.memory more granular and more up to date in shared worker. Without this flag, the memory information is still available, but it is bucketized and updated less frequently. This flag also applys to workers.

#### `--enable-privacy-sandbox-ads-apis`

- **符号**: `switches::kEnablePrivacySandboxAdsApis` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Privacy Sandbox 所有广告相关 API：Attribution Reporting、FLEDGE、Topics、Fenced Frames、Shared Storage、Private Aggregation。
> Enables Privacy Sandbox APIs: Attribution Reporting, Fledge, Topics, Fenced Frames, Shared Storage, Private Aggregation, and their associated features.

#### `--enable-service-binary-launcher`

- **符号**: `switches::kEnableServiceBinaryLauncher` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用 ServiceProcessLauncher 加载 service 二进制而非 utility 进程（仅测试用）。
> If true the ServiceProcessLauncher is used to launch services. This allows for service binaries to be loaded rather than using the utility process. This is only useful for tests.

#### `--enable-skia-benchmarking`

- **符号**: `switches::kEnableSkiaBenchmarking` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Skia benchmarking 扩展。
> Enables the Skia benchmarking extension.

#### `--enable-smooth-scrolling`

- **符号**: `switches::kEnableSmoothScrolling` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在支持的平台上启用平滑滚动动画。
> On platforms that support it, enables smooth scroll animation.

#### `--enable-spatial-navigation`

- **符号**: `switches::kEnableSpatialNavigation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用空间导航（方向键在可聚焦元素间跳）。
> Enable spatial navigation

#### `--enable-speech-dispatcher`

- **符号**: `switches::kEnableSpeechDispatcher` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX)
- **用途**：（Linux）允许把 TTS 请求送到 speech-dispatcher 服务（默认关，避免不稳定）。
> Allows sending text-to-speech requests to speech-dispatcher, a common Linux speech service. Because it's buggy, the user must explicitly enable it so that visiting a random webpage can't cause instability.

#### `--enable-strict-mixed-content-checking`

- **符号**: `switches::kEnableStrictMixedContentChecking` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：阻止 secure context 的所有非安全请求并禁止用户豁免。
> Blocks all insecure requests from secure contexts, and prevents the user from overriding that decision.

#### `--enable-strict-powerful-feature-restrictions`

- **符号**: `switches::kEnableStrictPowerfulFeatureRestrictions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：阻止在非安全源使用一批强力特性（如 device orientation）。
> Blocks insecure usage of a number of powerful features (device orientation, for example) that we haven't yet deprecated for the web at large.

#### `--enable-tracing-fraction`

- **符号**: `switches::kEnableTracingFraction` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按稳定哈希对 (0,1] 比例的测试开启 tracing，搭配其他 tracing 开关使用。
> When specified along with a value in the range (0,1] will --enable-tracing for (roughly) that percentage of tests being run. This is done in a stable manner such that the same tests are chosen each run, and under the assumption that tests hash equally across the range of possible values. The flag will enable all tracing categories for those tests, and none for the rest.
> This flag could be used with other tracing switches like --enable-tracing-format, but any other switches that will enable tracing will turn tracing on for all tests.

#### `--enable-usermedia-screen-capturing`

- **符号**: `switches::kEnableUserMediaScreenCapturing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 MediaStream API 的屏幕捕获。
> Enable screen capturing support for MediaStream API.

#### `--enable-viewport`

- **符号**: `switches::kEnableViewport` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 @viewport CSS 规则，并开启触屏 pinch 手势。
> Enables the use of the @viewport CSS rule, which allows pages to control aspects of their own layout. This also turns on touch-screen pinch gestures.

#### `--enable-vtune-support`

- **符号**: `switches::kEnableVtune` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Intel VTune profiler 支持。
> Enable the Vtune profiler support.

#### `--enable-webgl-developer-extensions`

- **符号**: `switches::kEnableWebGLDeveloperExtensions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用不对外公开的 WebGL 开发者扩展。
> Enables WebGL developer extensions which are not generally exposed to the web platform.

#### `--enable-webgl-draft-extensions`

- **符号**: `switches::kEnableWebGLDraftExtensions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用尚未社区批准的 WebGL 草案扩展。
> Enables WebGL extensions not yet approved by the community.

#### `--enable-webgl-image-chromium`

- **符号**: `switches::kEnableWebGLImageChromium` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许 WebGL 渲染到 scanout buffer 以支持 overlay。
> Enables WebGL rendering into a scanout buffer for overlay support.

#### `--file-url-path-alias`

- **符号**: `switches::kFileUrlPathAlias` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为 file:// URL 设别名替换，格式 `/alias=/replacement`。
> Define an alias root directory which is replaced with the replacement string in file URLs. The format is "/alias=/replacement", which would turn file:///alias/some/path.html into file:///replacement/some/path.html.

#### `--font-cache-shared-handle`

- **符号**: `switches::kFontCacheSharedHandle` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows DirectWrite）把浏览器与 Renderer 共享的 FontCache 句柄传过去。
> DirectWrite FontCache is shared by browser to renderers using shared memory. This switch allows us to pass the shared memory handle to the renderer.

#### `--force-presentation-receiver-for-testing`

- **符号**: `switches::kForcePresentationReceiverForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制以 presentation receiver 加载页面（测试用）。
> This forces pages to be loaded as presentation receivers.  Useful for testing behavior specific to presentation receivers. Spec: https://www.w3.org/TR/presentation-api/#interface-presentationreceiver

#### `--force-webrtc-ip-handling-policy`

- **符号**: `switches::kForceWebRtcIPHandlingPolicy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制覆盖 WebRTC IP 处理策略（对应 Preferences）。
> Override WebRTC IP handling policy to mimic the behavior when WebRTC IP handling policy is specified in Preferences.

#### `--gpu2-startup-dialog`

- **符号**: `switches::kGpu2StartupDialog` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：用于 GPU 信息收集的第二个 GPU 进程启动时弹对话框。
> Causes the second GPU process used for gpu info collection to display a dialog on launch.

#### `--gpu-launcher`

- **符号**: `switches::kGpuLauncher` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 进程启动前的命令行前缀（调试用），用法同 --renderer-cmd-prefix。
> Extra command line options for launching the GPU process (normally used for debugging). Use like renderer-cmd-prefix.

#### `--gpu-process`

- **符号**: `switches::kGpuProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `sandbox/policy/switches.cc`
- **用途**：标记：本进程是 GPU 子进程。
> Makes this process a GPU sub-process.

#### `--gpu-sandbox-start-early`

- **符号**: `switches::kGpuSandboxStartEarly` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在创建 GL context 之前就启用 GPU 沙箱。
> Starts the GPU sandbox before creating a GL context.

#### `--gpu-startup-dialog`

- **符号**: `switches::kGpuStartupDialog` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 进程启动时弹对话框，便于附加调试器。
> Causes the GPU process to display a dialog on launch.

#### `--hide-scrollbars`

- **符号**: `switches::kHideScrollbars` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：网页内容不生成滚动条（截图时需一致性常用）。
> Prevents creating scrollbars for web content. Useful for taking consistent screenshots.

#### `--ipc-connection-timeout`

- **符号**: `switches::kIPCConnectionTimeout` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：子进程等待浏览器 IPC 连接的秒数超时，超时自杀。
> Overrides the timeout, in seconds, that a child process waits for a connection from the browser before killing itself.

#### `--in-process-gpu`

- **符号**: `switches::kInProcessGPU` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 以线程形式跑在浏览器进程中（而非独立进程）。
> Run the GPU process as a thread in the browser process.

#### `--ipc-dump-directory`

- **符号**: `switches::kIpcDumpDirectory` &nbsp;&nbsp; **Buildflag**: defined(ENABLE_IPC_FUZZER)
- **用途**：把 Renderer 发给浏览器的 IPC 消息 dump 到目录，主要给 IPC fuzzer 采样用。
> Dumps IPC messages sent from renderer processes to the browser process to the given directory. Used primarily to gather samples for IPC fuzzing.

#### `--ipc-fuzzer-testcase`

- **符号**: `switches::kIpcFuzzerTestcase` &nbsp;&nbsp; **Buildflag**: defined(ENABLE_IPC_FUZZER)
- **用途**：指定 IPC fuzzer 使用的测试用例。
> Specifies the testcase used by the IPC fuzzer.

#### `--isolate-origins`

- **符号**: `switches::kIsolateOrigins` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为列出的 origin 要求独立 Renderer 进程，逗号分隔。
> Require dedicated processes for a set of origins, specified as a comma-separated list. For example: --isolate-origins=https://www.foo.com,https://www.bar.com

#### `--javascript-harmony`

- **符号**: `switches::kJavaScriptHarmony` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用实验性 Harmony（ES6+）特性。
> Enables experimental Harmony (ECMAScript 6) features.

#### `--as-browser`

- **符号**: `switches::kLaunchAsBrowser` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 browser 进程里跑测试。
> Flag to launch tests in the browser process.

#### `--load-webui-from-disk`

- **符号**: `switches::kLoadWebUIfromDisk` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(LOAD_WEBUI_FROM_DISK)
- **用途**：直接从磁盘加载 WebUI 文件而不是 pak（需要本地 checkout + 同时在 gn 里开 `load_webui_from_disk=true`）。
> Flag used to load WebUI files directly from disk instead of pak files. Meant to be used during local development only (requires a local checkout and build), and only works if used along with the GN load_webui_from_disk=true GN flag.

#### `--log-file`

- **符号**: `switches::kLogFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖通用日志的输出文件名（不影响记录哪些事件）。
> Overrides the default file name to use for general-purpose logging (does not affect which events are logged).

#### `--log-gpu-control-list-decisions`

- **符号**: `switches::kLogGpuControlListDecisions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打印 GPU 控制列表（blocklist）的判定结果。
> Logs GPU control list decisions when enforcing blocklist rules.

#### `--log-missing-unload-ack`

- **符号**: `switches::kLogMissingUnloadACK` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：render frame 的 unload 超时时打印错误日志。
> Log an error whenever the unload timeout for a render frame is exceeded.

#### `--log-level`

- **符号**: `switches::kLoggingLevel` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：设置最小日志级别，0=INFO 1=WARNING 2=ERROR 3=FATAL。
> Sets the minimum log level. Valid values are from 0 to 3: INFO = 0, WARNING = 1, LOG_ERROR = 2, LOG_FATAL = 3.

#### `--max-active-webgl-contexts`

- **符号**: `switches::kMaxActiveWebGLContexts` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：每 Renderer 进程允许的最大活动 WebGL context 数。
> Allows user to override maximum number of active WebGL contexts per renderer process.

#### `--max-decoded-image-size-mb`

- **符号**: `switches::kMaxDecodedImageSizeMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：解码后图像最大尺寸限制。
> Sets the maximium decoded image size limitation.

#### `--max-web-media-player-count`

- **符号**: `switches::kMaxWebMediaPlayerCount` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：每帧允许的最大 WebMediaPlayer 数量。
> Sets the maximum number of WebMediaPlayers allowed per frame.

#### `--message-loop-type-ui`

- **符号**: `switches::kMessageLoopTypeUi` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Utility 进程使用 UI 消息循环。
> Indicates the utility process should run with a message loop type of UI.

#### `--mock-cert-verifier-default-result-for-testing`

- **符号**: `switches::kMockCertVerifierDefaultResultForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：设置 MockCertVerifier 的默认结果（仅测试代码）。
> Set the default result for MockCertVerifier. This only works in test code.

#### `--mojo-local-storage`

- **符号**: `switches::kMojoLocalStorage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：使用基于 Mojo 的 LocalStorage 实现。
> Use a Mojo-based LocalStorage implementation.

#### `--no-unsandboxed-zygote`

- **符号**: `switches::kNoUnsandboxedZygote` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用非沙箱 zygote（多数桌面平台不建议用；为 Cast 等内存吃紧的场景而加）。
> Disables the unsandboxed zygote. Note: this flag should not be used on most platforms. It is introduced because some platforms (e.g. Cast) have very limited memory and binaries won't be updated when the browser process is running.

#### `--no-zygote`

- **符号**: `switches::kNoZygote` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不使用 zygote，直接 fork+exec 子进程；通常需同时 --no-sandbox。
> Disables the use of a zygote process for forking child processes. Instead, child processes will be forked and exec'd directly. Note that --no-sandbox should also be used together with this flag because the sandbox needs the zygote to work.

#### `--override-language-detection`

- **符号**: `switches::kOverrideLanguageDetection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖基于页面内容的语言检测结果。
> Overrides the language detection result determined based on the page contents.

#### `--pdf-renderer`

- **符号**: `switches::kPdfRenderer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Renderer 进程运行非 PPAPI 的 PDF 插件。
> Renderer process that runs the non-PPAPI PDF plugin.

#### `--private-aggregation-developer-mode`

- **符号**: `switches::kPrivateAggregationDeveloperMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Private Aggregation API 不走上报延迟。
> Causes the Private Aggregation API to run without reporting delays.

#### `--process-per-site`

- **符号**: `switches::kProcessPerSite` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：所有同站页面共享一个 Renderer 进程（进程合并，不是隔离！很多人误把它当 site-per-process）。
> Enable the "Process Per Site" process model for all domains. This mode consolidates same-site pages so that they share a single process. More details here: - https://www.chromium.org/developers/design-documents/process-models - The class comment in site_instance.h, listing the supported process models. IMPORTANT: This isn't to be confused with --site-per-process (which is about isolation, not consolidation). You probably want the other one.

#### `--process-per-tab`

- **符号**: `switches::kProcessPerTab` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按 BrowsingInstance（相互脚本可达的一组标签页）分配 Renderer；目前是 no-op。
> Runs each set of script-connected tabs (i.e., a BrowsingInstance) in its own renderer process.  We default to using a renderer process for each site instance (i.e., group of pages from the same registered domain with script connections to each other). TODO(creis): This flag is currently a no-op.  We should refactor it to avoid "unnecessary" process swaps for cross-site navigations but still swap when needed for security (e.g., isolated origins).

#### `--type`

- **符号**: `switches::kProcessType` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `sandbox/policy/switches.cc`
- **用途**：子进程类型（renderer/plugin/utility/zygote/...），空=浏览器。
> The value of this switch determines whether the process is started as a renderer or plugin host.  If it's empty, it's the browser.

#### `--protected-audiences-consented-debug-token`

- **符号**: `switches::kProtectedAudiencesConsentedDebugToken` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：给受信拍卖服务器传调试 token，使其允许记录该用户的拍卖信息。
> Causes Protected Audiences Bidding and Auction API to supply the provided debugging key to the trusted auction server. This tells the server that it okay to log information about this user's auction to help with debugging.

#### `--pull-to-refresh`

- **符号**: `switches::kPullToRefresh` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：下拉刷新手势；`0` 关、`1` 触控板+触屏、`2` 仅触屏；默认关闭。
> Enables or disables pull-to-refresh gesture in response to vertical overscroll. Set the value to '0' to disable the feature, set to '1' to enable it for both touchpad and touchscreen, and set to '2' to enable it only for touchscreen. Defaults to disabled.

#### `--raise-timer-frequency`

- **符号**: `switches::kRaiseTimerFrequency` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows）升高 Chrome 所有进程的定时器中断频率（实验用）。
> Raise the timer interrupt frequency in all Chrome processes, for experimental purposes. This feature is needed because as of Windows 10 2004 the scheduling effects of changing the timer interrupt frequency are not global, and this lets us prove/disprove whether this matters. See https://crbug.com/1128917

#### `--reduce-accept-language`

- **符号**: `switches::kReduceAcceptLanguage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：同时在 HTTP 头和 `navigator.languages` 里只保留最优先语言。
> Reduce the accept-language for HTTP header and JS navigator.languages, and only most preferred language: https://github.com/Tanych/accept-language.

#### `--reduce-accept-language-http`

- **符号**: `switches::kReduceAcceptLanguageHTTP` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：仅在 HTTP 头里只发送最优先语言。
> Reduce the accept-language for HTTP header, and only send most preferred language in the request header: https://github.com/Tanych/accept-language.

#### `--reduce-user-agent-minor-version`

- **符号**: `switches::kReduceUserAgentMinorVersion` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：UA 缩减第 4 阶段：把次版本号置零/收敛。
> Reduce the minor version number in the User-Agent string. This flag implements phase 4 of User-Agent reduction: https://blog.chromium.org/2021/09/user-agent-reduction-origin-trial-and-dates.html.

#### `--remote-allow-origins`

- **符号**: `switches::kRemoteAllowOrigins` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：只允许指定 origin 连接 DevTools WebSocket；`*` 表示任何 origin。
> Enables web socket connections from the specified origins only. '*' allows any origin.

#### `--remote-debugging-io-pipes`

- **符号**: `switches::kRemoteDebuggingIoPipes` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows）指定用于 DevTools 管道的入/出 pipe 句柄，形如 `3,4`。
> Specifies pipe names for the incoming and outbound messages on the Windows platform. This is a comma separated list of two pipe handles serialized as unsigned integers, e.g. "--remote-debugging-io-pipes=3,4".

#### `--remote-debugging-pipe`

- **符号**: `switches::kRemoteDebuggingPipe` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过 stdio pipe（in=3, out=4）或 --remote-debugging-io-pipes 走 DevTools 协议；可选 `JSON`（默认）或 `CBOR`。
> Enables remote debug over stdio pipes [in=3, out=4] or over the remote pipes specified in the 'remote-debugging-io-pipes' switch. Optionally, specifies the format for the protocol messages, can be either "JSON" (the default) or "CBOR".

#### `--remote-debugging-port`

- **符号**: `switches::kRemoteDebuggingPort` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在指定端口通过 HTTP 暴露 DevTools 协议。
> Enables remote debug over HTTP on the specified port.

#### `--renderer-client-id`

- **符号**: `switches::kRendererClientId` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：内部用：把 Renderer 的 client id 传给子进程。

#### `--renderer-cmd-prefix`

- **符号**: `switches::kRendererCmdPrefix` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Renderer 命令行前缀，如 `valgrind`、`xterm -e gdb --args`。
> The contents of this flag are prepended to the renderer command line. Useful values might be "valgrind" or "xterm -e gdb --args".

#### `--renderer`

- **符号**: `switches::kRendererProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `sandbox/policy/switches.cc`
- **用途**：子进程类型：Renderer。
> Causes the process to run as renderer instead of as browser.

#### `--launch-time-ticks`

- **符号**: `switches::kRendererProcessLaunchTimeTicks` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：浏览器 launch Renderer 时的 TimeTicks，用于时间对齐。
> Time the browser launched the renderer process (in TimeTicks).

#### `--renderer-process-limit`

- **符号**: `switches::kRendererProcessLimit` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Renderer 进程数上限（调得过高会耗资源甚至不稳定）。
> Overrides the default/calculated limit to the number of renderer processes. Very high values for this setting can lead to high memory/resource usage or instability.

#### `--renderer-startup-dialog`

- **符号**: `switches::kRendererStartupDialog` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Renderer 启动时弹对话框；Windows 非官方构建会附带 --no-sandbox。
> Causes the renderer process to display a dialog on launch. Passing this flag also adds sandbox::policy::kNoSandbox on Windows non-official builds, since that's needed to show a dialog.

#### `--run-manual`

- **符号**: `switches::kRunManualTestsFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：让只有标记 manual 的测试被执行。
> Manual tests only run when --run-manual is specified. This allows writing tests that don't run automatically but are still in the same test binary. This is useful so that a team that wants to run a few tests doesn't have to add a new binary that must be compiled on all builds.

#### `--sandbox-ipc`

- **符号**: `switches::kSandboxIPCProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：子进程类型：Sandbox IPC。
> Causes the process to run as a sandbox IPC subprocess.

#### `--shared-files`

- **符号**: `switches::kSharedFiles` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：传给子进程的一组文件描述符，格式 `<file_id>:<descriptor_id>,...`。
> Describes the file descriptors passed to a child process in the following list format: <file_id>:<descriptor_id>,<file_id>:<descriptor_id>,... where <file_id> is an ID string from the manifest of the service being launched and <descriptor_id> is the numeric identifier of the descriptor for the child process can use to retrieve the file descriptor from the global descriptor table.

#### `--single-process`

- **符号**: `switches::kSingleProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Renderer/插件与浏览器同进程运行（仅调试）。
> Runs the renderer and plugins in the same process as the browser

#### `--site-per-process`

- **符号**: `switches::kSitePerProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：严格站点隔离：每个 Renderer 只为一个 site 服务，跨 site 导航必换进程，跨 site iframe 进 OOPIF。
> Enforces a one-site-per-process security policy: * Each renderer process, for its whole lifetime, is dedicated to rendering pages for just one site. * Thus, pages from different sites are never in the same process. * A renderer process's access rights are restricted based on its site. * All cross-site navigations force process swaps. * <iframe>s are rendered out-of-process whenever the src= is cross-site.
> More details here: - https://www.chromium.org/developers/design-documents/site-isolation - https://www.chromium.org/developers/design-documents/process-models - The class comment in site_instance.h, listing the supported process models. IMPORTANT: this isn't to be confused with --process-per-site (which is about process consolidation, not isolation). You probably want this one.

#### `--skia-font-cache-limit-mb`

- **符号**: `switches::kSkiaFontCacheLimitMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Skia 字体缓存上限（MB）。
> Specifies the max number of bytes that should be used by the skia font cache. If the cache needs to allocate more, skia will purge previous entries.

#### `--skia-resource-cache-limit-mb`

- **符号**: `switches::kSkiaResourceCacheLimitMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Skia 资源缓存上限（MB）。
> Specifies the max number of bytes that should be used by the skia resource cache. The previous entries are purged from the cache when the memory useage exceeds this limit.

#### `--start-fullscreen`

- **符号**: `switches::kStartFullscreen` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动即进入全屏（相当于启动后按 F11）。
> Specifies if the browser should start in fullscreen mode, like if the user had pressed F11 right after startup.

#### `--enable-stats-collection-bindings`

- **符号**: `switches::kStatsCollectionController` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 Renderer 绑定 StatsCollectionController（按帧绑定，性能敏感，仅对应测试用）。
> Specifies if the |StatsCollectionController| needs to be bound in the renderer. This binding happens on per-frame basis and hence can potentially be a performance bottleneck. One should only enable it when running a test that needs to access the provided statistics.

#### `--target-device-scale-for-testing`

- **符号**: `switches::kTargetDeviceScaleForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Web test 指定目标设备缩放。
> Allows web tests to specify the target device scale for the test cases.

#### `--test-type`

- **符号**: `switches::kTestType` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：当前测试套类型（`browser`/`ui`/`gpu`）。
> Type of the current test harness ("browser" or "ui" or "gpu").

#### `--time-ticks-at-unix-epoch`

- **符号**: `switches::kTimeTicksAtUnixEpoch` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：以 Unix epoch 对齐的 TimeTicks 值（让多进程共享一个基点）。
> Accepts a number representing the time-ticks value at the Unix epoch. Since different processes can produce a different value for this due to system clock changes, this allows synchronizing them to a single value.

#### `--touch-events`

- **符号**: `switches::kTouchEventFeatureDetection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：开启触控事件特性检测；取值见下面的 `auto`/`disabled`/`enabled`。
> Enable support for touch event feature detection.

#### `--auto`

- **符号**: `switches::kTouchEventFeatureDetectionAuto` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `ui/base/ui_base_switches.h`
- **用途**：--touch-events 取值：自动（检测到触屏则启用）。
> The values the kTouchEventFeatureDetection switch may have, as in --touch-events=disabled. auto: enabled at startup when an attached touchscreen is present.

#### `--disabled`

- **符号**: `switches::kTouchEventFeatureDetectionDisabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `ui/base/ui_base_switches.h`, `ui/gl/gl_switches.cc`
- **用途**：--touch-events 取值：强制禁用。
> disabled: touch events are disabled.

#### `--enabled`

- **符号**: `switches::kTouchEventFeatureDetectionEnabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `ui/base/ui_base_switches.h`
- **用途**：--touch-events 取值：强制启用。
> enabled: touch events always enabled.

#### `--use-fake-codec-for-peer-connection`

- **符号**: `switches::kUseFakeCodecForPeerConnection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 PeerConnection 支持的编解码替换为一个假的 fake codec。
> Replaces the existing codecs supported in peer connection with a single fake codec entry that create a fake video encoder and decoder.

#### `--use-fake-ui-for-digital-identity`

- **符号**: `switches::kUseFakeUIForDigitalIdentity` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：绕过数字身份凭证的 OS 对话框，自动同意。
> Bypass the digital-identity-credential OS call. Simulate the user accepting the OS-presented dialog.

#### `--use-fake-ui-for-fedcm`

- **符号**: `switches::kUseFakeUIForFedCM` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：绕过 FedCM 账号选择弹窗，给值则选该 ID，不给则选第一个。
> Bypass the FedCM account selection dialog. If a value is provided for this switch, that account ID is selected, otherwise the first account is chosen.

#### `--use-fake-ui-for-media-stream`

- **符号**: `switches::kUseFakeUIForMediaStream` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：绕过 MediaStream 权限条，自动选默认设备；与 --use-fake-device-for-media-stream 搭配。
> Bypass the media stream infobar by selecting the default device for media streams (e.g. WebRTC). Works with --use-fake-device-for-media-stream. Prefer --auto-accept-camera-and-microphone-capture which does not interact with screen/tab capture.

#### `--use-mock-cert-verifier-for-testing`

- **符号**: `switches::kUseMockCertVerifierForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：使用 MockCertVerifier（仅测试代码）。
> Use the MockCertVerifier. This only works in test code.

#### `--utility-cmd-prefix`

- **符号**: `switches::kUtilityCmdPrefix` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Utility 进程命令行前缀，如 `valgrind`、`xterm -e gdb --args`。
> The contents of this flag are prepended to the utility process command line. Useful values might be "valgrind" or "xterm -e gdb --args".

#### `--utility-immediate-crash-for-testing`

- **符号**: `switches::kUtilityImmediateCrashForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Utility 进程启动极早期立即崩溃（测试用）。
> Crash the Utility process early in start-up, for testing.

#### `--utility`

- **符号**: `switches::kUtilityProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `sandbox/policy/switches.cc`
- **用途**：子进程类型：Utility。
> Causes the process to run as a utility subprocess.

#### `--utility-startup-dialog`

- **符号**: `switches::kUtilityStartupDialog` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Utility 进程启动时弹对话框。
> Causes the utility process to display a dialog on launch.

#### `--utility-sub-type`

- **符号**: `switches::kUtilitySubType` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在命令行标注 Utility 进程的子类型（便于识别用途，不影响服务）。
> This switch indicates the type of a utility process. It does not affect the services offered by the process, but is added to the command line to make it easier to identify the purpose of the utility process.

#### `--v8-cache-options`

- **符号**: `switches::kV8CacheOptions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：V8 代码缓存选项：`none`/`code`/`default`。
> Set options to cache V8 data. (none, code, or default)

#### `--browser-ui-tests-verify-pixels`

- **符号**: `switches::kVerifyPixels` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：让测试校验像素输出。
> Causes tests to attempt to verify pixel output.

#### `--video-image-texture-target`

- **符号**: `switches::kVideoImageTextureTarget` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：CHROMIUM_image 承载的 VideoFrame 纹理的 texture target。
> Texture target for CHROMIUM_image backed video frame textures.

#### `--wait-for-debugger-children`

- **符号**: `switches::kWaitForDebuggerChildren` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：所有子进程都等待调试器附加；可传过滤器决定哪些子进程等待。
> Will add kWaitForDebugger to every child processes. If a value is passed, it will be used as a filter to determine if the child process should have the kWaitForDebugger flag passed on or not.

#### `--wait-for-debugger-on-navigation`

- **符号**: `switches::kWaitForDebuggerOnNavigation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：每次导航都打印 Renderer URL 并等待调试器附加（或 SIGUSR1）。
> On every navigation a message with the renderer's URL will be logged and the renderer will wait for a debugger to be attached or SIGUSR1 to be sent to continue execution.

#### `--wait-for-debugger-webui`

- **符号**: `switches::kWaitForDebuggerWebUI` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：给 WebUI 测试用：等待调试器附加。
> Flag used by WebUI test runners to wait for debugger to be attached.

#### `--webauthn-remote-desktop-support`

- **符号**: `switches::kWebAuthRemoteDesktopSupport` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许受信的远程桌面客户端代表其他 origin 发起 WebAuthn 请求（仅放开扩展 API，不默认启用）。
> Allows trusted remote desktop clients to make WebAuthn requests on behalf of other origins. This switch only controls availability of the `remoteDesktopClientOverride` extension but doesn't by itself enable any origin to use it.

#### `--web-otp-backend`

- **符号**: `switches::kWebOtpBackend` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定 Web OTP API 的后端。
> Enables specified backend for the Web OTP API.

#### `--web-otp-backend-auto`

- **符号**: `switches::kWebOtpBackendAuto` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--web-otp-backend 取值：自动选择。
> Enables auto backend selection for Web OTP API.

#### `--web-otp-backend-sms-verification`

- **符号**: `switches::kWebOtpBackendSmsVerification` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--web-otp-backend 取值：SMS Verification（需短信含 app hash）。
> Enables Sms Verification backend for Web OTP API which requires app hash in SMS body.

#### `--web-otp-backend-user-consent`

- **符号**: `switches::kWebOtpBackendUserConsent` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--web-otp-backend 取值：User Consent。
> Enables User Consent backend for Web OTP API.

#### `--webrtc-event-logging`

- **符号**: `switches::kWebRtcLocalEventLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：无需访问 chrome://webrtc-internals 就能抓 WebRTC 事件日志到指定路径（自动化测试用）。
> Enable capture and local storage of WebRTC event logs without visiting chrome://webrtc-internals. This is useful for automated testing. It accepts the path to which the local logs would be stored. Disabling is not possible without restarting the browser and relaunching without this flag.

#### `--max-gum-fps`

- **符号**: `switches::kWebRtcMaxCaptureFramerate` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 getUserMedia 的最大帧率，如 `--max-gum-fps=17.5`。
> Override the maximum framerate as can be specified in calls to getUserMedia. This flag expects a value.  Example: --max-gum-fps=17.5

#### `--web-settings-for-testing`

- **符号**: `switches::kWebSettingsForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Web test 指定额外 web settings。
> Allows web tests to specify additional web settings for the test cases.

#### `--force-webxr-runtime`

- **符号**: `switches::kWebXrForceRuntime` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制启用并选择指定 WebXR runtime；其他 runtime 会被禁用。
> Forcibly enable and select the specified runtime for webxr. Note that this provides an alternative means of enabling a runtime, and will also functionally disable all other runtimes.

#### `--arcore`

- **符号**: `switches::kWebXrRuntimeArCore` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--force-webxr-runtime 取值：ARCore。
> The following are the runtimes that WebXr supports.

#### `--cardboard`

- **符号**: `switches::kWebXrRuntimeCardboard` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--force-webxr-runtime 取值：Cardboard。

#### `--no-xr-runtime`

- **符号**: `switches::kWebXrRuntimeNone` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：让 WebXR 当作没有任何 runtime 可用。
> Tell WebXr to assume that it does not support any runtimes.

#### `--openxr`

- **符号**: `switches::kWebXrRuntimeOpenXr` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--force-webxr-runtime 取值：OpenXR。

#### `--orientation-sensors`

- **符号**: `switches::kWebXrRuntimeOrientationSensors` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--force-webxr-runtime 取值：OrientationSensors。

#### `--webgl-antialiasing-mode`

- **符号**: `switches::kWebglAntialiasingMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：WebGL 抗锯齿模式：`none`/`explicit`/`implicit`。
> Set the antialiasing method used for webgl. (none, explicit, implicit)

#### `--webgl-msaa-sample-count`

- **符号**: `switches::kWebglMSAASampleCount` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：WebGL 开启 MSAA 时的默认采样数。
> Set a default sample count for webgl if msaa is enabled.

#### `--zygote-cmd-prefix`

- **符号**: `switches::kZygoteCmdPrefix` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Zygote 启动命令前缀（如 `gdb --args`）。
> The prefix used when starting the zygote process. (i.e. 'gdb --args')

#### `--zygote`

- **符号**: `switches::kZygoteProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `sandbox/policy/switches.cc`
- **用途**：子进程类型：Zygote。
> Causes the process to run as a zygote.


## 二、渲染 / 图形 / 合成 / Blink

涵盖 GPU 进程、GL/ANGLE/Vulkan 后端选择、Skia、合成器（cc / viz）、显示 (display)、字体 (gfx)、以及 Blink 渲染引擎相关开关。排错方向通常是：GPU 黑屏/花屏 → `gpu_switches` + `gl_switches`；滚动/动画卡顿 → `cc/base/switches` + `viz`；网页样式或 JS 行为异常 → Blink。


### 合成器 cc (cc/base/switches.cc)

Chromium Compositor (cc) 开关：tile 尺寸、滚动预测、合成调试可视化 (paint rects / layer borders) 等。

_开关数：33_


#### `--top-controls-hide-threshold`

- **符号**: `switches::kBrowserControlsHideThreshold` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（移动端）顶部控件需被隐藏多大百分比后才自动收起。
> Percentage of the browser controls need to be hidden before they will auto hide.

#### `--top-controls-show-threshold`

- **符号**: `switches::kBrowserControlsShowThreshold` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（移动端）顶部控件需被显示多大百分比后才自动展开。
> Percentage of the browser controls need to be shown before they will auto show.

#### `--cc-layer-tree-test-long-timeout`

- **符号**: `switches::kCCLayerTreeTestLongTimeout` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：增大 cc layer-tree 测试中内存检查器的超时。
> Increases timeout for memory checkers.

#### `--cc-layer-tree-test-no-timeout`

- **符号**: `switches::kCCLayerTreeTestNoTimeout` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 cc layer-tree 单元测试的超时。
> Prevents the layer tree unit tests from timing out.

#### `--cc-scroll-animation-duration-in-seconds`

- **符号**: `switches::kCCScrollAnimationDurationForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖滚动动画曲线的时长（秒）。
> Controls the duration of the scroll animation curve.

#### `--check-damage-early`

- **符号**: `switches::kCheckDamageEarly` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：提前检查 damage，无 damage 直接放弃该帧（避免 Android WebView 不必要的失效）。
> Checks damage early and aborts the frame if no damage, so that clients like Android WebView don't invalidate unnecessarily.

#### `--layer`

- **符号**: `switches::kCompositedLayerBorders` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--show-composited-layer-borders 取值：layer（按 layer 画边框）。

#### `--renderpass`

- **符号**: `switches::kCompositedRenderPassBorders` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--show-composited-layer-borders 取值：renderpass（按 render pass 画边框）。
> Parameters for kUIShowCompositedLayerBorders.

#### `--surface`

- **符号**: `switches::kCompositedSurfaceBorders` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--show-composited-layer-borders 取值：surface（按 surface 画边框）。

#### `--disable-checker-imaging`

- **符号**: `switches::kDisableCheckerImaging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁止把所有图像解码 defer 给 image decode service，忽略 PaintImage 的 DecodingMode 偏好。
> Disabled defering all image decodes to the image decode service, ignoring DecodingMode preferences specified on PaintImage.

#### `--disable-composited-antialiasing`

- **符号**: `switches::kDisableCompositedAntialiasing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭合成器中 layer 边缘的抗锯齿。
> Disables layer-edge anti-aliasing in the compositor.

#### `--disable-layer-tree-host-memory-pressure`

- **符号**: `switches::kDisableLayerTreeHostMemoryPressure` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 LayerTreeHost::OnMemoryPressure（内存压力来时不再丢资源）。
> Disables LayerTreeHost::OnMemoryPressure

#### `--disable-main-frame-before-activation`

- **符号**: `switches::kDisableMainFrameBeforeActivation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭“前一次 commit 激活前发送下一个 BeginMainFrame”，覆盖 --enable-main-frame-before-activation。
> Disables sending the next BeginMainFrame before the previous commit activates. Overrides the kEnableMainFrameBeforeActivation flag.

#### `--disable-threaded-animation`

- **符号**: `switches::kDisableThreadedAnimation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 cc 的线程化动画（动画退化到主线程跑）。

#### `--dump-compositor-frame`

- **符号**: `switches::kDumpCompositorFrame` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 Renderer 与 GPU 进程同时保存 CompositorFrame 数据（TreesInViz/TreeAnimationsInViz 用）；需 --no-sandbox；可指定 `begin,end` 帧范围，默认 1-10。
> Save CompositorFrame data in both renderer and gpu process for TreesInViz and TreeAnimationsInViz. This doesn't work without --no-sandbox. If no parameters are specified, by default, it records frame 1 - 10. If --dump-compositor-frame=begin,end is specified, it records frame [begin, end] inclusive.

#### `--enable-scaling-clipped-images`

- **符号**: `switches::kEnableClippedImageScaling` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许 GpuImageDecodeCache 缩放被裁剪图像（可能产生颜色溢色，临时绕过用）。
> Allows scaling clipped images in GpuImageDecodeCache. Note that this may cause color-bleeding. TODO(crbug.com/40160880): Remove this workaround flag once the underlying cache problems are solved.

#### `--enable-gpu-benchmarking`

- **符号**: `switches::kEnableGpuBenchmarking` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 GPU benchmarking 扩展。
> Enables the GPU benchmarking extension

#### `--enable-main-frame-before-activation`

- **符号**: `switches::kEnableMainFrameBeforeActivation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：前一次 commit 激活前就发送下一个 BeginMainFrame（流水线优化）。
> Enables sending the next BeginMainFrame before the previous commit activates.

#### `--log-on-ui-double-background-blur`

- **符号**: `switches::kLogOnUIDoubleBackgroundBlur` &nbsp;&nbsp; **Buildflag**: DCHECK_IS_ON()
- **用途**：若出现双重背景模糊则按错误打日志。
> Checks and logs double background blur as an error if any.

#### `--num-raster-threads`

- **符号**: `switches::kNumRasterThreads` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：光栅化任务使用的线程数。
> Controls the number of threads to use for raster tasks.

#### `--show-composited-layer-borders`

- **符号**: `switches::kShowCompositedLayerBorders` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为合成器 layer 画边框，便于调试 layer 合成。
> Renders a border around compositor layers to help debug and study layer compositing.

#### `--show-fps-counter`

- **符号**: `switches::kShowFPSCounter` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：右上角显示 FPS 与 GPU 内存占用 HUD；配合 `--enable-logging=stderr --vmodule="head*=1"` 还能输出到控制台。
> Draws a heads-up-display showing Frames Per Second as well as GPU memory usage. If you also use --enable-logging=stderr --vmodule="head*=1" then FPS will also be output to the console log.

#### `--show-layer-animation-bounds`

- **符号**: `switches::kShowLayerAnimationBounds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为 layer 动画的包围盒画边框。
> Renders a border that represents the bounding box for the layer's animation.

#### `--show-property-changed-rects`

- **符号**: `switches::kShowPropertyChangedRects` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 HUD 上为属性发生变化的 layer 画方框。
> Show rects in the HUD around layers whose properties have changed.

#### `--show-screenspace-rects`

- **符号**: `switches::kShowScreenSpaceRects` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 HUD 上画出每个 layer 屏幕空间变换后的包围盒。
> Show rects in the HUD around the screen-space transformed bounds of every layer.

#### `--show-surface-damage-rects`

- **符号**: `switches::kShowSurfaceDamageRects` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 HUD 上画出每个 render surface 记录到的 damage 区域。
> Show rects in the HUD around damage as it is recorded into each render surface.

#### `--slow-down-raster-scale-factor`

- **符号**: `switches::kSlowDownRasterScaleFactor` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把所有内容重复栅格化 N 次以模拟慢机器，例如 `--slow-down-raster-scale-factor=25`。
> Re-rasters everything multiple times to simulate a much slower machine. Give a scale factor to cause raster to take that many times longer to complete, such as --slow-down-raster-scale-factor=25.

#### `--ui-show-composited-layer-borders`

- **符号**: `switches::kUIShowCompositedLayerBorders` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：等价于 --show-composited-layer-borders，作用于 UI 合成。

#### `--ui-show-fps-counter`

- **符号**: `switches::kUIShowFPSCounter` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：等价于 --show-fps-counter，作用于 UI 合成。

#### `--ui-show-layer-animation-bounds`

- **符号**: `switches::kUIShowLayerAnimationBounds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：等价于 --show-layer-animation-bounds，作用于 UI 合成。

#### `--ui-show-property-changed-rects`

- **符号**: `switches::kUIShowPropertyChangedRects` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：等价于 --show-property-changed-rects，作用于 UI 合成。

#### `--ui-show-screenspace-rects`

- **符号**: `switches::kUIShowScreenSpaceRects` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：等价于 --show-screenspace-rects，作用于 UI 合成。

#### `--ui-show-surface-damage-rects`

- **符号**: `switches::kUIShowSurfaceDamageRects` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：等价于 --show-surface-damage-rects，作用于 UI 合成。


### Viz (components/viz/common/switches.cc)

显示合成服务 (viz) 开关，包括 SkiaRenderer、FrameRateLimit 等。

_开关数：10_


#### `--deadline-to-synchronize-surfaces`

- **符号**: `switches::kDeadlineToSynchronizeSurfaces` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：激活带依赖的 surface 时默认要等的 BeginFrame 帧数。
> The default number of the BeginFrames to wait to activate a surface with dependencies.

#### `--delegated-ink-renderer`

- **符号**: `switches::kDelegatedInkRenderer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制 Delegated Ink 渲染器：`skia`（默认）/`system`/`none`。
> Force the use of a Delegated Ink renderer as specified by the command line argument, rather than using system details. Acceptable values are: skia, system, none. Default to skia.

#### `--disable-adpf`

- **符号**: `switches::kDisableAdpf` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：即使设备支持也不通过 ADPF 上报帧时序。
> Disables reporting of frame timing via ADPF, even if supported on the device.

#### `--disable-frame-rate-limit`

- **符号**: `switches::kDisableFrameRateLimit` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 cc/display 调度器的 BeginFrame 限制；同时隐含 --disable-gpu-vsync。
> Disables begin frame limiting in both cc scheduler and display scheduler. Also implies --disable-gpu-vsync (see //ui/gl/gl_switches.h). TODO(ananta/jonross/sunnyps) http://crbug.com/346931323 We should remove or change this once VRR support is implemented for Windows and other platforms potentially.

#### `--double-buffer-compositing`

- **符号**: `switches::kDoubleBufferCompositing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 GL 缓冲队列的最大 pending frame 数设为 1（双缓冲）。
> Sets the number of max pending frames in the GL buffer queue to 1.

#### `--enable-hardware-overlays`

- **符号**: `switches::kEnableHardwareOverlays` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用硬件 overlay 合成；值 `single-fullscreen` 表示只把单一全屏 overlay 提升为主 framebuffer。
> Enable compositing individual elements via hardware overlays when permitted by device. Setting the flag to "single-fullscreen" will try to promote a single fullscreen overlay and use it as main framebuffer where possible.

#### `--run-all-compositor-stages-before-draw`

- **符号**: `switches::kRunAllCompositorStagesBeforeDraw` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：等每个合成阶段全部完成后再画下一帧（关闭流水线）。
> Effectively disables pipelining of compositor frame production stages by waiting for each stage to finish before completing a frame.

#### `--show-aggregated-damage`

- **符号**: `switches::kShowAggregatedDamage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在根 RenderPass 顶部加 DebugBorderDrawQuad 显示 surface 聚合后的 damage（注意：会把整块 output 标为 damaged）。
> Adds a DebugBorderDrawQuad to the top of the root RenderPass showing the damage rect after surface aggregation. Note that when enabled this feature sets the entire output rect as damaged after adding the quad to highlight the real damage rect, which could hide damage rect problems.

#### `--show-dc-layer-debug-borders`

- **符号**: `switches::kShowDCLayerDebugBorders` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为 DirectComposition layer 画调试边框：overlay 红、underlay 蓝。
> Show debug borders for DC layers - red for overlays and blue for underlays. The debug borders are offset from the layer rect by a few pixels for clarity.

#### `--tint-composited-content-modulate`

- **符号**: `switches::kTintCompositedContentModulate` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：调制合成染色，让 damage 与 page flip 更醒目（需配合 --tint-composited-content）。
> Modulates the debug compositor tint color so that damage and page flip updates are made clearly visible. This feature was useful in determining the root cause of https://b.corp.google.com/issues/183260320 . The tinting flag "tint-composited-content" must also be enabled for this flag to used.


### Viz Demo (components/viz/demo/common/switches.cc)

viz demo 可执行文件专用开关。

_开关数：1_


#### `--viz-demo-use-gpu`

- **符号**: `switches::kVizDemoUseGPU` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：viz demo 使用 GPU 渲染。


### GPU Command Buffer Client (gpu/command_buffer/client/gpu_switches.cc)

GPU 命令缓冲区客户端开关。

_开关数：2_


#### `--enable-gpu-client-logging`

- **符号**: `switches::kEnableGPUClientLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 GPU client 端日志。
> Enable GPU client logging.

#### `--enable-gpu-client-tracing`

- **符号**: `switches::kEnableGpuClientTracing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 Renderer 里把 GL 调用打 TRACE。
> Enables TRACE for GL calls in the renderer.


### GPU Command Buffer Service (gpu/command_buffer/service/gpu_switches.cc)

GPU 命令缓冲区服务端开关。

_开关数：18_


#### `--compile-shader-always-succeeds`

- **符号**: `switches::kCompileShaderAlwaysSucceeds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：shader 编译永远返回成功（链接仍可能失败）。
> Always return success when compiling a shader. Linking will still fail.

#### `--disable-gl-error-limit`

- **符号**: `switches::kDisableGLErrorLimit` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 GL 错误日志数量上限。
> Disable the GL error log limit.

#### `--disable-glsl-translator`

- **符号**: `switches::kDisableGLSLTranslator` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 GLSL translator。
> Disable the GLSL translator.

#### `--disable-gpu-program-cache`

- **符号**: `switches::kDisableGpuProgramCache` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 GPU program 缓存。
> Turn off gpu program caching

#### `--disable-shader-name-hashing`

- **符号**: `switches::kDisableShaderNameHashing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 shader 中用户定义名称的哈希。
> Turn off user-defined name hashing in shaders.

#### `--disable-vulkan-surface`

- **符号**: `switches::kDisableVulkanSurface` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 VK_KHR_surface 扩展，改用 bitblt 上屏。
> Disables VK_KHR_surface extension. Instead of using swapchain, bitblt will be used for present render result on screen.

#### `--enable-gpu-command-logging`

- **符号**: `switches::kEnableGPUCommandLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把所有 GPU 命令记入日志。
> Turn on Logging GPU commands.

#### `--enable-gpu-debugging`

- **符号**: `switches::kEnableGPUDebugging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：每条 GL 命令后调用 glGetError。
> Turn on Calling GL Error after every command.

#### `--enable-gpu-driver-debug-logging`

- **符号**: `switches::kEnableGPUDriverDebugLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 GPU 驱动调试消息日志。
> Enable logging of GPU driver debug messages.

#### `--enable-gpu-service-logging`

- **符号**: `switches::kEnableGPUServiceLoggingGPU` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `ui/gl/gl_switches.cc`
- **用途**：启用 GPU service 端日志（与 gl_switches.cc 同名，重复定义以避免 dll 依赖）。
> Enable GPU service logging. Note: This is the same switch as the one in gl_switches.cc. It's defined here again to avoid dependencies between dlls.

#### `--enable-threaded-texture-mailboxes`

- **符号**: `switches::kEnableThreadedTextureMailboxes` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在没有 share group 时模拟共享纹理（不一定所有平台可用）。
> Simulates shared textures when share groups are not available. Not available everywhere.

#### `--enforce-gl-minimums`

- **符号**: `switches::kEnforceGLMinimums` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制使用 GL 最小值（按规范下限做能力裁剪）。
> Enforce GL minimums.

#### `--force-gpu-mem-discardable-limit-mb`

- **符号**: `switches::kForceGpuMemDiscardableLimitMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 上 discardable 缓存的最大字节数（MB）。
> Sets the maximum GPU memory to use for discardable caches.

#### `--force-max-texture-size`

- **符号**: `switches::kForceMaxTextureSize` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制设置最大纹理尺寸（像素）。
> Sets the maximum texture size in pixels.

#### `--gl-shader-interm-output`

- **符号**: `switches::kGLShaderIntermOutput` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：shader 编译信息日志中包含 ANGLE 的中间表示（AST）。
> Include ANGLE's intermediate representation (AST) output in shader compilation info logs.

#### `--gpu-program-cache-size-kb`

- **符号**: `switches::kGpuProgramCacheSizeKb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU program 内存缓存上限（KB）。
> Sets the maximum size of the in-memory gpu program cache, in kb

#### `--use-vulkan`

- **符号**: `switches::kUseVulkan` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Vulkan 并选择实现（需 ENABLE_VULKAN，且通常需要 `--enable-features=Vulkan` 才用 Vulkan 做合成/光栅化）。
> Enable Vulkan support and select Vulkan implementation, must also have ENABLE_VULKAN defined. This only initializes Vulkan, the flag --enable-features=Vulkan must also be used to select Vulkan for compositing and rasterization.

#### `--swiftshader`

- **符号**: `switches::kVulkanImplementationNameSwiftshader` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `ui/gl/gl_switches.cc`
- **用途**：--use-vulkan 取值：使用 SwiftShader 实现。


### GPU 配置 (gpu/config/gpu_switches.cc)

GPU 进程的黑名单/白名单、活动驱动信息、GPU 启动参数等。

_开关数：51_


#### `--collect-dawn-info-eagerly`

- **符号**: `switches::kCollectDawnInfoEagerly` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Chrome 启动后立即启动 GPU 进程收集 Dawn 信息（默认延迟 120 秒）。
> Start the GPU process for Dawn info collection immediately after the browser starts. The default is to delay for 120 seconds.

#### `--disable-dawn-features`

- **符号**: `switches::kDisableDawnFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在创建 Dawn 设备时禁用指定的 Dawn features（toggles）。
> Set the Dawn features(toggles) disabled on the creation of Dawn devices.

#### `--disable-gpu-process-for-dx12-info-collection`

- **符号**: `switches::kDisableGpuProcessForDX12InfoCollection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用用于 DX12 信息收集的非沙箱 GPU 进程。
> Disables the non-sandboxed GPU process for DX12 info collection

#### `--disable-gpu-rasterization`

- **符号**: `switches::kDisableGpuRasterization` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 GPU 光栅化，仅 CPU 光栅化（覆盖 --enable-gpu-rasterization）。
> Disable GPU rasterization, i.e. rasterize on the CPU only. Overrides the kEnableGpuRasterization flag.

#### `--disable-gpu-shader-disk-cache`

- **符号**: `switches::kDisableGpuShaderDiskCache` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 GPU shader 磁盘缓存。
> Disables the GPU shader on disk cache.

#### `--disable-mipmap-generation`

- **符号**: `switches::kDisableMipmapGeneration` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 Skia 的 mipmap 生成（针对部分低内存设备的 workaround）。
> Disables mipmap generation in Skia. Used a workaround for select low memory devices, see https://crbug.com/1138979 for details.

#### `--disable-skia-graphite`

- **符号**: `switches::kDisableSkiaGraphite` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制禁用 Skia Graphite（与 enable 同时给则禁用优先）。
> Force disabling/enabling Skia Graphite. Disabling will take precedence over enabling if both are specified.

#### `--disable-skia-graphite-precompilation`

- **符号**: `switches::kDisableSkiaGraphitePrecompilation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制禁用 Skia Graphite 的 Pipeline Precompilation。
> Force disabling/enabling Skia Graphite's Pipeline Precompilation. Disabling will take precedence over enabling if both are specified.

#### `--disable-vulkan-fallback-to-gl-for-testing`

- **符号**: `switches::kDisableVulkanFallbackToGLForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Vulkan 初始化失败时不回退到 GL（用于测试 Vulkan 回归）。
> Disables falling back to GL based hardware rendering if initializing Vulkan fails. This is to allow tests to catch regressions in Vulkan.

#### `--enable-dawn-backend-validation`

- **符号**: `switches::kEnableDawnBackendValidation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Dawn 后端的 validation layer。
> Enable validation layers in Dawn backends.

#### `--enable-dawn-features`

- **符号**: `switches::kEnableDawnFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在创建 Dawn 设备时启用指定 Dawn features（toggles）。
> Set the Dawn features(toggles) enabled on the creation of Dawn devices.

#### `--enable-gpu-blocked-time`

- **符号**: `switches::kEnableGpuBlockedTime` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测量 GPU Main 线程在两次 SwapBuffers 之间被阻塞的时长。
> Enables measures of how long GPU Main Thread was blocked between SwapBuffers

#### `--enable-gpu-main-time-keeper-metrics`

- **符号**: `switches::kEnableGpuMainTimeKeeperMetrics` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 ThreadController 的 TimeKeeper UMA 指标，后缀 CrGpuMain。
> Enables ThreadControllerWithMessagePumpImpl's TimeKeeper UMA metrics using CrGpuMain as suffix.

#### `--enable-gpu-rasterization`

- **符号**: `switches::kEnableGpuRasterization` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启发式选择哪些 layer tile 用 Skia GPU 后端绘制（仅 GPU 加速合成下有效）。
> Allow heuristics to determine when a layer tile should be drawn with the Skia GPU backend. Only valid with GPU accelerated compositing.

#### `--enable-skia-graphite`

- **符号**: `switches::kEnableSkiaGraphite` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制启用 Skia Graphite。

#### `--enable-skia-graphite-precompilation`

- **符号**: `switches::kEnableSkiaGraphitePrecompilation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制启用 Skia Graphite 的 Pipeline Precompilation。

#### `--enable-unsafe-webgpu`

- **符号**: `switches::kEnableUnsafeWebGPU` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用未审定的 WebGPU 特性（不安全，仅开发/测试用）。

#### `--enable-vulkan-protected-memory`

- **符号**: `switches::kEnableVulkanProtectedMemory` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Vulkan 资源使用 protected memory。
> Enables using protected memory for vulkan resources.

#### `--enable-webgpu-developer-features`

- **符号**: `switches::kEnableWebGPUDeveloperFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 WebGPU 开发者特性（不对外公开）。
> Enables WebGPU developer features which are not generally exposed to the web platform.

#### `--force-browser-crash-on-gpu-crash`

- **符号**: `switches::kForceBrowserCrashOnGpuCrash` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 进程意外崩溃时强制 Chrome 崩溃（让测试明确失败）。
> Crash Chrome if GPU process crashes. This is to force a test to fail when GPU process crashes unexpectedly.

#### `--force-high-performance-gpu`

- **符号**: `switches::kForceHighPerformanceGPU` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在多 GPU 系统上强制选高性能 GPU。

#### `--force-separate-egl-display-for-webgl-testing`

- **符号**: `switches::kForceSeparateEGLDisplayForWebGLTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为 WebGL context 强制使用独立 EGL display（在单 GPU 设备上测试多 GPU 路径）。
> Force the use of a separate EGL display for WebGL contexts. Used for testing multi-GPU pathways on devices with only one valid GPU.

#### `--force-webgpu-compat`

- **符号**: `switches::kForceWebGPUCompat` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制所有 WebGPU 内容跑在 Compatibility 模式。
> Force all WebGPU content to run in WebGPU Compatibility mode.

#### `--gpu-blocklist-test-group`

- **符号**: `switches::kGpuBlocklistTestGroup` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：选择带特定 test_group ID 的另一组 GPU blocklist 条目。
> Select a different set of GPU blocklist entries with the specified test_group ID.

#### `--gpu-device-id`

- **符号**: `switches::kGpuDeviceId` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把活动显卡 device id 从浏览器进程传给 info-collection GPU 进程。
> Passes the active graphics device id from browser process to info collection GPU process.

#### `--gpu-disk-cache-size-kb`

- **符号**: `switches::kGpuDiskCacheSizeKB` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：显式设置 shader 磁盘缓存大小（KB）；默认 6MB（Android 默认 2MB，低端 128KB）。
> Allows explicitly specifying the shader disk cache size for embedded devices. Default value is 6MB. On Android, 2MB is default and 128KB for low-end devices.

#### `--gpu-driver-bug-list-test-group`

- **符号**: `switches::kGpuDriverBugListTestGroup` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：额外启用带特定 test_group ID 的 GPU driver bug list 条目（默认 group 0 仍生效）。
> Enable an extra set of GPU driver bug list entries with the specified test_group ID. Note the default test group (group 0) is still active.

#### `--gpu-driver-version`

- **符号**: `switches::kGpuDriverVersion` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把活动显卡驱动版本从浏览器进程传给 info-collection GPU 进程。
> Passes the active graphics driver version from browser process to info collection GPU process.

#### `--gpu-preferences`

- **符号**: `switches::kGpuPreferences` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把编码后的 GpuPreferences 传给 GPU 进程。
> Passes encoded GpuPreferences to GPU process.

#### `--gpu-revision`

- **符号**: `switches::kGpuRevision` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把活动显卡 revision 信息从浏览器进程传给 info-collection GPU 进程。
> Passes the active graphics revision info from browser process to info collection GPU process.

#### `--gpu-sub-system-id`

- **符号**: `switches::kGpuSubSystemId` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把活动显卡 subsystem id 从浏览器进程传给 info-collection GPU 进程。
> Passes the active graphics sub system id from browser process to info collection GPU process.

#### `--gpu-vendor-id`

- **符号**: `switches::kGpuVendorId` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把活动显卡 vendor id 从浏览器进程传给 info-collection GPU 进程。
> Passes the active graphics vendor id from browser process to info collection GPU process.

#### `--gpu-watchdog-timeout-seconds`

- **符号**: `switches::kGpuWatchdogTimeoutSeconds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 GPU watchdog 超时（秒）。
> Override value for the GPU watchdog timeout in seconds.

#### `--ignore-gpu-blocklist`

- **符号**: `switches::kIgnoreGpuBlocklist` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：忽略 GPU blocklist（即使硬件被列入也强制启用 GPU 加速）。
> Ignores GPU blocklist.

#### `--no-delay-for-dx12-vulkan-info-collection`

- **符号**: `switches::kNoDelayForDX12VulkanInfoCollection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Chrome 启动后立即启动用于 DX12/Vulkan 信息收集的非沙箱 GPU 进程（默认延迟 120 秒）。
> Start the non-sandboxed GPU process for DX12 and Vulkan info collection immediately after the browser starts. The default is to delay for 120 seconds.

#### `--skia-graphite-backend`

- **符号**: `switches::kSkiaGraphiteBackend` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Skia Graphite 后端：`dawn`（默认）或 `metal`（仅非官方开发版）。
> Specify which backend to use for Skia Graphite - "dawn" (default) or "metal" (only allowed on non-official developer builds).

#### `--dawn`

- **符号**: `switches::kSkiaGraphiteBackendDawn` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--skia-graphite-backend 取值：Dawn。

#### `--dawn-d3d11`

- **符号**: `switches::kSkiaGraphiteBackendDawnD3D11` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-webgpu-adapter 取值：Dawn D3D11。

#### `--dawn-d3d12`

- **符号**: `switches::kSkiaGraphiteBackendDawnD3D12` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-webgpu-adapter 取值：Dawn D3D12。

#### `--dawn-metal`

- **符号**: `switches::kSkiaGraphiteBackendDawnMetal` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-webgpu-adapter 取值：Dawn Metal。

#### `--dawn-opengles`

- **符号**: `switches::kSkiaGraphiteBackendDawnOpenGLES` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-webgpu-adapter 取值：Dawn OpenGL ES。

#### `--dawn-swiftshader`

- **符号**: `switches::kSkiaGraphiteBackendDawnSwiftshader` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-webgpu-adapter 取值：Dawn SwiftShader（Vulkan）。

#### `--dawn-vulkan`

- **符号**: `switches::kSkiaGraphiteBackendDawnVulkan` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-webgpu-adapter 取值：Dawn Vulkan。

#### `--metal`

- **符号**: `switches::kSkiaGraphiteBackendMetal` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `ui/gl/gl_switches.cc`
- **用途**：--skia-graphite-backend / WebGPU 取值：Metal。

#### `--suppress-performance-logs`

- **符号**: `switches::kSuppressPerformanceLogs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：屏蔽 GL_DEBUG_TYPE_PERFORMANCE 日志（避免 web test 输出比对失败）。
> Suppresses GL_DEBUG_TYPE_PERFORMANCE log messages for web tests that can get sent to the JS console and cause unnecessary test failures due test output log expectation comparisons.

#### `--use-redist-dml`

- **符号**: `switches::kUseRedistributableDirectML` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：尝试加载可分发版 DirectML.dll（用于在新 DirectML 集成进 Windows 之前测试 WebNN）。
> Try to use a redistributable DirectML.dll. Used for testing WebNN against newer DirectML release before it is integrated into Windows OS. Please see more info about DirectML releases at: https://learn.microsoft.com/en-us/windows/ai/directml/dml-version-history

#### `--use-webgpu-adapter`

- **符号**: `switches::kUseWebGPUAdapter` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：WebGPU 内容使用的 adapter。
> The adapter to use for WebGPU content.

#### `--use-webgpu-power-preference`

- **符号**: `switches::kUseWebGPUPowerPreference` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：WebGPU adapter 的 power preference 选择策略。
> The adapter selecting strategy related to GPUPowerPreference.

#### `--vulkan-heap-memory-limit-mb`

- **符号**: `switches::kVulkanHeapMemoryLimitMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Vulkan 内存堆上限（MB）。
> Specifies the heap limit for Vulkan memory. TODO(crbug.com/40161102): Remove this switch.

#### `--vulkan-sync-cpu-memory-limit-mb`

- **符号**: `switches::kVulkanSyncCpuMemoryLimitMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Vulkan 内存同步到 CPU 的上限（MB）。
> Specifies the sync CPU limit for total Vulkan memory. TODO(crbug.com/40161102): Remove this switch.

#### `--webview-draw-functor-uses-vulkan`

- **符号**: `switches::kWebViewDrawFunctorUsesVulkan` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Android WebView 内部）draw functor 使用 Vulkan。
> Indicate that the this is being used by Android WebView and its draw functor is using vulkan.


### Skia (skia/ext/switches.cc)

Skia 渲染相关开关。

_开关数：2_


#### `--text-contrast`

- **符号**: `switches::kTextContrast` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Skia 文本对比度参数。

#### `--text-gamma`

- **符号**: `switches::kTextGamma` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Skia 文本 gamma 参数。


### Blink (third_party/blink/common/switches.cc)

Blink 渲染引擎开关：blink-settings、dark-mode、tile 尺寸、JS runtime flags、站点隔离例外等。

_开关数：39_


#### `--allow-pre-commit-input`

- **符号**: `switches::kAllowPreCommitInput` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许在帧提交前处理输入事件（headless 使用）。
> Allows processing of input before a frame has been committed. TODO(crbug.com/987626): Used by headless. Look for a way not involving a command line switch.

#### `--blink-settings`

- **符号**: `switches::kBlinkSettings` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：设置 Blink Settings，格式 `name[=value],...`；布尔省略 `=value` 即 true，枚举用整数。
> Set blink settings. Format is <name>[=<value],<name>[=<value>],... The names are declared in Settings.json5. For boolean type, use "true", "false", or omit '=<value>' part to set to true. For enum type, use the int value of the enum value. Applied after other command line flags and prefs.

#### `--css-custom-state-deprecated-syntax-enabled`

- **符号**: `switches::kCSSCustomStateDeprecatedSyntaxEnabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为 CSSCustomStateDeprecatedSyntax 提供企业策略覆盖通道。
> Used to communicate managed policy for CSSCustomStateDeprecatedSyntax. This feature is typically controlled by a RuntimeEnabledFeature, but requires an enterprise policy override.

#### `--dark-mode-settings`

- **符号**: `switches::kDarkModeSettings` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：设置暗色模式参数，例 `InversionAlgorithm=<int>,ImagePolicy=<int>,ForegroundBrightnessThreshold=0..255,BackgroundBrightnessThreshold=0..255,ContrastPercent=-1.0..1.0`。
> Sets dark mode settings. Format is [<param>=<value>],[<param>=<value>],... The params take either int or float values. If params are not specified, the default dark mode settings is used. Valid params are given below. "InversionAlgorithm" takes int value of DarkModeInversionAlgorithm enum. "ImagePolicy" takes int value of DarkModeImagePolicy enum. "ForegroundBrightnessThreshold" takes 0 to 255 int value. "BackgroundBrightnessThreshold" takes 0 to 255 int value.
> "ContrastPercent" takes -1.0 to 1.0 float value. Higher the value, more the contrast.

#### `--data-url-in-svg-use-enabled`

- **符号**: `switches::kDataUrlInSvgUseEnabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过企业策略覆盖“SVGUseElement 中 data: URL”弃用开关。
> Overrides data: URLs in SVGUseElement deprecation through enterprise policy.

#### `--default-tile-height`

- **符号**: `switches::kDefaultTileHeight` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：合成 layer 默认 tile 高度。

#### `--default-tile-width`

- **符号**: `switches::kDefaultTileWidth` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：合成 layer 默认 tile 宽度。
> Sets the tile size used by composited layers.

#### `--disable-blob-url-partitioning`

- **符号**: `switches::kDisableBlobUrlPartitioning` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过企业策略切换 Blob URL 的 partition 行为。
> Toggles partitioning of Blob URLs through enterprise policy.

#### `--disable-image-animation-resync`

- **符号**: `switches::kDisableImageAnimationResync` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁止把图像动画重置到起点（避免跳过很多帧），仅在合成器图像动画启用时生效。
> Disallow image animations to be reset to the beginning to avoid skipping many frames. Only effective if compositor image animations are enabled.

#### `--disable-partial-raster`

- **符号**: `switches::kDisablePartialRaster` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 Renderer 的部分光栅化（也禁用 persistent GPU memory buffer）。
> Disable partial raster in the renderer. Disabling this switch also disables the use of persistent gpu memory buffers.

#### `--disable-prefer-compositing-to-lcd-text`

- **符号**: `switches::kDisablePreferCompositingToLCDText` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为保证 LCD 文本清晰而阻止创建合成层。
> Disable the creation of compositing layers when it would prevent LCD text.

#### `--disable-rgba-4444-textures`

- **符号**: `switches::kDisableRGBA4444Textures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 RGBA_4444 纹理。
> Disables RGBA_4444 textures.

#### `--disable-reduce-accept-language`

- **符号**: `switches::kDisableReduceAcceptLanguage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过企业策略覆盖 ReduceAcceptLanguage 特性。
> Override mechanism for ReduceAcceptLanguage. This feature is typically controlled by base features, but requires an enterprise policy override.

#### `--disable-standardized-browser-zoom`

- **符号**: `switches::kDisableStandardizedBrowserZoom` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：保留旧版非标 CSS zoom 行为（企业策略覆盖通道）。
> Override mechanism for preserving the old non-standard behavior of CSS zoom.

#### `--disable-zero-copy`

- **符号**: `switches::kDisableZeroCopy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 zero-copy 光栅化（不再直接写到 tile 关联的 GPU 内存）。
> Disable rasterizer that writes directly to GPU memory associated with tiles.

#### `--dump-blink-runtime-call-stats`

- **符号**: `switches::kDumpRuntimeCallStats` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打印 Blink Runtime Call Stats（需配合 --single-process）。
> Logs Runtime Call Stats. --single-process also needs to be used along with this for the stats to be logged.

#### `--enable-gpu-memory-buffer-compositor-resources`

- **符号**: `switches::kEnableGpuMemoryBufferCompositorResources` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：所有合成器资源都由 GPU memory buffer 承载。
> Specify that all compositor resources should be backed by GPU memory buffers.

#### `--enable-leak-detection-heap-snapshot`

- **符号**: `switches::kEnableLeakDetectionHeapSnapshot` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：使用 leak detection 时生成 heap snapshot 并 dump 到文件。
> Enables taking a heap snapshot and dumping it to file when using leak detection.

#### `--enable-prefer-compositing-to-lcd-text`

- **符号**: `switches::kEnablePreferCompositingToLCDText` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在可能影响 LCD 文本时也允许创建合成层。
> Enable the creation of compositing layers when it would prevent LCD text.

#### `--enable-rgba-4444-textures`

- **符号**: `switches::kEnableRGBA4444Textures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 RGBA_4444 纹理。
> Enables RGBA_4444 textures.

#### `--enable-raster-side-dark-mode-for-images`

- **符号**: `switches::kEnableRasterSideDarkModeForImages` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在光栅化侧对图像启用暗色模式。
> Enables raster side dark mode for images.

#### `--enable-zero-copy`

- **符号**: `switches::kEnableZeroCopy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 zero-copy 光栅化（直接写 tile 关联的 GPU 内存）。
> Enable rasterizer that writes directly to GPU memory associated with tiles.

#### `--force-gpu-mem-available-mb`

- **符号**: `switches::kForceGpuMemAvailableMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：cc 中 GPU 资源可用总内存上限（MB）。
> Sets the total amount of memory that may be allocated for GPU resources in cc.

#### `--gpu-rasterization-msaa-sample-count`

- **符号**: `switches::kGpuRasterizationMSAASampleCount` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 光栅化 MSAA 采样数（0 关闭）。
> The number of multisample antialiasing samples for GPU rasterization. Requires MSAA support on GPU to have an effect. 0 disables MSAA.

#### `--intensive-wake-up-throttling-policy`

- **符号**: `switches::kIntensiveWakeUpThrottlingPolicy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：IntensiveWakeUpThrottling 的企业策略覆盖：`1` 强制启用、`0` 强制关闭、未设则走默认。
> Used to communicate managed policy for the IntensiveWakeUpThrottling feature. This feature is typically controlled by base::Feature (see renderer/platform/scheduler/common/features.*) but requires an enterprise policy override. This is implicitly a tri-state, and can be either unset, or set to "1" for force enable, or "0" for force disable.

#### `--0`

- **符号**: `switches::kIntensiveWakeUpThrottlingPolicy_ForceDisable` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--intensive-wake-up-throttling-policy 取值：0（强制禁用）。

#### `--1`

- **符号**: `switches::kIntensiveWakeUpThrottlingPolicy_ForceEnable` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--intensive-wake-up-throttling-policy 取值：1（强制启用）。

#### `--js-flags`

- **符号**: `switches::kJavaScriptFlags` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：传给 JS 引擎（V8）的 flags。
> Specifies the flags passed to JS engine.

#### `--legacy-tech-report-policy-enabled`

- **符号**: `switches::kLegacyTechReportPolicyEnabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：标记是否设置了 legacy tech report URL；设置后 Blink 会向浏览器进程发送 report。
> A command line to indicate if there ia any legacy tech report urls being set. If so, we will send report from blink to browser process.

#### `--max-untiled-layer-height`

- **符号**: `switches::kMaxUntiledLayerHeight` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：超过该高度的合成 layer 才分 tile。
> Sets the width and height above which a composited layer will get tiled.

#### `--max-untiled-layer-width`

- **符号**: `switches::kMaxUntiledLayerWidth` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：超过该宽度的合成 layer 才分 tile。

#### `--min-height-for-gpu-raster-tile`

- **符号**: `switches::kMinHeightForGpuRasterTile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 光栅化 tile 的最小高度。
> Sets the min tile height for GPU raster.

#### `--network-quiet-timeout`

- **符号**: `switches::kNetworkQuietTimeout` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：IdlenessDetector 中 network-quiet 定时器的超时秒数（用于 FirstMeaningfulPaint 等信号）。
> Sets the timeout seconds of the network-quiet timers in IdlenessDetector. Used by embedders who want to change the timeout time in order to run web contents on various embedded devices and changeable network bandwidths in different regions. For example, it's useful when using FirstMeaningfulPaint signal to dismiss a splash screen.

#### `--show-layout-shift-regions`

- **符号**: `switches::kShowLayoutShiftRegions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：网页中显式为 layout shift 区域画边框。
> Visibly render a border around layout shift rects in the web page to help debug and study layout shifts.

#### `--show-paint-rects`

- **符号**: `switches::kShowPaintRects` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：网页中显式为 paint 区域画边框。
> Visibly render a border around paint rects in the web page to help debug and study painting behavior.

#### `--touch-selection-strategy`

- **符号**: `switches::kTouchTextSelectionStrategy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：触摸选择手柄拖动时文字选择的粒度：`character` 或 `direction`。
> Controls how text selection granularity changes when touch text selection handles are dragged. Should be "character" or "direction". If not specified, the platform default is used.

#### `--character`

- **符号**: `switches::kTouchTextSelectionStrategy_Character` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--touch-selection-strategy 取值：按字符。

#### `--direction`

- **符号**: `switches::kTouchTextSelectionStrategy_Direction` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--touch-selection-strategy 取值：按方向。

#### `--web-audio-bypass-output-buffering-opt-out`

- **符号**: `switches::kWebAudioBypassOutputBufferingOptOut` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：WebAudioBypassOutputBuffering 的企业策略覆盖。
> Used to communicate managed policy for WebAudioBypassOutputBuffering.  This feature is typically controlled by a RuntimeEnabledFeature, but requires an enterprise policy override.


### UI Base (ui/base/ui_base_switches.h)

ui/base 通用 UI 开关，含平台特定 (Win/Mac/Linux) 分支。

_开关数：20_


#### `--drm-virtual-connector-is-external`

- **符号**: `switches::kDRMVirtualConnectorIsExternal` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 DRM 虚拟 connector 视为外接显示器（VM 中切换显示模式用）。
> Treats DRM virtual connector as external to enable display mode change in VM.

#### `--disable-gtk-ime`

- **符号**: `switches::kDisableGtkIme` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX)
- **用途**：禁用 GTK IME 集成。
> Disables GTK IME integration.

#### `--disable-modal-animations`

- **符号**: `switches::kDisableModalAnimations` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：禁用模态对话框显示/隐藏动画。
> Disable animations for showing and hiding modal dialogs.

#### `--disallow-non-exact-resource-reuse`

- **符号**: `switches::kDisallowNonExactResourceReuse` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ResourcePool 请求时不复用尺寸不完全匹配的资源（layout/pixel 测试用，降低噪声）。
> Test related. Disable re-use of non-exact resources to fulfill ResourcePool requests. Intended only for use in layout or pixel tests to reduce noise.

#### `--force-caption-style`

- **符号**: `switches::kForceCaptionStyle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制 WebVTT 字幕样式。
> Forces the caption style for WebVTT captions.

#### `--force-dark-mode`

- **符号**: `switches::kForceDarkMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在支持平台上强制 UI 深色模式。
> Forces dark mode in UI for platforms that support it.

#### `--force-high-contrast`

- **符号**: `switches::kForceHighContrast` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：无视系统设置，强制原生 UI 与网页内容启用高对比度。
> Forces high-contrast mode for native UI and web content, regardless of system settings.

#### `--gtk-version`

- **符号**: `switches::kGtkVersionFlag` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX)
- **用途**：（Linux）指定要加载的 GTK 版本。
> Specify the GTK version to be loaded.

#### `--lang`

- **符号**: `switches::kLang` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定要加载的语言（语言文件），格式 `language[-country]`。Linux 用 LC_*/LANG 环境变量代替。
> The language file that we want to try to open. Of the form language[-country] where language is the 2 letter code from ISO-639. On Linux, this flag does not work; use the LC_*/LANG environment variables instead.

#### `--mangle-localized-strings`

- **符号**: `switches::kMangleLocalizedStrings` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把本地化字符串变长并加首尾标记，便于发现截断问题。
> Transform localized strings to be longer, with beginning and end markers to make truncation visually apparent.

#### `--qt-version`

- **符号**: `switches::kQtVersionFlag` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX)
- **用途**：（Linux）指定要加载的 Qt 版本。
> Specify the QT version to be loaded.

#### `--show-mac-overlay-borders`

- **符号**: `switches::kShowMacOverlayBorders` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：（macOS）为对应 overlay/局部 damage 的 CALayer 画边框。
> Show borders around CALayers corresponding to overlays and partial damage.

#### `--show-overdraw-feedback`

- **符号**: `switches::kShowOverdrawFeedback` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用颜色可视化 overdraw（蓝=1次/绿=2次/粉=3次/红≥4次）。
> Visualize overdraw by color-coding elements based on if they have other elements drawn underneath. This is good for showing where the UI might be doing more rendering work than necessary. The colors are hinting at the amount of overdraw on your screen for each pixel, as follows: True color: No overdraw. Blue: Overdrawn once. Green: Overdrawn twice. Pink: Overdrawn three times. Red: Overdrawn four or more times.

#### `--slow-down-compositing-scale-factor`

- **符号**: `switches::kSlowDownCompositingScaleFactor` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：重复绘制 N 次模拟慢机器，例如 `--slow-down-compositing-scale-factor=2`。
> Re-draw everything multiple times to simulate a much slower machine. Give a slow down factor to cause renderer to take that many times longer to complete, such as --slow-down-compositing-scale-factor=2.

#### `--system-font-family`

- **符号**: `switches::kSystemFontFamily` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS)
- **用途**：指定系统字体族名（headless 渲染确定性）。
> Specifies system font family name. Improves determinism when rendering pages in headless mode.

#### `--tint-composited-content`

- **符号**: `switches::kTintCompositedContent` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：给合成内容染色，配合 --tint-composited-content-modulate 使 damage 可视。
> Tint composited color.

#### `--top-chrome-touch-ui`

- **符号**: `switches::kTopChromeTouchUi` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：顶部 Chrome UI 的触屏优化布局开关。
> Controls touch-optimized UI layout for top chrome.

#### `--ui-disable-partial-swap`

- **符号**: `switches::kUIDisablePartialSwap` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用部分 swap（部分 OpenGL 驱动/模拟器需要）。
> Disable partial swap which is needed for some OpenGL drivers / emulators.

#### `--ui-toolkit`

- **符号**: `switches::kUiToolkitFlag` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX)
- **用途**：（Linux）指定 GUI 用的 toolkit。
> Specify the toolkit used to construct the Linux GUI.

#### `--use-system-clipboard`

- **符号**: `switches::kUseSystemClipboard` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（linux-chromeos）启用 ozone x11 剪贴板。
> Enables the ozone x11 clipboard for linux-chromeos.


### UI Compositor (ui/compositor/compositor_switches.cc)

ui/compositor 层开关。

_开关数：7_


#### `--disable-vsync-for-tests`

- **符号**: `switches::kDisableVsyncForTests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试用：关闭合成器 VSync。

#### `--enable-pixel-output-in-tests`

- **符号**: `switches::kEnablePixelOutputInTests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试用：强制产出像素输出。
> Forces tests to produce pixel output when they normally wouldn't.

#### `--ui-disable-zero-copy`

- **符号**: `switches::kUIDisableZeroCopy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：UI 合成关闭 zero-copy 上传。

#### `--ui-enable-rgba-4444-textures`

- **符号**: `switches::kUIEnableRGBA4444Textures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：UI 合成启用 RGBA-4444 纹理。

#### `--ui-enable-zero-copy`

- **符号**: `switches::kUIEnableZeroCopy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：UI 合成启用 zero-copy 上传。

#### `--ui-show-paint-rects`

- **符号**: `switches::kUIShowPaintRects` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 UI 上显示重绘区矩形。

#### `--ui-slow-animations`

- **符号**: `switches::kUISlowAnimations` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：UI 动画放慢（调试动画）。


### Display (ui/display/display_switches.cc)

显示器 / 缩放 / 强制 DPI 等开关。

_开关数：9_


#### `--ash-enable-software-mirroring`

- **符号**: `switches::kEnableSoftwareMirroring` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用基于软件的镜像显示。
> Enables software based mirroring.

#### `--ensure-forced-color-profile`

- **符号**: `switches::kEnsureForcedColorProfile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：若显示器色彩配置与 --force-color-profile 不一致则启动时崩溃（主要给 Mac 用）。
> Crash the browser at startup if the display's color profile does not match the forced color profile. This is necessary on Mac because Chrome's pixel output is always subject to the color conversion performed by the operating system. On all other platforms, this is a no-op.

#### `--force-device-scale-factor`

- **符号**: `switches::kForceDeviceScaleFactor` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖浏览器 UI 与内容的 device scale factor（DPR）。
> Overrides the device scale factor for the browser UI and the contents.

#### `--force-color-profile`

- **符号**: `switches::kForceDisplayColorProfile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把所有显示器都按指定色彩配置对待，取值 `srgb`/`generic-rgb`/`color-spin-gamma24` 等。
> Force all monitors to be treated as though they have the specified color profile. Accepted values are "srgb" and "generic-rgb" (currently used by Mac layout tests) and "color-spin-gamma24" (used by layout tests).

#### `--force-raster-color-profile`

- **符号**: `switches::kForceRasterColorProfile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制光栅化在指定色彩空间下进行。
> Force rastering to take place in the specified color profile. Accepted values are the same as for the kForceDisplayColorProfile case above.

#### `--ash-host-window-bounds`

- **符号**: `switches::kHostWindowBounds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Linux Aura/ash）设置窗口大小/位置/缩放/圆角，例如 `100+200-1024x768*2~15|15|12|12`，多个用逗号分隔模拟多屏。
> Sets a window size, optional position, optional scale factor and optional panel radii. "1024x768" creates a window of size 1024x768. "100+200-1024x768" positions the window at 100,200. "1024x768*2" sets the scale factor to 2 for a high DPI display. "1024x768~15|15|12|12" sets the radii of the panel corners as (upper_left=15px,upper_right=15px, lower_right=12px, upper_left=12px) "800,0+800-800x800" for two displays at 800x800 resolution.
> "800,0+800-800x800,0+1600-800x800" for three displays at 800x800 resolution.

#### `--screen-config`

- **符号**: `switches::kScreenConfig` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：FakeDisplayDelegate 用：指定初始多屏配置。
> Specifies the initial screen configuration, or state of all displays, for FakeDisplayDelegate, see class for format details.

#### `--secondary-display-layout`

- **符号**: `switches::kSecondaryDisplayLayout` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试用：副屏布局与偏移，格式 `<t|r|b|l>,<offset>`，如 `r,-100`。
> Specifies the layout mode and offsets for the secondary display for testing. The format is "<t|r|b|l>,<offset>" where t=TOP, r=RIGHT, b=BOTTOM and L=LEFT. For example, 'r,-100' means the secondary display is positioned on the right with -100 offset. (above than primary)

#### `--use-first-display-as-internal`

- **符号**: `switches::kUseFirstDisplayAsInternal` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 --ash-host-window-bounds 中第一块当成内置显示器（Linux 桌面调试用）。
> Uses the 1st display in --ash-host-window-bounds as internal display. This is for debugging on linux desktop.


### gfx (ui/gfx/switches.cc)

字体回退、字形渲染等通用图形开关。

_开关数：7_


#### `--animation-duration-scale`

- **符号**: `switches::kAnimationDurationScale` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：全局动画时长缩放系数（≥0）；只影响 LinearAnimation 系列。
> Scale factor to apply to every animation duration. Must be >= 0.0. This will only apply to LinearAnimation and its subclasses.

#### `--disable-font-subpixel-positioning`

- **符号**: `switches::kDisableFontSubpixelPositioning` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制关闭字体子像素定位（影响清晰度/字距/hinting/排版）。
> Force disables font subpixel positioning. This affects the character glyph sharpness, kerning, hinting and layout.

#### `--enable-native-gpu-memory-buffers`

- **符号**: `switches::kEnableNativeGpuMemoryBuffers` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Linux）启用原生可 CPU 映射的 GPU memory buffer。
> Enable native CPU-mappable GPU memory buffer support on Linux.

#### `--force-prefers-no-reduced-motion`

- **符号**: `switches::kForcePrefersNoReducedMotion` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：无视系统设置，强制 `prefers-reduced-motion: no-preference`。
> Forces whether the user desires no reduced motion, regardless of system settings.

#### `--force-prefers-reduced-motion`

- **符号**: `switches::kForcePrefersReducedMotion` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：无视系统设置，强制 `prefers-reduced-motion: reduce`。
> Forces whether the user desires reduced motion, regardless of system settings.

#### `--no-xshm`

- **符号**: `switches::kNoXshm` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS)
- **用途**：禁用 MIT-SHM 扩展（仅 Ozone/X11）。
> Disables MIT-SHM extension. In use only with Ozone/X11.

#### `--display`

- **符号**: `switches::kX11Display` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS)
- **用途**：（Ozone/X11）连接到的 X11 display，等价 GTK `--display=`。
> Which X11 display to connect to. Emulates the GTK+ "--display=" command line argument. In use only with Ozone/X11.


### OpenGL / ANGLE (ui/gl/gl_switches.cc)

GL/ANGLE/Vulkan 后端、直接合成 (DirectComposition)、DXGI、overlay 格式等 GPU 底层开关，是 Windows 上排查 GPU 问题的主入口。

_开关数：51_


#### `--d3d11-null`

- **符号**: `switches::kANGLEImplementationD3D11NULLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：D3D11 null（NULL/stub 驱动）。
> Special switches for "NULL"/stub driver implementations.

#### `--d3d11`

- **符号**: `switches::kANGLEImplementationD3D11Name` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：D3D11。

#### `--d3d11-warp-webgl`

- **符号**: `switches::kANGLEImplementationD3D11WarpForWebGLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：D3D11 WARP（仅 WebGL）。

#### `--d3d11-warp`

- **符号**: `switches::kANGLEImplementationD3D11WarpName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：D3D11 WARP。

#### `--d3d11on12`

- **符号**: `switches::kANGLEImplementationD3D11on12Name` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：D3D11on12。

#### `--d3d9`

- **符号**: `switches::kANGLEImplementationD3D9Name` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：D3D9（旧）。

#### `--default`

- **符号**: `switches::kANGLEImplementationDefaultName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器/取值：default。

#### `--metal-null`

- **符号**: `switches::kANGLEImplementationMetalNULLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：Metal null。

#### `--null`

- **符号**: `switches::kANGLEImplementationNullName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：NULL。

#### `--gl-egl`

- **符号**: `switches::kANGLEImplementationOpenGLEGLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：Desktop GL on EGL。

#### `--gles-egl`

- **符号**: `switches::kANGLEImplementationOpenGLESEGLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：GLES on EGL。

#### `--gles-null`

- **符号**: `switches::kANGLEImplementationOpenGLESNULLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：GLES null。

#### `--gles`

- **符号**: `switches::kANGLEImplementationOpenGLESName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：GLES。

#### `--gl-null`

- **符号**: `switches::kANGLEImplementationOpenGLNULLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：Desktop GL null。

#### `--gl`

- **符号**: `switches::kANGLEImplementationOpenGLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：Desktop GL。

#### `--swiftshader-webgl`

- **符号**: `switches::kANGLEImplementationSwiftShaderForWebGLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：SwiftShader (WebGL)。

#### `--vulkan-null`

- **符号**: `switches::kANGLEImplementationVulkanNULLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：Vulkan null。

#### `--vulkan`

- **符号**: `switches::kANGLEImplementationVulkanName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ANGLE 渲染器：Vulkan。

#### `--passthrough`

- **符号**: `switches::kCmdDecoderPassthroughName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-cmd-decoder 取值：Pass-through 命令解码器（不做校验/状态跟踪）。

#### `--validating`

- **符号**: `switches::kCmdDecoderValidatingName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-cmd-decoder 取值：Validating 命令解码器（默认）。
> The command decoder names that can be passed to --use-cmd-decoder.

#### `--direct-composition-video-swap-chain-format`

- **符号**: `switches::kDirectCompositionVideoSwapChainFormat` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）覆盖 DirectComposition SDR 视频 overlay 的 swap chain 格式。
> Used for overriding the swap chain format for direct composition SDR video overlays.

#### `--disable-angle-features`

- **符号**: `switches::kDisableANGLEFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用指定的 ANGLE features（逗号分隔）。
> Disables specified comma separated ANGLE features if found.

#### `--disable-d3d11-warp`

- **符号**: `switches::kDisableD3D11Warp` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）显式禁用 D3D11 WARP 回退（部分测试更想回退到 SwiftShader）。
> Explicitly disable D3D11 WARP fallback. Some test suites prefer falling back to swiftshader.

#### `--disable-direct-composition`

- **符号**: `switches::kDisableDirectComposition` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）禁用 DirectComposition。
> Disable DirectComposition.

#### `--disable-gl-drawing-for-tests`

- **符号**: `switches::kDisableGLDrawingForTests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试用：禁用产生像素输出的 GL 绘制（输出不正确，但测试更快）。
> Disables GL drawing operations which produce pixel output. With this the GL output will not be correct but tests will run faster.

#### `--disable-gl-extensions`

- **符号**: `switches::kDisableGLExtensions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用指定的 GL 扩展（逗号分隔）。
> Disables specified comma separated GL Extensions if found.

#### `--disable-gpu-driver-bug-workarounds`

- **符号**: `switches::kDisableGpuDriverBugWorkarounds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用各种 GPU 驱动 bug 的 workaround。
> Disable workarounds for various GPU driver bugs.

#### `--disable-gpu-vsync`

- **符号**: `switches::kDisableGpuVsync` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 不再与 vblank 同步呈现（关闭 VSync）。
> Stop the GPU from synchronizing presentation with vblank.

#### `--enable-angle-features`

- **符号**: `switches::kEnableANGLEFeatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用指定的 ANGLE features（逗号分隔，定义见 third_party/angle）。
> ANGLE features are defined per-backend in third_party/angle/include/platform Enables specified comma separated ANGLE features if found.

#### `--enable-direct-composition-video-overlays`

- **符号**: `switches::kEnableDirectCompositionVideoOverlays` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）即使硬件不支持也启用 DirectComposition 视频 overlay。
> Enable DirectComposition video overlays even if hardware doesn't support it.

#### `--enable-gpu-service-tracing`

- **符号**: `switches::kEnableGPUServiceTracing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把每个 GL 调用都打 TRACE。
> Turns on calling TRACE for every GL call.

#### `--enable-sgi-video-sync`

- **符号**: `switches::kEnableSgiVideoSync` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 SGI_video_sync 扩展（驱动/沙箱/WM 兼容性可能有问题）。
> Enable use of the SGI_video_sync extension, which can have driver/sandbox/window manager compatibility issues.

#### `--enable-swap-buffers-with-bounds`

- **符号**: `switches::kEnableSwapBuffersWithBounds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在支持时启用 SwapBuffersWithBounds。
> Enables SwapBuffersWithBounds if it is supported.

#### `--enable-unsafe-swiftshader`

- **符号**: `switches::kEnableUnsafeSwiftShader` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许 WebGL 使用 SwiftShader（默认禁止，作为安全兜底）。
> Allow usage of SwiftShader for WebGL

#### `--angle`

- **符号**: `switches::kGLImplementationANGLEName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-gl 取值：ANGLE。

#### `--egl`

- **符号**: `switches::kGLImplementationEGLName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-gl 取值：EGL/GLES2（Windows 默认）。

#### `--mock`

- **符号**: `switches::kGLImplementationMockName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-gl 取值：Mock（测试）。

#### `--stub`

- **符号**: `switches::kGLImplementationStubName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--use-gl 取值：Stub（测试）。

#### `--gpu-no-context-lost`

- **符号**: `switches::kGpuNoContextLost` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：声明 GPU context 在节能/屏保等情形下不会丢（不保证 GPU reset 等情况）。
> Inform Chrome that a GPU context will not be lost in power saving mode, screen saving mode, etc.  Note that this flag does not ensure that a GPU context will never be lost in any situations, say, a GPU reset.

#### `--override-use-software-gl-for-tests`

- **符号**: `switches::kOverrideUseSoftwareGLForTests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试用：强制使用软件 GL 而非硬件 GPU。
> Forces the use of software GL instead of hardware gpu for tests.

#### `--bgra`

- **符号**: `switches::kSwapChainFormatBGRA` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：swap chain 格式：BGRA。

#### `--nv12`

- **符号**: `switches::kSwapChainFormatNV12` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：DirectComposition SDR 视频 overlay swap chain 格式：NV12。
> Swap chain formats for direct composition SDR video overlays.

#### `--p010`

- **符号**: `switches::kSwapChainFormatP010` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：swap chain 格式：P010。

#### `--yuy2`

- **符号**: `switches::kSwapChainFormatYUY2` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：swap chain 格式：YUY2。

#### `--test-gl-lib`

- **符号**: `switches::kTestGLLib` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Linux 测试）desktop GL 绑定先尝试加载该 GL 库，失败再回退默认。
> Flag used for Linux tests: for desktop GL bindings, try to load this GL library first, but fall back to regular library if loading fails.

#### `--tint-dc-layer`

- **符号**: `switches::kTintDcLayer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）给 SwapChainPresenter 染色：解码 swap chain 蓝、VP blit 品红、带 staging 纹理的 VP blit 橙、MF proxy surface 绿。
> Tint `SwapChainPresenter` with the following colors: - Decode swap chain: blue - VP blit: magenta - VP blit w/ staging texture: orange - MF proxy surface: green This is similar to `HKLM\Software\Microsoft\Windows\DWM` `OverlayTestMode=1` in DWM, but to help understand `SwapChainPresenter` state.

#### `--use-angle`

- **符号**: `switches::kUseANGLE` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：选择 ANGLE 后端：default/d3d9/d3d11/warp/gl/gles。
> Select which ANGLE backend to use. Options are: default: Attempts several ANGLE renderers until one successfully initializes, varying ES support by platform. d3d9: Legacy D3D9 renderer, ES2 only. d3d11: D3D11 renderer, ES2 and ES3. warp: D3D11 renderer using software rasterization, ES2 and ES3. gl: Desktop GL renderer, ES2 and ES3. gles: GLES renderer, ES2 and ES3.

#### `--use-adapter-luid`

- **符号**: `switches::kUseAdapterLuid` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）GPU 进程使用指定 LUID 的显卡初始化。
> Initialize the GPU process using the adapter with the specified LUID. This is only used on Windows, as LUID is a Windows specific structure.

#### `--use-cmd-decoder`

- **符号**: `switches::kUseCmdDecoder` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：选择命令解码器：`passthrough`（跳过校验）或 `validating`。
> Use the Pass-through command decoder, skipping all validation and state tracking. Switch lives in ui/gl because it affects the GL binding initialization on platforms that would otherwise not default to using EGL bindings.

#### `--use-gl`

- **符号**: `switches::kUseGL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：选择 GPU 进程使用的 GL 实现：`desktop`/`egl`/`swiftshader`。
> Select which implementation of GL the GPU process should use. Options are: desktop: whatever desktop OpenGL the user has installed (Linux and Mac default). egl: whatever EGL / GLES2 the user has installed (Windows default - actually ANGLE). swiftshader: The SwiftShader software renderer.

#### `--use-gpu-in-tests`

- **符号**: `switches::kUseGpuInTests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试用：若可用则使用硬件 GPU。
> Use hardware gpu, if available, for tests.


## 三、媒体 / 音视频 / WebRTC / MIDI

音视频播放与编解码（包括 Windows 硬件编码 MF）、屏幕/摄像头采集、WebRTC、MIDI 等相关开关。常用于调试 H.264/HEVC/AV1 硬件解码、屏幕共享权限、音频环回采集等场景。


### Cast Streaming (components/cast_streaming/browser/cast_streaming_switches.h)

_开关数：1_


#### `--cast-streaming-receiver-port`

- **符号**: `switches::kCastStreamingReceiverPort` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Cast Streaming 接收端使用指定 UDP 端口（默认随机系统端口）。
> If set, allows to use a specific UDP port for Cast Streaming sessions on the receiver side. Otherwise the Cast Streaming receiver is using a random system port.


### Media (media/base/media_switches.cc)

音视频核心开关：音频环回、硬件解码/编码、DRM、WebRTC 编解码选择等。

_开关数：45_


#### `--alsa-input-device`

- **符号**: `switches::kAlsaInputDevice` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS) && BUILDFLAG(IS_FREEBSD) && \
- **用途**：（Linux）打开音频输入流时使用的 ALSA 设备名。
> The Alsa device to use when opening an audio input stream.

#### `--alsa-output-device`

- **符号**: `switches::kAlsaOutputDevice` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS) && BUILDFLAG(IS_FREEBSD) && \
- **用途**：（Linux）打开音频输出流时使用的 ALSA 设备名。
> The Alsa device to use when opening an audio stream.

#### `--audio-buffer-size`

- **符号**: `switches::kAudioBufferSize` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自定义音频缓冲区大小（调试用）。
> Allow users to specify a custom buffer size for debugging purpose.

#### `--audio-codecs-from-edid`

- **符号**: `switches::kAudioCodecsFromEDID` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_PASSTHROUGH_AUDIO_CODECS)
- **用途**：通过音频服务进程从 HDMI sink EDID 读取支持的音频编码器（以 Short Audio Descriptor 位图形式）。
> Audio codecs supported by the HDMI sink is retrieved from the audio service process. EDID contains the Short Audio Descriptors, which list the audio decoders supported, and the information is presented as a bitmask of supported audio codecs.

#### `--auto-grant-captured-surface-control-prompt`

- **符号**: `switches::kAutoGrantCapturedSurfaceControlPrompt` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自动授予 Captured Surface Control 权限，不再弹权限提示。
> Skip the permission prompt for Captured Surface Control.

#### `--autoplay-policy`

- **符号**: `switches::kAutoplayPolicy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自动播放策略，取值见下面三个（no-user-gesture-required / user-gesture-required / document-user-activation-required）。
> Command line flag name to set the autoplay policy.

#### `--cast-streaming-force-disable-hardware-h264`

- **符号**: `switches::kCastStreamingForceDisableHardwareH264` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Cast Streaming 完全禁用 H264 硬件编码（优先级高于 force-enable）。
> NOTE: callers should always use the free functions in /media/cast/encoding/encoding_support.h instead of accessing these features directly. TODO(crbug.com/286443864): Guard Cast Sender flags with !IS_ANDROID. If enabled, completely disables use of H264 hardware encoding for Cast Streaming sessions. Takes precedence over kCastStreamingForceEnableHardwareH264.

#### `--cast-streaming-force-disable-hardware-vp8`

- **符号**: `switches::kCastStreamingForceDisableHardwareVp8` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Cast Streaming 完全禁用 VP8 硬件编码（优先级高于 force-enable）。
> If enabled, completely disables use of VP8 hardware encoding for Cast Streaming sessions. Takes precedence over kCastStreamingForceEnableHardwareVp8.

#### `--cast-streaming-force-disable-hardware-vp9`

- **符号**: `switches::kCastStreamingForceDisableHardwareVp9` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Cast Streaming 完全禁用 VP9 硬件编码（优先级高于 force-enable）。
> If enabled, completely disables use of VP9 hardware encoding for Cast Streaming sessions. Takes precedence over kCastStreamingForceEnableHardwareVp9.

#### `--cast-streaming-force-enable-hardware-h264`

- **符号**: `switches::kCastStreamingForceEnableHardwareH264` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：即便平台默认禁用也强制启用 H264 硬件编码（要求 force-disable 未开）。
> If enabled, allows use of H264 hardware encoding for Cast Streaming sessions, even on platforms where it is disabled due to performance and reliability issues. kCastStreamingForceDisableHardwareH264 must be disabled for this flag to take effect.

#### `--cast-streaming-force-enable-hardware-vp8`

- **符号**: `switches::kCastStreamingForceEnableHardwareVp8` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：即便平台默认禁用也强制启用 VP8 硬件编码（要求 force-disable 未开）。
> If enabled, allows use of VP8 hardware encoding for Cast Streaming sessions, even on platforms where it is disabled due to performance and reliability issues. kCastStreamingForceDisableHardwareVp8 must be disabled for this flag to take effect.

#### `--cast-streaming-force-enable-hardware-vp9`

- **符号**: `switches::kCastStreamingForceEnableHardwareVp9` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：即便平台默认禁用也强制启用 VP9 硬件编码（要求 force-disable 未开）。
> If enabled, allows use of VP9 hardware encoding for Cast Streaming sessions, even on platforms where it is disabled due to performance and reliability issues. kCastStreamingForceDisableHardwareVp9 must be disabled for this flag to take effect.

#### `--clear-key-cdm-path-for-testing`

- **符号**: `switches::kClearKeyCdmPathForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定 Clear Key CDM 的路径（用于测试 External Clear Key 密钥系统）。
> Specifies the path to the Clear Key CDM for testing, which is necessary to support External Clear Key key system when library CDM is enabled. Note that External Clear Key key system support is also controlled by feature kExternalClearKeyForTesting.

#### `--disable-accelerated-mjpeg-decode`

- **符号**: `switches::kDisableAcceleratedMjpegDecode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用捕获帧的 MJPEG 硬件加速解码。
> Disable hardware acceleration of mjpeg decode for captured frame, where available.

#### `--disable-audio-input`

- **符号**: `switches::kDisableAudioInput` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制输入/输出流使用假音频流（无真麦克风）。
> Forces input and output stream creation to use fake audio streams.

#### `--disable-audio-output`

- **符号**: `switches::kDisableAudioOutput` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用音频输出（静音/无输出设备场景的测试）。

#### `--disable-background-media-suspend`

- **符号**: `switches::kDisableBackgroundMediaSuspend` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：后台标签页里的媒体不立即挂起。
> Do not immediately suspend media in background tabs.

#### `--disable-rtc-smoothness-algorithm`

- **符号**: `switches::kDisableRTCSmoothnessAlgorithm` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 WebRTC 新的平滑渲染算法。
> Disables the new rendering algorithm for webrtc, which is designed to improve the rendering smoothness.

#### `--document-user-activation-required`

- **符号**: `switches::kDocumentUserActivationRequiredPolicy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--autoplay-policy 取值：需 document 级用户激活。
> Autoplay policy that requires a document user activation.

#### `--enable-exclusive-audio`

- **符号**: `switches::kEnableExclusiveAudio` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows Vista+）低延迟音频路径启用独占模式。
> Use exclusive mode audio streaming for Windows Vista and higher. Leads to lower latencies for audio streams which uses the AudioParameters::AUDIO_PCM_LOW_LATENCY audio path. See http://msdn.microsoft.com/en-us/library/windows/desktop/dd370844.aspx for details.

#### `--enable-live-caption-pref-for-testing`

- **符号**: `switches::kEnableLiveCaptionPrefForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 kLiveCaptionEnabled 偏好默认置 true（测试用）。
> Sets the default value for the kLiveCaptionEnabled preference to true.

#### `--enable-primary-node-access-for-vkms-testing`

- **符号**: `switches::kEnablePrimaryNodeAccessForVkmsTesting` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(USE_V4L2_CODEC)
- **用途**：（V4L2 on ChromeOS VM）通过 vkms + minigbm dumb 驱动进行 buffer 分配的测试开关。
> This is needed for V4L2 testing using VISL (virtual driver) on cros VM with arm64-generic-vm. Minigbm buffer allocation is done using dumb driver with vkms.

#### `--fail-audio-stream-creation`

- **符号**: `switches::kFailAudioStreamCreation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：让 AudioManager 创建音频流总是失败（测试错误路径）。
> Causes the AudioManager to fail creating audio streams. Used when testing various failure cases.

#### `--fake-background-blur-toggle-period`

- **符号**: `switches::kFakeBackgroundBlurTogglePeriod` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 VideoFrameMetadata 里注入假背景模糊状态，按指定毫秒周期 ENABLED/DISABLED 循环。
> Inserts fake background blur state into `VideoFrameMetadata`. The value represents the period in milliseconds. eg. Setting it to 1000ms, will cause the blur state to cycle between reporting ENABLED for 500ms and DISABLED for 500ms.

#### `--force-video-overlays`

- **符号**: `switches::kForceVideoOverlays` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Android）视频播放器强制使用 SurfaceView 而非 SurfaceTexture（Cast 播放管线依赖）。
> Force media player using SurfaceView instead of SurfaceTexture on Android. Note: This is used by the Cast playback pipeline and must be kept.

#### `--force-wave-audio`

- **符号**: `switches::kForceWaveAudio` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows）即使支持 Core Audio 也强制使用 WaveOut/In API。
> Use Windows WaveOut/In audio API even if Core Audio is supported.

#### `--hardware-video-decode-framerate`

- **符号**: `switches::kHardwareVideoDecodeFrameRate` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(USE_V4L2_CODEC)
- **用途**：为 V4L2 硬件解码器锁定帧率（测试用，部分 Qualcomm 解码器需要）。
> Some (Qualcomm only at the moment) V4L2 video decoders require setting the framerate so that the hardware decoder can scale the clocks efficiently. This provides a mechanism during testing to lock the decoder framerate to a specific value.

#### `--mse-audio-buffer-size-limit-mb`

- **符号**: `switches::kMSEAudioBufferSizeLimitMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：MSE 音频缓冲区上限（MB，默认 12）。
> Allows explicitly specifying MSE audio/video buffer sizes as megabytes. Default values are 150M for video and 12M for audio.

#### `--mse-video-buffer-size-limit-mb`

- **符号**: `switches::kMSEVideoBufferSizeLimitMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：MSE 视频缓冲区上限（MB，默认 150）。

#### `--mute-audio`

- **符号**: `switches::kMuteAudio` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：对输出设备静音（自动化测试常用）。
> Mutes audio sent to the audio device so it is not audible during automated testing.

#### `--no-user-gesture-required`

- **符号**: `switches::kNoUserGestureRequiredPolicy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--autoplay-policy 取值：不需要任何用户手势。
> Autoplay policy that does not require any user gesture.

#### `--override-enabled-cdm-interface-version`

- **符号**: `switches::kOverrideEnabledCdmInterfaceVersion` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：只启用指定版本的库 CDM 接口（覆盖默认），便于本地测试/调试实验性 CDM。
> Overrides the default enabled library CDM interface version(s) with the one specified with this switch, which will be the only version enabled. For example, on a build where CDM 8, CDM 9 and CDM 10 are all supported (implemented), but only CDM 8 and CDM 9 are enabled by default: --override-enabled-cdm-interface-version=8 : Only CDM 8 is enabled --override-enabled-cdm-interface-version=9 : Only CDM 9 is enabled --override-enabled-cdm-interface-version=10 : Only CDM 10 is enabled --override-enabled
> -cdm-interface-version=11 : No CDM interface is enabled This can be used for local testing and debugging. It can also be used to enable an experimental CDM interface (which is always disabled by default) for testing while it's still in development.

#### `--override-hardware-secure-codecs-for-testing`

- **符号**: `switches::kOverrideHardwareSecureCodecsForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试用：覆盖 hardware secure codec 支持情况；可指定 vp8/vp9/avc1/hevc/dolbyvision/av01/mp4a/vorbis，以及 `<video>-no-clearlead`。
> Overrides hardware secure codecs support for testing. If specified, real platform hardware secure codecs check will be skipped. Valid codecs are: - video: "vp8", "vp9", "avc1", "hevc", "dolbyvision", "av01" - video that does not support clear lead: `<video>-no-clearlead`, where <video> is from the list above. - audio: "mp4a", "vorbis" Codecs are separated by comma.
> For example: --override-hardware-secure-codecs-for-testing=vp8,vp9-no-clearlead,vorbis --override-hardware-secure-codecs-for-testing=avc1,mp4a CENC encryption scheme is assumed to be supported for the specified codecs. If no valid codecs specified, no hardware secure codecs are supported. This can be used to disable hardware secure codecs support: --override-hardware-secure-codecs-for-testing

#### `--report-vp9-as-an-unsupported-mime-type`

- **符号**: `switches::kReportVp9AsAnUnsupportedMimeType` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 VP9 报告为不支持的 MIME 类型。
> Force to report VP9 as an unsupported MIME type.

#### `--system-aec-enabled`

- **符号**: `switches::kSystemAecEnabled` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(USE_CRAS)
- **用途**：强制启用系统侧的音频回声消除。
> Enforce system audio echo cancellation.

#### `--try-supported-channel-layouts`

- **符号**: `switches::kTrySupportedChannelLayouts` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：尝试按源声道布局播放，允许驱动做立体声→多声道扩展（部分驱动谎报能力会挂，所以默认关）。
> Instead of always using the hardware channel layout, check if a driver supports the source channel layout.  Avoids outputting empty channels and permits drivers to enable stereo to multichannel expansion.  Kept behind a flag since some drivers lie about supported layouts and hang when used.  See http://crbug.com/259165 for more details.

#### `--unsafely-allow-protected-media-identifier-for-domain`

- **符号**: `switches::kUnsafelyAllowProtectedMediaIdentifierForDomain` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：自动化测试用：给指定域名（逗号分隔）始终授予 protected-media-identifier 权限（不含端口）。
> For automated testing of protected content, this switch allows specific domains (e.g. example.com) to always allow the permission to share the protected media identifier. In this context, domain does not include the port number. User's content settings will not be affected by enabling this switch. Reference: https://crbug.com/41317087 Example: --unsafely-allow-protected-media-identifier-for-domain=a.com,b.ca

#### `--use-cras`

- **符号**: `switches::kUseCras` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(USE_CRAS)
- **用途**：（ChromeOS）使用 CRAS 音频服务。
> Use CRAS, the ChromeOS audio server.

#### `--use-fake-device-for-media-stream`

- **符号**: `switches::kUseFakeDeviceForMediaStream` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：MediaStream 使用假设备替代真摄像头/麦克风（参数见 FakeVideoCaptureDeviceFactory）。
> Use fake device for Media Stream to replace actual camera and microphone. For the list of allowed parameters, see FakeVideoCaptureDeviceFactory::ParseFakeDevicesConfigFromOptionsString().

#### `--use-fake-mjpeg-decode-accelerator`

- **符号**: `switches::kUseFakeMjpegDecodeAccelerator` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用假的 MJPEG 硬件解码器（不需要真硬件也能测与 GPU 服务通信）。
> Use a fake device for accelerated decoding of MJPEG. This allows, for example, testing of the communication to the GPU service without requiring actual accelerator hardware to be present.

#### `--use-file-for-fake-audio-capture`

- **符号**: `switches::kUseFileForFakeAudioCapture` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用 .wav 文件作为假麦克风；可加 `%noloop` 后缀表示不循环；需配合 --disable-audio-input 或 --use-fake-device-for-media-stream。
> Play a .wav file as the microphone. Note that for WebRTC calls we'll treat the bits as if they came from the microphone, which means you should disable audio processing (lest your audio file will play back distorted). The input file is converted to suit Chrome's audio buses if necessary, so most sane .wav files should work. You can pass either <path> to play the file looping or <path>%noloop to stop after playing the file to completion.
> Must also be used with kDisableAudioInput or kUseFakeDeviceForMediaStream.

#### `--use-file-for-fake-video-capture`

- **符号**: `switches::kUseFileForFakeVideoCapture` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用 .y4m 文件作为假摄像头输入。
> Use an .y4m file to play as the webcam. See the comments in media/capture/video/file_video_capture_device.h for more details.

#### `--user-gesture-required`

- **符号**: `switches::kUserGestureRequiredPolicy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--autoplay-policy 取值：需要用户手势。
> Autoplay policy to require a user gesture in order to play.

#### `--video-threads`

- **符号**: `switches::kVideoThreads` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：视频解码使用的线程数。
> Set number of threads to use for video decoding.

#### `--waveout-buffers`

- **符号**: `switches::kWaveOutBuffers` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows）WaveOut 使用的缓冲区数量。
> Number of buffers to use for WaveOut.


### Media Capture (media/capture/capture_switches.cc)

屏幕/摄像头采集开关。

_开关数：2_


#### `--disable-video-capture-use-gpu-memory-buffer`

- **符号**: `switches::kDisableVideoCaptureUseGpuMemoryBuffer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过 chrome://flags 覆盖 kVideoCaptureUseGpuMemoryBuffer：强制禁用。
> This is for the same feature controlled by kVideoCaptureUseGpuMemoryBuffer. kVideoCaptureUseGpuMemoryBuffer is settled by chromeos overlays. This flag is necessary to overwrite the settings via chrome:// flag. The behavior of chrome://flag#zero-copy-video-capture is as follows; Default  : Respect chromeos overlays settings. Enabled  : Force to enable kVideoCaptureUseGpuMemoryBuffer. Disabled : Force to disable kVideoCaptureUseGpuMemoryBuffer.

#### `--video-capture-use-gpu-memory-buffer`

- **符号**: `switches::kVideoCaptureUseGpuMemoryBuffer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用基于 GpuMemoryBuffer 的视频捕获 buffer 池。
> Enables GpuMemoryBuffer-based buffer pool.


## 四、网络 / 安全浏览 / 鉴权

网络栈（net/URLRequest/NetworkService）、连接/代理/HTTP/Cookie、Safe Browsing、Google 登录 (GAIA/GCM) 等相关开关。用来绕过证书验证、禁用 QUIC、自定义代理、调试同步登录时常用。


### 网络会话配置 (components/network_session_configurator/common/network_switches.cc)

HTTP/QUIC 会话参数开关。

_开关数：2_


#### `--disable-http2-grease-settings`

- **符号**: `switches::kDisableHttp2GreaseSettings` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 HTTP/2 SETTINGS 的 grease（用于防止中间盒假设。）

#### `--http2-grease-settings`

- **符号**: `switches::kEnableHttp2GreaseSettings` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 HTTP/2 SETTINGS grease（历史原因名字里没 `enable`）。
> `kEnableHttp2GreaseSettings` does not include the word "enable" for historical reasons.


### Safe Browsing (components/safe_browsing/core/common/safebrowsing_switches.cc)

安全浏览 (反钓鱼/反恶意软件) 开关。

_开关数：22_


#### `--mark_as_enterprise_blocked`

- **符号**: `switches::kArtificialCachedEnterpriseBlockedVerdictFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：缓存一条人工“企业拦截”裁决（测试用）。
> Command-line flag for caching an artificial blocked enterprise lookup verdict.

#### `--mark_as_enterprise_warned`

- **符号**: `switches::kArtificialCachedEnterpriseWarnedVerdictFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：缓存一条人工“企业告警”裁决（测试用）。
> Command-line flag for caching an artificial flagged enterprise lookup verdict.

#### `--mark_as_hash_prefix_real_time_phishing`

- **符号**: `switches::kArtificialCachedHashPrefixRealTimeVerdictFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：对 hash-prefix 实时查询缓存一条人工“钓鱼”裁决。
> Command-line flag for caching an artificial phishing verdict for hash-prefix real-time lookups.

#### `--mark_as_phish_guard_phishing`

- **符号**: `switches::kArtificialCachedPhishGuardVerdictFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：缓存一条人工 PhishGuard 不安全裁决。
> Command-line flag for caching an artificial PhishGuard unsafe verdict.

#### `--mark_as_real_time_phishing`

- **符号**: `switches::kArtificialCachedUrlRealTimeVerdictFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：对 URL 实时查询缓存一条人工“钓鱼”裁决。
> Command-line flag for caching an artificial phishing verdict for URL real-time lookups.

#### `--binary-upload-service-url`

- **符号**: `switches::kCloudBinaryUploadServiceUrlFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：BinaryUploadService 使用的备用 URL（DLP/企业内容分析）。

#### `--csd-debug-feature-directory`

- **符号**: `switches::kCsdDebugFeatureDirectoryFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把提取到的 CSD（客户端侦测）特征写到磁盘目录（顺带打开一些调试行为）。
> Command-line flag that can be used to write extracted CSD features to disk. This is also enables a few other behaviors that are useful for debugging.

#### `--safe-browsing-treat-user-as-advanced-protection`

- **符号**: `switches::kForceTreatUserAsAdvancedProtection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把当前用户当成 Advanced Protection 用户（测试用）。

#### `--mark_as_allowlisted_for_real_time`

- **符号**: `switches::kMarkAsHighConfidenceAllowlisted` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把逗号分隔的 URL 加入 high-confidence allowlist。
> List of comma-separated URLs to mark as present on the high-confidence allowlist.

#### `--mark_as_malware`

- **符号**: `switches::kMarkAsMalware` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把逗号分隔的 URL 标为恶意（本地库查询）。
> List of comma-separated URLs to mark as malware for local database checks.

#### `--mark_as_allowlisted_for_phish_guard`

- **符号**: `switches::kMarkAsPasswordProtectionAllowlisted` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把逗号分隔的 URL 加入密码保护/客户端侦测 allowlist。
> List of comma-separated URLs to mark as present on the password protection allowlist. Note this uses the client-side detection allowlist.

#### `--mark_as_phishing`

- **符号**: `switches::kMarkAsPhishing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把逗号分隔的 URL 标为钓鱼（本地库查询）。
> List of comma-separated URLs to mark as phishing for local database checks.

#### `--mark_as_uws`

- **符号**: `switches::kMarkAsUws` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把逗号分隔的 URL 标为 UwS/不受欢迎软件（本地库查询）。
> List of comma-separated URLs to mark as unwanted software for local database checks.

#### `--csd-model-override-path`

- **符号**: `switches::kOverrideCsdModelFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用绝对路径指定的文件覆盖当前 CSD 模型。
> Command-line flag that can be used to override the current CSD model. Must be provided with an absolute path.

#### `--safebrowsing-enable-enhanced-protection`

- **符号**: `switches::kSbEnableEnhancedProtection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Safe Browsing 增强保护。
> Enable Safe Browsing Enhanced Protection.

#### `--safebrowsing-manual-download-blacklist`

- **符号**: `switches::kSbManualDownloadBlocklist` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：逗号分隔的可执行文件 sha256 列表，下载保护会把它们当作危险文件（需本身是危险类型且未被签名/URL 豁免）。
> List of comma-separated sha256 hashes of executable files which the download-protection service should treat as "dangerous."  For a file to show a warning, it also must be considered a dangerous filetype and not be allowlisted otherwise (by signature or URL) and must be on a supported OS. Hashes are in hex. This is used for manual testing when looking for ways to by-pass download protection.

#### `--scam-detection-keyboard-lock-trigger-android`

- **符号**: `switches::kScamDetectionKeyboardLockTriggerAndroid` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Android）通过命令行启用 Scam Detection 的 keyboard lock 触发器。
> Enable the keyboard lock trigger of Scam Detection via command line for easier testing.

#### `--safe-browsing-skip-csd-allowlist`

- **符号**: `switches::kSkipCSDAllowlistOnPreclassification` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：跳过客户端侦测 allowlist 检查（视为不匹配）。
> If present, indicates that the client-side detection allowlist check should be skipped (it is treated as no match).

#### `--safe-browsing-skip-high-confidence-allowlist`

- **符号**: `switches::kSkipHighConfidenceAllowlist` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：跳过 high-confidence allowlist 检查（视为不匹配）。
> If the switch is present, any high-confidence allowlist check will return that it does not match the allowlist.

#### `--url-filtering-endpoint`

- **符号**: `switches::kUrlFilteringEndpointFlag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：ChromeEnterpriseRealTimeUrlLookupService 实时查询的备用 URL。
> Alternate URL to use for ChromeEnterpriseRealTimeUrlLookupService real-time lookups.

#### `--wp-max-file-opening-threads`

- **符号**: `switches::kWpMaxFileOpeningThreads` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：下载保护：同时打开文件的最大线程数（WhiteProtection）。

#### `--wp-max-parallel-active-requests`

- **符号**: `switches::kWpMaxParallelActiveRequests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：下载保护：最大并发活跃请求数。
> The command line flag to control the max amount of concurrent active requests.


### GAIA (google_apis/gaia/gaia_switches.cc)

Google 账号 (GAIA) OAuth 端点覆盖开关。

_开关数：9_


#### `--gaia-config-contents`

- **符号**: `switches::kGaiaConfigContents` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：直接以字符串形式提供 Gaia 配置（覆盖默认）。

#### `--gaia-config`

- **符号**: `switches::kGaiaConfigPath` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过文件路径提供 Gaia 配置（JSON）。

#### `--gaia-url`

- **符号**: `switches::kGaiaUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Gaia 登录服务器 URL。

#### `--google-apis-url`

- **符号**: `switches::kGoogleApisUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Google APIs URL。

#### `--google-url`

- **符号**: `switches::kGoogleUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Google 站点 URL。

#### `--lso-url`

- **符号**: `switches::kLsoUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Google LSO（Login Service Online）URL。

#### `--oauth2-client-id`

- **符号**: `switches::kOAuth2ClientID` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 OAuth2 客户端 ID。

#### `--oauth2-client-secret`

- **符号**: `switches::kOAuth2ClientSecret` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 OAuth2 客户端 secret。

#### `--oauth-account-manager-url`

- **符号**: `switches::kOAuthAccountManagerUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 OAuth 账号管理器 URL。


### GCM (google_apis/gcm/engine/gservices_switches.cc)

GCM 推送相关开关。

_开关数：3_


#### `--gcm-checkin-url`

- **符号**: `switches::kGCMCheckinURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GCM checkin 服务端点。
> Sets the checkin service endpoint that will be used for performing Google Cloud Messaging checkins.

#### `--gcm-mcs-endpoint`

- **符号**: `switches::kGCMMCSEndpoint` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GCM Mobile Connection Server 端点。
> Sets the Mobile Connection Server endpoint that will be used for Google Cloud Messaging.

#### `--gcm-registration-url`

- **符号**: `switches::kGCMRegistrationURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GCM 注册端点。
> Sets the registration endpoint that will be used for creating new Google Cloud Messaging registrations.


### Network Service (services/network/public/cpp/network_switches.cc)

网络服务进程开关：host resolver、忽略证书错误、日志、代理等。

_开关数：18_


#### `--additional-private-state-token-key-commitments`

- **符号**: `switches::kAdditionalTrustTokenKeyCommitments` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：手动追加 Private State Tokens 的 key commitments（JSON；命令行提供的 key 优先于 TrustTokenKeyCommitments::Set）。
> Manually sets additional Private State Tokens key commitments in the network service to the given value, which should be a JSON dictionary satisfying the requirements of TrustTokenKeyCommitmentParser::ParseMultipleIssuers. These keys are available in addition to keys provided by the most recent call to TrustTokenKeyCommitments::Set. For issuers with keys provided through both the command line and TrustTokenKeyCommitments::Set, the keys provided through the command line take precedence.
> This is because someone testing manually might want to pass additional keys via the command line to a real Chrome release with the component updater enabled, and it would be surprising if the manually-passed keys were overwritten some time after startup when the component updater runs.

#### `--disable-shared-dictionary-storage-cleanup-for-testing`

- **符号**: `switches::kDisableSharedDictionaryStorageCleanupForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 Shared Dictionary 存储清理任务（仅测试）。
> The switch to disable the shared dictionary storage clean up task. Only for testing.

#### `--force-effective-connection-type`

- **符号**: `switches::kForceEffectiveConnectionType` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：让 NQE（Network Quality Estimator）返回固定的 Effective Connection Type。
> Forces Network Quality Estimator (NQE) to return a specific effective connection type.

#### `--force-permission-policy-unload-default-enabled`

- **符号**: `switches::kForcePermissionPolicyUnloadDefaultEnabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Permissions-Policy 的 unload 不允许按默认关闭。
> If set, the unload event cannot be disabled by default by Permissions-Policy.

#### `--host-resolver-rules`

- **符号**: `switches::kHostResolverRules` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为 host resolver 指定解析规则（仅解析层，不影响 --proxy-server 等）。
> These mappings only apply to the host resolver.

#### `--host-rules`

- **符号**: `switches::kHostRules` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--host-resolver-rules 的已弃用别名，推荐直接用前者。
> Deprecated alias for backwards compatibility, now does the same thing as --host-resolver-rules which should be used instead. TODO(crbug.com/40070729): consider removing in some future release.

#### `--ignore-bad-message-for-testing`

- **符号**: `switches::kIgnoreBadMessageForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：忽略 Mojo 坏消息上报（仅测试）。
> The switch to ignore bad mojo message reports. Only for testing.

#### `--ignore-certificate-errors-spki-list`

- **符号**: `switches::kIgnoreCertificateErrorsSPKIList` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：匹配证书链中任一公钥哈希时忽略证书错误；需同时 --user-data-dir；值为逗号分隔的 base64 SHA-256 SPKI 指纹。
> A set of public key hashes for which to ignore certificate-related errors. If the certificate chain presented by the server does not validate, and one or more certificates have public key hashes that match a key from this list, the error is ignored. The switch value must be a comma-separated list of Base64-encoded SHA-256 SPKI Fingerprints (RFC 7469, Section 2.4). This switch has no effect unless --user-data-dir (as defined by the content embedder) is also present.

#### `--ip-address-space-overrides`

- **符号**: `switches::kIpAddressSpaceOverrides` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：手动覆盖 IP 端点到 address space 的映射：`<ip>:<port>=<public|private|local|loopback>`；port=0 覆盖所有端口。
> Specifies manual overrides to the IP endpoint -> IP address space mapping. This allows running local tests against "public" and "local" IP addresses. This switch is specified as a comma-separated list of overrides. Each override is given as a colon-separated "<endpoint>:<address space>" pair.
> Grammar, in pseudo-BNF format: switch := override-list override-list := override “,” override-list | <nil> override := ip-endpoint “=” address-space address-space := “public” | “private” | “local” | "loopback" ip-endpoint := ip-address ":" port ip-address := see `net::ParseURLHostnameToAddress()` for details port := integer in the [0-65535] range Any invalid entries in the comma-separated list are ignored. If the port specified is 0, all ports for the given ip-address will be overridden.
> See also the design doc: https://docs.google.com/document/d/1-umCGylIOuSG02k9KGDwKayt3bzBXtGwVlCQHHkIcnQ/edit# And the Web Platform Test RFC #72 behind it: https://github.com/web-platform-tests/rfcs/blob/master/rfcs/address_space_overrides.md Note that since the doc and the RFC were written, the address space names have changed slightly due to Local Network Access (LNA) replacing Private Network Access (PNA).

#### `--log-net-log`

- **符号**: `switches::kLogNetLog` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 net log 事件保存到文件；不给值则写到用户数据目录下的 `netlog.json`。
> Enables saving net log events to a file. If a value is given, it used as the path the the file, otherwise the file is named netlog.json and placed in the user data directory.

#### `--net-log-duration`

- **符号**: `switches::kLogNetLogDuration` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：NetLog 记录时长（秒），到时自动 stop 并 flush 到磁盘。
> Specifies the duration (in seconds) for network logging. When this flag is provided with a positive integer value X, Chrome will automatically stop collecting NetLog events after X seconds and flush the log to disk.

#### `--net-log-capture-mode`

- **符号**: `switches::kNetLogCaptureMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：NetLog 抓取粒度：`Default` / `IncludeSensitive` / `Everything`。
> Sets the granularity of events to capture in the network log. The mode can be set to one of the following values: "Default" "IncludeSensitive" "Everything" See the enums of the corresponding name in net_log_capture_mode.h for a description of their meanings.

#### `--net-log-max-size-mb`

- **符号**: `switches::kNetLogMaxSizeMb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：NetLog 文件最大大小（MB），超出后覆盖旧数据。
> Sets the maximum size, in megabytes. The log file can grow to before older data is overwritten. Do not use this flag if you want an unlimited file size.

#### `--ssl-key-log-file`

- **符号**: `switches::kSSLKeyLogFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 SSL 密钥材料写到指定文件用于解密抓包（格式见 NSS Key Log Format）。
> Causes SSL key material to be logged to the specified file for debugging purposes. See https://developer.mozilla.org/en-US/docs/Mozilla/Projects/NSS/Key_Log_Format for the format.

#### `--test-third-party-cookie-phaseout`

- **符号**: `switches::kTestThirdPartyCookiePhaseout` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试第三方 Cookie 下线相关行为。

#### `--unsafely-treat-insecure-origin-as-secure`

- **符号**: `switches::kUnsafelyTreatInsecureOriginAsSecure` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把指定（不安全）origin 视为 secure context，逗号分隔。
> Treat given (insecure) origins as secure origins. Multiple origins can be supplied as a comma-separated list. For the definition of secure contexts, see https://w3c.github.io/webappsec-secure-contexts/ and https://www.w3.org/TR/powerful-features/#is-origin-trustworthy Example: --unsafely-treat-insecure-origin-as-secure=http://a.test,http://b.test

#### `--use-first-party-set`

- **符号**: `switches::kUseFirstPartySet` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：手动指定 First-Party Set（与 --use-related-website-set 同义，已弃用，请使用后者）。
> Allows the manual specification of a First-Party Set. The format is the same as that of `--use-related-website-set`. DEPRECATED(crbug.com/1486689): This switch is under deprecation due to renaming "First-Party Set" to "Related Website Set". Please use `kUseRelatedWebsiteSet` instead.

#### `--use-related-website-set`

- **符号**: `switches::kUseRelatedWebsiteSet` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：手动指定 Related Website Set（stringified JSON，格式同 Related Website Sets 官方仓库）。
> Allows the manual specification of a Related Website Set. The set should be provided as a stringified JSON object, whose format matches the format of the JSON in https://github.com/GoogleChrome/related-website-sets.


## 五、多进程 / 沙箱 / 崩溃 / Tracing / Metrics

沙箱策略、Service Manager、崩溃上报、日志/Tracing、内存分析、指标上报等开关。排查子进程启动失败、沙箱被拒、内存泄漏时的主战场。


### Crash (components/crash/core/app/crash_switches.cc)

崩溃处理器进程相关开关。

_开关数：2_


#### `--crashpad-handler`

- **符号**: `switches::kCrashpadHandler` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）子进程类型：把 chrome.exe/setup.exe 当作 crashpad_handler 启动。
> A process type (switches::kProcessType) that indicates chrome.exe or setup.exe is being launched as crashpad_handler. This is only used on Windows. We bundle the handler into chrome.exe on Windows because there is high probability of a "new" .exe being blocked or interfered with by application firewalls, AV software, etc. On other platforms, crashpad_handler is a standalone executable.

#### `--crashpad-handler-pid`

- **符号**: `switches::kCrashpadHandlerPid` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS)
- **用途**：Crashpad handler 的进程 ID。
> The process ID of the Crashpad handler.


### Heap Profiling (components/heap_profiling/in_process/switches.cc)

进程内堆分析开关。

_开关数：1_


#### `--subproc-heap-profiling`

- **符号**: `switches::kSubprocessHeapProfiling` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制对子进程开启 Heap Profiling（浏览器进程在需要时自动加上）。
> Forces Heap Profiling on for a subprocess. The browser will add this when the subprocess should be profiled.


### Metrics (components/metrics/metrics_switches.cc)

UMA/UKM 指标上报开关。

_开关数：9_


#### `--export-uma-logs-to-file`

- **符号**: `switches::kExportUmaLogsToFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：观察会话期间所有 UMA 日志，关闭时自动导出到指定文件路径（不存在则创建）；同时让 chrome://metrics-internals 可查看所有 UMA 日志。例 `--export-uma-logs-to-file=/tmp/logs.json`。
> Enables the observing of all UMA logs created during the session and automatically exports them to the passed file path on shutdown (the file is created if it does not already exist). This also enables viewing all UMA logs in the chrome://metrics-internals debug page. The format of the exported file is outlined in MetricsServiceObserver::ExportLogsAsJson(). Example usage: --export-uma-logs-to-file=/tmp/logs.json

#### `--force-enable-metrics-reporting`

- **符号**: `switches::kForceEnableMetricsReporting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制开启 metrics 上报（不要在测试中用，会真实发到服务器）。
> Forces metrics reporting to be enabled. Should not be used for tests as it will send data to servers.

#### `--force-msbb-setting-on-for-ukm`

- **符号**: `switches::kForceMsbbSettingOnForUkm` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制 MSBB 设置为开以记录 UKM（仅自动化测试用）。
> Forces MSBB setting to be on for UKM recording. Should only be used in automated testing browser sessions in which it is infeasible or impractical to toggle the setting manually.

#### `--metrics-recording-only`

- **符号**: `switches::kMetricsRecordingOnly` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：只记录 metrics 报告但不上传（与 --force-enable-metrics-reporting 不同：会跑完整流程，但报告会被 drop）。
> Enables the recording of metrics reports but disables reporting. In contrast to kForceEnableMetricsReporting, this executes all the code that a normal client would use for reporting, except the report is dropped rather than sent to the server. This is useful for finding issues in the metrics code during UI and performance tests.

#### `--metrics-upload-interval`

- **符号**: `switches::kMetricsUploadIntervalSec` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 UMA/UKM 的上传时间间隔（秒，桌面默认 1800）。
> Override the standard time interval between each metrics report upload for UMA and UKM. It is useful to set to a short interval for debugging. Unit in seconds. (The default is 1800 seconds on desktop).

#### `--reset-variation-state`

- **符号**: `switches::kResetVariationState` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制重置一次性随机化的 FieldTrial 状态（即 Chrome Variations 状态）。
> Forces a reset of the one-time-randomized FieldTrials on this client, also known as the Chrome Variations state.

#### `--ukm-server-url`

- **符号**: `switches::kUkmServerUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 UKM 上报服务器 URL（仅 debug 构建可用）。
> Overrides the URL of the server that UKM reports are uploaded to. This can only be used in debug builds.

#### `--uma-insecure-server-url`

- **符号**: `switches::kUmaInsecureServerUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：默认 UMA 安全 URL 失败时使用的不安全 fallback URL（仅 debug 构建可用）。
> Overrides the URL of the server that UMA reports are uploaded to when the connection to the default secure URL fails (see |kUmaServerUrl|). This can only be used in debug builds.

#### `--uma-server-url`

- **符号**: `switches::kUmaServerUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 UMA 上报服务器 URL（仅 debug 构建可用）。
> Overrides the URL of the server that UMA reports are uploaded to. This can only be used in debug builds.


### Heap Profiling 服务 (components/services/heap_profiling/public/cpp/switches.cc)

独立堆分析服务开关。

_开关数：23_


#### `--memlog`

- **符号**: `switches::kMemlogMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Heap Profiling，并指定剖析对象（取值见下面 all/all-renderers/browser/gpu/manual/minimal/utility-and-browser/...）。

#### `--all`

- **符号**: `switches::kMemlogModeAll` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：剖析所有进程。

#### `--all-renderers`

- **符号**: `switches::kMemlogModeAllRenderers` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：剖析所有 Renderer。

#### `--all-utilities`

- **符号**: `switches::kMemlogModeAllUtilities` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：剖析所有 Utility。

#### `--browser`

- **符号**: `switches::kMemlogModeBrowser` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：仅剖析浏览器进程。

#### `--gpu`

- **符号**: `switches::kMemlogModeGpu` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：仅剖析 GPU 进程。

#### `--manual`

- **符号**: `switches::kMemlogModeManual` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：手动控制开始/结束。

#### `--minimal`

- **符号**: `switches::kMemlogModeMinimal` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：最小化（只剖析 browser+GPU）。

#### `--renderer-sampling`

- **符号**: `switches::kMemlogModeRendererSampling` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：以采样方式剖析 Renderer。

#### `--utility-and-browser`

- **符号**: `switches::kMemlogModeUtilityAndBrowser` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：剖析 Utility + 浏览器。

#### `--utility-sampling`

- **符号**: `switches::kMemlogModeUtilitySampling` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog 取值：以采样方式剖析 Utility。

#### `--memlog-sampling-rate`

- **符号**: `switches::kMemlogSamplingRate` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Heap Profiling 采样率（字节，取值见下面）。

#### `--100000`

- **符号**: `switches::kMemlogSamplingRate100KB` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog-sampling-rate 取值：100 KB。

#### `--10000`

- **符号**: `switches::kMemlogSamplingRate10KB` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog-sampling-rate 取值：10 KB。

#### `--1000000`

- **符号**: `switches::kMemlogSamplingRate1MB` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog-sampling-rate 取值：1 MB。

#### `--500000`

- **符号**: `switches::kMemlogSamplingRate500KB` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog-sampling-rate 取值：500 KB。

#### `--50000`

- **符号**: `switches::kMemlogSamplingRate50KB` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog-sampling-rate 取值：50 KB。

#### `--5000000`

- **符号**: `switches::kMemlogSamplingRate5MB` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog-sampling-rate 取值：5 MB。

#### `--memlog-stack-mode`

- **符号**: `switches::kMemlogStackMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Heap Profiling 调用栈采集模式（取值见下面）。

#### `--mixed`

- **符号**: `switches::kMemlogStackModeMixed` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog-stack-mode 取值：混合（native + 伪栈）。

#### `--native`

- **符号**: `switches::kMemlogStackModeNative` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `gpu/command_buffer/service/gpu_switches.cc`
- **用途**：--memlog-stack-mode 取值：原生回溯。

#### `--native-with-thread-names`

- **符号**: `switches::kMemlogStackModeNativeWithThreadNames` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog-stack-mode 取值：原生回溯并带线程名。

#### `--pseudo`

- **符号**: `switches::kMemlogStackModePseudo` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--memlog-stack-mode 取值：伪栈（基于 trace event）。


### Tracing (components/tracing/common/tracing_switches.cc)

Chrome tracing 启动开关。

_开关数：19_


#### `--background-tracing-output-path`

- **符号**: `switches::kBackgroundTracingOutputPath` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：background tracing 的本地输出目录（需配合 --enable-background-tracing）。
> Sets a local folder destination for tracing data. This is only used if kEnableBackgroundTracing is also specified.

#### `--default-trace-buffer-size-limit-in-kb`

- **符号**: `switches::kDefaultTraceBufferSizeLimitInKb` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：trace config 未指定 buffer size 时的默认值，对所有 trace 会话生效。
> This is only used when we did not set buffer size in trace config and will be used for all trace sessions. If not provided, we will use the default value provided in perfetto_config.cc

#### `--enable-background-tracing`

- **符号**: `switches::kEnableBackgroundTracing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过 ChromeFieldTracingConfig 序列化 proto 启用 background tracing。
> Enables background tracing by passing a scenarios config as an argument. The config is a serialized proto `perfetto.protos.ChromeFieldTracingConfig` defined in third_party/perfetto/protos/perfetto/config/chrome/scenario_config.proto.
> protoc can be used to generate a serialized proto config with protoc --encode=perfetto.protos.ChromeFieldTracingConfig --proto_path=third_party/perfetto/ third_party/perfetto/protos/perfetto/config/chrome/scenario_config.proto < {input txt config}.pbtxt > {output proto config}.pb

#### `--enable-tracing`

- **符号**: `switches::kEnableTracing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动时启用 tracing（与 --trace-startup 实现相同，差别在默认时长：enable-tracing 不限时）。

#### `--enable-tracing-format`

- **符号**: `switches::kEnableTracingFormat` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：设置 --enable-tracing 输出格式（与 --trace-startup-format 同义）。

#### `--enable-tracing-output`

- **符号**: `switches::kEnableTracingOutput` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：类似 --trace-startup-file 但生成更详细 basename；空值或以路径分隔符结尾视为目录；被 --trace-startup-file 覆盖。
> Similar to the flag above, with the following differences: - A more detailed basename will be generated. - If the value is empty or ends with path separator, the provided directory will be used (with empty standing for current directory) and a detailed basename file will be generated. It is ignored if --trace-startup-file is specified.

#### `--perfetto-disable-interning`

- **符号**: `switches::kPerfettoDisableInterning` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：perfetto proto 中每个 TraceEvent 都重复写出可 intern 的数据（不复用 string id）。
> Repeat internable data for each TraceEvent in the perfetto proto format.

#### `--trace-buffer-handle`

- **符号**: `switches::kTraceBufferHandle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（子进程内部）共享内存段句柄，子进程通过它把 tracing 数据回传给 tracing service；允许在沙箱建立前就开始 tracing。
> Handle to the shared memory segment a child process should use to transmit tracing data back to the tracing service. This flag allows tracing to be recorded before sandbox setup.

#### `--trace-config-file`

- **符号**: `switches::kTraceConfigFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过 JSON 配置文件启用 startup tracing（被 --trace-startup 覆盖）。
> Enables startup tracing by passing a file path containing the chrome Json tracing config as an argument. This flag will be ignored if --trace-startup is provided.

#### `--trace-config-handle`

- **符号**: `switches::kTraceConfigHandle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过 SMB 句柄传递序列化的 perfetto 配置，从启动起记录 TRACE_EVENT（被 --trace-startup 覆盖）。
> Causes TRACE_EVENT flags to be recorded from startup, passing a SMB handle containing the serialized perfetto config. This flag will be ignored if --trace-startup is provided.

#### `--trace-perfetto-config-file`

- **符号**: `switches::kTracePerfettoConfigFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过文件提供 perfetto 配置（序列化或 base64）启用 startup tracing。
> Enables startup tracing by passing a file path containing the perfetto config as an argument. The config is a serialized or base64 encoded proto `perfetto.protos.TraceConfig` defined in third_party/perfetto/protos/perfetto/config/trace_config.proto. This flag will be ignored if --trace-startup is provided.

#### `--trace-process-track-uuid`

- **符号**: `switches::kTraceProcessTrackUuid` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（子进程内部）由浏览器选择并传给子进程的 perfetto track uuid，便于稳定。
> Child process track uuid used by perfetto; this is chosen by the browser process and sent to child processes to get predictable track uuid.

#### `--trace-smb-size`

- **符号**: `switches::kTraceSmbSize` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：tracing 共享内存 buffer 大小（KB，默认 4096，桌面 SMB page 32KB / Android 4KB 的整数倍）。
> Configures the size of the shared memory buffer used for tracing. Value is provided in kB. Defaults to 4096. Should be a multiple of the SMB page size (currently 32kB on Desktop or 4kB on Android).

#### `--trace-startup`

- **符号**: `switches::kTraceStartup` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动起记录 TRACE_EVENT，可逗号指定 categories（如 `--trace-startup=base,net`）；trace-startup 默认时长 5 秒。
> Causes TRACE_EVENT flags to be recorded from startup. Optionally, can specify the specific trace categories to include (e.g. --trace-startup=base,net) otherwise, all events are recorded. Setting this flag results in the first call to BeginTracing() to receive all trace events since startup. Historically, --trace-startup was used for browser startup profiling and --enable-tracing was used for browsertest tracing.
> Now they are share the same implementation, but both are still supported to avoid disrupting existing workflows. The only difference between them is the default duration (5 seconds for trace-startup, unlimited for enable-tracing). If both are specified, 'trace-startup' takes precedence. In Chrome, you may find --trace-startup-file and --trace-startup-duration to control the auto-saving of the trace (not supported in the base-only TraceLog component).

#### `--trace-startup-duration`

- **符号**: `switches::kTraceStartupDuration` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：startup tracing 时长（秒）。--trace-startup 默认 5 秒；--enable-tracing 默认无限。
> Sets the time in seconds until startup tracing ends. If omitted: - if --trace-startup is specified, a default of 5 seconds is used. - if --enable-tracing is specified, tracing lasts until the browser is closed. Has no effect otherwise.

#### `--trace-startup-file`

- **符号**: `switches::kTraceStartupFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：startup tracing 输出文件路径，默认 `chrometrace.log`；值 `none` 表示不自动保存（首次手动 trace 接收所有事件）。
> If supplied, sets the file which startup tracing will be stored into, if omitted the default will be used "chrometrace.log" in the current directory. Has no effect unless --trace-startup is also supplied. Example: --trace-startup --trace-startup-file=/tmp/trace_event.log As a special case, can be set to 'none' - this disables automatically saving the result to a file and the first manually recorded trace will then receive all events since startup.

#### `--trace-startup-format`

- **符号**: `switches::kTraceStartupFormat` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：startup tracing 输出格式：`json` 或 `proto`（默认 proto；后者支持增量写）。仅在 trace-startup-owner 为 controller 时生效。
> Sets the output format for the trace, valid values are "json" and "proto". If not set, the current default is "proto". "proto", unlike json, supports writing the trace into the output file incrementally and is more likely to retain more data if the browser process unexpectedly terminates. Ignored if "trace-startup-owner" is not "controller".

#### `--trace-startup-owner`

- **符号**: `switches::kTraceStartupOwner` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：startup tracing 协调者：`controller`（默认）/`devtools`/`system`。
> Specifies the coordinator of the startup tracing session. If the legacy tracing backend is used instead of perfetto, providing this flag is not necessary. Valid values: 'controller', 'devtools', or 'system'. Defaults to 'controller'. If 'controller' is specified, the session is controlled and stopped via the TracingController (e.g. to implement the timeout). If 'devtools' is specified, the startup tracing session will be owned by DevTools and thus can be controlled (i.e.
> stopped) via the DevTools Tracing domain on the first session connected to the browser endpoint. If 'system' is specified, the system Perfetto service should already be tracing on a supported platform (currently only Android). Session is stopped through the normal methods for stopping system traces.

#### `--trace-startup-record-mode`

- **符号**: `switches::kTraceStartupRecordMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：tracing 记录模式（默认 record-until-full）。
> If supplied, sets the tracing record mode and options; otherwise, the default "record-until-full" mode will be used.


### Mojo Test (mojo/core/test/test_switches.cc)

mojo 核心测试开关。

_开关数：2_


#### `--mojo-is-broker`

- **符号**: `switches::kMojoIsBroker` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制本进程的 Mojo node 配置为 broker（仅 MojoTestSuiteBase 测试用）。
> Forces the process's global Mojo node to be configured as a broker. Only honored for test runners using MojoTestSuiteBase.

#### `--no-mojo`

- **符号**: `switches::kNoMojo` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：完全禁用 Mojo 初始化（仅测试子进程，见 base::MultiprocessTest）。
> Disables Mojo initialization completely in the process. Only applies to test child processes. See base::MultiprocessTest.


### 沙箱 (sandbox/policy/switches.cc)

沙箱策略开关：启用/禁用沙箱、沙箱类型、日志等。

_开关数：15_


#### `--add-xr-appcontainer-caps`

- **符号**: `switches::kAddXrAppContainerCaps` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows XR）给 XR 合成用的 AppContainer 沙箱追加额外能力。
> Add additional capabilities to the AppContainer sandbox used for XR compositing.

#### `--allow-sandbox-debugging`

- **符号**: `switches::kAllowSandboxDebugging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许调试沙箱进程（见 zygote_main_linux.cc）。
> Allows debugging of sandboxed processes (see zygote_main_linux.cc).

#### `--allow-third-party-modules`

- **符号**: `switches::kAllowThirdPartyModules` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（Windows 10+）关闭 BINARY_SIGNATURE 缓解策略以允许第三方模块注入。
> Allows third party modules to inject by disabling the BINARY_SIGNATURE mitigation policy on Win10+. Also has other effects in ELF.

#### `--disable-gpu-sandbox`

- **符号**: `switches::kDisableGpuSandbox` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 GPU 进程沙箱。
> Disables the GPU process sandbox.

#### `--disable-landlock-sandbox`

- **符号**: `switches::kDisableLandlockSandbox` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Android）禁用 Landlock 沙箱。
> Disables the Landlock sandbox (Android only).

#### `--disable-metal-shader-cache`

- **符号**: `switches::kDisableMetalShaderCache` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：（macOS）禁用 Metal shader 缓存（用 GPU 沙箱阻止访问）。
> Disables Metal's shader cache, using the GPU sandbox to prevent access to it.

#### `--disable-namespace-sandbox`

- **符号**: `switches::kDisableNamespaceSandbox` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Linux）禁用 namespace 沙箱。
> Disables usage of the namespace sandbox.

#### `--disable-seccomp-filter-sandbox`

- **符号**: `switches::kDisableSeccompFilterSandbox` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Linux）禁用 seccomp-bpf 沙箱。
> Disable the seccomp filter sandbox (seccomp-bpf) (Linux only).

#### `--disable-setuid-sandbox`

- **符号**: `switches::kDisableSetuidSandbox` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Linux）禁用 setuid 沙箱。
> Disable the setuid sandbox (Linux only).

#### `--enable-sandbox-logging`

- **符号**: `switches::kEnableSandboxLogging` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：（macOS）沙箱每次拒绝访问资源都写 syslog。
> Cause the OS X sandbox write to syslog every time an access to a resource is denied by the sandbox.

#### `--gpu-sandbox-allow-sysv-shm`

- **符号**: `switches::kGpuSandboxAllowSysVShm` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 沙箱放行 shmat() 系统调用。
> Allows shmat() system call in the GPU sandbox.

#### `--gpu-sandbox-failures-fatal`

- **符号**: `switches::kGpuSandboxFailuresFatal` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：GPU 沙箱失败视为致命错误。
> Makes GPU sandbox failures fatal.

#### `--no-sandbox`

- **符号**: `switches::kNoSandbox` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：所有正常沙箱化的进程都不再沙箱化（浏览器级测试开关）。
> Disables the sandbox for all process types that are normally sandboxed. Meant to be used as a browser-level switch for testing purposes only.

#### `--no-zygote-sandbox`

- **符号**: `switches::kNoZygoteSandbox` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS)
- **用途**：启动 zygote 时不加沙箱，由后续 fork 出的进程自己加沙箱。
> Instructs the zygote to launch without a sandbox. Processes forked from this type of zygote will apply their own custom sandboxes later.

#### `--service-sandbox-type`

- **符号**: `switches::kServiceSandboxType` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：对 service 进程使用的沙箱类型（取值见紧随的一组常量）。
> Type of sandbox to apply to the process running the service, one of the values in the next block.


### 内存检测 (services/resource_coordinator/memory_instrumentation/switches.cc)

内存快照/仪表化开关。

_开关数：2_


#### `--disable-chrome-tracing-computation`

- **符号**: `switches::kDisableChromeTracingComputation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：写 trace 时禁用 tracing service 的 graph 计算。
> Disable the tracing service graph compuation while writing the trace.

#### `--use-heap-profiling-proto-writer`

- **符号**: `switches::kUseHeapProfilingProtoWriter` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：memory_instrumentation 使用 heap profiling proto writer。


### Service 可执行 (services/service_manager/public/cpp/service_executable/switches.cc)

service 可执行文件公共开关。

_开关数：2_


#### `--service-name`

- **符号**: `switches::kServiceName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定要运行的 service 名（调试用，或同一可执行文件可作多 service）。
> Indicates the name of the service to run. Useful for debugging, or if a service executable is built to support being run as a number of potential different services.

#### `--service-request-attachment-name`

- **符号**: `switches::kServiceRequestAttachmentName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Mojo invitation 中作为 service 接收 PendingReceiver 的附件名。
> The name of the |mojo::PendingReceiver<service_manager::mojom::Service>| message pipe handle that is attached to the incoming Mojo invitation received by the service.


### Service Manager (services/service_manager/switches.cc)

Service Manager 全局开关。

_开关数：1_


#### `--enable-service-manager-tracing`

- **符号**: `switches::kEnableTracing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 service manager 的 tracing。
> Enable the tracing service.


## 六、扩展 / WebUI / 应用

扩展 (extensions)、应用 (apps)、WebUI flags 页面相关开关。


### 扩展更新 (chrome/browser/extensions/updater/extension_updater_switches.cc)

扩展更新源 URL 覆盖等。

_开关数：2_


#### `--extension-force-channel`

- **符号**: `switches::kSwitchExtensionForceChannel` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制扩展 updater 使用指定 channel。

#### `--extension-updater-test-request`

- **符号**: `switches::kSwitchTestRequestParam` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：标记扩展 updater 请求为测试请求。


### flags UI (components/webui/flags/flags_ui_switches.cc)

chrome://flags 相关开关。

_开关数：2_


#### `--flag-switches-begin`

- **符号**: `switches::kFlagSwitchesBegin` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：标记：about:flags 添加到命令行的开关从此处开始（仅用于在 about:version 显示，无功能效果）。
> These two flags are added around the switches about:flags adds to the command line. This is useful to see which switches were added by about:flags on about:version. They don't have any effect.

#### `--flag-switches-end`

- **符号**: `switches::kFlagSwitchesEnd` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：标记：about:flags 添加到命令行的开关至此结束。


### Extensions (extensions/common/switches.cc)

扩展系统开关：解锁扩展白名单、加载未打包扩展、Native Messaging 主机白名单等。

_开关数：31_


#### `--allow-future-manifest-version`

- **符号**: `switches::kAllowFutureManifestVersion` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许加载未来版本的扩展 manifest。

#### `--allow-http-background-page`

- **符号**: `switches::kAllowHTTPBackgroundPage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许扩展的后台页使用 HTTP。

#### `--allow-legacy-extension-manifests`

- **符号**: `switches::kAllowLegacyExtensionManifests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许加载旧版扩展 manifest。

#### `--allowlisted-extension-id`

- **符号**: `switches::kAllowlistedExtensionID` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把给定扩展 ID 加入白名单（拥有更多权限，测试用）。

#### `--disable-app-content-verification`

- **符号**: `switches::kDisableAppContentVerification` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用应用内容校验。

#### `--disable-extensions-file-access-check`

- **符号**: `switches::kDisableExtensionsFileAccessCheck` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用扩展的文件访问检查。

#### `--disable-extensions-http-throttling`

- **符号**: `switches::kDisableExtensionsHttpThrottling` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用扩展 HTTP 请求节流。

#### `--embedded-extension-options`

- **符号**: `switches::kEmbeddedExtensionOptions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用内嵌扩展选项页特性。

#### `--enable-ble-advertising-in-apps`

- **符号**: `switches::kEnableBLEAdvertising` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 apps 中启用 BLE 广播。

#### `--enable-crx-hash-check`

- **符号**: `switches::kEnableCrxHashCheck` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：下载扩展 .crx 时校验其哈希。

#### `--enable-experimental-extension-apis`

- **符号**: `switches::kEnableExperimentalExtensionApis` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用实验性扩展 API。

#### `--extension-process`

- **符号**: `switches::kExtensionProcess` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：标记当前进程为扩展 Renderer 进程。

#### `--extension-test-api-on-web-pages`

- **符号**: `switches::kExtensionTestApiOnWebPages` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许在网页中使用扩展测试 API。

#### `--extensions-install-verification`

- **符号**: `switches::kExtensionsInstallVerification` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：控制扩展安装校验（参数 `enforce`/`bootstrap`/`enforce_strict`）。

#### `--extensions-not-webstore`

- **符号**: `switches::kExtensionsNotWebstore` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把给定 ID 视为非 webstore 扩展。

#### `--extensions-on-chrome-urls`

- **符号**: `switches::kExtensionsOnChromeURLs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许扩展在 chrome:// 页面运行。

#### `--extensions-on-extension-urls`

- **符号**: `switches::kExtensionsOnExtensionURLs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许扩展在 chrome-extension:// 页面上运行内容脚本。

#### `--load-apps`

- **符号**: `switches::kLoadApps` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按目录加载指定 App（未打包）。

#### `--load-extension`

- **符号**: `switches::kLoadExtension` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：按目录加载未打包扩展（开发者模式），多个用逗号分隔。

#### `--offscreen-document-testing`

- **符号**: `switches::kOffscreenDocumentTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：offscreen document 测试相关开关。

#### `--set-extension-throttle-test-params`

- **符号**: `switches::kSetExtensionThrottleTestParams` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为扩展 throttle 测试设置参数。

#### `--show-component-extension-options`

- **符号**: `switches::kShowComponentExtensionOptions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：显示组件扩展的选项页（默认隐藏）。

#### `--enable-trace-app-source`

- **符号**: `switches::kTraceAppSource` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 App 来源的 trace。

#### `--custom-action-iph`

- **符号**: `switches::kZeroStatePromoCustomActionIph` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展 zero-state 提示的 variation：自定义 action IPH。

#### `--custom-ui-chip-iph`

- **符号**: `switches::kZeroStatePromoCustomUiChipIphV1` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展 zero-state 提示的 variation：自定义 UI chip IPH。

#### `--custom-ui-chip-iph-v2`

- **符号**: `switches::kZeroStatePromoCustomUiChipIphV2` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展 zero-state 提示的 variation：自定义 UI chip IPH v2。

#### `--custom-ui-chip-iph-v3`

- **符号**: `switches::kZeroStatePromoCustomUiChipIphV3` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展 zero-state 提示的 variation：自定义 UI chip IPH v3。

#### `--custom-ui-plain-link-iph`

- **符号**: `switches::kZeroStatePromoCustomUiPlainLinkIph` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：扩展 zero-state 提示的 variation：纯链接 IPH。

#### `--extension-zero-state-iph-variant`

- **符号**: `switches::kZeroStatePromoIphVariantParamName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定扩展 zero-state IPH 的 variant。

#### `--disable-extensions`

- **符号**: `switches::kDisableExtensions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用所有扩展。
> Disables extensions.

#### `--disable-extensions-except`

- **符号**: `switches::kDisableExtensionsExcept` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：除逗号分隔名单之外禁用其他所有扩展。
> Disable extensions except those specified in a comma-separated list.


## 七、隐私 / 安全 / Policy / Sync / Signin

企业策略、同步、身份 (Signin)、密码管理器、自动填充、WebAuthn、Trusted Vault 等开关。


### Bound Session (chrome/browser/signin/bound_session_credentials/bound_session_switches.cc)

_开关数：2_


#### `--bound-session-cookie-rotation-delay`

- **符号**: `switches::kCookieRotationDelay` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为 bound session cookie 轮换请求加人为延迟（毫秒）。
> Used to add artificial delay to the cookie rotation request. It expects as value a number representing the delay in milliseconds.

#### `--bound-session-cookie-rotation-result`

- **符号**: `switches::kCookieRotationResult` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：模拟 bound session cookie 轮换响应（值为 BoundSessionRefreshCookieFetcher::Result 枚举的整数；用于模拟错误而非成功）。
> Used to simulate the cookie rotation network request response. It expects as a value a number representing the enum value of `BoundSessionRefreshCookieFetcher::Result`. Note: This should be used to simulate error cases not success. If success `0` is used, bound cookies won't be set.


### WebAuthn (chrome/browser/webauthn/webauthn_switches.cc)

_开关数：3_


#### `--webauthn-gpm-pin-reset-reauth-url`

- **符号**: `switches::kGpmPinResetReauthUrlSwitch` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：WebAuthn Google Password Manager PIN 重置重新认证 URL。

#### `--webauthn-permit-enterprise-attestation`

- **符号**: `switches::kPermitEnterpriseAttestationOriginList` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许 WebAuthn 企业 attestation（按 RP ID 列表）。

#### `--webauthn-remote-proxied-requests-allowed-additional-origin`

- **符号**: `switches::kRemoteProxiedRequestsAllowedAdditionalOrigin` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为 WebAuthn 远程代理请求额外放行的 origin。


### 自动填充 (components/autofill/core/common/autofill_switches.cc)

_开关数：7_


#### `--autofill-api-key`

- **符号**: `switches::kAutofillAPIKey` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：调用 Autofill API 时使用的 API key（覆盖 Chrome 内置）。
> Sets the API key that will be used when calling Autofill API instead of using Chrome's baked key by default. You can use this to test new versions of the API that are not linked to the Chrome baked key yet.

#### `--autofill-server-url`

- **符号**: `switches::kAutofillServerURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖默认 Autofill 服务器 URL，格式 `scheme://host[:port]/prefix/`。
> Override the default autofill server URL with "scheme://host[:port]/prefix/".

#### `--autofill-upload-throttling-period-in-days`

- **符号**: `switches::kAutofillUploadThrottlingPeriodInDays` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Autofill 事件注册表的上传节流周期（天）。
> The number of days after which to reset the registry of autofill events for which an upload has been sent.

#### `--ignore-autocomplete-off-autofill`

- **符号**: `switches::kIgnoreAutocompleteOffForAutofill` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Autofill 数据（资料 + 信用卡）忽略 `autocomplete="off"`。
> Ignores autocomplete="off" for Autofill data (profiles + credit cards).

#### `--show-autofill-signatures`

- **符号**: `switches::kShowAutofillSignatures` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在表单和字段上标注 Autofill 签名（调试）。
> Annotates forms and fields with Autofill signatures.

#### `--show-autofill-type-predictions`

- **符号**: `switches::kShowAutofillTypePredictions` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在表单字段上标注 Autofill 类型预测。
> Annotates forms with Autofill field type predictions.

#### `--wallet-service-use-sandbox`

- **符号**: `switches::kWalletServiceUseSandbox` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：使用 Online Wallet 沙箱环境 URL（开发测试）。
> Use the sandbox Online Wallet service URL (for developer testing).


### Browser Sync (components/browser_sync/browser_sync_switches.cc)

浏览器层 Sync 相关开关。

_开关数：3_


#### `--disable-sync-invalidation-optimizations`

- **符号**: `switches::kDisableSyncInvalidationOptimizations` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 sync invalidation 在 Commit 请求中的优化（live test 防 flaky）。
> Disables the optimizations flags in Commit requests for sync invalidations. This is useful for live tests to avoid flakiness.

#### `--enable-local-sync-backend`

- **符号**: `switches::kEnableLocalSyncBackend` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 LoopbackServer 实现的本地 sync 后端。
> Enabled the local sync backend implemented by the LoopbackServer.

#### `--local-sync-backend-dir`

- **符号**: `switches::kLocalSyncBackendDir` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：本地 sync 后端目录（仅 --enable-local-sync-backend 生效时有意义）。
> Specifies the local sync backend directory. The name is chosen to mimic user-data-dir etc. This flag only matters if the enable-local-sync-backend flag is present.


### OS Crypt (components/os_crypt/common/os_crypt_switches.h)

操作系统密钥链 (macOS Keychain / Windows DPAPI / Linux Secret Service) 访问开关。

_开关数：1_


#### `--use-mock-keychain`

- **符号**: `switches::kUseMockKeychain` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_APPLE)
- **用途**：使用 mock keychain（避免阻塞对话框导致超时，测试用）。
> Uses mock keychain for testing purposes, which prevents blocking dialogs from causing timeouts.


### 密码管理器 (components/password_manager/core/browser/password_manager_switches.cc)

_开关数：4_


#### `--enable-encryption-selection`

- **符号**: `switches::kEnableEncryptionSelection` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX)
- **用途**：允许用户在设置里禁用密码后端。
> Enables the feature of allowing the user to disable the backend via a setting.

#### `--enable-share-button-unbranded`

- **符号**: `switches::kEnableShareButtonUnbranded` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（unbranded 构建）密码详情 UI 显示密码分享按钮。
> Enables Password Sharing button in password details UI in settings when running unbranded builds.

#### `--password-store`

- **符号**: `switches::kPasswordStore` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX)
- **同名定义**: `headless/public/switches.h`
- **用途**：（Linux）选择密码加密存储后端：`kwallet`/`kwallet5`/`kwallet6`/`gnome-libsecret`/`basic`，其他值则自动检测。
> Specifies which encryption storage backend to use. Possible values are kwallet, kwallet5, kwallet6, gnome-libsecret, basic. Any other value will lead to Chrome detecting the best backend automatically.

#### `--password-change-url`

- **符号**: `switches::kPasswordChangeUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在指定 URL（按 eTLD+1 匹配）测试密码变更功能。
> This switch allows testing password change feature on provided URL. Password change will be offered by submitting password form on any URL with matching eTLD+1.


### 企业策略 (components/policy/core/common/policy_switches.cc)

云端策略/机器级策略/策略验证开关。

_开关数：7_


#### `--policy`

- **符号**: `switches::kChromePolicy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：通过命令行直接下发策略值。
> Set policy value by command line.

#### `--device-management-url`

- **符号**: `switches::kDeviceManagementUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：设备管理后端 URL，用于拉取配置策略与执行设备任务。
> Specifies the URL at which to communicate with the device management backend to fetch configuration policies and perform other device tasks.

#### `--encrypted-reporting-url`

- **符号**: `switches::kEncryptedReportingUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：加密 Reporting 上报的 URL。
> Specifies the URL at which to upload encrypted reports.

#### `--file-storage-server-upload-url`

- **符号**: `switches::kFileStorageServerUploadUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：File Storage Server 上传 URL（上报日志/支持包）。
> Specifies the URL at which to communicate with File Storage Server (go/crosman-file-storage-server) to upload log and support packet files.

#### `--policy-verification-key`

- **符号**: `switches::kPolicyVerificationKey` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用命令行传入的 verification_key 替换内置的（仅 unit/browser 测试）。
> Replace the original verification_key with the one provided by the command line flag. Can be used only for unit tests or browser tests.

#### `--realtime-reporting-url`

- **符号**: `switches::kRealtimeReportingUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：实时 Reporting 上传 URL。
> Specifies the URL at which to upload real-time reports.

#### `--secure-connect-api-url`

- **符号**: `switches::kSecureConnectApiUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Secure Connect API 基础 URL。
> Specifies the base URL to contact the secure connect Api.


### Signin (components/signin/public/base/signin_switches.cc)

账号登录相关开关。

_开关数：2_


#### `--clear-token-service`

- **符号**: `switches::kClearTokenService` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：使用前清空 token service（模拟凭证过期，测试用）。
> Clears the token service before using it. This allows simulating the expiration of credentials during testing.

#### `--force-fre-default-browser-step`

- **符号**: `switches::kForceFreDefaultBrowserStep` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(ENABLE_DICE_SUPPORT)
- **用途**：（桌面）首次运行体验中强制启用“设为默认浏览器”步骤。
> Force enable the default browser step in the first run experience on Desktop.


### Sync (components/sync/base/command_line_switches.cc)

Chrome Sync 服务端点、调试开关。

_开关数：7_


#### `--disable-sync`

- **符号**: `switches::kDisableSync` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 Google 账号浏览器数据同步。
> Disables syncing browser data to a Google Account.

#### `--sync-deferred-startup-timeout-seconds`

- **符号**: `switches::kSyncDeferredStartupTimeoutSeconds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 sync deferred init 的 fallback 超时（秒）。
> Allows overriding the deferred init fallback timeout.

#### `--sync-include-specifics`

- **符号**: `switches::kSyncIncludeSpecificsInProtocolLog` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：控制 chrome://sync-internals 中 “Capture Specifics” 标志的初始状态。
> Controls whether the initial state of the "Capture Specifics" flag on chrome://sync-internals is enabled.

#### `--sync-protocol-log-buffer-size`

- **符号**: `switches::kSyncProtocolLogBufferSize` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：chrome://sync-internals 中 ProtocolEvents 的缓冲条数。
> Controls the number of ProtocolEvents that are buffered, and thus can be displayed on newly-opened chrome://sync-internals tabs.

#### `--sync-url`

- **符号**: `switches::kSyncServiceURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 profile sync 的默认服务器。
> Overrides the default server used for profile sync.

#### `--sync-short-initial-retry-override`

- **符号**: `switches::kSyncShortInitialRetryOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：出错后让 sync 极快地重试（指数退避起步），调试用。
> This flag causes sync to retry very quickly (see polling_constants.h) the when it encounters an error, as the first step towards exponential backoff.

#### `--sync-short-nudge-delay-for-test`

- **符号**: `switches::kSyncShortNudgeDelayForTest` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：极大缩短 nudge 周期（仅集成测试用，正常情况会去除合并/防过载）。
> This flag significantly shortens the delay between nudge cycles. Its primary purpose is to speed up integration tests. The normal delay allows coalescing and prevention of server overload, so don't use this unless you're really sure that it's what you want.


### Trusted Vault (components/trusted_vault/command_line_switches.cc)

_开关数：2_


#### `--https://securitydomain-pa.googleapis.com/v1/`

- **符号**: `switches::kDefaultTrustedVaultServiceURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Trusted Vault 默认服务 URL（常量字符串，未必在命令行直接使用）。

#### `--trusted-vault-service-url`

- **符号**: `switches::kTrustedVaultServiceURLSwitch` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：trusted vault 密码短语使用的 vault 服务器 URL。
> Specifies the vault server used for trusted vault passphrase.


## 八、搜索 / 翻译 / NTP / Dom Distiller

默认搜索引擎、翻译、新标签页 (NTP)、阅读模式 (DOM Distiller) 等开关。


### Chrome Google (chrome/browser/google/switches.cc)

_开关数：2_


#### `--simulate-update-error-code`

- **符号**: `switches::kSimulateUpdateErrorCode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）模拟更新检查返回指定 GoogleUpdateErrorCode；需配合 --simulate-update-hresult。
> Simulates a GoogleUpdateErrorCode error by the update check. Must be supplied with |kSimulateUpdateHresult| switch.

#### `--simulate-update-hresult`

- **符号**: `switches::kSimulateUpdateHresult` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows）模拟更新检查返回的 HRESULT；不指定（hex）则默认 E_FAIL。
> Simulates a specific HRESULT error code returned by the update check. If the switch value is not specified (as hex) then it defaults to E_FAIL.


### NTP Modules (chrome/browser/new_tab_page/modules/modules_switches.cc)

新标签页模块调试开关。

_开关数：1_


#### `--signed-out-ntp-modules`

- **符号**: `switches::kSignedOutNtpModulesSwitch` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：未登录状态也允许显示 NTP 模块。


### DOM Distiller (components/dom_distiller/core/dom_distiller_switches.cc)

阅读模式相关开关。

_开关数：11_


#### `--adaboost`

- **符号**: `switches::kAdaBoost` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--reader-mode-heuristics 取值：AdaBoost 模型。

#### `--allarticles`

- **符号**: `switches::kAllArticles` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--reader-mode-heuristics 取值：所有页面都视为可阅读。

#### `--alwaystrue`

- **符号**: `switches::kAlwaysTrue` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--reader-mode-heuristics 取值：始终判定为可蒸馏。

#### `--enable-distillability-service`

- **符号**: `switches::kEnableDistillabilityService` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用页面可蒸馏性服务。

#### `--enable-dom-distiller`

- **符号**: `switches::kEnableDomDistiller` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 DOM Distiller（阅读模式）。

#### `--none`

- **符号**: `switches::kNone` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `device/vr/public/cpp/switches.cc`
- **用途**：--reader-mode-heuristics 取值：none（关闭启发判定）。

#### `--opengraph`

- **符号**: `switches::kOGArticle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--reader-mode-heuristics 取值：基于 OpenGraph 元数据。

#### `--discoverability`

- **符号**: `switches::kReaderModeDiscoverabilityParamName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--reader-mode-feedback 取值：discoverability。

#### `--reader-mode-feedback`

- **符号**: `switches::kReaderModeFeedback` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：阅读模式反馈采集模式。

#### `--reader-mode-heuristics`

- **符号**: `switches::kReaderModeHeuristics` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：阅读模式可蒸馏性启发：`adaboost`/`opengraph`/`allarticles`/`alwaystrue`/`none`。

#### `--offer-in-settings`

- **符号**: `switches::kReaderModeOfferInSettings` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--reader-mode-discoverability 取值：在设置中提供阅读模式开关。


### Google Base URL (components/google/core/common/google_switches.cc)

用于覆盖 google.com 基地址，常用于测试。

_开关数：2_


#### `--google-base-url`

- **符号**: `switches::kGoogleBaseURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定与 Google 通信时用的备用基 URL（测试用）。
> Specifies an alternate URL to use for speaking to Google. Useful for testing.

#### `--ignore-google-port-numbers`

- **符号**: `switches::kIgnoreGooglePortNumbers` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：忽略 google_util 中 PortPermission 指定的端口限制，便于与 EmbeddedTestServer 搭配测试。
> When set, this will ignore the PortPermission passed in the google_util.h methods and ignore the port numbers. This makes it easier to run tests for features that use these methods (directly or indirectly) with the EmbeddedTestServer, which is more representative of production.


### NTP Tiles (components/ntp_tiles/switches.cc)

新标签页瓷片来源开关。

_开关数：1_


#### `--enable-ntp-search-engine-country-detection`

- **符号**: `switches::kEnableNTPSearchEngineCountryDetection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用默认搜索引擎国家在 NTP 显示该国热门站点。
> Enables using the default search engine country to show country specific popular sites on the NTP.


### Regional (components/regional_capabilities/regional_capabilities_switches.cc)

_开关数：4_


#### `--DEFAULT_EEA`

- **符号**: `switches::kDefaultListCountryOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--search-engine-choice-country 特殊取值：启用 Waffle 程序，覆盖搜索引擎列表为默认集合。
> Special value for the `kSearchEngineChoiceCountry` command-line flag. Enables the Waffle program and overrides the list of search engines to display the default set.

#### `--EEA_ALL`

- **符号**: `switches::kEeaListCountryOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--search-engine-choice-country 特殊取值：启用 Waffle 程序，覆盖搜索引擎列表为全部 EEA 引擎。
> Special value for the `kSearchEngineChoiceCountry` command-line flag. Enables the Waffle program and overrides the list of search engines to display the list of all EEA engines.

#### `--search-engine-choice-country`

- **符号**: `switches::kSearchEngineChoiceCountry` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Profile 国家（用于搜索引擎选择等区域判定，测试用）；可传 2 字母国家码、program 名或特殊列表覆盖值。
> Overrides the profile country (which is among other things used for search engine choice region checks for example). Intended for testing. Parameter can be one of 3 things: - 2-letter country codes => Will override the profile country - A program name => Will override the country and the program - A specific list override => Will override the program, but instead of overriding the country, will use special values to force the search engine list to some preset testing ones.

#### `--TAIYAKI`

- **符号**: `switches::kTaiyakiProgramOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--search-engine-choice-country 特殊取值：启用 Taiyaki 程序（不支持的平台/构建会回退到默认）。
> Special value for the `kSearchEngineChoiceCountry` command-line flag. Enables the Taiyaki program. On unsupported platform / build types, will fall back to default program / unknown country.


### 搜索引擎 (components/search_engines/search_engines_switches.cc)

默认搜索引擎覆盖开关。

_开关数：5_


#### `--disable-search-engine-choice-screen`

- **符号**: `switches::kDisableSearchEngineChoiceScreen` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用搜索引擎选择屏幕（测试/自动化用）。
> Disable the search engine choice screen for testing / autmation.

#### `--extra-search-query-params`

- **符号**: `switches::kExtraSearchQueryParams` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在搜索/Instant URL 中追加额外 query 参数（测试用）。
> Additional query params to insert in the search and instant URLs.  Useful for testing.

#### `--force-search-engine-choice-screen`

- **符号**: `switches::kForceSearchEngineChoiceScreen` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：无视区域和已做选择，强制显示搜索引擎选择屏幕。
> Force-enable showing the search engine choice screen for testing regardless of region or choice already having been made.

#### `--ignore-no-first-run-for-search-engine-choice-screen`

- **符号**: `switches::kIgnoreNoFirstRunForSearchEngineChoiceScreen` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试用：忽略 --no-first-run 对搜索选择对话框的抑制。
> Override the --no-first-run dialog suppression for the search dialog for testing

#### `--NO_REPROMPT`

- **符号**: `switches::kSearchEngineChoiceNoRepromptString` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--search-engine-choice-trigger-reprompt-params 特殊值：不再次提示选择。
> The string that's passed to `switches::kSearchEngineChoiceTriggerRepromptParams` so that we don't reprompt users with the choice screen.


### Doodle Logos (components/search_provider_logos/switches.cc)

搜索页 doodle 后端开关。

_开关数：3_


#### `--google-doodle-url`

- **符号**: `switches::kGoogleDoodleUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖拉取 Google Doodle 的 URL。
> Overrides the URL used to fetch the current Google Doodle. Example: https://www.google.com/async/ddljson Testing? Try: https://www.gstatic.com/chrome/ntp/doodle_test/ddljson_android0.json https://www.gstatic.com/chrome/ntp/doodle_test/ddljson_android1.json https://www.gstatic.com/chrome/ntp/doodle_test/ddljson_android2.json https://www.gstatic.com/chrome/ntp/doodle_test/ddljson_android3.json https://www.gstatic.com/chrome/ntp/doodle_test/ddljson_android4.json

#### `--search-provider-logo-url`

- **符号**: `switches::kSearchProviderLogoURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用静态 URL 替代默认搜索引擎 logo。
> Use a static URL for the logo of the default search engine. Example: https://www.google.com/branding/logo.png

#### `--third-party-doodle-url`

- **符号**: `switches::kThirdPartyDoodleURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖第三方搜索引擎使用的 Doodle URL。
> Overrides the Doodle URL to use for third-party search engines. Testing? Try: https://www.gstatic.com/chrome/ntp/doodle_test/third_party_simple.json https://www.gstatic.com/chrome/ntp/doodle_test/third_party_animated.json


### 翻译 (components/translate/core/common/translate_switches.cc)

翻译后端/调试开关。

_开关数：3_


#### `--translate-ranker-model-url`

- **符号**: `switches::kTranslateRankerModelURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖翻译 ranker 模型下载 URL。
> Overrides the URL from which the translate ranker model is downloaded.

#### `--translate-script-url`

- **符号**: `switches::kTranslateScriptURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Google Translate 默认服务器。
> Overrides the default server used for Google Translate.

#### `--translate-security-origin`

- **符号**: `switches::kTranslateSecurityOrigin` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Translate 在隔离世界中运行所用的 security-origin。
> Overrides security-origin with which Translate runs in an isolated world.


## 九、Variations / 优化 / 组件更新 / 反馈

Finch 试验 (variations)、Optimization Guide、组件更新器 (component_updater)、错误页、反馈、Client Hints 等基础设施开关。


### Client Hints (components/client_hints/common/switches.cc)

_开关数：1_


#### `--initialize-client-hints-storage`

- **符号**: `switches::kInitializeClientHintsStorage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：预填充 Client Hints 存储。JSON 字典，key 为 origin，value 为逗号分隔的 client hint token 列表；相当于从该 origin 收到了对应 `Accept-CH` 响应头。仅作用于非隐身/非访客 Profile。
> Pre-load the client hint storage. Takes a JSON dict, with each key being an origin (RFC 6454 Section 6.2) and each value a comma-separated list of client hint tokens (RFC 8942 Section 3.1, client-hints-infrastructure Section 7.1). Each origin/token-list entry will be parsed and persisted to the Client Hints storage as though the token-list had come through an Accept-CH response header from a navigation from the origin.
> The initialization will only apply to non-OffTheRecord profiles, meaning incognito or guest profiles will not have the storage applied.


### 组件更新 (components/component_updater/component_updater_switches.cc)

组件更新器开关。

_开关数：4_


#### `--campaigns-test-tag`

- **符号**: `switches::kCampaignsTestTag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：选择测试 cohort 的 campaigns 文件版本，例 `--campaigns-test-tag=dev1`。
> Switch to control which serving campaigns file versions to select in test cohort. Example: `--campaigns-test-tag=dev1` will select test cohort which tag matches dev1.

#### `--component-updater`

- **符号**: `switches::kComponentUpdater` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：组件更新器调试选项，逗号分隔；仅浏览器进程有效。
> Comma-separated options to troubleshoot the component updater. Only valid for the browser process.

#### `--component-updater-trust-tokens-component-path`

- **符号**: `switches::kComponentUpdaterTrustTokensComponentPath` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：测试用：覆盖 Trust Tokens key commitment 组件路径。
> Optional testing override of the Trust Tokens key commitment component's path.

#### `--demo-app-test-tag`

- **符号**: `switches::kDemoModeTestTag` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：选择测试 cohort 的 demo mode app 版本。
> Switch to control which serving demo mode app versions to select in test cohort. Example: `--demo-app-test-tag=dev1` will select test cohort which tag matches dev1.


### Data Sharing (components/data_sharing/public/switches.h)

_开关数：1_


#### `--data-sharing-debug-logs`

- **符号**: `switches::kDataSharingDebugLoggingEnabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：无需打开 chrome://data-sharing-internals 也持久化 data sharing 日志。
> Switch for whether or not data sharing logs are stored at startup instead of only being stored/logged when chrome://data-sharing-internals is open.


### 错误页 (components/error_page/common/error_page_switches.cc)

_开关数：2_


#### `--disable-dinosaur-easter-egg`

- **符号**: `switches::kDisableDinosaurEasterEgg` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用离线页的小恐龙彩蛋。
> Disables the dinosaur easter egg on the offline interstitial.

#### `--enable-dinosaur-easter-egg-alt-images`

- **符号**: `switches::kEnableDinosaurEasterEggAltGameImages` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用小恐龙彩蛋的替换图像。
> Enable the dinosaur easter egg alternative images.


### 反馈 (components/feedback/feedback_switches.cc)

_开关数：1_


#### `--feedback-server`

- **符号**: `switches::kFeedbackServer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用户反馈提交使用的备用反馈服务器。
> Alternative feedback server to use when submitting user feedback


### Infobars (components/infobars/core/infobars_switches.h)

_开关数：1_


#### `--disable-infobars`

- **符号**: `switches::kDisableInfoBars` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用所有 infobar。


### Optimization Guide (components/optimization_guide/core/optimization_guide_switches.cc)

优化指南服务 / 模型下发开关。

_开关数：27_


#### `--enable-optimization-guide-debug-logs`

- **符号**: `switches::kDebugLoggingEnabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Optimization Guide 调试日志。

#### `--disable-checking-optimization-guide-user-permissions`

- **符号**: `switches::kDisableCheckingUserPermissionsForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Optimization Guide 跳过用户权限检查（测试用）。

#### `--disable-fetching-hints-at-navigation-start`

- **符号**: `switches::kDisableFetchingHintsAtNavigationStartForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁止在导航开始时拉取 hints。

#### `--disable-model-download-verification`

- **符号**: `switches::kDisableModelDownloadVerificationForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用模型下载校验。

#### `--enable-model-quality-dogfood-logging`

- **符号**: `switches::kEnableModelQualityDogfoodLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：无视客户端其他设置，强制启用模型质量日志（仅 dogfood 客户端）。
> Enables model quality logs regardless of other client-side settings, as long as the client is a dogfood client.

#### `--optimization-guide-fetch-hints-override`

- **符号**: `switches::kFetchHintsOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 hints 拉取调度与延迟，启动时立即对给定 host 列表（逗号分隔）拉取 hints。
> Overrides scheduling and time delays for fetching hints and causes a hints fetch immediately on start up using the provided comma separate lists of hosts.

#### `--optimization-guide-fetch-hints-override-timer`

- **符号**: `switches::kFetchHintsOverrideTimer` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 hints 拉取调度，使用 TopHostProvider 在启动时立即拉取（测试用）。
> Overrides the hints fetch scheduling and delay, causing a hints fetch immediately on start up using the TopHostProvider. This is meant for testing.

#### `--optimization-guide-get-free-disk-space-with-user-visible-priority-task`

- **符号**: `switches::kGetFreeDiskSpaceWithUserVisiblePriorityTask` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用 user-visible 优先级任务获取磁盘剩余空间。

#### `--optimization-guide-google-api-key-configuration-check-override`

- **符号**: `switches::kGoogleApiKeyConfigurationCheckOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Google API key 权限配置检查。
> Enables overriding Google API key configuration check for permissions.

#### `--optimization_guide_hints_override`

- **符号**: `switches::kHintsProtoOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖来自组件更新器的 Hints Protobuf（值需为 base64 编码的 Configuration 二进制）；启动时阻塞等待 hints 解析。
> Overrides the Hints Protobuf that would come from the component updater. If the value of this switch is invalid, regular hint processing is used. The value of this switch should be a base64 encoding of a binary Configuration message, found in optimization_guide's hints.proto. Providing a valid value to this switch causes Chrome startup to block on hints parsing.

#### `--optimization-guide-model-execution-enable-remote-debug-logging`

- **符号**: `switches::kModelExecutionEnableRemoteDebugLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在请求头中标记需要模型执行服务在响应头里返回调试日志。
> Adds header to indicate to return debug logging data from the model execution service via response header.

#### `--optimization-guide-model-execution-validate`

- **符号**: `switches::kModelExecutionValidate` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：触发服务端 AI 模型执行的校验（集成测试用）。
> Triggers validation of the server-side AI model execution. Used for integration testing.

#### `--optimization-guide-model-override`

- **符号**: `switches::kModelOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用模型拉取，按命令行覆盖模型文件路径与元数据，格式 `OPTIMIZATION_TARGET:file_path[:base64_metadata]`，逗号分隔多组。
> Disables the fetching of models and overrides the file path and metadata to be used for the session to use what's passed via command-line instead of what is already stored. We expect that the string be a comma-separated string of model overrides with each model override be: OPTIMIZATION_TARGET_STRING:file_path or OPTIMIZATION_TARGET_STRING:file_path:base64_encoded_any_proto_model_metadata.
> It is possible this only works on Desktop since file paths are less easily accessible on Android, but may work.

#### `--model-quality-service-api-key`

- **符号**: `switches::kModelQualityServiceAPIKey` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 ModelQuality Service 的 API Key。
> Overrides the ModelQuality Service API Key for remote requests to be made.

#### `--model-quality-service-url`

- **符号**: `switches::kModelQualityServiceURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 ModelQuality Service URL。
> Overrides the model quality service URL.

#### `--optimization-guide-model-validate`

- **符号**: `switches::kModelValidate` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：触发模型校验（手动测试）。
> Triggers validation of the model. Used for manual testing.

#### `--optimization-guide-ondevice-model-adaptations-override`

- **符号**: `switches::kOnDeviceModelAdaptationsOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖端上模型 adaptations 的文件路径。
> Overrides the on-device model adaptation file paths for on-device model execution.

#### `--optimization-guide-ondevice-model-execution-override`

- **符号**: `switches::kOnDeviceModelExecutionOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖端上模型执行的文件路径。
> Overrides the on-device model file paths for on-device model execution.

#### `--ondevice-validation-request-override`

- **符号**: `switches::kOnDeviceValidationRequestOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动后延时一段时间运行端上模型校验，可指定输入文本与输出文件路径。
> Enables the on-device model to run validation at startup after a delay. A text file can be provided used as input for the validation job and an output file path can be provided to write the response to.

#### `--ondevice-validation-write-to-file`

- **符号**: `switches::kOnDeviceValidationWriteToFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把端上验证结果写入指定文件。

#### `--optimization-guide-language-override`

- **符号**: `switches::kOptimizationGuideLanguageOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：向后端发送语言代码。
> Allows sending an language code to the backend.

#### `--optimization-guide-service-api-key`

- **符号**: `switches::kOptimizationGuideServiceAPIKey` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Optimization Guide Service 的 API Key。
> Overrides the Optimization Guide Service API Key for remote requests to be made.

#### `--optimization-guide-service-get-hints-url`

- **符号**: `switches::kOptimizationGuideServiceGetHintsURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 HintsFetcher 远程拉取 hints 的 URL。
> Overrides the Optimization Guide Service URL that the HintsFetcher will request remote hints from.

#### `--optimization-guide-service-get-models-url`

- **符号**: `switches::kOptimizationGuideServiceGetModelsURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 PredictionModelFetcher 远程拉取 model 与 host features 的 URL。
> Overrides the Optimization Guide Service URL that the PredictionModelFetcher will request remote models and host features from.

#### `--optimization-guide-service-model-execution-url`

- **符号**: `switches::kOptimizationGuideServiceModelExecutionURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Optimization Guide 模型执行 URL。
> Overrides the Optimization Guide model execution URL.

#### `--purge-optimization-guide-store`

- **符号**: `switches::kPurgeHintsStore` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动时清空已拉取与组件 hints 存储，确保使用新数据。
> Purges the store containing fetched and component hints on startup, so that it's guaranteed to be using fresh data.

#### `--purge-model-and-features-store`

- **符号**: `switches::kPurgeModelAndFeaturesStore` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启动时清空预测模型与 host model features 存储，确保使用新数据。
> Purges the store containing prediction medels and host model features on startup, so that it's guaranteed to be using fresh data.


### 页面内容注解 (components/page_content_annotations/core/page_content_annotations_switches.cc)

_开关数：6_


#### `--enable-page-content-annotations-logging`

- **符号**: `switches::kPageContentAnnotationsLoggingEnabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 page content annotations 日志。

#### `--page-content-annotations-skip-fcp-wait-for-testing`

- **符号**: `switches::kPageContentAnnotationsSkipFCPWaitForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：page content annotations 请求不等 FCP（测试用）。
> If enabled, page content annotations requests will not wait for FCP.

#### `--page-content-annotations-validation-batch-size`

- **符号**: `switches::kPageContentAnnotationsValidationBatchSizeOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：page content annotations 校验批次大小。

#### `--page-content-annotations-validation-content-visibility`

- **符号**: `switches::kPageContentAnnotationsValidationContentVisibility` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用指定 annotation 类型在启动后延时校验，值是逗号分隔的输入文本。
> Enables the specific annotation type to run validation at startup after a delay. A comma separated list of inputs can be given as a value which will be used as input for the validation job.

#### `--page-content-annotations-validation-startup-delay-seconds`

- **符号**: `switches::kPageContentAnnotationsValidationStartupDelaySeconds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：page content annotations 校验启动延时（秒）。

#### `--page-content-annotations-validation-write-to-file`

- **符号**: `switches::kPageContentAnnotationsValidationWriteToFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 page content annotations 校验输出写到指定文件。
> Writes the output of page content annotation validations to the given file.


### 权限 (components/permissions/switches.cc)

_开关数：1_


#### `--deny-permission-prompts`

- **符号**: `switches::kDenyPermissionPrompts` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：所有权限询问一律按“拒绝”处理（不再弹提示）。
> Prevents permission prompts from appearing by denying instead of showing prompts.


### Variations (components/variations/variations_switches.cc)

Finch 试验种子/参数覆盖开关，常用于强制 A/B 分组。

_开关数：21_


#### `--accept-empty-variations-seed-signature`

- **符号**: `switches::kAcceptEmptySeedSignatureForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：加载 variations seed 时接受空签名（测试用）。
> Accept an empty signature when loading a variations seed. This is for testing purposes.

#### `--disable-field-trial-config`

- **符号**: `switches::kDisableFieldTrialTestingConfig` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 fieldtrial_testing_config.json 中配置的 Field Trial 测试。
> Disable field trial tests configured in fieldtrial_testing_config.json.

#### `--disable-variations-safe-mode`

- **符号**: `switches::kDisableVariationsSafeMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 Variations 安全模式。
> Disable variations safe mode.

#### `--disable-variations-seed-fetch`

- **符号**: `switches::kDisableVariationsSeedFetch` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用从服务器拉取 variations seed（测试用）。
> Disable fetching of variations seed from the server for testing.

#### `--disable-variations-seed-fetch-throttling`

- **符号**: `switches::kDisableVariationsSeedFetchThrottling` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（移动端）关闭 variations seed 拉取的节流。
> Disables throttling for fetching the variations seed on mobile platforms. The seed will be fetched on startup and every time the app enters the foreground, regardless of the time passed in between the fetches. On Desktop, this switch has no effect (the seed is fetched periodically instead).

#### `--enable-benchmarking`

- **符号**: `switches::kEnableBenchmarking` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 variations benchmarking 模式（含义见 flag_descriptions.cc）。
> TODO(asvitkine): Consider removing or renaming this functionality. See flag_descriptions.cc for more details.

#### `--enable-benchmarking-api`

- **符号**: `switches::kEnableBenchmarkingApi` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 benchmarking JavaScript API。
> Enables the benchmarking JavaScript API.

#### `--enable-field-trial-config`

- **符号**: `switches::kEnableFieldTrialTestingConfig` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 fieldtrial_testing_config.json 中的 Field Trial 测试（官方渠道默认不开，需此开关）。
> Enable field trial tests configured in fieldtrial_testing_config.json. If the "disable_fieldtrial_testing_config" GN flag is set to true, then this switch is a no-op. Otherwise, for non-Chrome branded builds, the testing config is already applied by default, unless the "--disable-field-trial-config", "--force-fieldtrials", and/or "--variations-server-url" switches are passed.
> It is however possible to apply the testing config as well as specify additional field trials (using "--force-fieldtrials") by using this switch. For Chrome-branded builds, the testing config is not enabled by default, so this switch is required to enable it.

#### `--enable-finch-seed-delta-compression`

- **符号**: `switches::kEnableFinchSeedDeltaCompression` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Android 首次运行路径）拉取新 seed 时启用增量压缩。
> Enables delta-compression when fetching a new seed via the "first run" code path on Android.

#### `--fake-variations-channel`

- **符号**: `switches::kFakeVariationsChannel` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为 Variations 过滤伪造 channel：`stable`/`beta`/`dev`/`canary`（测试用，官方构建也生效）。
> Fakes the channel of the browser for purposes of Variations filtering. This is to be used for testing only. Possible values are "stable", "beta", "dev" and "canary". This works for official builds as well.

#### `--force-disable-variation-ids`

- **符号**: `switches::kForceDisableVariationIds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：从 X-Client-Data 头里强制移除指定 Variation IDs（64bit 编码列表；前缀 `t` 表 Trigger）。
> Forces to remove Chrome Variation Ids from being sent in X-Client-Data header, specified as a 64-bit encoded list of numeric experiment ids. Ids prefixed with the character "t" will be treated as Trigger Variation Ids.

#### `--force-fieldtrial-params`

- **符号**: `switches::kForceFieldTrialParams` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制 field trial 参数，格式 `Trial.Group:k1/v1/k2/v2,...`（名称需 URL 转义）。
> This option can be used to force parameters of field trials when testing changes locally. The argument is a param list of (key, value) pairs prefixed by an associated (trial, group) pair. You specify the param list for multiple (trial, group) pairs with a comma separator. Example: "Trial1.Group1:k1/v1/k2/v2,Trial2.Group2:k3/v3/k4/v4" Trial names, groups names, parameter names, and value should all be URL escaped for all non-alphanumeric characters.

#### `--force-variation-ids`

- **符号**: `switches::kForceVariationIds` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在 X-Client-Data 头里额外塞入指定 Variation IDs（仅测试用；生产代码禁止）。
> Forces additional Chrome Variation Ids that will be sent in X-Client-Data header, specified as a 64-bit encoded list of numeric experiment ids. Ids prefixed with the character "t" will be treated as Trigger Variation Ids. IMPORTANT: You can use this switch for test purposes (e.g. a manual command line run or from a unit test), but NOT for production code in the browser, as the latter is not allowed for privacy reasons (except for the current use by about:flags code).

#### `--variations-insecure-server-url`

- **符号**: `switches::kVariationsInsecureServerURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：variations 服务器主 URL 请求失败时使用的不安全 fallback URL（到此 URL 的请求仍加密）。
> Specifies a custom URL for the server to use as an insecure fallback when requests to |kVariationsServerURL| fail. Requests to this URL will be encrypted.

#### `--variations-override-country`

- **符号**: `switches::kVariationsOverrideCountry` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Variations 评估使用的 country（区别于 chrome://translate-internals 的持久化设置，此开关不跨会话）。
> Allows overriding the country used for evaluating variations. This is similar to the "Override Variations Country" entry on chrome://translate-internals, but is exposed as a command-line flag to allow testing First Run scenarios. Additionally, unlike chrome://translate-internals, the value isn't persisted across sessions.

#### `--variations-seed-corpus`

- **符号**: `switches::kVariationsSeedCorpus` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：使用指定的 variations seed corpus；未指定/无法识别时用默认。
> Specifies the value of the variations seed corpus to use. When unspecified or unrecognized, the default corpus will be used.

#### `--variations-seed-fetch-interval`

- **符号**: `switches::kVariationsSeedFetchInterval` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 variations seed 拉取间隔（分钟，最小 1，默认 30）。
> Override the time interval between each variation seed fetches. Unit is in minutes. The minimum is 1 minute. The default is 30 minutes.

#### `--variations-seed-version`

- **符号**: `switches::kVariationsSeedVersion` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：向子进程共享 variations seed 版本。
> Used to share variations seed version with child processes.

#### `--variations-server-url`

- **符号**: `switches::kVariationsServerURL` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Variations 上报/拉取服务器 URL；非官方构建设置此值即启用 Variations。
> Specifies a custom URL for the server which reports variation data to the client. Specifying this switch enables the Variations service on unofficial builds. See variations_service.cc.

#### `--variations-state-file`

- **符号**: `switches::kVariationsStateFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：用文件中的 command-line variations 值（来自 chrome://version 的“Command-line Variations”）。
> Use features defined in the value. Use this flag to reproduce experiments related issues. Copy 'Command-line Variations' value from chrome://version page. Save it to a file and pass it via this flag. The value is a base64 encoded JSON format produced by `variations::VariationsCommandLine::WriteToString`.

#### `--variations-test-seed-path`

- **符号**: `switches::kVariationsTestSeedJsonPath` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：从指定 JSON 文件填充 Local State 的 seed（需含 compressed-seed + signature 两 key）。
> Specifies the location of a seed file for Local State's seed to be populated from. The seed file must be in json format with the keys |kVariationsCompressedSeed| and |kVariationsSeedSignature|.


## 十、设备 / 输入 / 窗口系统 / 无障碍

VR、手柄、HID、鼠标键盘事件、窗口管理 (WM)、Ozone 平台、无障碍 (a11y)、DevTools UI 等开关。


### 输入 (components/input/switches.cc)

输入事件开关。

_开关数：3_


#### `--disable-hang-monitor`

- **符号**: `switches::kDisableHangMonitor` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：屏蔽 Renderer 的 hang 对话框（慢的 unload handler 可能阻止页面关闭，必要时用任务管理器结束）。
> Suppresses hang monitor dialogs in renderer processes.  This may allow slow unload handlers on a page to prevent the tab from closing, but the Task Manager can be used to terminate the offending process in this case.

#### `--disable-pinch`

- **符号**: `switches::kDisablePinch` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用合成器加速的触屏 pinch 缩放手势。
> Disables compositor-accelerated touch-screen pinch gestures.

#### `--validate-input-event-stream`

- **符号**: `switches::kValidateInputEventStream` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（debug 构建）断言输入事件流的合法性。
> In debug builds, asserts that the stream of input events is valid.


### UI DevTools (components/ui_devtools/switches.cc)

原生 UI DevTools 开关。

_开关数：1_


#### `--enable-ui-devtools`

- **符号**: `switches::kEnableUiDevTools` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 UI DevTools（mus/ash 等）服务器；值是端口号，默认 9223。
> Enables DevTools server for UI (mus, ash, etc). Value should be the port the server is started on. Default port is 9223.


### Gamepad (device/gamepad/public/cpp/gamepad_switches.cc)

手柄开关。

_开关数：1_


#### `--gamepad-polling-interval`

- **符号**: `switches::kGamepadPollingInterval` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖手柄轮询间隔（缩短可降低输入延迟，但 CPU 占用更高）。
> Overrides the gamepad polling interval. Decreasing the interval improves input latency of buttons and axes but may negatively affect performance due to more CPU time spent in the input polling thread.


### VR (device/vr/public/cpp/switches.cc)

WebXR / OpenXR 运行时开关。

_开关数：3_


#### `--webxr-hand-anonymization-strategy`

- **符号**: `switches::kWebXrHandAnonymizationStrategy` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：WebXR 手部数据匿名化策略（取值见下面 fallback/runtime）。

#### `--fallback`

- **符号**: `switches::kWebXrHandAnonymizationStrategyFallback` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--webxr-hand-anonymization-strategy 取值：fallback。

#### `--runtime`

- **符号**: `switches::kWebXrHandAnonymizationStrategyRuntime` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：--webxr-hand-anonymization-strategy 取值：runtime（由运行时执行匿名化）。


### HID (services/device/public/cpp/hid/hid_switches.cc)

WebHID 开关。

_开关数：1_


#### `--disable-hid-blocklist`

- **符号**: `switches::kDisableHidBlocklist` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 HID 设备 blocklist。
> Disable the HID blocklist.


### 无障碍 (ui/accessibility/accessibility_switches.cc)

a11y 调试开关。

_开关数：11_


#### `--disable-renderer-accessibility`

- **符号**: `switches::kDisableRendererAccessibility` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：关闭 Renderer 中的无障碍。
> Turns off the accessibility in the renderer.

#### `--enable-experimental-accessibility-autoclick`

- **符号**: `switches::kEnableExperimentalAccessibilityAutoclick` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：显示尚未上线的自动点击新功能。
> Shows additional automatic click features that haven't launched yet.

#### `--enable-experimental-accessibility-labels-debugging`

- **符号**: `switches::kEnableExperimentalAccessibilityLabelsDebugging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：可视化调试无障碍图像描述（labels）特性。
> Enables support for visually debugging the accessibility labels feature, which provides images descriptions for screen reader users.

#### `--enable-experimental-accessibility-language-detection`

- **符号**: `switches::kEnableExperimentalAccessibilityLanguageDetection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：对页面静态文本启用语言检测，结果暴露给读屏等辅助技术。
> Enables language detection on in-page text content which is then exposed to assistive technology such as screen readers.

#### `--enable-experimental-accessibility-language-detection-dynamic`

- **符号**: `switches::kEnableExperimentalAccessibilityLanguageDetectionDynamic` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：对动态内容启用语言检测，结果暴露给辅助技术。
> Enables language detection for dynamic content which is then exposed to assistive technology such as screen readers.

#### `--enable-experimental-accessibility-manifest-v3`

- **符号**: `switches::kEnableExperimentalAccessibilityManifestV3` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：迁移期内允许无障碍扩展使用 manifest v3。
> Switches accessibility extensions to use extensions manifest v3 while the migration is still in progress.

#### `--enable-experimental-accessibility-switch-access-text`

- **符号**: `switches::kEnableExperimentalAccessibilitySwitchAccessText` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：启用 Switch Access 的文本输入相关进行中特性。
> Enables in progress Switch Access features for text input.

#### `--enable-mac-accessibility-api-migration`

- **符号**: `switches::kEnableMacAccessibilityAPIMigration` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（macOS）切换到新版基于 NSAccessibility 属性的 API。
> Enables the switchover to the newer NSAccessibility property-based API.

#### `--enable-magnifier-debug-draw-rect`

- **符号**: `switches::kEnableMagnifierDebugDrawRect` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为放大区域绘制矩形（不真正放大），调试 magnifier。
> Enables debug feature for drawing rectangle around magnified region, without zooming in.

#### `--force-renderer-accessibility`

- **符号**: `switches::kForceRendererAccessibility` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制 Renderer 无障碍开启（无须检测到读屏）；被 --disable-renderer-accessibility 覆盖；可选参数指定 AXMode bundle：`basic`/`form-controls`/`complete`。
> Force renderer accessibility to be on instead of enabling it on demand when a screen reader is detected. The disable-renderer-accessibility switch overrides this if present. This switch has an optional parameter that forces an AXMode bundle. The three available bundle settings are: 'basic', 'form-controls', and 'complete'. If the bundle argument is invalid, then the forced AXMode will default to 'complete'.
> If the bundle argument is missing, then the initial AXMode will default to complete but allow changes to the AXMode during execution.

#### `--generate-accessibility-test-expectations`

- **符号**: `switches::kGenerateAccessibilityTestExpectations` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：生成无障碍测试期望文件（基线）。


### UI Events (ui/events/event_switches.cc)

鼠标/触控/手势事件开关。

_开关数：7_


#### `--compensate-for-unstable-pinch-zoom`

- **符号**: `switches::kCompensateForUnstablePinchZoom` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：补偿不稳定 pinch zoom（部分触屏直线移动有抖动）。
> Enable compensation for unstable pinch zoom. Some touch screens display significant amount of wobble when moving a finger in a straight line. This makes two finger scroll trigger an oscillating pinch zoom. See crbug.com/394380 for details.

#### `--disable-cancel-all-touches`

- **符号**: `switches::kDisableCancelAllTouches` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_OZONE)
- **用途**：禁用 CancelAllTouches()，改为按单点取消的实现。
> Disable CancelAllTouches() function for the implementation on cancel single touches.

#### `--edge-touch-filtering`

- **符号**: `switches::kEdgeTouchFiltering` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_OZONE)
- **用途**：启用边缘触摸过滤（变形本/平板有用）。
> Tells Chrome to do edge touch filtering. Useful for convertible tablet.

#### `--enable-microphone-mute-switch-device`

- **符号**: `switches::kEnableMicrophoneMuteSwitchDeviceSwitch` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_OZONE)
- **用途**：启用对“麦克风静音开关”设备状态的检测，触发时关闭内置音频输入。
> Enables logic to detect microphone mute switch device state, which disables internal audio input when toggled.

#### `--pen-devices`

- **符号**: `switches::kPenDevices` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS)
- **用途**：把指定 XInput2 设备的事件解释为笔事件，id 可由 `xinput list` 获取。
> Tells chrome to interpret events from these devices as pen events. Only available with XInput 2 (i.e. X server 1.8 or above). The id's of the devices can be retrieved from 'xinput list'.

#### `--touch-devices`

- **符号**: `switches::kTouchDevices` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_LINUX) && BUILDFLAG(IS_CHROMEOS)
- **用途**：把指定 XInput2 设备的事件解释为触摸事件，id 可由 `xinput list` 获取。
> Tells chrome to interpret events from these devices as touch events. Only available with XInput 2 (i.e. X server 1.8 or above). The id's of the devices can be retrieved from 'xinput list'.

#### `--touch-slop-distance`

- **符号**: `switches::kTouchSlopDistance` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖手势检测的 touch slop 距离（像素，浮点）。
> Overrides touch slop distance for gesture detection. The touch slop distance is the maximum distance from the starting point of a touch sequence that a gesture can travel before it can no longer be considered a tap. Scroll gestures can only begin after this distance has been travelled. The switch value is a floating point number that is interpreted as a distance in pixels.


### Ozone (ui/ozone/public/ozone_switches.cc)

Ozone 平台后端开关 (主要用于 Linux)。

_开关数：9_


#### `--disable-explicit-dma-fences`

- **符号**: `switches::kDisableExplicitDmaFences` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用显式 DMA-fence。
> Disable explicit DMA-fences

#### `--disable-wayland-ime`

- **符号**: `switches::kDisableWaylandIme` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 Wayland IME。
> Disable wayland input method editor.

#### `--enable-wayland-ime`

- **符号**: `switches::kEnableWaylandIme` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：尝试启用 Wayland IME。
> Try to enable wayland input method editor.

#### `--ozone-dump-file`

- **符号**: `switches::kOzoneDumpFile` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Ozone 图像 dump 路径。
> Specify location for image dumps.

#### `--ozone-override-screen-size`

- **符号**: `switches::kOzoneOverrideScreenSize` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 ozone 屏幕尺寸。
> Specifies ozone screen size.

#### `--ozone-platform`

- **符号**: `switches::kOzonePlatform` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：指定 ozone 平台实现，例如 `wayland`、`x11`、`headless`。
> Specify ozone platform implementation to use.

#### `--render-node-override`

- **符号**: `switches::kRenderNodeOverride` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：显式选择用于创建 gbm_device 的 DRM render node。
> Allows explicitly picking a DRM render node to create gbm_device for rendering.

#### `--use-wayland-explicit-grab`

- **符号**: `switches::kUseWaylandExplicitGrab` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打开 popup 窗口时使用显式 grab。
> Use explicit grab when opening popup windows. See https://crbug.com/1220274

#### `--wayland-text-input-version`

- **符号**: `switches::kWaylandTextInputVersion` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Wayland text-input 协议版本：默认 `1`，可设 `3` 以试验 v3。
> Specify wayland text-input protocol version. Defaults to "1" for text-input-v1. Can specify value "3" for experimental text-input-v3 support.


### Views (ui/views/views_switches.h)

Chromium Views 工具集开关。

_开关数：3_


#### `--disable-input-event-activation-protection`

- **符号**: `switches::kDisableInputEventActivationProtectionForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用按钮显示后立即点击的“误触”过滤（自动化测试需要）。
> Disables the disregarding of potentially unintended input events such as button clicks that happen instantly after the button is shown. Use this for integration tests that do automated clicks etc.

#### `--draw-view-bounds-rects`

- **符号**: `switches::kDrawViewBoundsRects` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为每个 View 画半透明红框；当 GetContentBounds() ≠ GetLocalBounds() 时再叠一层蓝框。
> Draws a semitransparent red rect to indicate the bounds of each view. Also, draws a blue semitransparent rect when GetContentBounds() differs from GetLocalBounds().

#### `--view-stack-traces`

- **符号**: `switches::kViewStackTraces` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：View 构造时记录调用栈，便于调试。
> Captures stack traces on View construction to provide better debug info.


### Window Manager (ui/wm/core/wm_core_switches.cc)

窗口管理器开关。

_开关数：1_


#### `--wm-window-animations-disabled`

- **符号**: `switches::kWindowAnimationsDisabled` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用窗口动画。
> If present animations are disabled.


## 十一、Headless / 自动化 / 测试

Headless Chrome、测试基础设施 (test_switches)、extensions_shell 等。


### Chrome Headless Mode (chrome/browser/headless/headless_mode_switches.h)

_开关数：2_


#### `--allow-chrome-scheme-url`

- **符号**: `switches::kAllowChromeSchemeUrl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：headless 模式允许访问 chrome:// URL。
> Allows headless mode to access any URL whose scheme is chrome://.

#### `--enable-gpu`

- **符号**: `switches::kEnableGPU` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：headless 默认用 SwiftShader，此开关关闭强制软件渲染、回到正常驱动选择逻辑（不保证一定能启用硬件 GPU，仍受 X display 等条件限制）。
> Enable hardware GPU support. Headless uses swiftshader by default for consistency across headless environments. This flag just turns forcing of swiftshader off and lets us revert to regular driver selection logic. Alternatively, specific drivers may be forced with --use-gl or --use-angle. Nethier approach guarantees that hardware GPU support will be enabled, as this is still conditional on headless having access to X display etc.


### Chrome Test (chrome/test/base/test_switches.cc)

_开关数：3_


#### `--also-emit-success-logs`

- **符号**: `switches::kAlsoEmitSuccessLogs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：成功的测试也输出完整 event trace 日志。
> Also emit full event trace logs for successful tests.

#### `--devtools-code-coverage`

- **符号**: `switches::kDevtoolsCodeCoverage` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：JS 代码覆盖率输出目录；指定后在所选 browser tests 中开启覆盖率。
> Directory to output JavaScript code coverage. When supplied enables coverage in selected browser tests.

#### `--perf-test-print-uma-means`

- **符号**: `switches::kPerfTestPrintUmaMeans` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打印性能测试监控的直方图均值（仅 PerformanceTest 子类）。
> Show the mean value of histograms that native performance tests are monitoring. Note that this is only applicable for PerformanceTest subclasses.


### Headless 命令处理 (components/headless/command_handler/headless_command_switches.cc)

_开关数：9_


#### `--default-background-color`

- **符号**: `switches::kDefaultBackgroundColor` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：页面未指定背景色时使用的颜色，hex RGB(A)，例 `ff0000ff`（红）/`00000000`（透明）。
> The background color to be used if the page doesn't specify one. Provided as RGB or RGBA integer value in hex, e.g. 'ff0000ff' for red or '00000000' for transparent.

#### `--disable-pdf-tagging`

- **符号**: `switches::kDisablePDFTagging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打印 PDF 时不输出 tag。
> Do not emit tags when printing PDFs.

#### `--dump-dom`

- **符号**: `switches::kDumpDom` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把序列化后的 DOM（doctype + outerHTML）打印到 stdout。
> Print the serialized DOM (doctype + document.documentElement.outerHTML) to stdout.

#### `--generate-pdf-document-outline`

- **符号**: `switches::kGeneratePDFDocumentOutline` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：在打印的 PDF 中嵌入文档大纲。
> Embed the document outline into printed PDFs.

#### `--no-pdf-header-footer`

- **符号**: `switches::kNoPDFHeaderFooter` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：打印 PDF 时不显示页眉页脚。
> Do not display header and footer in the printed PDF file.

#### `--print-to-pdf`

- **符号**: `switches::kPrintToPDF` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把当前页保存为 PDF 文件。
> Save a PDF file of the loaded page.

#### `--screenshot`

- **符号**: `switches::kScreenshot` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：保存当前页面截图。
> Save a screenshot of the loaded page.

#### `--timeout`

- **符号**: `switches::kTimeout` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：headless 模式：N 毫秒后强制 stop（取消所有导航并触发 DOMContentLoaded）。
> Issues a stop after the specified number of milliseconds.  This cancels all navigation and causes the DOMContentLoaded event to fire.

#### `--virtual-time-budget`

- **符号**: `switches::kVirtualTimeBudget` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：headless 模式虚拟时间预算（毫秒）：网络请求未完成时虚拟时间不前进，完成后定时器触发，用尽预算后停止。
> If set the system waits the specified number of virtual milliseconds before deeming the page to be ready.  For determinism virtual time does not advance while there are pending network fetches (i.e no timers will fire). Once all network fetches have completed, timers fire and if the system runs out of virtual time is fastforwarded so the next timer fires immediately, until the specified virtual time budget is exhausted.


### Components Test (components/test/test_switches.cc)

_开关数：1_


#### `--initialize-mojo-as-broker`

- **符号**: `switches::kInitializeMojoAsBroker` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：强制 spawn 出的子进程 Mojo broker 初始化（部分测试需要）。
> Used by some tests to force Mojo broker initialization in a spawned child process.


### Content Shell (content/shell/common/shell_switches.h)

content_shell / chrome_shell 测试程序专用开关。

_开关数：9_


#### `--content-shell-hide-toolbar`

- **符号**: `switches::kContentShellHideToolbar` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：content_shell 主窗口隐藏 toolbar。
> Hides toolbar from content_shell's host window.

#### `--content-shell-host-window-size`

- **符号**: `switches::kContentShellHostWindowSize` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：content_shell 主窗口尺寸，例 `800x600`。
> Size for the content_shell's host window (i.e. "800x600").

#### `--crash-dumps-dir`

- **符号**: `switches::kCrashDumpsDir` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `headless/public/switches.h`
- **用途**：Crashpad minidump 存储目录（iOS/tvOS 默认 app cache 目录）。
> The directory Crashpad should store minidumps in. iOS and tvOS default to app's cache directory.

#### `--disable-system-font-check`

- **符号**: `switches::kDisableSystemFontCheck` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用系统字体检查。
> Disables the check for the system font when specified.

#### `--expose-internals-for-testing`

- **符号**: `switches::kExposeInternalsForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 `window.internals` 暴露给 JS（用于交互式开发与调试 web tests）。
> Exposes the window.internals object to JavaScript for interactive development and debugging of web tests that rely on it.

#### `--isolated-context-origins`

- **符号**: `switches::kIsolatedContextOrigins` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：为指定 origin 列表（逗号分隔）启用带 `[IsolatedContext]` 标记的 IDL API。
> Enables APIs guarded with the [IsolatedContext] IDL attribute for the given comma-separated list of origins.

#### `--remote-debugging-address`

- **符号**: `switches::kRemoteDebuggingAddress` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：远程调试监听地址（默认 loopback）；注意远程调试无认证，监听过宽有安全风险。
> Use the given address instead of the default loopback for accepting remote debugging connections. Note that the remote debugging protocol does not perform any authentication, so exposing it too widely can be a security risk.

#### `--run-web-tests`

- **符号**: `switches::kRunWebTests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 Content Shell 跑在 web test 模式（注入 blink web tests 专用行为）。
> Runs Content Shell in web test mode, injecting test-only behaviour for blink web tests.

#### `--test-register-standard-scheme`

- **符号**: `switches::kTestRegisterStandardScheme` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把指定 scheme 注册为 standard scheme（测试）。
> Register the provided scheme as a standard scheme.


### Web Tests (content/web_test/common/web_test_switches.h)

layout/web test 框架（run_web_tests）开关。

_开关数：13_


#### `--allow-external-pages`

- **符号**: `switches::kAllowExternalPages` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：web tests 允许访问外部页面。
> Allow access to external pages during web tests.

#### `--always-use-complex-text`

- **符号**: `switches::kAlwaysUseComplexText` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：web tests 总是走 complex text 路径。
> Always use the complex text path for web tests.

#### `--crash-on-failure`

- **符号**: `switches::kCrashOnFailure` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：leak detector 发现泄漏时立即崩溃（与 --enable-leak-detection 配合）。
> When specified to "enable-leak-detection" command-line option, causes the leak detector to cause immediate crash when found leak.

#### `--debug-devtools`

- **符号**: `switches::kDebugDevTools` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：以 debug 模式跑 devtools 测试（不打包/不压缩）。
> Run devtools tests in debug mode (not bundled and minified)

#### `--disable-auto-wpt-origin-isolation`

- **符号**: `switches::kDisableAutoWPTOriginIsolation` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：禁用 web platform test 域的自动 origin 隔离（开 opt-in origin isolation 的测试需要关闭它）。
> Disables the automatic origin isolation of web platform test domains. We normally origin-isolate them for better test coverage, but tests of opt-in origin isolation need to disable this.

#### `--disable-headless-mode`

- **符号**: `switches::kDisableHeadlessMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Content Shell 不以 headless 启动，让测试尝试使用硬件 GPU；仅在 --run-web-tests 下生效。
> Disables the shell from beginning in headless mode. Tests will then attempt to use the hardware GPU for rendering. This is only followed when kRunWebTests is set.

#### `--enable-accelerated-2d-canvas`

- **符号**: `switches::kEnableAccelerated2DCanvas` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（web tests）启用 2D Canvas 加速。
> Enable accelerated 2D canvas.

#### `--enable-font-antialiasing`

- **符号**: `switches::kEnableFontAntialiasing` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（web tests pixel test）启用字体抗锯齿。
> Enable font antialiasing for pixel tests.

#### `--enable-leak-detection`

- **符号**: `switches::kEnableLeakDetection` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：网页加载的泄漏检测（验证 reload 是否正确释放对象）。
> Enables the leak detection of loading webpages. This allows us to check whether or not reloading a webpage releases web-related objects correctly.

#### `--encode-binary`

- **符号**: `switches::kEncodeBinary` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：把 web test 二进制结果（图像、音频）用 base64 编码输出。
> Encode binary web test results (images, audio) using base64.

#### `--inspector-protocol-log`

- **符号**: `switches::kInspectorProtocolLog` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Inspector-protocol 测试用的 CDP log 文件路径（每行一条 JSON 协议消息）。
> Specifies the path to a file containing a Chrome DevTools protocol log. Each line in the log file is expected to be a protocol message in the JSON format. The test runner will use this log file to script the backend for any inspector-protocol tests that run. Usually you would want to run a single test using the log to reproduce timeouts or crashes.

#### `--reset-browsing-instance-between-tests`

- **符号**: `switches::kResetBrowsingInstanceBetweenTests` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：每条 web test 都在新 BrowsingInstance 中跑（origin isolation 测试必备）。
> Forces each web test to be run in a new BrowsingInstance. Required for origin isolation web tests where the BrowsingInstance retains state from origin isolation requests, but this flag may benefit other web tests.

#### `--stable-release-mode`

- **符号**: `switches::kStableReleaseMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：web tests 把 Content Shell 当作 stable release 测试，禁用部分平台特性（仅 --run-web-tests 下生效）。
> This makes us disable some web-platform runtime features so that we test content_shell as if it was a stable release. It is only followed when kRunWebTest is set. For the features' level, see third_party/blink/renderer/platform/RuntimeEnabledFeatures.md


### Headless Public (headless/public/switches.h)

_开关数：12_


#### `--allow-video-codecs`

- **符号**: `switches::kAllowVideoCodecs` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：headless 允许使用的视频编码列表（逗号分隔，大小写不敏感）：`*` 全匹配，前缀 `-` 表示禁用，首条匹配决定结果；编码名见 media/base/video_codecs.cc。
> A comma-separated, case-insenitive list of video codecs to allow. If specified, codecs not matching the list will not be used. '*' will match everything, '-' at the start of an entry means codec is disallowed. First entry that matches determines the outcome. Codec names are as returned by `GetCodecName()` in media/base/video_codecs.cc

#### `--block-new-web-contents`

- **符号**: `switches::kBlockNewWebContents` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：headless 禁止所有 popup 与 `window.open`。
> If true, then all pop-ups and calls to window.open will fail.

#### `--deterministic-mode`

- **符号**: `switches::kDeterministicMode` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：确定性元开关，会打开多个开关让 begin frames 通过 DevTools 协议下发（实验性）。
> A meta flag. This sets a number of flags which put the browser into deterministic mode where begin frames should be issued over DevToolsProtocol (experimental).

#### `--disable-cookie-encryption`

- **符号**: `switches::kDisableCookieEncryption` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（headless）用户 Profile 中 Cookie 不加密。
> Whether cookies stored as part of user profile are encrypted.

#### `--disable-crash-reporter`

- **符号**: `switches::kDisableCrashReporter` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（headless）禁用崩溃上报（official 构建默认开）。
> Disable crash reporter for headless. It is enabled by default in official builds.

#### `--enable-bfcache`

- **符号**: `switches::kEnableBackForwardCache` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（headless）启用前进/后退缓存。
> Enable Back Forward Cache support

#### `--enable-begin-frame-control`

- **符号**: `switches::kEnableBeginFrameControl` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：begin frames 通过 DevTools 协议下发（实验性）。
> Whether or not begin frames should be issued over DevToolsProtocol (experimental).

#### `--font-render-hinting`

- **符号**: `switches::kFontRenderHinting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：headless 字体 hinting：`none`/`slight`/`medium`/`full`（默认）/`max`。
> Sets font render hinting when running headless, affects Skia rendering and whether glyph subpixel positioning is enabled. Possible values: none|slight|medium|full|max. Default: full.

#### `--force-new-browsing-instance`

- **符号**: `switches::kForceNewBrowsingInstance` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：每次导航都使用新 BrowsingInstance。
> Forces each navigation to use a new BrowsingInstance.

#### `--force-reporting-destination-attested`

- **符号**: `switches::kForceReportingDestinationAttested` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（headless）强制 reporting destination 视为已 attestation。
> Force reporting destination attested for headless shell.

#### `--no-system-proxy-config-service`

- **符号**: `switches::kNoSystemProxyConfigService` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：不使用系统代理配置服务。
> Do not use system proxy configuration service.

#### `--screen-info`

- **符号**: `switches::kScreenInfo` &nbsp;&nbsp; **Buildflag**: 全平台
- **同名定义**: `ui/gfx/switches.cc`
- **用途**：headless 屏幕信息，例 `{0,0 800x600}{800,0 600x800}`（详见 components/headless/screen_info）。
> Headless screen info in the format: {0,0 800x600}{800,0 600x800}. See //components/headless/screen_info/README.md for more details.


## 十二、其它 / 杂项

Mojo、Skia、chrome/browser 下一些较专门的子模块 (predictors / actor / nearby_sharing / new_tab_page modules 等)。


### Actor (chrome/browser/actor/actor_switches.cc)

_开关数：2_


#### `--attempt-form-filling-tool-skips-ui`

- **符号**: `switches::kAttemptFormFillingToolSkipsUI` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：AttemptFormFillingTool 跳过数据选择 UI，每段表单都直接采用第一个建议（仅测试用）。
> Bypasses the selection of the data to be filled by the `AttemptFormFillingTool` and just picks the first suggestion for each form section. This is only intended for testing.

#### `--disable-actor-safety-checks`

- **符号**: `switches::kDisableActorSafetyChecks` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：绕过 Actor 的若干安全检查（如要求开 SafeBrowsing、blocklist 等）；HTTPS 等检查不受影响。
> Bypasses several of the actor's safety checks, such as requiring SafeBrowsing to be enabled and the blocklist. Note that some checks, like requiring https, are not affected by this.


### Device Trust Attestation (attestation_switches.cc)

_开关数：1_


#### `--use-va-dev-keys`

- **符号**: `switches::kUseVaDevKeys` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（企业 Device Trust 证明）使用 VA 开发环境密钥。


### Nearby Share (chrome/browser/nearby_sharing/common/nearby_share_switches.cc)

_开关数：5_


#### `--nearby-share-certificate-validity-period-hours`

- **符号**: `switches::kNearbyShareCertificateValidityPeriodHours` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Nearby Share 证书的有效期（小时，必须 >0）。
> Overrides the default validity period for Nearby Share certificates. Value must be larger than 0.

#### `--nearby-share-device-id`

- **符号**: `switches::kNearbyShareDeviceID` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖默认设备 ID（默认是随机 10 字符）。
> Overrides the default device ID to provide a stable ID in test environments. By default we generate a random 10-character string.

#### `--nearbysharing-http-host`

- **符号**: `switches::kNearbyShareHTTPHost` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖 Nearby Share 使用的 Google APIs URL。
> Overrides the default URL for Google APIs (https://www.googleapis.com) used by Nearby Share

#### `--nearby-share-num-private-certificates`

- **符号**: `switches::kNearbyShareNumPrivateCertificates` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：覆盖默认生成的私有证书数量（必须 >0）。
> Overrides the default number of private certificates generated. Value must be larger than 0.

#### `--nearby-share-verbose-logging`

- **符号**: `switches::kNearbyShareVerboseLogging` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：Nearby Share 启用详细日志。
> Enables verbose logging level for Nearby Share.


### Predictors (chrome/browser/predictors/predictors_switches.cc)

预加载/预测器开关。

_开关数：1_


#### `--loading-predictor-allow-local-request-for-testing`

- **符号**: `switches::kLoadingPredictorAllowLocalRequestForTesting` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：允许 loading predictor 对本地 IP 进行预取（默认禁止，测试用）。
> Allows the loading predictor to do prefetches to local IP addresses. This is needed for testing as such requests are blocked by default for security.


### Windows 服务 (chrome/windows_services/service_program/switches.h)

Chrome 安装为 Windows 服务时用到的开关。

_开关数：3_


#### `--log-file-handle`

- **符号**: `switches::kLogFileHandle` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows service）传递日志文件句柄值的开关。
> A switch conveying a handle value for the file to which the service should emit its logs.

#### `--log-file-source`

- **符号**: `switches::kLogFileSource` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows service）日志文件句柄所在进程的 PID。
> A switch conveying the PID of the process in which the log file handle value is valid.

#### `--unattended-test`

- **符号**: `switches::kUnattendedTest` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：（Windows service）标记 service 是为无人值守测试运行（环境变量含 CHROME_HEADLESS）。
> A switch that indicates that the service is running on behalf of an unattended test (i.e., one for which CHROME_HEADLESS is set in its environment block).


### Mojo Proxy (mojo/proxy/switches.cc)

mojo 代理进程开关。

_开关数：5_


#### `--attachment-name`

- **符号**: `switches::kAttachmentName` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：mojo_proxy：约定单个 Mojo invitation 附件的名称（与 --num-attachments 二选一）。
> For client applications who expect a single Mojo invitation attachment with a free-form name assigned to it, this specifies that attachment name. Either this or kNumericAttachmentNames must be specified on the command line.

#### `--host-ipcz-transport-fd`

- **符号**: `switches::kHostIpczTransportFd` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：mojo_proxy：宿主继承下来的 Unix socket fd（必填）。
> The integer value of a file descriptor inherited by the mojo_proxy process when launched by its host. This descriptor references a Unix socket which is connected to the host process which launched this proxy to sit between the host and some legacy client application. Required.

#### `--inherit-ipcz-broker`

- **符号**: `switches::kInheritIpczBroker` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：mojo_proxy：默认认为宿主是 broker；指定该开关则认为宿主是非 broker 但提供共享 broker。
> By default, mojo_proxy assumes its host is a broker. When this flag is given it instead assumes its host is a non-broker who is offering to share their broker. The proxy must be configured correctly in this regard or all connections through it will fail.

#### `--legacy-client-fd`

- **符号**: `switches::kLegacyClientFd` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：mojo_proxy：连接 legacy 客户端的 Unix socket fd（必填）。
> The integer value of a file descriptor inherited by the mojo_proxy process when launched by its host. This descriptor references a Unix socket which is connected to the legacy client application to be the target of this proxy. Required.

#### `--num-attachments`

- **符号**: `switches::kNumAttachments` &nbsp;&nbsp; **Buildflag**: 全平台
- **用途**：mojo_proxy：约定 invitation 附件采用 0 起的 64 位整数命名，指定数量。
> For client applications who expect Mojo invitation attachments to be assigned zero-based 64-bit integral values, this specifies the number of in-use attachments. The names are implicitly sequental integers starting from 0.


### WebNN (services/webnn/webnn_switches.h)

Web Neural Network API 服务开关。

_开关数：12_


#### `--webnn-coreml-dump-model`

- **符号**: `switches::kWebNNCoreMlDumpModel` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_MAC)
- **用途**：把生成的 Core ML 模型复制到指定文件夹（需 GPU 沙箱可访问，或加 --no-sandbox）。
> Copy the generated Core ML model to the folder specified by --webnn-coreml-dump-model, note: folder needs to be accessible from the GPU sandbox or use --no-sandbox. Usage: --no-sandbox --webnn-coreml-dump-model=/tmp/CoreMLModels

#### `--webnn-ort-disable-cpu-fallback`

- **符号**: `switches::kWebNNOrtDisableCpuFallback` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：ONNX Runtime 关闭默认 CPU EP 兜底（NPU/EP 失败时直接失败，便于调试）。
> This switch allows us to disable the fallback to default ORT CPU EP which is enabled by default. When this switch is set: 1. ORT session creation fails if non-CPU EPs cannot fully support all graph nodes. 2. Disables OpenVINO EP internal CPU fallback if NPU model compilation fails (for debugging). Usage: --webnn-ort-disable-cpu-fallback

#### `--webnn-ort-dump-model`

- **符号**: `switches::kWebNNOrtDumpModel` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：ONNX Runtime 把图级优化后的 ONNX 模型保存到指定目录。
> Set the folder specified by --webnn-ort-dump-model for ONNX Runtime to save optimized ONNX model after graph level transformations. Usage: --no-sandbox --webnn-ort-dump-model=/tmp/ort_models

#### `--webnn-ort-enable-profiling`

- **符号**: `switches::kWebNNOrtEnableProfiling` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：采集 ORT profile 数据用于性能分析（输出 `prefix_date_time.json`）。
> This switch allows us to collect ORT profile data for performance analysis. The profile data file is generated in Chrome's folder with a fixed naming format "prefix_date_time.json". The prefix can be provided by user or use "WebNNOrtProfile" as default. Usage: --no-sandbox --webnn-ort-enable-profiling="WebNNOrtOvCpuProfile"

#### `--webnn-ort-ep-device`

- **符号**: `switches::kWebNNOrtEpDevice` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：强制 ONNX Runtime 选择匹配 EP 名 + 硬件 vendor/device id 的单一 EP，例 `OpenVINOExecutionProvider,0x8086,0x4680`。
> Force ONNX Runtime to only select one specific execution provider (EP) device which matches the given EP name, hardware vendor id and hardware device id for WebNN during the ORT session creation stage. Notes: 1. Available EP devices can be queried in logs when the `webnn-ort-logging-level` is set to `VERBOSE` or `INFO`. If no matching device is found, the ORT session creation fails. 2.
> The CPU fallback EP device is added implicitly in ORT by default unless disabled explicitly by `webnn-ort-disable-cpu-fallback` switch. The value should be in the format: <ep_name>,<hardware_vendor_id>,<hardware_device_id> Please note that all the entries must be provided in order, and both hardware_vendor_id and hardware_device_id are hexadecimal strings. Usage: For example, specifying an Intel GPU device using OpenVINO EP: --webnn-ort-ep-device=OpenVINOExecutionProvider,0x8086,0x4680

#### `--webnn-ort-ep-library-path-for-testing`

- **符号**: `switches::kWebNNOrtEpLibraryPathForTesting` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（测试）强制按 `<ep_name>?<library_path>` 加载某 EP 库；非 ship 用，需配合 --allow-third-party-modules。
> Specify the ORT EP name and library path pair via this switch for testing development EP builds. Libraries of the ORT EP specified by the EP name are forced to be loaded from the specified path. This switch is not to be used in shipping scenarios and is ignored by default. The value should be in the format <ep_name>?<ep_library_path>.
> Usage: --webnn-ort-ep-library-path-for-testing=OpenVINOExecutionProvider?"C:\Program Files\ONNXRuntime-EP\onnxruntime_providers_openvino_plugin.dll" --allow-third-party-modules

#### `--webnn-ort-graph-optimization-level`

- **符号**: `switches::kWebNNOrtGraphOptimizationLevel` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：ORT 图优化等级：`DISABLE_ALL`/`BASIC`/`EXTENDED`/`ALL`。
> Configure the graph optimization level of ONNX Runtime. Usage: --webnn-ort-graph-optimization-level=DISABLE_ALL Other levels could be "BASIC", "EXTENDED" and "ALL".

#### `--webnn-ort-ignore-ep-blocklist`

- **符号**: `switches::kWebNNOrtIgnoreEpBlocklist` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：启用所有 EP，忽略启用 flag blocklist。
> Enable all execution providers, ignoring the enabled flag blocklist.

#### `--webnn-ort-ignore-ihv-eps`

- **符号**: `switches::kWebNNOrtIgnoreIhvEps` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：忽略所有 IHV EP，仅用默认 CPU 与 DML EP。
> Ignore all IHV execution providers, only the default CPUExecutionProvider and DmlExecutionProvider will be used.

#### `--webnn-ort-library-path-for-testing`

- **符号**: `switches::kWebNNOrtLibraryPathForTesting` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：（测试）强制从指定路径加载 onnxruntime.dll；非 ship 用，需配合 --allow-third-party-modules。
> Force onnxruntime.dll to be loaded from a location specified by the switch for testing development ORT build. This switch is not to be used in shipping scenarios and is ignored by default. Usage: --webnn-ort-library-path-for-testing="C:\Program Files\ONNXRuntime" --allow-third-party-modules

#### `--webnn-ort-logging-level`

- **符号**: `switches::kWebNNOrtLoggingLevel` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(IS_WIN)
- **用途**：ONNX Runtime 日志级别：`VERBOSE`/`INFO`/`WARNING`/`ERROR`（默认）/`FATAL`。
> Configure the logging severity level of ONNX Runtime. Usage: --webnn-ort-logging-level=VERBOSE Other severity levels could be "INFO", "WARNING", "ERROR" (default), and "FATAL".

#### `--webnn-tflite-dump-model`

- **符号**: `switches::kWebNNTfliteDumpModel` &nbsp;&nbsp; **Buildflag**: BUILDFLAG(WEBNN_USE_TFLITE)
- **用途**：把生成的 TFLite 模型保存到指定目录（需 GPU 沙箱可访问，或加 --no-sandbox）。
> Save the generated TFLite model file to the folder specified by --webnn-tflite-dump-model. Note, the folder needs to be accessible from the GPU process sandbox or --no-sandbox must be used. Usage: --no-sandbox --webnn-tflite-dump-model=/tmp/tflite_models


---

_本文档由 `xenon_overlay/doc/tools/extract_switches.py` + `xenon_overlay/doc/tools/render_switches.py` 自动生成。_
