// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/content/browser/video_sniffer_observer.h"

#include "base/logging.h"
#include "base/strings/string_util.h"
#include "content/public/browser/web_contents.h"
#include "third_party/blink/public/mojom/loader/resource_load_info.mojom.h"

#include "xenon_overlay/content/browser/video_sniffer_manager.h"

namespace xenon {

VideoSnifferObserver::VideoSnifferObserver(content::WebContents* web_contents)
    : content::WebContentsObserver(web_contents),
      content::WebContentsUserData<VideoSnifferObserver>(*web_contents) {}

VideoSnifferObserver::~VideoSnifferObserver() = default;

void VideoSnifferObserver::ResourceLoadComplete(
    content::RenderFrameHost* render_frame_host,
    const content::GlobalRequestID& request_id,
    const GURL& original_url,
    const blink::mojom::ResourceLoadInfo& resource_load_info) {
  
  std::string mime_type = resource_load_info.mime_type;
  std::string url = resource_load_info.final_url.spec();

  // 匹配常见的视频/音频 MimeType 或扩展名
  if (mime_type.find("video/") != std::string::npos ||
      mime_type.find("audio/") != std::string::npos ||
      mime_type == "application/vnd.apple.mpegurl" ||
      mime_type == "application/x-mpegurl" ||
      base::EndsWith(url, ".mp4", base::CompareCase::INSENSITIVE_ASCII) ||
      base::EndsWith(url, ".m3u8", base::CompareCase::INSENSITIVE_ASCII)) {
      
      LOG(INFO) << "[Xenon Video Sniffer] Found Media:"
                << "\n  MimeType: " << mime_type 
                << "\n  URL: " << url;
      
      VideoSnifferManager::GetInstance()->AddMedia(mime_type, url);
  }
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(VideoSnifferObserver);

}  // namespace xenon
