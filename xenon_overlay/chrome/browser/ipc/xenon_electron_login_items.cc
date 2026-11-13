// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_login_items.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "build/build_config.h"
#include "crypto/hash.h"

#if BUILDFLAG(IS_WIN)
#include "base/win/registry.h"
#endif

namespace xenon::ipc {
namespace {
using Result = std::pair<base::Value, std::string>;
Result Failure(std::string error) {
  return {base::Value(), std::move(error)};
}

struct Options {
  bool set = false;
  bool open_at_login = false;
  bool enabled = true;
  std::vector<std::string> args;
};

std::string ParseOptions(const base::Value& arguments, Options* output) {
  if (!arguments.is_dict()) {
    return "ERR_INVALID_ARG_TYPE: login item request must be an object";
  }
  const auto* method = arguments.GetDict().FindString("method");
  if (!method || (*method != "get" && *method != "set")) {
    return "ERR_INVALID_ARG_VALUE: login item method must be get or set";
  }
  output->set = *method == "set";
  const auto* value = arguments.GetDict().Find("options");
  if (!value) {
    return output->set
               ? "ERR_INVALID_ARG_TYPE: login item settings must be an object"
               : std::string();
  }
  if (!value->is_dict()) {
    return "ERR_INVALID_ARG_TYPE: login item options must be an object";
  }
  for (const auto [name, option] : value->GetDict()) {
    if (name == "path" || name == "name" || name == "type" ||
        name == "serviceName") {
      return "ERR_NOT_SUPPORTED: hosted login item cannot replace " + name;
    }
    if (name == "args") {
      if (!option.is_list()) {
        return "ERR_INVALID_ARG_TYPE: login item args must be an array";
      }
      for (const auto& arg : option.GetList()) {
        if (!arg.is_string() ||
            arg.GetString().find('\0') != std::string::npos) {
          return "ERR_INVALID_ARG_TYPE: login item args must be NUL-free "
                 "strings";
        }
        output->args.push_back(arg.GetString());
      }
    } else if (name == "openAtLogin" || name == "enabled" ||
               name == "openAsHidden") {
      if (!output->set) {
        return "ERR_INVALID_ARG_VALUE: unexpected login item query option " +
               name;
      }
      if (!option.is_bool()) {
        return "ERR_INVALID_ARG_TYPE: login item " + name + " must be boolean";
      }
      if (name == "openAtLogin") {
        output->open_at_login = option.GetBool();
      } else if (name == "enabled") {
        output->enabled = option.GetBool();
      } else if (option.GetBool()) {
        return "ERR_NOT_SUPPORTED: hidden login item launch is unavailable";
      }
    } else {
      return "ERR_NOT_SUPPORTED: unsupported login item option " + name;
    }
  }
  return {};
}

#if BUILDFLAG(IS_WIN)
constexpr wchar_t kRunKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\"
    L"Run";

std::wstring ItemName(const ElectronLoginItemContext& context) {
  // NormalizeFilePath below resolves junctions and final on-disk spelling;
  // folding ASCII also makes drive-letter case immaterial.
  std::string identity =
      base::ToLowerASCII(context.application_path.AsUTF8Unsafe());
  identity.push_back('\0');
  identity.append(
      base::ToLowerASCII(context.user_data_directory.AsUTF8Unsafe()));
  return L"XenonElectron-" + base::ASCIIToWide(base::HexEncodeLower(
                                 crypto::hash::Sha256(identity)));
}

Result RegistryFailure(LONG error) {
  return Failure("ERR_SYSTEM_ERROR: login item registry operation failed (" +
                 base::NumberToString(error) + ")");
}

LONG DeleteOwnedValue(const wchar_t* key_name, const std::wstring& item) {
  base::win::RegKey key;
  LONG status = key.Open(HKEY_CURRENT_USER, key_name, KEY_SET_VALUE);
  if (status == ERROR_FILE_NOT_FOUND) {
    return ERROR_SUCCESS;
  }
  if (status == ERROR_SUCCESS) {
    status = key.DeleteValue(item.c_str());
  }
  return status == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : status;
}

Result RunWindows(const ElectronLoginItemContext& context,
                  const Options& options) {
  const std::wstring item = ItemName(context);
  base::CommandLine command(context.executable_path);
  command.AppendSwitchPath("user-data-dir", context.user_data_directory);
  command.AppendSwitchPath("xenon-electron-app", context.launch_path);
  if (!options.args.empty()) {
    // Never permit app arguments to override the Browser-owned launch target.
    command.AppendArg("--");
    for (const auto& arg : options.args) {
      command.AppendArg(arg);
    }
  }
  const std::wstring expected = command.GetCommandLineString();
  // Windows Run values have a documented maximum of 260 characters.
  if (options.set && options.open_at_login && expected.size() > 260) {
    return Failure(
        "ERR_NOT_SUPPORTED: login item command exceeds Windows Run limit");
  }
  if (options.set) {
    if (!options.open_at_login) {
      LONG status = DeleteOwnedValue(kRunKey, item);
      if (status != ERROR_SUCCESS) {
        return RegistryFailure(status);
      }
      status = DeleteOwnedValue(kApprovedKey, item);
      return status == ERROR_SUCCESS ? Result{base::Value(), {}}
                                     : RegistryFailure(status);
    }
    base::win::RegKey key;
    LONG status = key.Create(HKEY_CURRENT_USER, kRunKey, KEY_SET_VALUE);
    if (status == ERROR_SUCCESS) {
      status = key.WriteValue(item.c_str(), expected.c_str());
    }
    if (status != ERROR_SUCCESS) {
      return RegistryFailure(status);
    }
    if (options.enabled) {
      status = DeleteOwnedValue(kApprovedKey, item);
    } else {
      base::win::RegKey approved;
      status = approved.Create(HKEY_CURRENT_USER, kApprovedKey, KEY_SET_VALUE);
      if (status == ERROR_SUCCESS) {
        const std::array<uint8_t, 12> disabled{3};
        status = approved.WriteValue(item.c_str(), disabled.data(),
                                     disabled.size(), REG_BINARY);
      }
    }
    return status == ERROR_SUCCESS ? Result{base::Value(), {}}
                                   : RegistryFailure(status);
  }

  base::win::RegKey run;
  std::wstring stored;
  LONG status = run.Open(HKEY_CURRENT_USER, kRunKey, KEY_QUERY_VALUE);
  if (status == ERROR_SUCCESS) {
    status = run.ReadValue(item.c_str(), &stored);
  }
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    return RegistryFailure(status);
  }
  const auto stored_command = base::CommandLine::FromString(stored);
  const auto same_path = [](const base::FilePath& left,
                            const base::FilePath& right) {
    return base::FilePath::CompareEqualIgnoreCase(left.value(), right.value());
  };
  const bool owned =
      status == ERROR_SUCCESS &&
      same_path(stored_command.GetProgram(), context.executable_path) &&
      same_path(stored_command.GetSwitchValuePath("user-data-dir"),
                context.user_data_directory) &&
      same_path(stored_command.GetSwitchValuePath("xenon-electron-app"),
                context.launch_path) &&
      stored_command.GetSwitches().size() == 2;
  const bool registered =
      owned && stored_command.GetArgs() == command.GetArgs();
  bool enabled = true;
  base::win::RegKey approved;
  status = approved.Open(HKEY_CURRENT_USER, kApprovedKey, KEY_QUERY_VALUE);
  if (status == ERROR_SUCCESS) {
    std::array<uint8_t, 12> bytes{};
    DWORD size = bytes.size(), type = 0;
    status = approved.ReadValue(item.c_str(), bytes.data(), &size, &type);
    if (status == ERROR_SUCCESS) {
      if (type != REG_BINARY || size != bytes.size()) {
        return Failure(
            "ERR_INVALID_STATE: malformed login item approval value");
      }
      // Windows uses 2/6 for enabled entries and 3/7 for disabled entries.
      if (bytes[0] != 0 && bytes[0] != 2 && bytes[0] != 3 && bytes[0] != 6 &&
          bytes[0] != 7) {
        return Failure("ERR_INVALID_STATE: unknown login item approval state");
      }
      enabled = bytes[0] == 0 || bytes[0] == 2 || bytes[0] == 6;
    }
  }
  if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND) {
    return RegistryFailure(status);
  }
  base::ListValue launch_items;
  if (owned) {
    base::ListValue args;
    // The actual executable is the shared host, so callers need its routing
    // switches and terminator as well as the application arguments to describe
    // the registered launch faithfully.
    for (size_t i = 1; i < stored_command.argv().size(); ++i) {
      args.Append(base::WideToUTF8(stored_command.argv()[i]));
    }
    launch_items.Append(base::DictValue()
                            .Set("name", base::WideToUTF8(item))
                            .Set("path", context.executable_path.AsUTF8Unsafe())
                            .Set("args", std::move(args))
                            .Set("scope", "user")
                            .Set("enabled", enabled));
  }
  return {base::Value(base::DictValue()
                          .Set("openAtLogin", registered)
                          .Set("executableWillLaunchAtLogin", owned && enabled)
                          .Set("launchItems", std::move(launch_items))),
          {}};
}
#endif

Result Run(ElectronLoginItemContext context, base::Value arguments) {
  Options options;
  if (auto error = ParseOptions(arguments, &options); !error.empty()) {
    return Failure(std::move(error));
  }
  if (!context.application_path.IsAbsolute() ||
      !context.launch_path.IsAbsolute() ||
      !context.executable_path.IsAbsolute() ||
      !context.user_data_directory.IsAbsolute()) {
    return Failure(
        "ERR_NOT_SUPPORTED: login item requires a restartable hosted "
        "application");
  }
#if BUILDFLAG(IS_WIN)
  base::FilePath application_path, user_data_directory;
  if (!base::NormalizeFilePath(context.application_path, &application_path) ||
      !base::NormalizeFilePath(context.user_data_directory,
                               &user_data_directory)) {
    return Failure(
        "ERR_SYSTEM_ERROR: unable to resolve login item application or profile "
        "identity");
  }
  context.application_path = std::move(application_path);
  context.user_data_directory = std::move(user_data_directory);
  return RunWindows(context, options);
#else
  return Failure(
      "ERR_NOT_SUPPORTED: hosted application login items are unavailable on "
      "this platform");
#endif
}
}  // namespace

void CallElectronLoginItemSettings(ElectronLoginItemContext context,
                                   base::Value arguments,
                                   ElectronLoginItemReply reply) {
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&Run, std::move(context), std::move(arguments)),
      base::BindOnce(
          [](ElectronLoginItemReply reply, Result result) {
            std::move(reply).Run(std::move(result.first),
                                 std::move(result.second));
          },
          std::move(reply)));
}
}  // namespace xenon::ipc
