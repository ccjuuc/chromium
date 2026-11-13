// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ipc/xenon_electron_media_permission.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/values.h"
#include "build/build_config.h"
#include "chrome/browser/media/webrtc/capture_policy_utils.h"
#include "chrome/browser/media/webrtc/desktop_capture_devices_util.h"
#include "chrome/browser/media/webrtc/desktop_media_picker_controller.h"
#include "chrome/browser/media/webrtc/media_capture_devices_dispatcher.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/weak_document_ptr.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/mediastream/media_stream.mojom.h"
#include "url/origin.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_permissions.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_session_bridge.h"

#if BUILDFLAG(IS_MAC)
#include "chrome/browser/media/webrtc/system_media_capture_permissions_mac.h"
#endif

namespace xenon::ipc {
namespace {
using StreamType = blink::mojom::MediaStreamType;
using Result = blink::mojom::MediaStreamRequestResult;
struct MediaDocument {
  content::WeakDocumentPtr document;
  base::RepeatingCallback<bool()> valid;
};

bool HasElectronIdentity(content::RenderFrameHost* frame) {
  return frame && FindElectronSessionProvenance(
                      frame->GetProcess()->GetStoragePartition())
                      .has_value();
}

content::RenderFrameHost* ValidFrame(const MediaDocument& document) {
  auto* frame = document.document.AsRenderFrameHostIfValid();
  return frame && frame->IsActive() && !document.valid.is_null() &&
                 document.valid.Run()
             ? frame
             : nullptr;
}

void Deny(content::MediaResponseCallback callback,
          Result result = Result::PERMISSION_DENIED) {
  std::move(callback).Run(blink::mojom::StreamDevicesSet(), result, nullptr);
}

base::DictValue DisplayDetails(const content::MediaStreamRequest& request) {
  return base::DictValue()
      .Set("securityOrigin", request.security_origin.spec())
      .Set("videoRequested", request.video_type != StreamType::NO_SERVICE)
      .Set("audioRequested", request.audio_type != StreamType::NO_SERVICE)
      .Set("userGesture", request.user_gesture);
}

void CompleteCapture(MediaDocument document,
                     content::MediaResponseCallback callback,
                     blink::mojom::StreamDevices devices,
                     std::unique_ptr<content::MediaStreamUI> ui) {
  if (!ValidFrame(document)) {
    Deny(std::move(callback));
    return;
  }
  if (!devices.video_device && !devices.audio_device) {
    Deny(std::move(callback), Result::NO_HARDWARE);
    return;
  }
  blink::mojom::StreamDevicesSet streams;
  streams.stream_devices.push_back(devices.Clone());
  std::move(callback).Run(streams, Result::OK, std::move(ui));
}

void CaptureDesktop(MediaDocument document,
                    content::MediaStreamRequest request,
                    content::MediaResponseCallback callback,
                    base::DictValue selection) {
  auto* frame = ValidFrame(document);
  if (!frame || selection.empty()) {
    Deny(std::move(callback));
    return;
  }
  auto* contents = content::WebContents::FromRenderFrameHost(frame);
  const auto* video = selection.FindDict("video");
  const auto* id = video ? video->FindString("id") : nullptr;
  const auto* name = video ? video->FindString("name") : nullptr;
  const auto source =
      id ? content::DesktopMediaID::Parse(*id) : content::DesktopMediaID();
  if (!name || name->find('\0') != std::string::npos || source.is_null() ||
      (source.type != content::DesktopMediaID::TYPE_SCREEN &&
       source.type != content::DesktopMediaID::TYPE_WINDOW) ||
      source.id == content::DesktopMediaID::kFakeId ||
      request.video_type == StreamType::NO_SERVICE) {
    Deny(std::move(callback), Result::INVALID_STATE);
    return;
  }
  if (source.type == content::DesktopMediaID::TYPE_SCREEN &&
      request.exclude_monitor_type_surfaces) {
    Deny(std::move(callback), Result::INVALID_STATE);
    return;
  }
  const auto level =
      capture_policy::GetAllowedCaptureLevel(request.security_origin, contents);
  if ((source.type == content::DesktopMediaID::TYPE_SCREEN &&
       level < AllowedScreenCaptureLevel::kDesktop) ||
      (source.type == content::DesktopMediaID::TYPE_WINDOW &&
       level < AllowedScreenCaptureLevel::kWindow)) {
    Deny(std::move(callback), Result::CAPTURE_NOT_ALLOWED_BY_POLICY);
    return;
  }
#if BUILDFLAG(IS_MAC)
  if (system_media_permissions::CheckSystemScreenCapturePermission() !=
      system_permission_settings::SystemPermission::kAllowed) {
    Deny(std::move(callback), Result::PERMISSION_DENIED_BY_SYSTEM);
    return;
  }
#endif
  const auto* audio = selection.FindString("audio");
  const bool capture_audio =
      audio && request.audio_type != StreamType::NO_SERVICE;
  if (selection.contains("audio") &&
      (!audio || (*audio != "loopback" && *audio != "loopbackWithMute"))) {
    Deny(std::move(callback), Result::INVALID_STATE);
    return;
  }
  if (capture_audio &&
      (request.exclude_system_audio ||
       !DesktopMediaPickerController::IsSystemAudioCaptureSupported(
           DesktopMediaPicker::Params::RequestSource::kGetDisplayMedia))) {
    Deny(std::move(callback), Result::NOT_SUPPORTED);
    return;
  }
  // Reuse Chromium's real device identifiers, display metadata, capture
  // indicator and platform backend. An application reply alone is not a stream.
  GetDevicesForDesktopCapture(
      contents, source, request.video_type, request.audio_type,
      request.security_origin, capture_audio,
      capture_audio && *audio == "loopbackWithMute",
      request.suppress_local_audio_playback, request.restrict_own_audio,
      /*display_notification=*/false, contents->GetTitle(),
      request.captured_surface_control_active,
      base::BindOnce(&CompleteCapture, document, std::move(callback)));
}

void PermissionDecided(MediaDocument document,
                       content::MediaStreamRequest request,
                       content::MediaResponseCallback callback,
                       bool granted) {
  auto* frame = ValidFrame(document);
  if (!frame || !granted) {
    Deny(std::move(callback));
    return;
  }
  if (request.video_type == StreamType::DISPLAY_VIDEO_CAPTURE ||
      request.video_type == StreamType::DISPLAY_VIDEO_CAPTURE_THIS_TAB) {
    auto details = DisplayDetails(request);
    RequestElectronDisplayMedia(frame, std::move(details),
                                base::BindOnce(&CaptureDesktop, document,
                                               request, std::move(callback)));
    return;
  }
  if (request.video_type == StreamType::GUM_DESKTOP_VIDEO_CAPTURE &&
      (request.audio_type == StreamType::NO_SERVICE ||
       request.audio_type == StreamType::GUM_DESKTOP_AUDIO_CAPTURE)) {
    // Legacy chromeMediaSourceId carries a desktop source chosen by the app.
    // The default -1 source means the whole desktop, as in Electron.
    const auto source =
        (request.requested_video_device_ids.empty() ||
         request.requested_video_device_ids.front().empty())
            ? content::DesktopMediaID(content::DesktopMediaID::TYPE_SCREEN, -1)
            : content::DesktopMediaID::Parse(
                  request.requested_video_device_ids.front());
    if (source.is_null() ||
        (source.type != content::DesktopMediaID::TYPE_SCREEN &&
         source.type != content::DesktopMediaID::TYPE_WINDOW) ||
        source.id == content::DesktopMediaID::kFakeId) {
      Deny(std::move(callback), Result::INVALID_STATE);
      return;
    }
    base::DictValue selection;
    selection.Set(
        "video",
        base::DictValue().Set("id", source.ToString()).Set("name", "Screen"));
    if (request.audio_type != StreamType::NO_SERVICE) {
      selection.Set("audio", request.disable_local_echo ? "loopbackWithMute"
                                                        : "loopback");
    }
    CaptureDesktop(document, request, std::move(callback),
                   std::move(selection));
    return;
  }
  // Tab-frame selection and multi-screen capture need their own authorization
  // and routing; a desktop source must not silently stand in for either.
  Deny(std::move(callback), Result::NOT_SUPPORTED);
}
}  // namespace

bool HandleElectronMediaAccessRequest(
    content::WebContents* contents,
    const content::MediaStreamRequest& request,
    content::MediaResponseCallback& callback) {
  auto* frame = content::RenderFrameHost::FromID(request.render_process_id,
                                                 request.render_frame_id);
  if (!HasElectronIdentity(frame)) {
    if (contents && HasElectronIdentity(contents->GetPrimaryMainFrame())) {
      Deny(std::move(callback));
      return true;
    }
    return false;
  }
  if (!contents ||
      content::WebContents::FromRenderFrameHost(frame) != contents ||
      !frame->IsActive() || !IsActiveElectronPermissionFrame(frame) ||
      frame->GetLastCommittedOrigin() != request.url_origin) {
    Deny(std::move(callback));
    return true;
  }
  const MediaDocument document{frame->GetWeakDocumentPtr(),
                               GetElectronPermissionFrameValidator(frame)};
  const bool device =
      (request.audio_type == StreamType::DEVICE_AUDIO_CAPTURE ||
       request.video_type == StreamType::DEVICE_VIDEO_CAPTURE) &&
      (request.audio_type == StreamType::NO_SERVICE ||
       request.audio_type == StreamType::DEVICE_AUDIO_CAPTURE) &&
      (request.video_type == StreamType::NO_SERVICE ||
       request.video_type == StreamType::DEVICE_VIDEO_CAPTURE);
  if (device) {
    // Chromium calls the Profile permission delegate, which forwards these
    // requests to the authenticated Session handler. Keep its real device
    // enumeration, feature-policy checks and OS permission handling.
    MediaCaptureDevicesDispatcher::GetInstance()->ProcessMediaAccessRequest(
        contents, request,
        base::BindOnce(
            [](MediaDocument document, content::MediaResponseCallback callback,
               const blink::mojom::StreamDevicesSet& streams, Result result,
               std::unique_ptr<content::MediaStreamUI> ui) {
              if (!ValidFrame(document)) {
                Deny(std::move(callback));
                return;
              }
              std::move(callback).Run(streams, result, std::move(ui));
            },
            document, std::move(callback)),
        nullptr);
    return true;
  }
  auto details = DisplayDetails(request);
  details.Set("mediaTypes", base::ListValue());
  RequestElectronPermission(frame, "media", std::move(details),
                            base::BindOnce(&PermissionDecided, document,
                                           request, std::move(callback)));
  return true;
}

std::optional<bool> CheckElectronMediaAccessPermission(
    content::RenderFrameHost* frame,
    const url::Origin& origin,
    StreamType type) {
  if (!HasElectronIdentity(frame)) {
    return std::nullopt;
  }
  if (!frame->IsActive() || !IsActiveElectronPermissionFrame(frame)) {
    return false;
  }
  return CheckElectronPermission(
      frame, "media", origin.GetURL(),
      base::DictValue()
          .Set("securityOrigin", origin.GetURL().spec())
          .Set("mediaType", type == StreamType::DEVICE_AUDIO_CAPTURE ? "audio"
                            : type == StreamType::DEVICE_VIDEO_CAPTURE
                                ? "video"
                                : "unknown"));
}
}  // namespace xenon::ipc
