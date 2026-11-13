// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_notification.h"

#include "build/build_config.h"
#include "xenon_overlay/buildflags/buildflags.h"

#if BUILDFLAG(ENABLE_XENON_SERVICE) && !BUILDFLAG(IS_ANDROID)

#include <map>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/functional/callback_helpers.h"
#include "base/strings/utf_string_conversions.h"
#include "base/uuid.h"
#include "chrome/browser/notifications/notification_display_service_impl.h"
#include "chrome/browser/notifications/notification_display_service_factory.h"
#include "chrome/browser/notifications/notification_handler.h"
#include "chrome/browser/profiles/profile.h"
#include "content/public/browser/browser_thread.h"
#include "ui/base/models/image_model.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/image/image.h"
#include "ui/message_center/public/cpp/notification.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_native_image_bridge.h"

namespace xenon::ipc {
namespace {

constexpr auto kNotificationType = NotificationHandler::Type::XENON_ELECTRON;

NotificationDisplayServiceImpl* GetDisplayService(Profile* profile, bool create) {
  if (!profile || profile->ShutdownStarted()) {
    return nullptr;
  }
  return create ? NotificationDisplayServiceImpl::GetForProfile(profile)
                : static_cast<NotificationDisplayServiceImpl*>(
                      NotificationDisplayServiceFactory::GetForProfileIfExists(
                          profile));
}

// Owned by the profile's display service. No global Profile pointers survive
// profile shutdown. Native IDs are fresh for each show, including JS ID reuse.
class ElectronNotificationHandler final : public NotificationHandler {
 public:
  struct Entry {
    std::string owner;
    int id;
    ElectronNotificationEvent callback;
  };

  void Add(const std::string& native_id,
           std::string owner,
           int id,
           ElectronNotificationEvent callback) {
    entries_.emplace(native_id,
                     Entry{std::move(owner), id, std::move(callback)});
  }

  void CloseOwner(NotificationDisplayServiceImpl* service,
                  const std::string& owner,
                  std::optional<int> id = std::nullopt) {
    std::vector<std::string> closing;
    for (auto it = entries_.begin(); it != entries_.end();) {
      if (it->second.owner == owner && (!id || it->second.id == *id)) {
        closing.push_back(it->first);
        it = entries_.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto& native_id : closing) {
      service->Close(kNotificationType, native_id);
    }
  }

  void OnShow(Profile*, const std::string& native_id) override {
    Emit(native_id, "show");
  }

  void OnClose(Profile*,
               const GURL&,
               const std::string& native_id,
               bool,
               base::OnceClosure completed_closure) override {
    base::ScopedClosureRunner completed(std::move(completed_closure));
    auto it = entries_.find(native_id);
    if (it == entries_.end()) {
      return;
    }
    Entry entry = std::move(it->second);
    entries_.erase(it);
    entry.callback.Run(base::Value(base::DictValue()
                                       .Set("id", entry.id)
                                       .Set("event", "close")
                                       .Set("args", base::ListValue())));
  }

  void OnClick(Profile*,
               const GURL&,
               const std::string& native_id,
               const std::optional<int>& action_index,
               const std::optional<std::u16string>& reply,
               base::OnceClosure completed_closure) override {
    base::ScopedClosureRunner completed(std::move(completed_closure));
    // Actions and inline reply are deliberately not exposed until their
    // platform option mapping is implemented. A plain body click is supported.
    if (!action_index && !reply) {
      Emit(native_id, "click");
    }
  }

 private:
  void Emit(const std::string& native_id, const std::string& event) {
    const auto it = entries_.find(native_id);
    if (it == entries_.end()) {
      return;
    }
    // Copy before calling user transport; it may synchronously retire owners.
    const Entry entry = it->second;
    entry.callback.Run(base::Value(base::DictValue()
                                       .Set("id", entry.id)
                                       .Set("event", event)
                                       .Set("args", base::ListValue())));
  }

  std::map<std::string, Entry> entries_;
};

ElectronNotificationHandler* GetHandler(NotificationDisplayServiceImpl* service,
                                        bool create) {
  if (!service) {
    return nullptr;
  }
  auto* handler = service->GetNotificationHandler(kNotificationType);
  if (!handler && create) {
    service->AddNotificationHandler(
        kNotificationType, std::make_unique<ElectronNotificationHandler>());
    handler = service->GetNotificationHandler(kNotificationType);
  }
  return static_cast<ElectronNotificationHandler*>(handler);
}

base::expected<message_center::Notification, std::string> MakeNotification(
    const base::DictValue& options,
    const std::string& app_name,
    const std::string& owner,
    const std::string& native_id) {
  const auto string_option = [&](const char* key) -> std::string_view {
    const auto* value = options.FindString(key);
    return value ? std::string_view(*value) : std::string_view();
  };
  for (const auto* name :
       {"title", "body", "iconPng", "timeoutType", "urgency"}) {
    if (options.contains(name) && !options.FindString(name)) {
      return base::unexpected("ERR_INVALID_ARG_TYPE: invalid notification " +
                              std::string(name));
    }
  }
  if (options.contains("silent") && !options.FindBool("silent")) {
    return base::unexpected("ERR_INVALID_ARG_TYPE: silent must be a boolean");
  }
  message_center::RichNotificationData data;
  // This is an application notification. In particular, macOS must not derive
  // website attribution from its intentionally empty web origin.
  data.context_message = base::UTF8ToUTF16(app_name);
  data.silent = options.FindBool("silent").value_or(false);
  const auto& timeout = string_option("timeoutType");
  if (!timeout.empty() && timeout != "default" && timeout != "never") {
    return base::unexpected("ERR_INVALID_ARG_VALUE: invalid timeoutType");
  }
  data.never_timeout = timeout == "never";
  const auto& urgency = string_option("urgency");
  if (!urgency.empty() && urgency != "normal" && urgency != "low" &&
      urgency != "critical") {
    return base::unexpected("ERR_INVALID_ARG_VALUE: invalid urgency");
  }
  data.priority = urgency == "critical" ? message_center::MAX_PRIORITY
                  : urgency == "low"    ? message_center::LOW_PRIORITY
                                        : message_center::DEFAULT_PRIORITY;
  ui::ImageModel icon;
  const auto& encoded = string_option("iconPng");
  if (!encoded.empty()) {
    // Reuse the nativeImage decoder's encoded size and decoded pixel bounds.
    auto decoded = CallNativeImageApi(
        "nativeImage.decode",
        base::DictValue().Set("dataBase64", std::string(encoded)));
    if (!decoded->success) {
      return base::unexpected(decoded->error);
    }
    const auto* png = decoded->value.GetDict().FindString("pngBase64");
    std::string bytes;
    if (!png || png->empty() || !base::Base64Decode(*png, &bytes)) {
      return base::unexpected(
          "ERR_INVALID_ARG_VALUE: invalid notification icon");
    }
    icon = ui::ImageModel::FromImage(gfx::Image::CreateFrom1xBitmap(
        gfx::PNGCodec::Decode(base::as_byte_span(bytes))));
  }
  data.settings_button_handler = message_center::SettingsButtonHandler::NONE;
  return message_center::Notification(
      message_center::NOTIFICATION_TYPE_SIMPLE, native_id,
      base::UTF8ToUTF16(string_option("title")),
      base::UTF8ToUTF16(string_option("body")), icon,
      base::UTF8ToUTF16(app_name), GURL(),
      message_center::NotifierId(message_center::NotifierType::APPLICATION,
                                 owner),
      data, nullptr);
}

}  // namespace

base::expected<base::Value, std::string> CallElectronNotification(
    Profile* profile,
    const std::string& owner,
    const std::string& app_name,
    const base::Value& request,
    ElectronNotificationEvent event_callback) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  const auto* args = request.GetIfDict();
  const auto* method = args ? args->FindString("method") : nullptr;
  if (!method) {
    return base::unexpected(
        "ERR_INVALID_ARG_TYPE: notification requires method");
  }
  if (*method != "isSupported" && *method != "show" && *method != "close") {
    return base::unexpected("ERR_NOT_SUPPORTED: unknown notification method");
  }
  auto* service = GetDisplayService(profile, *method != "close");
  if (*method == "isSupported") {
    return base::Value(service && service->IsXenonNotificationSupported());
  }
  const auto id = args->FindInt("id");
  if (!id || *id <= 0) {
    return base::unexpected("ERR_INVALID_ARG_VALUE: invalid notification ID");
  }
  if (*method == "close") {
    if (auto* handler = GetHandler(service, false)) {
      handler->CloseOwner(service, owner, id);
    }
    return base::Value();
  }
  if (!service) {
    return base::unexpected("ERR_ABORTED: notification profile is unavailable");
  }
  const auto* options = args->FindDict("options");
  if (!options || !event_callback) {
    return base::unexpected(
        "ERR_INVALID_ARG_TYPE: invalid notification options");
  }
  const std::string native_id =
      "xenon-electron-" + base::Uuid::GenerateRandomV4().AsLowercaseString();
  auto notification = MakeNotification(*options, app_name, owner, native_id);
  if (!notification.has_value()) {
    return base::unexpected(std::move(notification.error()));
  }
  auto* handler = GetHandler(service, true);
  handler->CloseOwner(service, owner, id);
  handler->Add(native_id, owner, *id, std::move(event_callback));
  // Chromium queues Display until the platform bridge is ready. OnShow means
  // submission to that bridge, not proof of visibility under OS quiet modes.
  service->Display(kNotificationType, *notification, nullptr);
  return base::Value();
}

void CloseElectronNotifications(Profile* profile, const std::string& owner) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  auto* service = GetDisplayService(profile, false);
  if (auto* handler = GetHandler(service, false)) {
    handler->CloseOwner(service, owner);
  }
}

}  // namespace xenon::ipc

#else

namespace xenon::ipc {
base::expected<base::Value, std::string> CallElectronNotification(
    Profile*, const std::string&, const std::string&, const base::Value& request,
    ElectronNotificationEvent) {
  const auto* args = request.GetIfDict();
  const auto* method = args ? args->FindString("method") : nullptr;
  if (method && *method == "isSupported") {
    return base::Value(false);
  }
  return base::unexpected("ERR_NOT_SUPPORTED: Electron notifications are disabled");
}

void CloseElectronNotifications(Profile*, const std::string&) {}
}  // namespace xenon::ipc

#endif
