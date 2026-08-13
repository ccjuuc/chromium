// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_VIDEO_SNIFFER_WEBUI_CONTROLLER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_VIDEO_SNIFFER_WEBUI_CONTROLLER_H_

#include <memory>
#include <string>

#include "base/values.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"

namespace xenon {

// WebUI controller for chrome://video-sniffer/
class VideoSnifferWebUIController : public content::WebUIController {
 public:
  explicit VideoSnifferWebUIController(content::WebUI* web_ui);
  ~VideoSnifferWebUIController() override;

  VideoSnifferWebUIController(const VideoSnifferWebUIController&) = delete;
  VideoSnifferWebUIController& operator=(const VideoSnifferWebUIController&) = delete;

 private:
  void HandleGetSniffedMedia(const base::ListValue& args);
  void HandleClearSniffedMedia(const base::ListValue& args);
  void HandleDownloadMedia(const base::ListValue& args);
  void HandleMergeMedia(const base::ListValue& args);
};

class VideoSnifferWebUIConfig : public content::WebUIConfig {
 public:
  VideoSnifferWebUIConfig();
  ~VideoSnifferWebUIConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_VIDEO_SNIFFER_WEBUI_CONTROLLER_H_
