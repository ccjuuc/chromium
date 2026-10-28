// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/common/ipc/xenon_privileged_schemes.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <string_view>
#include <utility>

#include "base/base64.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "xenon_overlay/public/xenon_ipc_switches.h"

namespace xenon::ipc {
namespace {

// Internal process transport, not an alternative source of browser policy.
constexpr char kSnapshotSwitch[] = "xenon-privileged-schemes-snapshot";
constexpr size_t kMaxSchemes = 32;
constexpr size_t kMaxSnapshotSize = 16 * 1024;

struct StartupState {
  bool initialized = false;
  PrivilegedSchemes schemes;
  std::string error;
  std::string serialized;
};

StartupState& State() {
  static base::NoDestructor<StartupState> state;
  return *state;
}

bool Fail(std::string* error, std::string message) {
  *error = std::move(message);
  return false;
}

bool IsReservedScheme(std::string_view scheme) {
  // Do not consult url::Get*Schemes here: reading the registry marks it used
  // and would prevent the startup Add*Scheme calls which follow this parser.
  constexpr auto kReserved = std::to_array<std::string_view>(
      {"about", "android-app",  "blob",          "content",
       "data",  "devtools",     "dom-distiller", "externalfile",
       "file",  "filesystem",   "ftp",           "http",
       "https", "isolated-app", "javascript",    "mailto",
       "tel",   "urn",          "view-source",   "ws",
       "wss"});
  return base::StartsWith(scheme, "chrome") ||
         std::ranges::contains(kReserved, scheme);
}

bool ParseList(const base::ListValue& list,
               PrivilegedSchemes* schemes,
               std::string* error) {
  if (list.size() > kMaxSchemes) {
    return Fail(error, "privilegedSchemes exceeds 32 schemes");
  }
  std::map<std::string, PrivilegedScheme> parsed;
  for (const auto& entry : list) {
    if (!entry.is_dict()) {
      return Fail(error, "privilegedSchemes entries must be objects");
    }
    const auto& dict = entry.GetDict();
    for (const auto [key, value] : dict) {
      if (key != "scheme" && key != "privileges") {
        return Fail(error, "Unknown privilegedSchemes entry field: " + key);
      }
    }
    const auto* name = dict.FindString("scheme");
    if (!name || name->empty() || name->size() > 64 ||
        !base::IsAsciiAlpha((*name)[0]) ||
        !std::ranges::all_of(*name, [](char c) {
          return base::IsAsciiAlpha(c) || base::IsAsciiDigit(c) || c == '+' ||
                 c == '-' || c == '.';
        })) {
      return Fail(error, "Expected a valid ASCII URL scheme without ':'");
    }
    PrivilegedScheme result;
    result.scheme = base::ToLowerASCII(*name);
    if (IsReservedScheme(result.scheme)) {
      return Fail(error, "Cannot register reserved scheme: " + result.scheme);
    }
    if (const auto* value = dict.Find("privileges")) {
      if (!value->is_dict()) {
        return Fail(error, "Scheme privileges must be an object");
      }
      for (const auto [key, privilege] : value->GetDict()) {
        if (!privilege.is_bool()) {
          return Fail(error, "Scheme privilege must be boolean: " + key);
        }
        const bool enabled = privilege.GetBool();
        if (key == "standard") {
          result.standard = enabled;
        } else if (key == "secure") {
          result.secure = enabled;
        } else if (key == "bypassCSP") {
          result.bypass_csp = enabled;
        } else if (key == "supportFetchAPI") {
          result.support_fetch_api = enabled;
        } else if (key == "corsEnabled") {
          result.cors_enabled = enabled;
        } else if (key == "allowServiceWorkers" || key == "stream") {
          if (enabled) {
            return Fail(error, "Unsupported scheme privilege: " + key);
          }
        } else {
          return Fail(error, "Unknown scheme privilege: " + key);
        }
      }
    }
    const auto [it, inserted] = parsed.emplace(result.scheme, result);
    if (!inserted && it->second != result) {
      return Fail(error,
                  "Conflicting declarations for scheme: " + result.scheme);
    }
  }
  PrivilegedSchemes output;
  for (auto& [name, scheme] : parsed) {
    output.push_back(std::move(scheme));
  }
  *schemes = std::move(output);
  error->clear();
  return true;
}

base::ListValue SerializeSchemes(const PrivilegedSchemes& schemes) {
  base::ListValue list;
  for (const auto& scheme : schemes) {
    list.Append(base::DictValue()
                    .Set("scheme", scheme.scheme)
                    .Set("privileges",
                         base::DictValue()
                             .Set("standard", scheme.standard)
                             .Set("secure", scheme.secure)
                             .Set("bypassCSP", scheme.bypass_csp)
                             .Set("supportFetchAPI", scheme.support_fetch_api)
                             .Set("corsEnabled", scheme.cors_enabled)));
  }
  return list;
}

bool ReadSnapshot(std::string_view json, StartupState* state) {
  if (json.size() > kMaxSnapshotSize) {
    return Fail(&state->error, "Startup scheme snapshot is too large");
  }
  auto value = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!value || !value->is_dict() || value->GetDict().size() != 2) {
    return Fail(&state->error, "Invalid startup scheme snapshot");
  }
  const auto* error = value->GetDict().FindString("error");
  const auto* schemes = value->GetDict().Find("schemes");
  if (!error || !schemes) {
    return Fail(&state->error, "Invalid startup scheme snapshot fields");
  }
  if (!error->empty()) {
    return Fail(&state->error, *error);
  }
  return ParsePrivilegedSchemes(*schemes, &state->schemes, &state->error);
}

}  // namespace

bool ParsePrivilegedSchemes(const base::Value& value,
                            PrivilegedSchemes* schemes,
                            std::string* error) {
  if (!schemes || !error) {
    return false;
  }
  if (!value.is_list()) {
    return Fail(error, "privilegedSchemes must be an array");
  }
  return ParseList(value.GetList(), schemes, error);
}

bool LoadPrivilegedSchemeDeclarations(
    const base::FilePath& host_directory,
    const base::FilePath& explicit_application,
    PrivilegedSchemes* schemes,
    std::string* error) {
  if (!schemes || !error) {
    return false;
  }
  std::set<base::FilePath> descriptors;
  if (!host_directory.empty()) {
    base::FileEnumerator files(host_directory, false,
                               base::FileEnumerator::FILES,
                               FILE_PATH_LITERAL("*.xenon.json"));
    for (auto path = files.Next(); !path.empty(); path = files.Next()) {
      descriptors.insert(std::move(path));
    }
  }
  if (explicit_application.MatchesExtension(FILE_PATH_LITERAL(".json"))) {
    if (!explicit_application.IsAbsolute()) {
      return Fail(error,
                  "Electron application/configuration path must be absolute");
    }
    descriptors.insert(explicit_application);
  }
  base::ListValue combined;
  PrivilegedSchemes output;
  for (const auto& path : descriptors) {
    std::string text;
    if (!base::ReadFileToStringWithMaxSize(path, &text, 64 * 1024)) {
      return Fail(error,
                  "Unable to read scheme descriptor: " + path.AsUTF8Unsafe());
    }
    auto value = base::JSONReader::Read(text, base::JSON_PARSE_RFC);
    if (!value || !value->is_dict()) {
      return Fail(error, "Scheme descriptor must be a JSON object: " +
                             path.AsUTF8Unsafe());
    }
    const auto* declaration = value->GetDict().Find("privilegedSchemes");
    if (!declaration) {
      continue;
    }
    PrivilegedSchemes parsed;
    if (!ParsePrivilegedSchemes(*declaration, &parsed, error)) {
      *error = path.AsUTF8Unsafe() + ": " + *error;
      return false;
    }
    for (auto& item : SerializeSchemes(parsed)) {
      combined.Append(std::move(item));
    }
    if (!ParseList(combined, &output, error)) {
      *error = path.AsUTF8Unsafe() + ": " + *error;
      return false;
    }
    combined = SerializeSchemes(output);
  }
  *schemes = std::move(output);
  error->clear();
  return true;
}

void InitializeStartupPrivilegedSchemes() {
  auto& state = State();
  if (state.initialized) {
    return;
  }
  state.initialized = true;
  const auto& command_line = *base::CommandLine::ForCurrentProcess();
  if (!command_line.GetSwitchValueASCII("type").empty()) {
    // A child without the snapshot has no custom privileges. Do not discover
    // files relative to a helper executable or a sandboxed working directory.
    if (command_line.HasSwitch(kSnapshotSwitch)) {
      std::string decoded;
      const auto encoded = command_line.GetSwitchValueASCII(kSnapshotSwitch);
      if (encoded.size() > 2 * kMaxSnapshotSize ||
          !base::Base64Decode(encoded, &decoded)) {
        state.error = "Invalid encoded startup scheme snapshot";
      } else {
        ReadSnapshot(decoded, &state);
      }
    }
  } else {
    base::FilePath host_directory;
    base::PathService::Get(base::DIR_EXE, &host_directory);
    // Match the existing host descriptor lookup on every platform. In a macOS
    // bundle this is Contents/MacOS; a descriptor in Resources can be selected
    // explicitly with --xenon-electron-app, just like any external descriptor.
    LoadPrivilegedSchemeDeclarations(
        host_directory, command_line.GetSwitchValuePath(switches::kElectronApp),
        &state.schemes, &state.error);
  }
  if (!state.error.empty()) {
    state.schemes.clear();
    LOG(ERROR) << "Unable to initialize Electron privileged schemes: "
               << state.error;
  }
  std::string json;
  base::JSONWriter::Write(base::DictValue()
                              .Set("schemes", SerializeSchemes(state.schemes))
                              .Set("error", state.error.substr(0, 2048)),
                          &json);
  state.serialized = base::Base64EncodeEarlyStartup(base::as_byte_span(json));
}

const PrivilegedSchemes& GetStartupPrivilegedSchemes() {
  return State().schemes;
}

bool ValidatePrivilegedSchemesAgainstStartup(const base::ListValue& schemes,
                                             std::string* error) {
  if (!error) {
    return false;
  }
  PrivilegedSchemes requested;
  if (!ParseList(schemes, &requested, error)) {
    return false;
  }
  const auto& state = State();
  if (!state.initialized) {
    return Fail(error, "Privileged scheme startup registration has not run");
  }
  if (!state.error.empty()) {
    return Fail(error, state.error);
  }
  for (const auto& scheme : requested) {
    const auto it = std::ranges::find(state.schemes, scheme.scheme,
                                      &PrivilegedScheme::scheme);
    if (it == state.schemes.end() || *it != scheme) {
      return Fail(error,
                  "Scheme privileges were not declared at browser startup: " +
                      scheme.scheme +
                      "; update privilegedSchemes and restart the host");
    }
  }
  error->clear();
  return true;
}

void AppendStartupPrivilegedSchemes(base::CommandLine* command_line) {
  const auto& state = State();
  if (state.initialized) {
    command_line->AppendSwitchASCII(kSnapshotSwitch, state.serialized);
  }
}

}  // namespace xenon::ipc
