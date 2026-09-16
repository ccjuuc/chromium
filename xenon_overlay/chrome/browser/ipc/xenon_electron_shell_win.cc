// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_shell.h"

#include <windows.h>

#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/single_thread_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/win/scoped_co_mem.h"
#include "url/gurl.h"

namespace xenon::ipc {
namespace {

mojom::IpcResultPtr Success(base::Value value = base::Value()) {
  auto result = mojom::IpcResult::New();
  result->success = true;
  result->value = std::move(value);
  return result;
}

mojom::IpcResultPtr Failure(std::string error) {
  auto result = mojom::IpcResult::New();
  result->success = false;
  result->error = std::move(error);
  return result;
}

bool HasEmbeddedNul(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

std::string WindowsError(const std::string& operation, DWORD code) {
  return operation + " failed (Windows error " + base::NumberToString(code) +
         ")";
}

mojom::IpcResultPtr CallShell(base::DictValue arguments) {
  base::ScopedBlockingCall blocking(FROM_HERE, base::BlockingType::MAY_BLOCK);
  const std::string& operation = *arguments.FindString("operation");
  const bool external = operation == "shell.openExternal";
  const bool open_path = operation == "shell.openPath";
  const std::string* input = arguments.FindString(external ? "url" : "path");
  if (!input) {
    return Failure("ERR_INVALID_ARG_TYPE: shell target must be a string");
  }
  if (input->empty() || HasEmbeddedNul(*input)) {
    return Failure(
        "ERR_INVALID_ARG_VALUE: shell target is empty or contains NUL");
  }

  std::wstring target;
  base::FilePath path;
  if (external) {
    const GURL url(*input);
    if (!url.is_valid() || !url.has_scheme()) {
      return Failure(
          "ERR_INVALID_ARG_VALUE: shell.openExternal requires a valid URL");
    }
    // Match Chromium's platform launcher: quote the canonical URL, preventing
    // spaces from being interpreted as additional handler arguments.
    target = L"\"" + base::UTF8ToWide(url.spec()) + L"\"";
  } else {
    path = base::MakeAbsoluteFilePath(base::FilePath::FromUTF8Unsafe(*input));
    if (path.empty() || !base::PathExists(path)) {
      const std::string error = "Path does not exist";
      return open_path ? Success(base::Value(error))
                       : Failure("ENOENT: " + error);
    }
    target = path.value();
  }

  if (operation == "shell.showItemInFolder") {
    base::win::ScopedCoMem<ITEMIDLIST> folder;
    base::win::ScopedCoMem<ITEMIDLIST> item;
    HRESULT hr = SHParseDisplayName(
        path.DirName().AsEndingWithSeparator().value().c_str(), nullptr,
        &folder, 0, nullptr);
    if (SUCCEEDED(hr)) {
      hr = SHParseDisplayName(path.value().c_str(), nullptr, &item, 0, nullptr);
    }
    if (SUCCEEDED(hr)) {
      PCUITEMID_CHILD child = ILFindLastID(item.get());
      hr = SHOpenFolderAndSelectItems(folder.get(), 1, &child, 0);
    }
    return SUCCEEDED(hr)
               ? Success()
               : Failure("ERR_SHELL_OPERATION_FAILED: " +
                         WindowsError(operation, static_cast<DWORD>(hr)));
  }

  base::FilePath working_directory;
  if (!base::PathService::Get(base::DIR_SYSTEM, &working_directory)) {
    return Failure(
        "ERR_SHELL_OPERATION_FAILED: cannot resolve system directory");
  }
  bool activate = true;
  bool log_usage = false;
  if (const base::Value* options = arguments.Find("options")) {
    if (!options->is_dict()) {
      return Failure("ERR_INVALID_ARG_TYPE: shell options must be an object");
    }
    for (const auto [key, value] : options->GetDict()) {
      if (key == "activate" || key == "logUsage") {
        if (!value.is_bool()) {
          return Failure("ERR_INVALID_ARG_TYPE: shell " + key +
                         " must be a boolean");
        }
        (key == "activate" ? activate : log_usage) = value.GetBool();
      } else if (key == "workingDirectory") {
        if (!value.is_string()) {
          return Failure(
              "ERR_INVALID_ARG_TYPE: workingDirectory must be a string");
        }
        if (value.GetString().empty() || HasEmbeddedNul(value.GetString())) {
          return Failure("ERR_INVALID_ARG_VALUE: invalid workingDirectory");
        }
        working_directory = base::MakeAbsoluteFilePath(
            base::FilePath::FromUTF8Unsafe(value.GetString()));
        if (working_directory.empty() ||
            !base::DirectoryExists(working_directory)) {
          return Failure(
              "ERR_INVALID_ARG_VALUE: workingDirectory does not exist");
        }
      } else {
        return Failure("ERR_NOT_SUPPORTED: unsupported shell option " + key);
      }
    }
  }
  SHELLEXECUTEINFOW info = {sizeof(info)};
  info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_NOASYNC;
  if (log_usage) {
    info.fMask |= SEE_MASK_FLAG_LOG_USAGE;
  }
  info.lpVerb = L"open";
  info.lpFile = target.c_str();
  info.lpDirectory = working_directory.value().c_str();
  info.nShow = activate ? SW_SHOWNORMAL : SW_SHOWNOACTIVATE;
  if (!ShellExecuteExW(&info)) {
    const std::string error = WindowsError(operation, GetLastError());
    return open_path ? Success(base::Value(error))
                     : Failure("ERR_SHELL_OPERATION_FAILED: " + error);
  }
  return open_path ? Success(base::Value(std::string())) : Success();
}

}  // namespace

void CallElectronShell(base::DictValue arguments,
                       ElectronApiCallback callback) {
  base::ThreadPool::CreateCOMSTATaskRunner(
      {base::MayBlock(), base::TaskPriority::USER_VISIBLE})
      ->PostTaskAndReplyWithResult(
          FROM_HERE, base::BindOnce(&CallShell, std::move(arguments)),
          std::move(callback));
}

}  // namespace xenon::ipc
