// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_renderer_command_line.h"

#include <algorithm>
#include <utility>

#include "base/base64.h"
#include "base/containers/fixed_flat_set.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/strings/string_split.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "url/gurl.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

namespace xenon::ipc {
namespace {
constexpr char kOverridesSwitch[] = "xenon-electron-renderer-overrides";
constexpr size_t kMaxPayloadBytes = 16 * 1024;

base::CommandLine::StringType NativeString(const std::string& value) {
#if BUILDFLAG(IS_WIN)
  return base::UTF8ToWide(value);
#else
  return value;
#endif
}
}  // namespace

void AppendElectronIpcRendererHints(const base::CommandLine& browser,
                                    bool application_partition,
                                    base::CommandLine* renderer) {
  for (const char* name :
       {switches::kEnable, switches::kRendererIpcCandidate,
        switches::kAllowAllOrigins, switches::kAllowedOrigins}) {
    renderer->RemoveSwitch(name);
  }
  if (!browser.HasSwitch(switches::kEnable)) {
    return;
  }
  renderer->AppendSwitch(switches::kEnable);
  if (application_partition) {
    renderer->AppendSwitch(switches::kRendererIpcCandidate);
  }
  if (browser.HasSwitch(switches::kAllowAllOrigins)) {
    renderer->AppendSwitch(switches::kAllowAllOrigins);
  }
  if (browser.HasSwitch(switches::kAllowedOrigins)) {
    renderer->AppendSwitchASCII(
        switches::kAllowedOrigins,
        browser.GetSwitchValueASCII(switches::kAllowedOrigins));
  }
}

ElectronRendererIpcHint::ElectronRendererIpcHint(
    const base::CommandLine& command_line)
    : enabled_(command_line.HasSwitch(switches::kEnable)),
      application_partition_(
          command_line.HasSwitch(switches::kRendererIpcCandidate) ||
          command_line.HasSwitch("single-process")),
      allow_all_origins_(command_line.HasSwitch(switches::kAllowAllOrigins)) {
  for (const auto& value : base::SplitString(
           command_line.GetSwitchValueASCII(switches::kAllowedOrigins), ",",
           base::TRIM_WHITESPACE, base::SPLIT_WANT_NONEMPTY)) {
    auto origin = url::Origin::Create(GURL(value));
    if (!origin.opaque()) {
      allowed_origins_.push_back(std::move(origin));
    }
  }
}

ElectronRendererIpcHint::~ElectronRendererIpcHint() = default;

bool ElectronRendererIpcHint::MayRequestConfig(const url::Origin& origin,
                                               bool main_frame) const {
  if (!enabled_) {
    return false;
  }
  if (application_partition_) {
    return true;
  }
  if (!main_frame) {
    return false;
  }
  if (allow_all_origins_) {
    return true;
  }
  if (origin.opaque()) {
    return false;
  }
  return std::ranges::contains(allowed_origins_, origin);
}

bool IsElectronRendererSwitch(std::string_view name) {
  static constexpr auto kSwitches = base::MakeFixedFlatSet<std::string_view>({
      "autoplay-policy",
      "disable-background-timer-throttling",
      "disable-blink-features",
      "disable-features",
      "disable-lcd-text",
      "disable-renderer-backgrounding",
      "disable-smooth-scrolling",
      "enable-blink-features",
      "enable-experimental-web-platform-features",
      "enable-features",
      "enable-precise-memory-info",
      "force-color-profile",
      "force-device-scale-factor",
      "js-flags",
      "lang",
      "max-active-webgl-contexts",
  });
  return kSwitches.contains(name);
}

void AppendElectronRendererOverrides(const base::DictValue& overrides,
                                     base::CommandLine* command_line) {
  if (overrides.empty()) {
    return;
  }
  const auto json = base::WriteJson(overrides);
  if (json && json->size() <= kMaxPayloadBytes) {
    command_line->AppendSwitchASCII(kOverridesSwitch,
                                    base::Base64Encode(*json));
  }
}

void ApplyElectronRendererOverrides(base::CommandLine* command_line) {
  if (command_line->GetSwitchValueASCII("type") != "renderer") {
    return;
  }
  const auto encoded = command_line->GetSwitchValueASCII(kOverridesSwitch);
  command_line->RemoveSwitch(kOverridesSwitch);
  if (encoded.empty() || encoded.size() > kMaxPayloadBytes * 2) {
    return;
  }
  std::string json;
  if (!base::Base64Decode(encoded, &json) || json.size() > kMaxPayloadBytes) {
    return;
  }
  const auto values = base::JSONReader::ReadDict(json, base::JSON_PARSE_RFC);
  if (!values) {
    return;
  }
  for (const auto [name, value] : *values) {
    if (!IsElectronRendererSwitch(name) ||
        (!value.is_none() &&
         (!value.is_string() ||
          value.GetString().find('\0') != std::string::npos))) {
      return;
    }
  }
  for (const auto [name, value] : *values) {
    command_line->RemoveSwitch(name);
    if (value.is_string()) {
      command_line->AppendSwitchNative(name, NativeString(value.GetString()));
    }
  }
}

}  // namespace xenon::ipc
