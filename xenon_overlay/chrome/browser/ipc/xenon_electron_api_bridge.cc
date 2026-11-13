// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/callback_list.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "build/build_config.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkColor.h"
#include "ui/base/clipboard/clipboard.h"
#include "ui/base/clipboard/clipboard_constants.h"
#include "ui/base/clipboard/scoped_clipboard_writer.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/native_theme/os_settings_provider.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_crash_reporter_bridge.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_shell.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_native_image_bridge.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_types.h"

#if BUILDFLAG(IS_MAC)
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_app_mac.h"
#endif

#if BUILDFLAG(IS_WIN)
#include <windows.h>
#include <dwmapi.h>
#endif

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

base::DictValue ReadSystemTheme() {
  const auto& provider = ui::OsSettingsProvider::Get();
  base::DictValue state;
  state.Set("shouldUseDarkColors",
            provider.PreferredColorScheme() ==
                ui::NativeTheme::PreferredColorScheme::kDark);
  state.Set("shouldUseHighContrastColors", provider.ForcedColorsActive());
  state.Set("shouldUseInvertedColorScheme", provider.PrefersInvertedColors());
  state.Set("prefersReducedTransparency", provider.PrefersReducedTransparency());
  if (const auto accent = provider.AccentColor()) {
    state.Set("accentColor",
              base::StringPrintf("%02x%02x%02x%02x", SkColorGetR(*accent),
                                 SkColorGetG(*accent), SkColorGetB(*accent),
                                 SkColorGetA(*accent)));
  }
  return state;
}

class ElectronThemeEventBridge {
 public:
  static ElectronThemeEventBridge& Get() {
    static base::NoDestructor<ElectronThemeEventBridge> instance;
    return *instance;
  }

  void SetCallback(ElectronThemeEventCallback callback) {
    subscription_ = {};
    callback_ = std::move(callback);
    if (!callback_) {
      state_.clear();
      return;
    }
    state_ = ReadSystemTheme();
    subscription_ = ui::OsSettingsProvider::RegisterOsSettingsChangedCallback(
        base::BindRepeating(&ElectronThemeEventBridge::OnSettingsChanged,
                            base::Unretained(this)));
  }

 private:
  void OnSettingsChanged(bool force_notify) {
    auto state = ReadSystemTheme();
    const bool accent_changed = state.Find("accentColor") != nullptr &&
        (!state_.Find("accentColor") ||
         *state.Find("accentColor") != *state_.Find("accentColor"));
    auto old_native = state_.Clone();
    auto new_native = state.Clone();
    old_native.Remove("accentColor");
    new_native.Remove("accentColor");
    const bool native_changed = force_notify || old_native != new_native;
    state_ = state.Clone();
    if (accent_changed || native_changed) {
      callback_.Run(base::DictValue()
                        .Set("state", std::move(state))
                        .Set("accentChanged", accent_changed)
                        .Set("nativeThemeChanged", native_changed));
    }
  }

  ElectronThemeEventCallback callback_;
  base::CallbackListSubscription subscription_;
  base::DictValue state_;
};

class AutoUpdaterEventBridge : public updater::XenonUpdateManager::Observer {
 public:
  AutoUpdaterEventBridge() {
    updater::XenonUpdateManager::GetInstance()->AddObserver(this);
  }

  static AutoUpdaterEventBridge& Get() {
    static base::NoDestructor<AutoUpdaterEventBridge> instance;
    return *instance;
  }

  void SetCallback(AutoUpdaterEventCallback callback) {
    callback_ = std::move(callback);
  }

  void OnCheckingForUpdate() override { Emit("checking-for-update"); }
  void OnUpdateAvailable(const updater::UpdateManifest& manifest) override {
    Emit("update-available", base::ListValue().Append(manifest.ToValue()));
  }
  void OnUpdateNotAvailable(const std::string& version) override {
    Emit("update-not-available",
         base::ListValue().Append(base::DictValue().Set("version", version)));
  }
  void OnDownloadProgress(const updater::DownloadProgress& progress) override {
    Emit("download-progress", base::ListValue().Append(progress.ToValue()));
  }
  void OnUpdateDownloaded(const updater::UpdateManifest& manifest) override {
    Emit("update-downloaded", base::ListValue().Append(manifest.ToValue()));
  }
  void OnUpdateError(const std::string& message) override {
    Emit("error", base::ListValue().Append(message));
  }

 private:
  void Emit(const std::string& name, base::ListValue arguments = {}) {
    if (callback_) {
      callback_.Run(name, std::move(arguments));
    }
  }

  AutoUpdaterEventCallback callback_;
};

void CallAutoUpdater(const base::DictValue& arguments,
                     const std::string& operation,
                     ElectronApiCallback callback) {
  auto* manager = xenon::updater::XenonUpdateManager::GetInstance();
  if (operation == "autoUpdater.configure" ||
      operation == "autoUpdater.getConfiguration") {
    if (operation == "autoUpdater.configure") {
      for (const char* key : {"autoDownload", "autoInstallOnAppQuit"}) {
        if (const base::Value* value = arguments.Find(key);
            value && !value->is_bool()) {
          std::move(callback).Run(
              Failure(std::string("ERR_INVALID_ARG_TYPE: ") + key +
                      " must be a boolean"));
          return;
        }
      }
      if (auto value = arguments.FindBool("autoDownload")) {
        manager->SetAutoDownload(*value);
      }
      if (auto value = arguments.FindBool("autoInstallOnAppQuit")) {
        manager->SetAutoInstallOnAppQuit(*value);
      }
    }
    std::move(callback).Run(Success(base::Value(
        base::DictValue()
            .Set("autoDownload", manager->GetAutoDownload())
            .Set("autoInstallOnAppQuit", manager->GetAutoInstallOnAppQuit()))));
    return;
  }
  if (operation == "autoUpdater.setFeedURL") {
    const std::string* url = arguments.FindString("url");
    if (!url) {
      std::move(callback).Run(Failure(
          "ERR_INVALID_ARG_VALUE: autoUpdater.setFeedURL requires url"));
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
    // This acknowledges only the request. Preparation, OS launch approval and
    // rollback finish asynchronously; it is not an installation success.
    result.Set("status", "requested");
    std::move(callback).Run(Success(base::Value(std::move(result))));
    return;
  }
  if (operation == "autoUpdater.getState") {
    base::DictValue result;
    result.Set("state",
               xenon::updater::UpdateStateToString(manager->GetState()));
    result.Set("error", manager->GetLastError());
    result.Set("is_installer_package", manager->GetStagedIsInstallerPackage());
    result.Set("installer_opened", manager->GetInstallerOpened());
    result.Set("requires_elevation", manager->GetRequiresElevation());
    result.Set("elevation_approval_required",
               manager->GetElevationApprovalRequired());
    if (manager->GetManifest()) {
      result.Set("manifest", manager->GetManifest()->ToValue().Clone());
    }
    std::move(callback).Run(Success(base::Value(std::move(result))));
    return;
  }
  std::move(callback).Run(Failure(
      "ERR_NOT_SUPPORTED: unsupported autoUpdater operation " + operation));
}

bool IsSupportedClipboardMime(std::string_view mime) {
  return mime == ui::kMimeTypePlainText || mime == ui::kMimeTypeHtml ||
         mime == ui::kMimeTypeRtf || mime == ui::kMimeTypeSvg ||
         mime == ui::kMimeTypePng;
}

void ReadClipboardMime(const std::string& mime,
                       ElectronApiCallback callback) {
  constexpr auto buffer = ui::ClipboardBuffer::kCopyPaste;
  if (mime == ui::kMimeTypePlainText || mime == ui::kMimeTypeSvg) {
    auto text_callback = base::BindOnce(
        [](ElectronApiCallback callback, std::u16string text) {
          std::move(callback).Run(Success(base::Value(
              base::Base64Encode(base::UTF16ToUTF8(text)))));
        },
        std::move(callback));
    if (mime == ui::kMimeTypePlainText) {
      ui::Clipboard::GetForCurrentThread()->ReadText(
          buffer, std::nullopt, std::move(text_callback));
    } else {
      ui::Clipboard::GetForCurrentThread()->ReadSvg(
          buffer, std::nullopt, std::move(text_callback));
    }
  } else if (mime == ui::kMimeTypeHtml) {
    ui::Clipboard::GetForCurrentThread()->ReadHTML(
        buffer, std::nullopt,
        base::BindOnce(
            [](ElectronApiCallback callback, std::u16string markup, GURL,
               uint32_t start, uint32_t end) {
              // The platform may include an HTML document wrapper; the MIME
              // representation is the fragment reported by the clipboard.
              if (start > end || end > markup.size()) {
                std::move(callback).Run(Failure(
                    "ERR_INVALID_STATE: invalid clipboard HTML fragment"));
                return;
              }
              std::move(callback).Run(Success(base::Value(base::Base64Encode(
                  base::UTF16ToUTF8(markup.substr(start, end - start))))));
            },
            std::move(callback)));
  } else if (mime == ui::kMimeTypeRtf) {
    ui::Clipboard::GetForCurrentThread()->ReadRTF(
        buffer, std::nullopt,
        base::BindOnce(
            [](ElectronApiCallback callback, std::string rtf) {
              std::move(callback).Run(
                  Success(base::Value(base::Base64Encode(rtf))));
            },
            std::move(callback)));
  } else {
    ui::Clipboard::GetForCurrentThread()->ReadPng(
        buffer, std::nullopt,
        base::BindOnce(
            [](ElectronApiCallback callback, const std::vector<uint8_t>& png) {
              std::move(callback).Run(
                  Success(base::Value(base::Base64Encode(png))));
            },
            std::move(callback)));
  }
}

void WriteClipboardItems(const base::DictValue& arguments,
                         ElectronApiCallback callback) {
  const auto* items = arguments.FindList("items");
  if (!items) {
    std::move(callback).Run(
        Failure("ERR_INVALID_ARG_TYPE: clipboard items must be an array"));
    return;
  }
  struct Representation {
    std::string mime;
    std::string bytes;
    SkBitmap image;
  };
  std::vector<Representation> prepared;
  for (const auto& item : *items) {
    if (!item.is_list() || item.GetList().empty()) {
      std::move(callback).Run(Failure(
          "ERR_INVALID_ARG_TYPE: clipboard item must contain representations"));
      return;
    }
    for (const auto& value : item.GetList()) {
      const auto* representation = value.GetIfDict();
      const auto* mime = representation ? representation->FindString("mime") : nullptr;
      const auto* data = representation ? representation->FindString("data") : nullptr;
      if (!mime || !data) {
        std::move(callback).Run(Failure(
            "ERR_INVALID_ARG_TYPE: clipboard representation requires mime and data strings"));
        return;
      }
      if (!IsSupportedClipboardMime(*mime)) {
        std::move(callback).Run(Failure(
            "ERR_NOT_SUPPORTED: unsupported clipboard MIME type " + *mime));
        return;
      }
      Representation entry;
      entry.mime = *mime;
      if (!base::Base64Decode(*data, &entry.bytes)) {
        std::move(callback).Run(Failure(
            "ERR_INVALID_ARG_VALUE: clipboard payload is not valid base64"));
        return;
      }
      if (*mime == ui::kMimeTypePng) {
        entry.image = gfx::PNGCodec::Decode(base::as_byte_span(entry.bytes));
        if (entry.image.isNull()) {
          std::move(callback).Run(Failure(
              "ERR_INVALID_ARG_VALUE: clipboard image/png payload is not a valid PNG"));
          return;
        }
      }
      prepared.push_back(std::move(entry));
    }
  }
  // Validate and decode every entry before creating a writer. A later invalid
  // representation must leave the previous clipboard contents intact.
  {
    ui::ScopedClipboardWriter writer(ui::ClipboardBuffer::kCopyPaste);
    for (const auto& entry : prepared) {
      if (entry.mime == ui::kMimeTypePlainText) {
        writer.WriteText(base::UTF8ToUTF16(entry.bytes));
      } else if (entry.mime == ui::kMimeTypeHtml) {
        writer.WriteHTML(base::UTF8ToUTF16(entry.bytes), std::string());
      } else if (entry.mime == ui::kMimeTypeRtf) {
        writer.WriteRTF(entry.bytes);
      } else if (entry.mime == ui::kMimeTypeSvg) {
        writer.WriteSvg(base::UTF8ToUTF16(entry.bytes));
      } else {
        writer.WriteImage(entry.image);
      }
    }
  }
  std::move(callback).Run(Success());
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
  if (operation == "clipboard.readTypes" || operation == "clipboard.has") {
    const std::string* mime = arguments.FindString("mime");
    const bool has = operation == "clipboard.has";
    if (has && !mime) {
      std::move(callback).Run(
          Failure("ERR_INVALID_ARG_TYPE: clipboard MIME type must be a string"));
      return;
    }
    ui::Clipboard::GetForCurrentThread()->ReadAvailableStandardAndCustomFormatNames(
        buffer, std::nullopt,
        base::BindOnce(
            [](ElectronApiCallback callback, bool has, std::string mime,
               std::vector<std::u16string> types) {
              if (has) {
                const auto requested = base::UTF8ToUTF16(mime);
                std::move(callback).Run(Success(base::Value(
                    std::find(types.begin(), types.end(), requested) != types.end())));
                return;
              }
              base::ListValue result;
              for (const auto& type : types)
                result.Append(base::UTF16ToUTF8(type));
              std::move(callback).Run(Success(base::Value(std::move(result))));
            },
            std::move(callback), has, mime ? *mime : std::string()));
    return;
  }
  if (operation == "clipboard.writeItems") {
    WriteClipboardItems(arguments, std::move(callback));
    return;
  }
  if (operation == "clipboard.readType") {
    const std::string* mime = arguments.FindString("mime");
    if (!mime) {
      std::move(callback).Run(
          Failure("ERR_INVALID_ARG_TYPE: clipboard MIME type must be a string"));
    } else if (!IsSupportedClipboardMime(*mime)) {
      std::move(callback).Run(Failure(
          "ERR_NOT_SUPPORTED: unsupported clipboard MIME type " + *mime));
    } else {
      ReadClipboardMime(*mime, std::move(callback));
    }
    return;
  }
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

void SetAutoUpdaterEventCallback(AutoUpdaterEventCallback callback) {
  AutoUpdaterEventBridge::Get().SetCallback(std::move(callback));
}

void SetElectronThemeEventCallback(ElectronThemeEventCallback callback) {
  ElectronThemeEventBridge::Get().SetCallback(std::move(callback));
}

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
  if (*operation == "nativeTheme.getState") {
    std::move(callback).Run(Success(base::Value(ReadSystemTheme())));
    return;
  }
  if (*operation == "systemPreferences.getAccentColor") {
    auto state = ReadSystemTheme();
    const auto* accent = state.FindString("accentColor");
    std::move(callback).Run(accent ? Success(base::Value(*accent)) :
        Failure("ERR_NOT_SUPPORTED: the operating system exposes no accent color"));
    return;
  }
  if (*operation == "systemPreferences.isAeroGlassEnabled") {
#if BUILDFLAG(IS_WIN)
    BOOL enabled = FALSE;
    const HRESULT status = ::DwmIsCompositionEnabled(&enabled);
    std::move(callback).Run(SUCCEEDED(status) ? Success(base::Value(!!enabled)) :
        Failure("ERR_SYSTEM_ERROR: DwmIsCompositionEnabled failed"));
#else
    std::move(callback).Run(Failure(
        "ERR_NOT_SUPPORTED: isAeroGlassEnabled is only supported on Windows"));
#endif
    return;
  }
  if (base::StartsWith(*operation, "clipboard.")) {
    CallClipboard(arguments.GetDict(), *operation, std::move(callback));
    return;
  }
  if (base::StartsWith(*operation, "nativeImage.")) {
    std::move(callback).Run(CallNativeImageApi(*operation, arguments.GetDict()));
    return;
  }
  if (base::StartsWith(*operation, "crashReporter.")) {
    CallCrashReporterApi(*operation, std::move(callback));
    return;
  }
#if BUILDFLAG(IS_MAC)
  if (*operation == "app.getLoginItemSettings") {
    GetElectronLoginItemSettings(arguments.GetDict(), std::move(callback));
    return;
  }
#endif
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
