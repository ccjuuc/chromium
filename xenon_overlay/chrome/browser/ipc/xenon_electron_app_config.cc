// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_config.h"

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/json/json_reader.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/unguessable_token.h"
#include "chrome/common/chrome_paths.h"
#include "components/crash/core/app/crashpad.h"
#include "crypto/hash.h"
#include "net/base/filename_util.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_app_runtime.h"
#include "xenon_overlay/common/asar/archive.h"
#include "xenon_overlay/common/ipc/xenon_privileged_schemes.h"

namespace xenon::ipc {
namespace {

bool AppRelativePath(const base::FilePath& root,
                     const std::string& relative,
                     base::FilePath* path,
                     std::string* error) {
  const auto suffix = base::FilePath::FromUTF8Unsafe(relative);
  if (suffix.empty() || suffix.IsAbsolute() || suffix.ReferencesParent()) {
    *error = "Expected a path inside the application: " + relative;
    return false;
  }
  *path = root.Append(suffix).NormalizePathSeparators();
  return true;
}

}  // namespace

PreparedElectronAppConfig::PreparedElectronAppConfig(
    mojom::IpcMainConfigPtr config,
    std::unique_ptr<asar::PreparedArchivePublicKeys> archive_keys)
    : config_(std::move(config)), archive_keys_(std::move(archive_keys)) {}

PreparedElectronAppConfig::~PreparedElectronAppConfig() = default;

base::expected<std::unique_ptr<PreparedElectronAppConfig>, std::string>
PrepareElectronAppConfig(const base::FilePath& path,
                         const std::string& container_id) {
  mojom::IpcMainConfigPtr config;
  std::string error;
  if (!LoadElectronAppConfig(path, container_id, &config, &error)) {
    return base::unexpected(std::move(error));
  }
  if (config->app_name.empty()) {
    config->app_name = "Application";
  }
  if (config->app_version.empty()) {
    config->app_version = GetAppExecutableVersion(
        base::FilePath::FromUTF8Unsafe(config->executable_path));
    if (config->app_version.empty()) {
      config->app_version = "0.0.0";
    }
  }
  base::FilePath host_user_data;
  if (!base::PathService::Get(chrome::DIR_USER_DATA, &host_user_data) ||
      !host_user_data.IsAbsolute()) {
    return base::unexpected(
        "Cannot prepare Electron app without host user data path");
  }
  config->host_user_data_directory = host_user_data.AsUTF8Unsafe();
  // Crashpad initializes this immutable path before application launches. The
  // getter only copies that startup state and does not require the UI sequence.
  const auto crash_dumps = crash_reporter::GetCrashpadDatabasePath();
  config->host_crash_dumps_directory =
      crash_dumps ? crash_dumps->AsUTF8Unsafe() : std::string();

  std::vector<asar::ArchiveKeyConfig> keys;
  keys.reserve(config->archive_public_keys.size());
  for (const auto& key : config->archive_public_keys) {
    keys.push_back(
        {base::FilePath::FromUTF8Unsafe(key->root_path), key->public_key_pem});
  }
  auto archive_keys = asar::PrepareArchivePublicKeys(keys);
  if (!archive_keys) {
    return base::unexpected("Invalid ASAR public key configuration");
  }
  return std::unique_ptr<PreparedElectronAppConfig>(
      new PreparedElectronAppConfig(
          std::move(config), std::make_unique<asar::PreparedArchivePublicKeys>(
                                 std::move(*archive_keys))));
}

bool LoadElectronAppConfig(const base::FilePath& path,
                           const std::string& container_id,
                           mojom::IpcMainConfigPtr* config,
                           std::string* error) {
  if (!config || !error) {
    return false;
  }
  if (!path.IsAbsolute()) {
    *error = "Electron application/configuration path must be absolute";
    return false;
  }
  base::DictValue descriptor;
  base::FilePath application = path;
  if (path.MatchesExtension(FILE_PATH_LITERAL(".json"))) {
    std::string text;
    if (!base::ReadFileToStringWithMaxSize(path, &text, 64 * 1024)) {
      *error =
          "Unable to read Electron host configuration: " + path.AsUTF8Unsafe();
      return false;
    }
    auto json = base::JSONReader::Read(text, base::JSON_PARSE_RFC);
    if (!json || !json->is_dict()) {
      *error = "Electron host configuration must be a JSON object";
      return false;
    }
    descriptor = std::move(json->GetDict());
    constexpr auto kStrings = std::to_array<std::string_view>(
        {"application", "executable", "main", "name", "version",
         "archivePublicKey"});
    for (const auto [key, value] : descriptor) {
      if (std::ranges::contains(kStrings, key)) {
        if (value.is_string() && !value.GetString().empty()) {
          continue;
        }
      } else if (key == "versionFromExecutable" && value.is_bool()) {
        continue;
      } else if (key == "nativeAddonExecution" && value.is_string() &&
                 (value.GetString() == "standard" ||
                  value.GetString() == "shared")) {
        continue;
      } else if (key == "userDataPolicy" && value.is_string() &&
                 (value.GetString() == "isolated" ||
                  value.GetString() == "legacy-host")) {
        continue;
      } else if (key == "storageId" && value.is_string() &&
                 !value.GetString().empty() &&
                 value.GetString().size() <= 256 &&
                 value.GetString().find('\0') == std::string::npos) {
        continue;
      } else if (key == "parentWindowPairing" && value.is_list()) {
        continue;
      } else if (key == "privilegedSchemes") {
        PrivilegedSchemes parsed;
        if (!ParsePrivilegedSchemes(value, &parsed, error)) {
          return false;
        }
        continue;
      }
      *error = "Invalid Electron host configuration field: " + key;
      return false;
    }
    const auto* app = descriptor.FindString("application");
    if (!app) {
      *error = "Electron host configuration requires application";
      return false;
    }
    application = base::FilePath::FromUTF8Unsafe(*app);
    if (!application.IsAbsolute()) {
      application = path.DirName().Append(application);
    }
  }

  base::FilePath normalized_application;
  if (!base::NormalizeFilePath(base::MakeAbsoluteFilePath(application),
                               &normalized_application)) {
    *error =
        "Electron application does not exist: " + application.AsUTF8Unsafe();
    return false;
  }
  application = std::move(normalized_application);
  auto result = mojom::IpcMainConfig::New();
  result->container_id = container_id;
  if (const auto* storage_id = descriptor.FindString("storageId")) {
    result->storage_id = *storage_id;
  }
  if (const auto* policy = descriptor.FindString("userDataPolicy");
      policy && *policy == "legacy-host") {
    result->user_data_policy = mojom::UserDataPolicy::kLegacyHost;
  }
  // Descriptor-relative paths are resolved against the path the caller
  // selected. Preserve it for relaunch: resolving a descriptor symlink here
  // would change that base directory on the next launch.
  result->launch_path =
      (path.MatchesExtension(FILE_PATH_LITERAL(".json"))
           ? path.NormalizePathSeparators().StripTrailingSeparators()
           : application)
          .AsUTF8Unsafe();
  if (descriptor.FindString("nativeAddonExecution") &&
      *descriptor.FindString("nativeAddonExecution") == "shared") {
    result->native_addon_execution = mojom::NativeAddonExecution::kShared;
  }
  base::ScopedClosureRunner remove_keys;
  if (const auto* key_path = descriptor.FindString("archivePublicKey")) {
    auto public_key_path = base::FilePath::FromUTF8Unsafe(*key_path);
    if (!public_key_path.IsAbsolute()) {
      public_key_path = path.DirName().Append(public_key_path);
    }
    public_key_path = base::MakeAbsoluteFilePath(public_key_path);
    std::string public_key;
    if (!base::ReadFileToStringWithMaxSize(public_key_path, &public_key,
                                           64 * 1024)) {
      *error = "Unable to read configured ASAR public key";
      return false;
    }
    const auto& key_root = application;
    const std::string owner =
        "app-config:" + base::UnguessableToken::Create().ToString();
    if (!asar::SetArchivePublicKeys(owner, {{key_root, public_key}})) {
      *error = "Invalid or conflicting ASAR public key";
      return false;
    }
    remove_keys.ReplaceClosure(
        base::BindOnce(&asar::RemoveArchivePublicKeys, owner));
    result->archive_public_keys.push_back(mojom::IpcArchivePublicKey::New(
        key_root.AsUTF8Unsafe(), std::move(public_key)));
  }

  ElectronApplicationInfo app;
  if (!ResolveElectronApplication(application, &app, error)) {
    return false;
  }
  result->app_path = app.app_path.AsUTF8Unsafe();
  result->app_name = app.name;
  result->app_version = app.version;
  result->runtime_directory = app.runtime_directory.AsUTF8Unsafe();
  result->working_directory = result->runtime_directory;
  result->resources_directory = app.resources_directory.AsUTF8Unsafe();
  result->is_packaged = app.is_packaged;
  if (const auto* name = descriptor.FindString("name")) {
    result->app_name = *name;
  }
  if (const auto* version = descriptor.FindString("version")) {
    result->app_version = *version;
  }
  if (const auto* main = descriptor.FindString("main")) {
    base::FilePath entry;
    if (!AppRelativePath(app.app_path, *main, &entry, error)) {
      return false;
    }
    result->main_script_path = entry.AsUTF8Unsafe();
  }
  base::FilePath executable;
  if (const auto* value = descriptor.FindString("executable")) {
    executable = base::FilePath::FromUTF8Unsafe(*value);
    if (!executable.IsAbsolute()) {
      executable = path.DirName().Append(executable);
    }
    executable = base::MakeAbsoluteFilePath(executable);
    if (executable.empty()) {
      *error = "Unable to resolve configured executable";
      return false;
    }
  }
  if (!ResolveAppExecutable(executable, &executable, error)) {
    return false;
  }
  result->executable_path = executable.AsUTF8Unsafe();
  if (descriptor.FindBool("versionFromExecutable").value_or(false)) {
    const auto version = GetAppExecutableVersion(executable);
    if (version.empty()) {
      *error = "Configured executable has no readable application version";
      return false;
    }
    result->app_version = version;
  }
  if (const auto* pages = descriptor.FindList("parentWindowPairing")) {
    for (const auto& page : *pages) {
      base::FilePath document;
      if (!page.is_string() ||
          !AppRelativePath(app.app_path, page.GetString(), &document, error)) {
        *error = "parentWindowPairing requires application-relative page paths";
        return false;
      }
      result->parent_window_pairing_urls.push_back(
          net::FilePathToFileURL(document).spec());
    }
  }
  *config = std::move(result);
  return true;
}

base::expected<base::FilePath, std::string>
ResolveElectronAppStorageUserDataPath(const mojom::IpcMainConfig& config) {
  if (config.host_user_data_directory.find('\0') != std::string::npos ||
      config.storage_id.size() > 256 ||
      config.storage_id.find('\0') != std::string::npos) {
    return base::unexpected(
        "ERR_INVALID_ARG_VALUE: invalid application storage configuration");
  }
  const auto host =
      base::FilePath::FromUTF8Unsafe(config.host_user_data_directory)
          .NormalizePathSeparators()
          .StripTrailingSeparators();
  if (!host.IsAbsolute()) {
    return base::unexpected(
        "ERR_INVALID_ARG_VALUE: host userData must be absolute");
  }
  switch (config.user_data_policy) {
    case mojom::UserDataPolicy::kLegacyHost:
      return host;
    case mojom::UserDataPolicy::kIsolated:
      break;
    default:
      return base::unexpected("ERR_INVALID_ARG_VALUE: unknown userData policy");
  }
  std::string identity;
  if (!config.storage_id.empty()) {
    identity = "storage:" + config.storage_id;
  } else if (!config.container_id.empty() && config.container_id != "default") {
    identity = "container:" + config.container_id;
  } else if (!config.app_path.empty()) {
    if (config.app_path.find('\0') != std::string::npos) {
      return base::unexpected(
          "ERR_INVALID_ARG_VALUE: application path must not contain NUL");
    }
    const auto app = base::FilePath::FromUTF8Unsafe(config.app_path);
    if (!app.IsAbsolute()) {
      return base::unexpected(
          "ERR_INVALID_ARG_VALUE: application storage requires an absolute "
          "installation path");
    }
    identity =
        "installation:" +
        app.NormalizePathSeparators().StripTrailingSeparators().AsUTF8Unsafe();
  } else {
    // The command-line/embedded diagnostic container has no installation.
    identity = "container:default";
  }
  return host.AppendASCII("ElectronApps")
      .AppendASCII(base::HexEncodeLower(crypto::hash::Sha256(identity)))
      .AppendASCII("UserData");
}

}  // namespace xenon::ipc
