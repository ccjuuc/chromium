// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_app_command_line.h"

#include <algorithm>
#include <map>
#include <utility>

#include "base/containers/fixed_flat_set.h"
#include "base/json/json_writer.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "xenon_overlay/common/ipc/xenon_renderer_command_line.h"

namespace xenon::ipc {
namespace {
constexpr size_t kMaxCommandLineBytes = 16 * 1024;
constexpr size_t kMaxRendererOverridesBytes = 8 * 1024;

base::CommandLine::StringType NativeString(const std::string& value) {
#if BUILDFLAG(IS_WIN)
  return base::UTF8ToWide(value);
#else
  return value;
#endif
}
std::string Utf8String(const base::CommandLine::StringType& value) {
#if BUILDFLAG(IS_WIN)
  return base::WideToUTF8(value);
#else
  return value;
#endif
}
bool ValidName(const std::string& name) {
  return !name.empty() && name.size() <= 256 &&
         base::IsAsciiAlphaNumeric(name.front()) &&
         std::ranges::all_of(name, [](char c) {
           return base::IsAsciiAlphaNumeric(c) || c == '-' || c == '_';
         });
}
auto& CommandLines() {
  static base::NoDestructor<std::map<std::string, ElectronAppCommandLine>>
      values;
  return *values;
}
bool RequiresSharedHostStartup(std::string_view name) {
  static constexpr auto kSwitches = base::MakeFixedFlatSet<std::string_view>({
      "enable-sandbox",
      "no-sandbox",
      "proxy-bypass-list",
      "proxy-pac-url",
      "proxy-server",
      "remote-debugging-pipe",
      "remote-debugging-port",
      "renderer-cmd-prefix",
      "single-process",
      "type",
      "user-data-dir",
  });
  return kSwitches.contains(name);
}
}  // namespace

ElectronAppCommandLine::ElectronAppCommandLine(base::CommandLine command_line)
    : command_line_(std::move(command_line)) {}
ElectronAppCommandLine::~ElectronAppCommandLine() = default;

void ElectronAppCommandLine::EnableSandbox() {
  command_line_.RemoveSwitch("no-sandbox");
  command_line_.AppendSwitch("enable-sandbox");
}

bool ElectronAppCommandLine::Call(const base::Value& request,
                                  base::Value* result,
                                  std::string* error) {
  const auto* args = request.GetIfDict();
  const auto* method = args ? args->FindString("method") : nullptr;
  const auto* name = args ? args->FindString("name") : nullptr;
  const auto* value = args ? args->FindString("value") : nullptr;
  if (!method || (*method == "appendArgument"
                      ? !value || value->find('\0') != std::string::npos
                      : !name || !ValidName(*name))) {
    *error = "ERR_INVALID_ARG_VALUE: invalid command line arguments";
    return false;
  }
  const auto key = name ? base::ToLowerASCII(*name) : std::string();
  if (*method == "hasSwitch") {
    *result = base::Value(command_line_.HasSwitch(key));
    return true;
  }
  if (*method == "getSwitchValue") {
    *result = base::Value(Utf8String(command_line_.GetSwitchValueNative(key)));
    return true;
  }
  auto next = command_line_;
  if ((*method == "appendSwitch" || *method == "removeSwitch") &&
      RequiresSharedHostStartup(key)) {
    *error =
        "ERR_NOT_SUPPORTED: this switch must be configured when the "
        "shared browser is launched: " +
        key;
    return false;
  }
  auto overrides = renderer_overrides_.Clone();
  if (*method == "appendSwitch") {
    if (!value || value->find('\0') != std::string::npos) {
      *error = "ERR_INVALID_ARG_VALUE: invalid command line switch value";
      return false;
    }
    next.RemoveSwitch(key);
    next.AppendSwitchNative(key, NativeString(*value));
    if (IsElectronRendererSwitch(key)) {
      overrides.Set(key, *value);
    }
  } else if (*method == "removeSwitch") {
    next.RemoveSwitch(key);
    if (IsElectronRendererSwitch(key)) {
      overrides.Set(key, base::Value());
    }
  } else if (*method == "appendArgument") {
    next.AppendArgNative(NativeString(*value));
  } else {
    *error = "ERR_NOT_SUPPORTED: unknown command line operation";
    return false;
  }
  const auto serialized = base::WriteJson(overrides);
  if (Utf8String(next.GetCommandLineString()).size() > kMaxCommandLineBytes ||
      !serialized || serialized->size() > kMaxRendererOverridesBytes) {
    *error = "ERR_OUT_OF_RANGE: application command line is too large";
    return false;
  }
  command_line_ = std::move(next);
  renderer_overrides_ = std::move(overrides);
  *result = base::Value();
  return true;
}

bool CallElectronAppCommandLine(const std::string& owner,
                                const base::Value& request,
                                base::Value* result,
                                std::string* error) {
  const auto entry =
      CommandLines()
          .try_emplace(owner, *base::CommandLine::ForCurrentProcess())
          .first;
  return entry->second.Call(request, result, error);
}

void CloseElectronAppCommandLine(const std::string& owner) {
  CommandLines().erase(owner);
}

void EnableSandboxInElectronAppCommandLine(const std::string& owner) {
  CommandLines()
      .try_emplace(owner, *base::CommandLine::ForCurrentProcess())
      .first->second.EnableSandbox();
}

void AppendElectronAppRendererCommandLine(const std::string& owner,
                                          base::CommandLine* command_line) {
  const auto it = CommandLines().find(owner);
  if (it != CommandLines().end()) {
    AppendElectronRendererOverrides(it->second.renderer_overrides(),
                                    command_line);
  }
}

}  // namespace xenon::ipc
