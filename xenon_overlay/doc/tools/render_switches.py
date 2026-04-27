"""Render assets/switches.json to a categorized Chinese Markdown document.

Output: assets/chromium_switches_zh.md

Organization:
  1. Each source file is mapped to a Chinese "big category" + Chinese sub-title.
  2. The document is grouped by big category; inside, each sub-category (file)
     has a Chinese intro written below, followed by every switch.
  3. Each switch entry shows:
       ### `--switch-string[=<value>]`
       - **Symbol**: `switches::kFoo`  |  **File**: path  |  **Buildflag**: ...
       - 英文官方注释 (quoted)
"""

from __future__ import annotations

import json
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
IN_JSON = ROOT / "xenon_overlay" / "doc" / "switches.json"
OUT_MD = ROOT / "xenon_overlay" / "doc" / "chromium_switches_zh.md"
# Optional per-switch Chinese translations, keyed by the CLI switch string
# (e.g. "disable-gpu").  Missing entries fall back to English-only.
ZH_JSON = ROOT / "xenon_overlay" / "doc" / "tools" / "zh_comments.json"

# ---------------------------------------------------------------------------
# Category mapping: file path -> (big_category_id, sub_title_zh, sub_intro_zh)
# ---------------------------------------------------------------------------

# Big categories in display order.
BIG_CATEGORIES: list[tuple[str, str, str]] = [
    ("core", "一、核心 / 启动 / 浏览器主流程",
     "Chrome 浏览器主进程、多进程启动、用户资料（Profile）、打印、WebUI、应用模式（App Mode）等最常用的开关集中在这里。" 
     "大部分桌面端日常调试和功能开关都在 `chrome_switches` 和 `content_switches`。"),
    ("render", "二、渲染 / 图形 / 合成 / Blink",
     "涵盖 GPU 进程、GL/ANGLE/Vulkan 后端选择、Skia、合成器（cc / viz）、显示 (display)、字体 (gfx)、以及 Blink 渲染引擎相关开关。"
     "排错方向通常是：GPU 黑屏/花屏 → `gpu_switches` + `gl_switches`；滚动/动画卡顿 → `cc/base/switches` + `viz`；网页样式或 JS 行为异常 → Blink。"),
    ("media", "三、媒体 / 音视频 / WebRTC / MIDI",
     "音视频播放与编解码（包括 Windows 硬件编码 MF）、屏幕/摄像头采集、WebRTC、MIDI 等相关开关。"
     "常用于调试 H.264/HEVC/AV1 硬件解码、屏幕共享权限、音频环回采集等场景。"),
    ("network", "四、网络 / 安全浏览 / 鉴权",
     "网络栈（net/URLRequest/NetworkService）、连接/代理/HTTP/Cookie、Safe Browsing、Google 登录 (GAIA/GCM) 等相关开关。"
     "用来绕过证书验证、禁用 QUIC、自定义代理、调试同步登录时常用。"),
    ("process", "五、多进程 / 沙箱 / 崩溃 / Tracing / Metrics",
     "沙箱策略、Service Manager、崩溃上报、日志/Tracing、内存分析、指标上报等开关。"
     "排查子进程启动失败、沙箱被拒、内存泄漏时的主战场。"),
    ("extensions", "六、扩展 / WebUI / 应用",
     "扩展 (extensions)、应用 (apps)、WebUI flags 页面相关开关。"),
    ("privacy", "七、隐私 / 安全 / Policy / Sync / Signin",
     "企业策略、同步、身份 (Signin)、密码管理器、自动填充、WebAuthn、Trusted Vault 等开关。"),
    ("search", "八、搜索 / 翻译 / NTP / Dom Distiller",
     "默认搜索引擎、翻译、新标签页 (NTP)、阅读模式 (DOM Distiller) 等开关。"),
    ("variations", "九、Variations / 优化 / 组件更新 / 反馈",
     "Finch 试验 (variations)、Optimization Guide、组件更新器 (component_updater)、错误页、反馈、Client Hints 等基础设施开关。"),
    ("device", "十、设备 / 输入 / 窗口系统 / 无障碍",
     "VR、手柄、HID、鼠标键盘事件、窗口管理 (WM)、Ozone 平台、无障碍 (a11y)、DevTools UI 等开关。"),
    ("headless", "十一、Headless / 自动化 / 测试",
     "Headless Chrome、测试基础设施 (test_switches)、extensions_shell 等。"),
    ("misc", "十二、其它 / 杂项",
     "Mojo、Skia、chrome/browser 下一些较专门的子模块 (predictors / actor / nearby_sharing / new_tab_page modules 等)。"),
]

# Map each file to (big_cat, sub_title). If a file is not listed here, it
# falls back to the last category "misc".
FILE_MAP: dict[str, tuple[str, str, str]] = {
    # ---------- core ----------
    "chrome/common/chrome_switches.cc":
        ("core", "Chrome 浏览器 (chrome/common/chrome_switches.cc)",
         "Chrome 桌面浏览器主进程和产品逻辑使用最多的开关集合：应用模式、Profile、启动 URL、打印、诊断、WebUI、安装/升级等。"),
    "content/public/common/content_switches.cc":
        ("core", "Content 层 (content/public/common/content_switches.cc)",
         "内容层 (content) 暴露给 Chrome 与嵌入者的跨进程通用开关：Renderer/GPU/Utility 子进程命令行、站点隔离、IPC、可访问性等。"),
    "base/base_switches.cc":
        ("core", "Base 基础库 (base/base_switches.cc)",
         "最底层的通用开关：Feature 开关、Field Trial、崩溃报告、低端设备模式、共享内存句柄、Profiling 等。"),
    "base/test/test_switches.cc":
        ("core", "Base Test (base/test/test_switches.cc)", "base 测试框架相关开关。"),
    "base/i18n/base_i18n_switches.cc":
        ("core", "Base i18n (base/i18n/base_i18n_switches.cc)", "国际化/区域 (locale) 覆盖相关开关。"),
    "components/embedder_support/switches.cc":
        ("core", "Embedder Support (components/embedder_support/switches.cc)",
         "embedder_support 组件暴露给浏览器壳的通用开关，如 User-Agent 覆盖等。"),
    "apps/switches.cc":
        ("core", "Apps (apps/switches.cc)", "Chrome Apps（打包应用）相关开关。"),

    # ---------- render ----------
    "ui/gl/gl_switches.cc":
        ("render", "OpenGL / ANGLE (ui/gl/gl_switches.cc)",
         "GL/ANGLE/Vulkan 后端、直接合成 (DirectComposition)、DXGI、overlay 格式等 GPU 底层开关，是 Windows 上排查 GPU 问题的主入口。"),
    "gpu/config/gpu_switches.cc":
        ("render", "GPU 配置 (gpu/config/gpu_switches.cc)",
         "GPU 进程的黑名单/白名单、活动驱动信息、GPU 启动参数等。"),
    "gpu/command_buffer/service/gpu_switches.cc":
        ("render", "GPU Command Buffer Service (gpu/command_buffer/service/gpu_switches.cc)", "GPU 命令缓冲区服务端开关。"),
    "gpu/command_buffer/client/gpu_switches.cc":
        ("render", "GPU Command Buffer Client (gpu/command_buffer/client/gpu_switches.cc)", "GPU 命令缓冲区客户端开关。"),
    "cc/base/switches.cc":
        ("render", "合成器 cc (cc/base/switches.cc)",
         "Chromium Compositor (cc) 开关：tile 尺寸、滚动预测、合成调试可视化 (paint rects / layer borders) 等。"),
    "components/viz/common/switches.cc":
        ("render", "Viz (components/viz/common/switches.cc)",
         "显示合成服务 (viz) 开关，包括 SkiaRenderer、FrameRateLimit 等。"),
    "components/viz/demo/common/switches.cc":
        ("render", "Viz Demo (components/viz/demo/common/switches.cc)", "viz demo 可执行文件专用开关。"),
    "ui/compositor/compositor_switches.cc":
        ("render", "UI Compositor (ui/compositor/compositor_switches.cc)", "ui/compositor 层开关。"),
    "ui/display/display_switches.cc":
        ("render", "Display (ui/display/display_switches.cc)", "显示器 / 缩放 / 强制 DPI 等开关。"),
    "ui/gfx/switches.cc":
        ("render", "gfx (ui/gfx/switches.cc)", "字体回退、字形渲染等通用图形开关。"),
    "third_party/blink/common/switches.cc":
        ("render", "Blink (third_party/blink/common/switches.cc)",
         "Blink 渲染引擎开关：blink-settings、dark-mode、tile 尺寸、JS runtime flags、站点隔离例外等。"),
    "third_party/blink/public/common/switches.h":
        ("render", "Blink (third_party/blink/public/common/switches.h)", "Blink 公共头里 inline 定义的开关。"),
    "ui/base/ui_base_switches.h":
        ("render", "UI Base (ui/base/ui_base_switches.h)", "ui/base 通用 UI 开关，含平台特定 (Win/Mac/Linux) 分支。"),
    "skia/ext/switches.cc":
        ("render", "Skia (skia/ext/switches.cc)", "Skia 渲染相关开关。"),

    # ---------- media ----------
    "media/base/media_switches.cc":
        ("media", "Media (media/base/media_switches.cc)",
         "音视频核心开关：音频环回、硬件解码/编码、DRM、WebRTC 编解码选择等。"),
    "media/capture/capture_switches.cc":
        ("media", "Media Capture (media/capture/capture_switches.cc)", "屏幕/摄像头采集开关。"),
    "media/midi/midi_switches.cc":
        ("media", "MIDI (media/midi/midi_switches.cc)", "Web MIDI 开关。"),
    "media/gpu/windows/mf_video_encoder_switches.cc":
        ("media", "Windows MediaFoundation 编码 (media/gpu/windows/mf_video_encoder_switches.cc)",
         "Windows 上基于 MediaFoundation 的硬件视频编码器开关。"),

    # ---------- network ----------
    "services/network/public/cpp/network_switches.cc":
        ("network", "Network Service (services/network/public/cpp/network_switches.cc)",
         "网络服务进程开关：host resolver、忽略证书错误、日志、代理等。"),
    "components/network_session_configurator/common/network_switches.cc":
        ("network", "网络会话配置 (components/network_session_configurator/common/network_switches.cc)",
         "HTTP/QUIC 会话参数开关。"),
    "components/safe_browsing/core/common/safebrowsing_switches.cc":
        ("network", "Safe Browsing (components/safe_browsing/core/common/safebrowsing_switches.cc)",
         "安全浏览 (反钓鱼/反恶意软件) 开关。"),
    "google_apis/gaia/gaia_switches.cc":
        ("network", "GAIA (google_apis/gaia/gaia_switches.cc)", "Google 账号 (GAIA) OAuth 端点覆盖开关。"),
    "google_apis/gcm/engine/gservices_switches.cc":
        ("network", "GCM (google_apis/gcm/engine/gservices_switches.cc)", "GCM 推送相关开关。"),

    # ---------- process ----------
    "sandbox/policy/switches.cc":
        ("process", "沙箱 (sandbox/policy/switches.cc)", "沙箱策略开关：启用/禁用沙箱、沙箱类型、日志等。"),
    "services/service_manager/switches.cc":
        ("process", "Service Manager (services/service_manager/switches.cc)", "Service Manager 全局开关。"),
    "services/service_manager/public/cpp/service_executable/switches.cc":
        ("process", "Service 可执行 (services/service_manager/public/cpp/service_executable/switches.cc)",
         "service 可执行文件公共开关。"),
    "components/crash/core/app/crash_switches.cc":
        ("process", "Crash (components/crash/core/app/crash_switches.cc)", "崩溃处理器进程相关开关。"),
    "components/tracing/common/tracing_switches.cc":
        ("process", "Tracing (components/tracing/common/tracing_switches.cc)", "Chrome tracing 启动开关。"),
    "components/heap_profiling/in_process/switches.cc":
        ("process", "Heap Profiling (components/heap_profiling/in_process/switches.cc)", "进程内堆分析开关。"),
    "components/services/heap_profiling/public/cpp/switches.cc":
        ("process", "Heap Profiling 服务 (components/services/heap_profiling/public/cpp/switches.cc)",
         "独立堆分析服务开关。"),
    "components/metrics/metrics_switches.cc":
        ("process", "Metrics (components/metrics/metrics_switches.cc)", "UMA/UKM 指标上报开关。"),
    "services/resource_coordinator/memory_instrumentation/switches.cc":
        ("process", "内存检测 (services/resource_coordinator/memory_instrumentation/switches.cc)",
         "内存快照/仪表化开关。"),
    "mojo/core/test/test_switches.cc":
        ("process", "Mojo Test (mojo/core/test/test_switches.cc)", "mojo 核心测试开关。"),

    # ---------- extensions ----------
    "extensions/common/switches.cc":
        ("extensions", "Extensions (extensions/common/switches.cc)",
         "扩展系统开关：解锁扩展白名单、加载未打包扩展、Native Messaging 主机白名单等。"),
    "chrome/browser/extensions/updater/extension_updater_switches.cc":
        ("extensions", "扩展更新 (chrome/browser/extensions/updater/extension_updater_switches.cc)",
         "扩展更新源 URL 覆盖等。"),
    "components/webui/flags/flags_ui_switches.cc":
        ("extensions", "flags UI (components/webui/flags/flags_ui_switches.cc)", "chrome://flags 相关开关。"),

    # ---------- privacy ----------
    "components/policy/core/common/policy_switches.cc":
        ("privacy", "企业策略 (components/policy/core/common/policy_switches.cc)",
         "云端策略/机器级策略/策略验证开关。"),
    "components/signin/public/base/signin_switches.cc":
        ("privacy", "Signin (components/signin/public/base/signin_switches.cc)", "账号登录相关开关。"),
    "components/sync/base/command_line_switches.cc":
        ("privacy", "Sync (components/sync/base/command_line_switches.cc)", "Chrome Sync 服务端点、调试开关。"),
    "components/browser_sync/browser_sync_switches.cc":
        ("privacy", "Browser Sync (components/browser_sync/browser_sync_switches.cc)", "浏览器层 Sync 相关开关。"),
    "components/sync_bookmarks/switches.cc":
        ("privacy", "Sync 书签 (components/sync_bookmarks/switches.cc)", "书签同步开关。"),
    "components/password_manager/core/browser/password_manager_switches.cc":
        ("privacy", "密码管理器 (components/password_manager/core/browser/password_manager_switches.cc)", ""),
    "components/autofill/core/common/autofill_switches.cc":
        ("privacy", "自动填充 (components/autofill/core/common/autofill_switches.cc)", ""),
    "components/trusted_vault/command_line_switches.cc":
        ("privacy", "Trusted Vault (components/trusted_vault/command_line_switches.cc)", ""),
    "chrome/browser/webauthn/webauthn_switches.cc":
        ("privacy", "WebAuthn (chrome/browser/webauthn/webauthn_switches.cc)", ""),
    "chrome/browser/signin/bound_session_credentials/bound_session_switches.cc":
        ("privacy", "Bound Session (chrome/browser/signin/bound_session_credentials/bound_session_switches.cc)", ""),
    "components/enterprise/browser/enterprise_switches.cc":
        ("privacy", "企业 (components/enterprise/browser/enterprise_switches.cc)", ""),

    # ---------- search ----------
    "components/search_engines/search_engines_switches.cc":
        ("search", "搜索引擎 (components/search_engines/search_engines_switches.cc)", "默认搜索引擎覆盖开关。"),
    "components/search_engines/search_engine_choice/search_engine_choice_switches.cc":
        ("search", "搜索引擎选择 (components/search_engines/search_engine_choice/search_engine_choice_switches.cc)",
         "欧盟搜索引擎选择屏幕开关。"),
    "components/translate/core/common/translate_switches.cc":
        ("search", "翻译 (components/translate/core/common/translate_switches.cc)", "翻译后端/调试开关。"),
    "components/dom_distiller/core/dom_distiller_switches.cc":
        ("search", "DOM Distiller (components/dom_distiller/core/dom_distiller_switches.cc)", "阅读模式相关开关。"),
    "components/ntp_tiles/switches.cc":
        ("search", "NTP Tiles (components/ntp_tiles/switches.cc)", "新标签页瓷片来源开关。"),
    "components/search_provider_logos/switches.cc":
        ("search", "Doodle Logos (components/search_provider_logos/switches.cc)", "搜索页 doodle 后端开关。"),
    "components/regional_capabilities/regional_capabilities_switches.cc":
        ("search", "Regional (components/regional_capabilities/regional_capabilities_switches.cc)", ""),
    "components/google/core/common/google_switches.cc":
        ("search", "Google Base URL (components/google/core/common/google_switches.cc)",
         "用于覆盖 google.com 基地址，常用于测试。"),
    "chrome/browser/google/switches.cc":
        ("search", "Chrome Google (chrome/browser/google/switches.cc)", ""),
    "chrome/browser/new_tab_page/modules/modules_switches.cc":
        ("search", "NTP Modules (chrome/browser/new_tab_page/modules/modules_switches.cc)", "新标签页模块调试开关。"),

    # ---------- variations ----------
    "components/variations/variations_switches.cc":
        ("variations", "Variations (components/variations/variations_switches.cc)",
         "Finch 试验种子/参数覆盖开关，常用于强制 A/B 分组。"),
    "components/optimization_guide/core/optimization_guide_switches.cc":
        ("variations", "Optimization Guide (components/optimization_guide/core/optimization_guide_switches.cc)",
         "优化指南服务 / 模型下发开关。"),
    "components/component_updater/component_updater_switches.cc":
        ("variations", "组件更新 (components/component_updater/component_updater_switches.cc)", "组件更新器开关。"),
    "components/page_content_annotations/core/page_content_annotations_switches.cc":
        ("variations", "页面内容注解 (components/page_content_annotations/core/page_content_annotations_switches.cc)", ""),
    "components/feedback/feedback_switches.cc":
        ("variations", "反馈 (components/feedback/feedback_switches.cc)", ""),
    "components/permissions/switches.cc":
        ("variations", "权限 (components/permissions/switches.cc)", ""),
    "components/error_page/common/error_page_switches.cc":
        ("variations", "错误页 (components/error_page/common/error_page_switches.cc)", ""),
    "components/client_hints/common/switches.cc":
        ("variations", "Client Hints (components/client_hints/common/switches.cc)", ""),
    "components/infobars/core/infobars_switches.h":
        ("variations", "Infobars (components/infobars/core/infobars_switches.h)", ""),
    "components/data_sharing/public/switches.h":
        ("variations", "Data Sharing (components/data_sharing/public/switches.h)", ""),

    # ---------- device ----------
    "device/vr/public/cpp/switches.cc":
        ("device", "VR (device/vr/public/cpp/switches.cc)", "WebXR / OpenXR 运行时开关。"),
    "device/gamepad/public/cpp/gamepad_switches.cc":
        ("device", "Gamepad (device/gamepad/public/cpp/gamepad_switches.cc)", "手柄开关。"),
    "services/device/public/cpp/hid/hid_switches.cc":
        ("device", "HID (services/device/public/cpp/hid/hid_switches.cc)", "WebHID 开关。"),
    "components/input/switches.cc":
        ("device", "输入 (components/input/switches.cc)", "输入事件开关。"),
    "ui/events/event_switches.cc":
        ("device", "UI Events (ui/events/event_switches.cc)", "鼠标/触控/手势事件开关。"),
    "ui/wm/core/wm_core_switches.cc":
        ("device", "Window Manager (ui/wm/core/wm_core_switches.cc)", "窗口管理器开关。"),
    "ui/ozone/public/ozone_switches.cc":
        ("device", "Ozone (ui/ozone/public/ozone_switches.cc)", "Ozone 平台后端开关 (主要用于 Linux)。"),
    "ui/accessibility/accessibility_switches.cc":
        ("device", "无障碍 (ui/accessibility/accessibility_switches.cc)", "a11y 调试开关。"),
    "components/ui_devtools/switches.cc":
        ("device", "UI DevTools (components/ui_devtools/switches.cc)", "原生 UI DevTools 开关。"),
    "ui/views/views_switches.h":
        ("device", "Views (ui/views/views_switches.h)", "Chromium Views 工具集开关。"),

    # ---------- headless ----------
    "components/headless/command_handler/headless_command_switches.cc":
        ("headless", "Headless 命令处理 (components/headless/command_handler/headless_command_switches.cc)", ""),
    "chrome/test/base/test_switches.cc":
        ("headless", "Chrome Test (chrome/test/base/test_switches.cc)", ""),
    "headless/public/switches.h":
        ("headless", "Headless Public (headless/public/switches.h)", ""),
    "components/test/test_switches.cc":
        ("headless", "Components Test (components/test/test_switches.cc)", ""),

    # ---------- misc ----------
    "mojo/proxy/switches.cc":
        ("misc", "Mojo Proxy (mojo/proxy/switches.cc)", "mojo 代理进程开关。"),
    "chrome/browser/predictors/predictors_switches.cc":
        ("misc", "Predictors (chrome/browser/predictors/predictors_switches.cc)", "预加载/预测器开关。"),
    "chrome/browser/actor/actor_switches.cc":
        ("misc", "Actor (chrome/browser/actor/actor_switches.cc)", ""),
    "chrome/browser/nearby_sharing/common/nearby_share_switches.cc":
        ("misc", "Nearby Share (chrome/browser/nearby_sharing/common/nearby_share_switches.cc)", ""),
    "components/media_router/common/providers/cast/certificate/switches.cc":
        ("misc", "Cast 证书 (components/media_router/common/providers/cast/certificate/switches.cc)", ""),
    "chrome/browser/enterprise/connectors/device_trust/attestation/browser/attestation_switches.cc":
        ("misc", "Device Trust Attestation (attestation_switches.cc)", ""),
}


def categorize(rel_file: str) -> tuple[str, str, str]:
    if rel_file in FILE_MAP:
        return FILE_MAP[rel_file]
    # Equivalent .h / .cc: try swapping the extension.
    if rel_file.endswith(".h") and rel_file[:-2] + ".cc" in FILE_MAP:
        return FILE_MAP[rel_file[:-2] + ".cc"]
    if rel_file.endswith(".cc") and rel_file[:-3] + ".h" in FILE_MAP:
        return FILE_MAP[rel_file[:-3] + ".h"]
    # Explicit extra mappings (header-only definitions we noticed):
    extra_map: dict[str, tuple[str, str, str]] = {
        "components/os_crypt/common/os_crypt_switches.h":
            ("privacy", "OS Crypt (components/os_crypt/common/os_crypt_switches.h)",
             "操作系统密钥链 (macOS Keychain / Windows DPAPI / Linux Secret Service) 访问开关。"),
        "components/cast_streaming/browser/cast_streaming_switches.h":
            ("media", "Cast Streaming (components/cast_streaming/browser/cast_streaming_switches.h)", ""),
        "chrome/browser/headless/headless_mode_switches.h":
            ("headless", "Chrome Headless Mode (chrome/browser/headless/headless_mode_switches.h)", ""),
        "chrome/windows_services/service_program/switches.h":
            ("misc", "Windows 服务 (chrome/windows_services/service_program/switches.h)",
             "Chrome 安装为 Windows 服务时用到的开关。"),
        "content/shell/common/shell_switches.h":
            ("headless", "Content Shell (content/shell/common/shell_switches.h)",
             "content_shell / chrome_shell 测试程序专用开关。"),
        "content/web_test/common/web_test_switches.h":
            ("headless", "Web Tests (content/web_test/common/web_test_switches.h)",
             "layout/web test 框架（run_web_tests）开关。"),
        "services/webnn/webnn_switches.h":
            ("misc", "WebNN (services/webnn/webnn_switches.h)",
             "Web Neural Network API 服务开关。"),
    }
    if rel_file in extra_map:
        return extra_map[rel_file]
    # Fallback: derive big category from path prefix heuristically.
    if rel_file.startswith(("gpu/", "ui/gl", "third_party/blink")):
        return ("render", f"Render ({rel_file})", "")
    if rel_file.startswith("media/"):
        return ("media", f"Media ({rel_file})", "")
    if rel_file.startswith(("components/sync", "components/password_manager", "components/autofill",
                              "components/signin")):
        return ("privacy", f"Privacy ({rel_file})", "")
    if rel_file.startswith("chrome/"):
        return ("misc", f"Chrome ({rel_file})", "")
    return ("misc", f"Other ({rel_file})", "")


# ---------------------------------------------------------------------------
# Render helpers
# ---------------------------------------------------------------------------

def fmt_buildflag(bf: str) -> str:
    """Format a raw buildflag string for display.

    Strips trivial include-guard conditions like `!defined(FOO_H_)` that leak
    in because our parser treats every `#ifndef` as a condition.
    """
    if not bf:
        return "全平台"
    import re as _re
    # Split on logical operators; this is a rough split but good enough for display.
    parts = [p.strip() for p in _re.split(r"\s+(?:&&|\|\|)\s+", bf)]
    kept: list[str] = []
    for p in parts:
        m = _re.fullmatch(r"!defined\((\w+)\)", p)
        if m and m.group(1).endswith("_H_"):
            continue
        # Also drop its positive form `defined(FOO_H_)` just in case.
        m2 = _re.fullmatch(r"defined\((\w+)\)", p)
        if m2 and m2.group(1).endswith("_H_"):
            continue
        kept.append(p)
    if not kept:
        return "全平台"
    return " && ".join(kept)


def fmt_switch_heading(value: str) -> str:
    """Form the CLI heading, showing =<value> when it's clearly a parameterised switch."""
    return f"`--{value}`"


def load_zh_comments() -> dict[str, str]:
    if not ZH_JSON.exists():
        return {}
    try:
        data = json.loads(ZH_JSON.read_text(encoding="utf-8"))
    except Exception:
        return {}
    if isinstance(data, dict):
        return {str(k): str(v) for k, v in data.items() if v}
    return {}


def render(records: list[dict]) -> str:
    zh_map = load_zh_comments()
    # group by big_category -> sub_title -> records (preserve order)
    by_big: dict[str, list[tuple[str, str, list[dict]]]] = defaultdict(list)
    index_for_sub: dict[tuple[str, str], int] = {}

    for r in records:
        big, sub, _ = categorize(r["file"])
        key = (big, sub)
        if key not in index_for_sub:
            index_for_sub[key] = len(by_big[big])
            _, _, intro = categorize(r["file"])
            by_big[big].append((sub, intro, []))
        by_big[big][index_for_sub[key]][2].append(r)

    lines: list[str] = []
    lines.append("# Chromium 命令行开关全览（桌面 / Windows 视角）\n")
    lines.append(
        "> 自动从 Chromium 源码 (`H:\\chromium_142\\src`) 扫描生成。已过滤掉 Android / iOS / "
        "ChromeOS / Fuchsia / Cast 仅限平台的开关。\n"
    )
    lines.append(
        f"> 共收录 **{len(records)}** 个开关（去重后），扫描了 "
        "所有 `*switches*.cc/.h` 以及若干 `command_line_switches` 文件。\n"
    )
    lines.append(
        "> 每个开关下方的英文引用来自源码原注释（最权威准确的用途说明），"
        "分类标题与概述为中文。Buildflag 一列表明该开关只在特定编译条件下生效，"
        "写着 “全平台” 则没有条件编译限制。\n"
    )
    lines.append("> 使用方式：`chrome.exe --switch-name[=value] ...`。\n")

    # --- table of contents -------------------------------------------------
    lines.append("\n## 目录\n")
    for big_id, big_title, _ in BIG_CATEGORIES:
        if big_id not in by_big:
            continue
        lines.append(f"- [{big_title}](#{anchor(big_title)})")
        for sub_title, _, recs in by_big[big_id]:
            lines.append(f"  - [{sub_title} ({len(recs)})](#{anchor(sub_title)})")
    lines.append("")

    # --- common switches quick reference -----------------------------------
    lines.append("## 常用开关速查（中文）\n")
    lines.append(quick_reference())
    lines.append("")

    # --- body -------------------------------------------------------------
    for big_id, big_title, big_intro in BIG_CATEGORIES:
        if big_id not in by_big:
            continue
        lines.append(f"\n## {big_title}\n")
        lines.append(big_intro + "\n")
        for sub_title, sub_intro, recs in by_big[big_id]:
            lines.append(f"\n### {sub_title}\n")
            if sub_intro:
                lines.append(sub_intro + "\n")
            lines.append(f"_开关数：{len(recs)}_\n")
            for r in recs:
                name = r["name"]
                value = r["value"]
                comment = r["comment"].strip()
                bf = fmt_buildflag(r["buildflag"])
                lines.append(f"\n#### {fmt_switch_heading(value)}")
                lines.append("")
                lines.append(
                    f"- **符号**: `switches::{name}` "
                    f"&nbsp;&nbsp; **Buildflag**: {bf}"
                )
                if r.get("also_in"):
                    lines.append(
                        "- **同名定义**: " + ", ".join(f"`{p}`" for p in r["also_in"])
                    )
                zh = zh_map.get(value, "").strip()
                if zh:
                    lines.append(f"- **用途**：{zh}")
                if comment:
                    for para in break_paragraphs(comment):
                        lines.append(f"> {para}")
                elif not zh:
                    lines.append("> _（源码中没有注释）_")
            lines.append("")

    lines.append("\n---\n")
    lines.append("_本文档由 `xenon_overlay/doc/tools/extract_switches.py` + `xenon_overlay/doc/tools/render_switches.py` 自动生成。_\n")
    return "\n".join(lines)


def anchor(title: str) -> str:
    # Simple GitHub-ish anchor: lower, remove punctuation, spaces -> -
    import re
    a = title.lower()
    a = re.sub(r"[()`/\\.:]", "", a)
    a = a.replace(" ", "-")
    a = re.sub(r"-+", "-", a)
    return a.strip("-")


def break_paragraphs(s: str) -> list[str]:
    # Comments are joined with spaces but often read better as one blockquote,
    # so keep a single line; just make sure it doesn't exceed ~500 chars per
    # line so Markdown viewers stay happy.
    s = s.strip()
    if len(s) <= 500:
        return [s]
    out = []
    while len(s) > 500:
        cut = s.rfind(". ", 0, 500)
        if cut < 200:
            cut = 500
        out.append(s[:cut + 1].strip())
        s = s[cut + 1:].strip()
    if s:
        out.append(s)
    return out


# ---------------------------------------------------------------------------
# Quick reference section (hand-written Chinese)
# ---------------------------------------------------------------------------

QUICK_REFERENCE: list[tuple[str, list[tuple[str, str]]]] = [
    ("启动与基础", [
        ("--user-data-dir=<dir>", "指定用户数据目录，隔离配置/缓存/扩展，调试多实例常用。"),
        ("--profile-directory=<name>", "在 user-data-dir 下启动指定 Profile。"),
        ("--no-first-run", "跳过首次运行向导，自动化常用。"),
        ("--no-default-browser-check", "不弹出“设为默认浏览器”提示。"),
        ("--disable-features=<F1,F2>", "关闭指定 Feature（chrome://flags 里的实验项）。"),
        ("--enable-features=<F1,F2>", "启用指定 Feature，可追加 `<F>:param/value` 形式指定参数。"),
        ("--lang=<code>", "指定 UI 语言（底层走 base_i18n_switches）。"),
        ("--log-level=0..3", "提高日志等级（0=INFO，3=ERROR）。"),
        ("--enable-logging=stderr", "将日志输出到 stderr（配合 `--v=<n>` 打开 VLOG）。"),
        ("--v=<n>", "VLOG 详细级别。"),
        ("--vmodule=pattern=level,...", "按文件/模块开启 VLOG。"),
    ]),
    ("渲染 / GPU 调试", [
        ("--disable-gpu", "关闭 GPU 加速，所有内容走软件渲染。"),
        ("--disable-gpu-compositing", "关闭合成 GPU，只保留 2D 软件合成。"),
        ("--use-angle=<backend>", "指定 ANGLE 后端：d3d11 / d3d9 / gl / gles / vulkan / metal 等。"),
        ("--use-gl=<backend>", "指定 GL 后端：desktop / egl / swiftshader 等。"),
        ("--enable-gpu-rasterization", "启用 GPU 光栅化。"),
        ("--disable-gpu-vsync", "解除 GPU 垂直同步，调试帧率上限。"),
        ("--show-paint-rects", "在网页上画出每帧重绘区域。"),
        ("--show-layer-animation-bounds", "高亮动画中图层边界。"),
        ("--enable-gpu-benchmarking", "开启 JS `chrome.gpuBenchmarking` 测试接口。"),
        ("--disable-accelerated-video-decode", "禁用硬件视频解码。"),
        ("--disable-accelerated-2d-canvas", "禁用 2D Canvas GPU 加速。"),
    ]),
    ("网络 / 证书 / 代理", [
        ("--proxy-server=<scheme://host:port>", "指定代理服务器。"),
        ("--proxy-bypass-list=\"<list>\"", "绕过代理的地址列表。"),
        ("--host-resolver-rules=\"MAP * 127.0.0.1\"", "自定义 DNS 解析规则。"),
        ("--ignore-certificate-errors", "忽略所有 SSL/TLS 错误（调试用，有安全风险）。"),
        ("--test-type", "内部测试模式标志，常与上面证书绕过一起使用。"),
        ("--disable-quic", "禁用 QUIC。"),
        ("--enable-quic", "强制启用 QUIC。"),
        ("--log-net-log=<file>", "输出 NetLog JSON 到指定文件。"),
        ("--ssl-key-log-file=<file>", "导出 TLS 密钥（配合 Wireshark 解密 TLS）。"),
        ("--disable-background-networking", "关闭后台网络活动（升级检查、Field Trial 等）。"),
    ]),
    ("站点隔离 / 安全", [
        ("--site-per-process", "强制每个站点独立渲染进程（严格站点隔离）。"),
        ("--disable-site-isolation-trials", "禁用站点隔离 Finch 试验。"),
        ("--disable-web-security", "关闭同源策略（仅调试，切勿用于日常浏览）。"),
        ("--allow-insecure-localhost", "localhost 自签 HTTPS 不报错。"),
        ("--allow-running-insecure-content", "允许 HTTPS 页面加载 HTTP 子资源。"),
        ("--unsafely-treat-insecure-origin-as-secure=<url>", "把 http 源当作 secure context（用于本地 PWA 调试）。"),
    ]),
    ("扩展 / 应用", [
        ("--disable-extensions", "禁用所有扩展。"),
        ("--load-extension=<path>", "加载未打包扩展。"),
        ("--disable-extensions-except=<paths>", "保留指定扩展，禁用其他。"),
        ("--allowlisted-extension-id=<id>", "将扩展 ID 加入解锁白名单（访问受限 API）。"),
        ("--whitelisted-extension-id=<id>", "旧名；同上。"),
        ("--pack-extension=<path>", "打包 CRX。"),
        ("--pack-extension-key=<pem>", "指定 CRX 签名私钥。"),
    ]),
    ("沙箱 / 进程", [
        ("--no-sandbox", "完全关闭沙箱（极其不安全，仅用于排错）。"),
        ("--disable-gpu-sandbox", "仅关闭 GPU 沙箱。"),
        ("--renderer-startup-dialog", "Renderer 启动时弹对话框，便于附加调试器。"),
        ("--gpu-startup-dialog", "GPU 进程启动时弹对话框。"),
        ("--wait-for-debugger-children=<proc>", "指定子进程类型启动时等待调试器。"),
        ("--single-process", "所有组件跑在一个进程（仅调试，崩溃模型不同）。"),
    ]),
    ("Headless / 自动化", [
        ("--headless[=new|old]", "Headless 模式；`new` 是新版 Headless，默认在 133 之后。"),
        ("--remote-debugging-port=<port>", "开启 DevTools 协议端口，支持 Puppeteer/CDP。"),
        ("--remote-allow-origins=<origins>", "允许指定 origin 访问 DevTools WebSocket。"),
        ("--window-size=W,H", "初始窗口大小。"),
        ("--virtual-time-budget=<ms>", "Headless 下加速虚拟时间。"),
        ("--screenshot[=path]", "Headless 模式截屏。"),
        ("--dump-dom", "Headless 下 dump 最终 DOM。"),
        ("--print-to-pdf[=file]", "Headless 下打印成 PDF。"),
    ]),
    ("Tracing / Profiling", [
        ("--trace-startup=<categories>", "启动即开始 Tracing，到 trace-startup-duration 秒为止。"),
        ("--trace-startup-file=<path>", "Tracing 输出文件。"),
        ("--trace-shutdown", "进程退出时自动写出 Trace。"),
        ("--enable-heap-profiling", "启用进程内堆采样。"),
        ("--memlog=<mode>", "启动内存日志服务。"),
    ]),
    ("变体 (Variations) / Field Trials", [
        ("--force-fieldtrials=<name/group/...>", "强制 Field Trial 分组（测试用）。"),
        ("--variations-server-url=<url>", "覆盖 Finch 种子下载地址。"),
        ("--fake-variations-channel=<channel>", "伪造 Finch 判定的发布通道。"),
    ]),
]


def quick_reference() -> str:
    out: list[str] = []
    out.append("以下是最常用的一组桌面端开关，按场景分组，其余 1000+ 条在下文按模块给出官方注释。")
    out.append("")
    for title, rows in QUICK_REFERENCE:
        out.append(f"### {title}\n")
        out.append("| 开关 | 用途 |")
        out.append("| --- | --- |")
        for sw, desc in rows:
            out.append(f"| `{sw}` | {desc} |")
        out.append("")
    return "\n".join(out)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    data = json.loads(IN_JSON.read_text(encoding="utf-8"))
    records = data["switches"]
    md = render(records)
    OUT_MD.parent.mkdir(parents=True, exist_ok=True)
    OUT_MD.write_text(md, encoding="utf-8")
    print(f"records rendered: {len(records)}")
    print(f"output: {OUT_MD}")
    size_kb = OUT_MD.stat().st_size / 1024
    print(f"size: {size_kb:.1f} KiB")


if __name__ == "__main__":
    main()
