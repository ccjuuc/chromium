// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permission_delegate.h"

#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "base/barrier_closure.h"
#include "base/callback_list.h"
#include "base/functional/bind.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"
#include "components/keyed_service/core/keyed_service_shutdown_notifier.h"
#include "components/permissions/permission_util.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/device_service.h"
#include "content/public/browser/permission_controller_delegate.h"
#include "content/public/browser/permission_request_description.h"
#include "content/public/browser/permission_status_subscription.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "services/device/public/mojom/geolocation_control.mojom.h"
#include "third_party/blink/public/common/permissions/permission_utils.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permissions.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_shutdown_notifier_factory.h"

namespace xenon::ipc {

std::string_view ElectronPermissionName(blink::PermissionType permission) {
  using Type = blink::PermissionType;
  switch (permission) {
    case Type::MIDI_SYSEX:
      return "midiSysex";
    case Type::NOTIFICATIONS:
      return "notifications";
    case Type::GEOLOCATION:
      return "geolocation";
    case Type::GEOLOCATION_APPROXIMATE:
      return "geolocation";
    case Type::PROTECTED_MEDIA_IDENTIFIER:
      return "mediaKeySystem";
    case Type::MIDI:
      return "midi";
    case Type::PERSISTENT_STORAGE:
      return "persistent-storage";
    case Type::AUDIO_CAPTURE:
    case Type::VIDEO_CAPTURE:
    case Type::CAMERA_PAN_TILT_ZOOM:
      return "media";
    case Type::BACKGROUND_SYNC:
      return "background-sync";
    case Type::SENSORS:
      return "sensors";
    case Type::PAYMENT_HANDLER:
      return "payment-handler";
    case Type::BACKGROUND_FETCH:
      return "background-fetch";
    case Type::IDLE_DETECTION:
      return "idle-detection";
    case Type::PERIODIC_BACKGROUND_SYNC:
      return "periodic-background-sync";
    case Type::WAKE_LOCK_SCREEN:
      return "screen-wake-lock";
    case Type::WAKE_LOCK_SYSTEM:
      return "system-wake-lock";
    case Type::NFC:
      return "nfc";
    case Type::CLIPBOARD_READ_WRITE:
      return "clipboard-read";
    case Type::CLIPBOARD_SANITIZED_WRITE:
      return "clipboard-sanitized-write";
    case Type::VR:
      return "vr";
    case Type::AR:
      return "ar";
    case Type::STORAGE_ACCESS_GRANT:
      return "storage-access";
    case Type::WINDOW_MANAGEMENT:
      return "window-management";
    case Type::LOCAL_FONTS:
      return "local-fonts";
    case Type::DISPLAY_CAPTURE:
      return "display-capture";
    case Type::TOP_LEVEL_STORAGE_ACCESS:
      return "top-level-storage-access";
    case Type::CAPTURED_SURFACE_CONTROL:
      return "captured-surface-control";
    case Type::SMART_CARD:
      return "smart-card";
    case Type::WEB_PRINTING:
      return "web-printing";
    case Type::SPEAKER_SELECTION:
      return "speaker-selection";
    case Type::KEYBOARD_LOCK:
      return "keyboardLock";
    case Type::POINTER_LOCK:
      return "pointerLock";
    case Type::AUTOMATIC_FULLSCREEN:
      return "automatic-fullscreen";
    case Type::HAND_TRACKING:
      return "hand-tracking";
    case Type::WEB_APP_INSTALLATION:
      return "web-app-installation";
    case Type::LOCAL_NETWORK_ACCESS:
      return "local-network-access";
    case Type::LOCAL_NETWORK:
      return "local-network";
    case Type::LOOPBACK_NETWORK:
      return "loopback-network";
    case Type::NUM:
      return {};
  }
  return {};
}

namespace {

using Delegate = content::PermissionControllerDelegate;
using Result = content::PermissionResult;
using Status = blink::mojom::PermissionStatus;
using SubscriptionId = content::PermissionController::SubscriptionId;
using Descriptor = blink::mojom::PermissionDescriptorPtr;

const char kDelegateKey[] = "xenon-electron-permission-delegate";

Result Decision(bool granted) {
  return Result(granted ? Status::GRANTED : Status::DENIED);
}

base::DictValue Details(blink::PermissionType permission,
                        const GURL& embedding_origin) {
  base::DictValue details;
  details.Set("embeddingOrigin", embedding_origin.spec());
  if (permission == blink::PermissionType::AUDIO_CAPTURE) {
    details.Set("mediaType", "audio");
  } else if (permission == blink::PermissionType::VIDEO_CAPTURE ||
             permission == blink::PermissionType::CAMERA_PAN_TILT_ZOOM) {
    details.Set("mediaType", "video");
  }
  return details;
}

bool DocumentAllows(content::RenderFrameHost* frame,
                    blink::PermissionType permission) {
  if (!frame || frame->IsNestedWithinFencedFrame()) {
    return false;
  }
  const auto feature =
      blink::PermissionTypeToPermissionsPolicyFeature(permission);
  return !feature || frame->IsFeatureEnabled(*feature);
}

class ElectronPermissionDelegate final : public Delegate,
                                         public base::SupportsUserData::Data {
 public:
  ElectronPermissionDelegate(
      content::BrowserContext* context,
      base::RepeatingCallback<Delegate*()> fallback_getter)
      : context_(context), fallback_getter_(std::move(fallback_getter)) {
    SetSubscriptions(nullptr);
    policy_observer_ = ObserveElectronPermissionChanges(base::BindRepeating(
        &ElectronPermissionDelegate::RefreshElectronSubscriptions,
        weak_factory_.GetWeakPtr()));
    shutdown_observer_ =
        ElectronSessionShutdownNotifierFactory::GetInstance()
            ->Get(context)
            ->Subscribe(base::BindOnce(&ElectronPermissionDelegate::Shutdown,
                                       weak_factory_.GetWeakPtr()));
  }
  ~ElectronPermissionDelegate() override = default;

  void RequestPermissionsFromCurrentDocument(
      content::RenderFrameHost* frame,
      const content::PermissionRequestDescription& description,
      base::OnceCallback<void(const std::vector<Result>&)> callback) override {
    if (!IsElectronPermissionFrame(frame)) {
      if (auto* delegate = fallback()) {
        delegate->RequestPermissionsFromCurrentDocument(frame, description,
                                                        std::move(callback));
      } else {
        std::move(callback).Run(std::vector<Result>(
            description.permissions.size(), Decision(false)));
      }
      return;
    }
    if (description.permissions.empty()) {
      std::move(callback).Run({});
      return;
    }
    auto results = std::make_shared<std::vector<Result>>(
        description.permissions.size(), Decision(false));
    struct RequestGroup {
      std::string permission;
      base::DictValue details;
      std::vector<std::pair<size_t, blink::PermissionType>> entries;
    };
    std::vector<RequestGroup> groups;
    std::optional<size_t> media_group;
    bool audio = false;
    bool video = false;
    for (size_t index = 0; index < description.permissions.size(); ++index) {
      const auto& descriptor = description.permissions[index];
      const auto type =
          descriptor
              ? blink::MaybePermissionDescriptorToPermissionType(descriptor)
              : std::nullopt;
      if (shutting_down_ || !type || ElectronPermissionName(*type).empty() ||
          !DocumentAllows(frame, *type)) {
        continue;
      }
      const std::string name(ElectronPermissionName(*type));
      if (name == "media") {
        audio |= *type == blink::PermissionType::AUDIO_CAPTURE;
        video |= *type != blink::PermissionType::AUDIO_CAPTURE;
        if (media_group) {
          groups[*media_group].entries.emplace_back(index, *type);
          continue;
        }
        media_group = groups.size();
      }
      auto details = Details(
          *type, permissions::PermissionUtil::GetLastCommittedOriginAsURL(
                     frame->GetMainFrame()));
      details.Set("userGesture", description.user_gesture);
      groups.push_back({name, std::move(details), {{index, *type}}});
    }
    if (media_group) {
      auto& details = groups[*media_group].details;
      // A single getUserMedia request receives one Electron "media" decision,
      // including both audio and video. Preserve Chromium's original result
      // order when completing its multi-permission callback.
      details.Remove("mediaType");
      base::ListValue media_types;
      if (audio) {
        media_types.Append("audio");
      }
      if (video) {
        media_types.Append("video");
      }
      details.Set("mediaTypes", std::move(media_types));
      details.Set("securityOrigin",
                  description.requesting_origin.is_empty()
                      ? frame->GetLastCommittedOrigin().GetURL().spec()
                      : description.requesting_origin.spec());
    }
    if (groups.empty()) {
      std::move(callback).Run(*results);
      return;
    }
    auto done = base::BarrierClosure(
        groups.size(),
        base::BindOnce(
            [](std::shared_ptr<std::vector<Result>> results,
               base::OnceCallback<void(const std::vector<Result>&)> callback) {
              std::move(callback).Run(*results);
            },
            results, std::move(callback)));
    for (auto& group : groups) {
      RequestElectronPermission(
          frame, group.permission, std::move(group.details),
          base::BindOnce(
              [](base::WeakPtr<ElectronPermissionDelegate> self,
                 content::WeakDocumentPtr document,
                 std::vector<std::pair<size_t, blink::PermissionType>> entries,
                 std::shared_ptr<std::vector<Result>> results,
                 base::RepeatingClosure done, bool granted) {
                auto* frame = document.AsRenderFrameHostIfValid();
                granted = granted && self && !self->shutting_down_ && frame;
                for (const auto& [index, type] : entries) {
                  const bool allowed = granted && DocumentAllows(frame, type);
                  if (allowed) {
                    self->ApplyGrant(frame, type);
                  }
                  (*results)[index] = Decision(allowed);
                }
                done.Run();
              },
              weak_factory_.GetWeakPtr(), frame->GetWeakDocumentPtr(),
              std::move(group.entries), results, done));
    }
  }

  Status GetPermissionStatus(const Descriptor& permission,
                             const GURL& requesting_origin,
                             const GURL& embedding_origin) override {
    auto* delegate = fallback();
    return delegate ? delegate->GetPermissionStatus(
                          permission, requesting_origin, embedding_origin)
                    : Status::DENIED;
  }

  Result GetPermissionResultForOriginWithoutContext(
      const Descriptor& permission,
      const url::Origin& requesting_origin,
      const url::Origin& embedding_origin) override {
    auto* delegate = fallback();
    return delegate ? delegate->GetPermissionResultForOriginWithoutContext(
                          permission, requesting_origin, embedding_origin)
                    : Decision(false);
  }

  Result GetPermissionResultForCurrentDocument(
      const Descriptor& permission,
      content::RenderFrameHost* frame,
      bool include_device_status) override {
    if (!IsElectronPermissionFrame(frame)) {
      auto* delegate = fallback();
      return delegate ? delegate->GetPermissionResultForCurrentDocument(
                            permission, frame, include_device_status)
                      : Decision(false);
    }
    auto result =
        CheckFrame(permission, frame, frame->GetLastCommittedOrigin().GetURL(),
                   permissions::PermissionUtil::GetLastCommittedOriginAsURL(
                       frame->GetMainFrame()));
    // App approval cannot manufacture an OS-level device grant. Device access
    // itself is still enforced by Chromium's device/MediaCapture backends.
    if (result.status == Status::GRANTED && include_device_status) {
      if (auto* delegate = fallback()) {
        auto native = delegate->GetPermissionResultForCurrentDocument(
            permission, frame, true);
        if (native.source ==
            content::PermissionStatusSource::APP_LEVEL_SETTINGS) {
          return native;
        }
      }
    }
    return result;
  }

  Result GetPermissionResultForWorker(const Descriptor& permission,
                                      content::RenderProcessHost* process,
                                      const GURL& worker_origin) override {
    if (!IsElectronPermissionProcess(process)) {
      auto* delegate = fallback();
      return delegate ? delegate->GetPermissionResultForWorker(
                            permission, process, worker_origin)
                      : Decision(false);
    }
    const auto type =
        blink::MaybePermissionDescriptorToPermissionType(permission);
    if (shutting_down_ || !type || ElectronPermissionName(*type).empty()) {
      return Decision(false);
    }
    return Decision(CheckElectronWorkerPermission(
        process, std::string(ElectronPermissionName(*type)), worker_origin,
        Details(*type, worker_origin)));
  }

  Result GetPermissionResultForEmbeddedRequester(
      const Descriptor& permission,
      content::RenderFrameHost* frame,
      const url::Origin& requesting_origin) override {
    if (!IsElectronPermissionFrame(frame)) {
      auto* delegate = fallback();
      return delegate ? delegate->GetPermissionResultForEmbeddedRequester(
                            permission, frame, requesting_origin)
                      : Decision(false);
    }
    return CheckFrame(permission, frame, requesting_origin.GetURL(),
                      frame->GetLastCommittedOrigin().GetURL());
  }

  void ResetPermission(blink::PermissionType permission,
                       const GURL& requesting_origin,
                       const GURL& embedding_origin) override {
    if (auto* delegate = fallback()) {
      delegate->ResetPermission(permission, requesting_origin,
                                embedding_origin);
    }
  }

  SubscriptionId SubscribeToContentSettingsTypeChange(
      ContentSettingsType type,
      const GURL& requesting_origin,
      const GURL& embedding_origin,
      base::RepeatingCallback<void(const PermissionSetting&)> callback)
      override {
    auto* delegate = fallback();
    return delegate ? delegate->SubscribeToContentSettingsTypeChange(
                          type, requesting_origin, embedding_origin,
                          std::move(callback))
                    : SubscriptionId();
  }

  void UnsubscribeFromContentSettingsTypeChange(SubscriptionId id) override {
    if (auto* delegate = fallback()) {
      delegate->UnsubscribeFromContentSettingsTypeChange(id);
    }
  }

  void OnPermissionStatusChangeSubscriptionAdded(SubscriptionId id) override {
    auto* original = subscriptions() ? subscriptions()->Lookup(id) : nullptr;
    if (!original || shutting_down_) {
      return;
    }
    if (IsElectronSubscription(*original)) {
      electron_subscriptions_.insert(id);
      original->permission_result = CheckSubscription(*original);
      return;
    }
    auto* delegate = fallback();
    if (!delegate) {
      return;
    }
    // PermissionManager iterates every entry in its subscriptions map when a
    // content setting changes. Give it only the ordinary browser entries so
    // native site settings cannot overwrite an Electron handler's decision.
    auto copy = std::make_unique<content::PermissionResultSubscription>(
        original->permission_descriptor.Clone());
    copy->requesting_origin = original->requesting_origin;
    copy->embedding_origin = original->embedding_origin;
    copy->requesting_origin_delegation = original->requesting_origin_delegation;
    copy->render_frame_id = original->render_frame_id;
    copy->render_process_id = original->render_process_id;
    copy->should_include_device_status = original->should_include_device_status;
    copy->callback = base::BindRepeating(
        &ElectronPermissionDelegate::OnNativeSubscriptionChanged,
        weak_factory_.GetWeakPtr(), id);
    native_subscriptions_.AddWithID(std::move(copy), id);
    delegate->SetSubscriptions(&native_subscriptions_);
    delegate->OnPermissionStatusChangeSubscriptionAdded(id);
    auto* current = native_subscriptions_.Lookup(id);
    if (current && subscriptions() && subscriptions()->Lookup(id)) {
      auto* target = subscriptions()->Lookup(id);
      target->permission_result = current->permission_result;
      // Native permission attribution may replace the embedding origin (for
      // example, an inner WebContents hosted by an NTP or a MIME extension).
      // PermissionController uses this same origin to match its overrides.
      target->embedding_origin = current->embedding_origin;
      target->requesting_origin_delegation =
          current->requesting_origin_delegation;
    }
  }

  void UnsubscribeFromPermissionResultChange(SubscriptionId id) override {
    if (electron_subscriptions_.erase(id)) {
      return;
    }
    if (!native_subscriptions_.Lookup(id)) {
      return;
    }
    if (auto* delegate = fallback()) {
      delegate->UnsubscribeFromPermissionResultChange(id);
    }
    native_subscriptions_.Remove(id);
  }

  std::optional<gfx::Rect> GetExclusionAreaBoundsInScreen(
      content::WebContents* contents) const override {
    auto* delegate = fallback();
    return delegate ? delegate->GetExclusionAreaBoundsInScreen(contents)
                    : std::nullopt;
  }

  bool IsPermissionOverridable(
      blink::PermissionType permission,
      base::optional_ref<const url::Origin> requesting_origin,
      base::optional_ref<const url::Origin> embedding_origin) override {
    auto* delegate = fallback();
    return delegate && delegate->IsPermissionOverridable(
                           permission, requesting_origin, embedding_origin);
  }

 private:
  Delegate* fallback() const {
    return shutting_down_ || context_->ShutdownStarted() || !fallback_getter_
               ? nullptr
               : fallback_getter_.Run();
  }

  Result CheckFrame(const Descriptor& permission,
                    content::RenderFrameHost* frame,
                    const GURL& requesting_origin,
                    const GURL& embedding_origin) {
    const auto type =
        blink::MaybePermissionDescriptorToPermissionType(permission);
    if (shutting_down_ || !type || ElectronPermissionName(*type).empty() ||
        !DocumentAllows(frame, *type)) {
      return Decision(false);
    }
    return Decision(CheckElectronPermission(
        frame, std::string(ElectronPermissionName(*type)), requesting_origin,
        Details(*type, embedding_origin)));
  }

  void ApplyGrant(content::RenderFrameHost* frame, blink::PermissionType type) {
    if (type == blink::PermissionType::MIDI_SYSEX) {
      content::ChildProcessSecurityPolicy::GetInstance()
          ->GrantSendMidiSysExMessage(frame->GetProcess()->GetDeprecatedID());
    } else if (type == blink::PermissionType::GEOLOCATION ||
               type == blink::PermissionType::GEOLOCATION_APPROXIMATE) {
      if (!geolocation_control_.is_bound()) {
        content::GetDeviceService().BindGeolocationControl(
            geolocation_control_.BindNewPipeAndPassReceiver());
        geolocation_control_.reset_on_disconnect();
      }
      geolocation_control_->UserDidOptIntoLocationServices();
    }
  }

  bool IsElectronSubscription(
      const content::PermissionResultSubscription& subscription) const {
    if (subscription.render_frame_id != -1) {
      return IsElectronPermissionFrame(content::RenderFrameHost::FromID(
          subscription.render_process_id, subscription.render_frame_id));
    }
    return IsElectronPermissionProcess(
        content::RenderProcessHost::FromID(subscription.render_process_id));
  }

  Result CheckSubscription(const content::PermissionResultSubscription& sub) {
    if (sub.render_frame_id != -1) {
      auto* frame = content::RenderFrameHost::FromID(sub.render_process_id,
                                                     sub.render_frame_id);
      if (!frame || !IsElectronPermissionFrame(frame)) {
        return Decision(false);
      }
      return GetPermissionResultForCurrentDocument(
          sub.permission_descriptor, frame, sub.should_include_device_status);
    }
    auto* process = content::RenderProcessHost::FromID(sub.render_process_id);
    if (!process || !IsElectronPermissionProcess(process)) {
      return Decision(false);
    }
    return GetPermissionResultForWorker(sub.permission_descriptor, process,
                                        sub.requesting_origin);
  }

  void OnNativeSubscriptionChanged(SubscriptionId id,
                                   Result result,
                                   bool ignore_override) {
    auto* original = subscriptions() ? subscriptions()->Lookup(id) : nullptr;
    if (!original || shutting_down_) {
      return;
    }
    original->permission_result = result;
    auto callback = original->callback;
    callback.Run(result, ignore_override);
  }

  void RefreshElectronSubscriptions() {
    if (shutting_down_ || !subscriptions()) {
      return;
    }
    // A callback may unsubscribe itself or close its WebContents. Never retain
    // a raw subscription pointer or iterate the mutable set across callbacks.
    const std::vector<SubscriptionId> ids(electron_subscriptions_.begin(),
                                          electron_subscriptions_.end());
    for (const auto id : ids) {
      auto* subscription = subscriptions()->Lookup(id);
      if (!subscription) {
        continue;
      }
      const auto previous = subscription->permission_result;
      auto result = CheckSubscription(*subscription);
      subscription = subscriptions() ? subscriptions()->Lookup(id) : nullptr;
      if (!subscription || !electron_subscriptions_.contains(id)) {
        continue;
      }
      subscription->permission_result = result;
      if (previous && previous->status == result.status) {
        continue;
      }
      auto callback = subscription->callback;
      auto self = weak_factory_.GetWeakPtr();
      callback.Run(result, false);
      if (!self) {
        return;
      }
      if (shutting_down_ || !subscriptions()) {
        return;
      }
    }
  }

  void Shutdown() {
    shutting_down_ = true;
    policy_observer_ = {};
    geolocation_control_.reset();
    SetSubscriptions(nullptr);
    native_subscriptions_.Clear();
    electron_subscriptions_.clear();
    // PermissionManager's keyed-service Shutdown clears its subscription map
    // pointer and observers before this BrowserContext-owned map is destroyed.
  }

  const raw_ptr<content::BrowserContext> context_;
  base::RepeatingCallback<Delegate*()> fallback_getter_;
  bool shutting_down_ = false;
  content::PermissionController::SubscriptionsMap native_subscriptions_;
  std::set<SubscriptionId> electron_subscriptions_;
  mojo::Remote<device::mojom::GeolocationControl> geolocation_control_;
  base::CallbackListSubscription policy_observer_;
  base::CallbackListSubscription shutdown_observer_;
  base::WeakPtrFactory<ElectronPermissionDelegate> weak_factory_{this};
};

}  // namespace

content::PermissionControllerDelegate* GetElectronPermissionControllerDelegate(
    content::BrowserContext* context,
    base::RepeatingCallback<content::PermissionControllerDelegate*()>
        fallback_getter) {
  if (!context || context->ShutdownStarted()) {
    return nullptr;
  }
  auto* existing = static_cast<ElectronPermissionDelegate*>(
      context->GetUserData(kDelegateKey));
  if (existing) {
    return existing;
  }
  auto delegate = std::make_unique<ElectronPermissionDelegate>(
      context, std::move(fallback_getter));
  auto* result = delegate.get();
  context->SetUserData(kDelegateKey, std::move(delegate));
  return result;
}

}  // namespace xenon::ipc
