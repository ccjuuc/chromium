// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_shell.h"

#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_types.h"

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

void CallAutoUpdater(const base::DictValue& arguments,
                     const std::string& operation,
                     ElectronApiCallback callback) {
  auto* manager = xenon::updater::XenonUpdateManager::GetInstance();
  if (operation == "autoUpdater.setFeedURL") {
    const std::string* url = arguments.FindString("url");
    if (!url) {
      std::move(callback).Run(
          Failure("ERR_INVALID_ARG_VALUE: autoUpdater.setFeedURL requires url"));
      return;
    }
    manager->SetFeedURL(*url);
    std::move(callback).Run(Success());
    return;
  }
  if (operation == "autoUpdater.getFeedURL") {
    base::DictValue result;
    result.Set("url", manager->GetFeedURL());
    std::move(callback).Run(Success(base::Value(std::move(result))));
    return;
  }
  if (operation == "autoUpdater.checkForUpdates") {
    const std::string* url = arguments.FindString("url");
    manager->CheckForUpdates(url ? *url : "");
    base::DictValue result;
    result.Set("status", "checking");
    std::move(callback).Run(Success(base::Value(std::move(result))));
    return;
  }
  if (operation == "autoUpdater.downloadUpdate") {
    manager->DownloadUpdate();
    base::DictValue result;
    result.Set("status", "downloading");
    std::move(callback).Run(Success(base::Value(std::move(result))));
    return;
  }
  if (operation == "autoUpdater.quitAndInstall") {
    manager->QuitAndInstall();
    base::DictValue result;
    result.Set("status", "installing");
    std::move(callback).Run(Success(base::Value(std::move(result))));
    return;
  }
  if (operation == "autoUpdater.getState") {
    base::DictValue result;
    result.Set("state",
               xenon::updater::UpdateStateToString(manager->GetState()));
    if (manager->GetManifest()) {
      result.Set("manifest", manager->GetManifest()->ToValue().Clone());
    }
    std::move(callback).Run(Success(base::Value(std::move(result))));
    return;
  }
  std::move(callback).Run(
      Failure("ERR_NOT_SUPPORTED: unsupported autoUpdater operation " + operation));
}

void CallClipboard(const base::DictValue& arguments,
                   const std::string& operation,
                   ElectronApiCallback callback) {
  const base::Value* type = arguments.Find("type");
  if (type && !type->is_string()) {
    std::move(callback).Run(
        Failure("ERR_INVALID_ARG_TYPE: clipboard type must be a string"));
    return;
  }
  // This bridge currently exposes the system clipboard only. Never silently
  // redirect an explicitly requested selection buffer to the system clipboard.
  if (type && type->GetString() != "clipboard") {
    std::move(callback).Run(
        Failure("ERR_NOT_SUPPORTED: only the clipboard buffer is supported"));
    return;
  }
  constexpr auto buffer = ui::ClipboardBuffer::kCopyPaste;
  if (operation == "clipboard.readText") {
    ui::Clipboard::GetForCurrentThread()->ReadText(
        buffer, std::nullopt,
        base::BindOnce(
            [](ElectronApiCallback callback, std::u16string text) {
              std::move(callback).Run(
                  Success(base::Value(base::UTF16ToUTF8(text))));
            },
            std::move(callback)));
    return;
  }
  if (operation == "clipboard.readHTML") {
    ui::Clipboard::GetForCurrentThread()->ReadHTML(
        buffer, std::nullopt,
        base::BindOnce(
            [](ElectronApiCallback callback, std::u16string markup, GURL,
               uint32_t, uint32_t) {
              std::move(callback).Run(
                  Success(base::Value(base::UTF16ToUTF8(markup))));
            },
            std::move(callback)));
    return;
  }
  if (operation == "clipboard.clear") {
    ui::Clipboard::GetForCurrentThread()->Clear(buffer);
    std::move(callback).Run(Success());
    return;
  }
  if (operation == "clipboard.writeText" ||
      operation == "clipboard.writeHTML") {
    const bool html = operation == "clipboard.writeHTML";
    const std::string* value = arguments.FindString(html ? "markup" : "text");
    if (!value) {
      std::move(callback).Run(
          Failure("ERR_INVALID_ARG_TYPE: clipboard content must be a string"));
      return;
    }
    // The writer commits in its destructor, before reporting completion.
    {
      ui::ScopedClipboardWriter writer(buffer);
      if (html) {
        writer.WriteHTML(base::UTF8ToUTF16(*value), std::string());
      } else {
        writer.WriteText(base::UTF8ToUTF16(*value));
      }
    }
    std::move(callback).Run(Success());
    return;
  }
  std::move(callback).Run(
      Failure("ERR_NOT_SUPPORTED: unsupported Electron API " + operation));
}

}  // namespace

void CallElectronApi(base::Value arguments, ElectronApiCallback callback) {
  // Renderer IPC transports the arguments after the channel as a list, while
  // the main-process BrowserWindow host call passes the request directly.
  if (arguments.is_list()) {
    auto values = std::move(arguments).TakeList();
    if (values.size() != 1) {
      std::move(callback).Run(Failure(
          "ERR_INVALID_ARG_TYPE: Electron API expects one request object"));
      return;
    }
    arguments = std::move(values.front());
  }
  if (!arguments.is_dict()) {
    std::move(callback).Run(Failure(
        "ERR_INVALID_ARG_TYPE: Electron API request must be an object"));
    return;
  }
  const std::string* operation = arguments.GetDict().FindString("operation");
  if (!operation) {
    std::move(callback).Run(Failure(
        "ERR_INVALID_ARG_TYPE: Electron API operation must be a string"));
    return;
  }
  if (base::StartsWith(*operation, "clipboard.")) {
    CallClipboard(arguments.GetDict(), *operation, std::move(callback));
    return;
  }
  if (base::StartsWith(*operation, "autoUpdater.")) {
    CallAutoUpdater(arguments.GetDict(), *operation, std::move(callback));
    return;
  }
  if (*operation == "shell.openExternal" || *operation == "shell.openPath" ||
      *operation == "shell.showItemInFolder") {
    CallElectronShell(std::move(arguments).TakeDict(), std::move(callback));
    return;
  }
  std::move(callback).Run(
      Failure("ERR_NOT_SUPPORTED: unsupported Electron API " + *operation));
}

}  // namespace xenon::ipc
