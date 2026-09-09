// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/xenon_media_controls_host.h"

#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "components/download/public/common/download_url_parameters.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/render_frame_host.h"
#include "mojo/public/cpp/bindings/self_owned_receiver.h"
#include "net/traffic_annotation/network_traffic_annotation.h"

namespace {

net::NetworkTrafficAnnotationTag MediaDownloadTrafficAnnotation() {
  return net::DefineNetworkTrafficAnnotation("xl_media_controls_download", R"(
      semantics {
        sender: "XL Media Controls"
        description:
          "User downloads a video from the video controls toolbar using the XL "
          "download button."
        trigger: "User clicks the XL download button."
        data: "The media file to download."
        destination: WEBSITE
        last_reviewed: "2026-09-03"
        user_data {
          type: SENSITIVE_URL
        }
        internal {
          contacts {
            owners: "//chrome/browser/OWNERS"
          }
        }
      }
      policy {
        cookies_allowed: YES
        setting:
          "This feature cannot be disabled."
        policy_exception_justification:
          "This is a user-initiated download."
      })");
}

}  // namespace

XenonMediaControlsHost::XenonMediaControlsHost(
    content::RenderFrameHost* frame_host)
    : frame_host_(frame_host) {}

XenonMediaControlsHost::~XenonMediaControlsHost() = default;

void XenonMediaControlsHost::DownloadMediaUrl(
    const GURL& media_url,
    const std::string& suggested_filename) {
  if (!frame_host_ || !media_url.is_valid()) {
    return;
  }

  Profile* profile =
      Profile::FromBrowserContext(frame_host_->GetBrowserContext());
  if (!profile) {
    return;
  }

  content::DownloadManager* download_manager = profile->GetDownloadManager();
  if (!download_manager) {
    return;
  }

  std::unique_ptr<download::DownloadUrlParameters> params =
      frame_host_->CreateDownloadUrlParameters(
          media_url, MediaDownloadTrafficAnnotation());
  if (!params) {
    return;
  }

  if (!suggested_filename.empty()) {
    params->set_suggested_name(base::UTF8ToUTF16(suggested_filename));
  }
  download_manager->DownloadUrl(std::move(params));
}

void BindXenonMediaControlsHost(
    content::RenderFrameHost* frame_host,
    mojo::PendingReceiver<xl_media_controls::mojom::XlMediaControlsHost>
        receiver) {
  mojo::MakeSelfOwnedReceiver(
      std::make_unique<XenonMediaControlsHost>(frame_host),
      std::move(receiver));
}
