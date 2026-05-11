// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CONTENT_BROWSER_VIDEO_SNIFFER_MANAGER_H_
#define XENON_OVERLAY_CONTENT_BROWSER_VIDEO_SNIFFER_MANAGER_H_

#include <map>
#include <string>
#include <vector>

#include "base/no_destructor.h"
#include "content/common/content_export.h"

namespace content {
class BrowserContext;
}

namespace xenon {

struct CONTENT_EXPORT SniffedMedia {
  SniffedMedia(std::string m, std::string u);
  SniffedMedia(const SniffedMedia& other);
  ~SniffedMedia();

  std::string mime_type;
  std::string url;
};

struct CONTENT_EXPORT MediaGroup {
  MediaGroup();
  MediaGroup(const MediaGroup& other);
  ~MediaGroup();

  std::string main_url;
  std::string mime_type;
  std::vector<std::string> segments;
};

class CONTENT_EXPORT VideoSnifferManager {
 public:
  static VideoSnifferManager* GetInstance();

  void AddMedia(const std::string& mime_type, const std::string& url);
  std::vector<MediaGroup> GetGroups() const;
  void ClearMedia();

  // Trigger download and synthesis for a specific main URL
  void SynthesizeGroup(const std::string& main_url, content::BrowserContext* context);

 private:
  MediaGroup* GetGroupForUrl(const std::string& url);
  friend class base::NoDestructor<VideoSnifferManager>;
  VideoSnifferManager();
  ~VideoSnifferManager();

  std::map<std::string, MediaGroup> groups_;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CONTENT_BROWSER_VIDEO_SNIFFER_MANAGER_H_
