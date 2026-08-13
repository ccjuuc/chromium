// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CONTENT_BROWSER_VIDEO_SNIFFER_OBSERVER_H_
#define XENON_OVERLAY_CONTENT_BROWSER_VIDEO_SNIFFER_OBSERVER_H_

#include "content/common/content_export.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom-forward.h"
#include "base/no_destructor.h"

namespace content {
class WebContents;
struct GlobalRequestID;
}

namespace xenon {

class CONTENT_EXPORT VideoSnifferObserver 
    : public content::WebContentsObserver,
      public content::WebContentsUserData<VideoSnifferObserver> {
 public:
  ~VideoSnifferObserver() override;

  VideoSnifferObserver(const VideoSnifferObserver&) = delete;
  VideoSnifferObserver& operator=(const VideoSnifferObserver&) = delete;

 private:
  explicit VideoSnifferObserver(content::WebContents* web_contents);
  friend class content::WebContentsUserData<VideoSnifferObserver>;

  // content::WebContentsObserver implementation:
  void ResourceLoadComplete(
      content::RenderFrameHost* render_frame_host,
      const content::GlobalRequestID& request_id,
      const GURL& original_url,
      const blink::mojom::ResourceLoadInfo& resource_load_info) override;

  WEB_CONTENTS_USER_DATA_KEY_DECL();
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CONTENT_BROWSER_VIDEO_SNIFFER_OBSERVER_H_
