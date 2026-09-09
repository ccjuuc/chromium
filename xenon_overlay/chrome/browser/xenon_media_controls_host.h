// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_XENON_MEDIA_CONTROLS_HOST_H_
#define XENON_OVERLAY_CHROME_BROWSER_XENON_MEDIA_CONTROLS_HOST_H_

#include "components/xl_media_controls/public/mojom/xl_media_controls.mojom.h"
#include "content/public/browser/render_frame_host.h"
#include "url/gurl.h"

class XenonMediaControlsHost
    : public xl_media_controls::mojom::XlMediaControlsHost {
 public:
  explicit XenonMediaControlsHost(content::RenderFrameHost* frame_host);
  ~XenonMediaControlsHost() override;

  XenonMediaControlsHost(const XenonMediaControlsHost&) = delete;
  XenonMediaControlsHost& operator=(const XenonMediaControlsHost&) = delete;

  // xl_media_controls::mojom::XlMediaControlsHost:
  void DownloadMediaUrl(const GURL& media_url,
                        const std::string& suggested_filename) override;

 private:
  raw_ptr<content::RenderFrameHost> frame_host_;
};

void BindXenonMediaControlsHost(
    content::RenderFrameHost* frame_host,
    mojo::PendingReceiver<xl_media_controls::mojom::XlMediaControlsHost>
        receiver);

#endif  // XENON_OVERLAY_CHROME_BROWSER_XENON_MEDIA_CONTROLS_HOST_H_
