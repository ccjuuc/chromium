// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/video_sniffer_webui_controller.h"

#include "base/functional/bind.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"
#include "xenon_overlay/content/browser/video_sniffer_manager.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_request_utils.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"

namespace xenon {

namespace {
constexpr char kHost[] = "video-sniffer";
}  // namespace

VideoSnifferWebUIController::VideoSnifferWebUIController(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kHost);

  source->AddResourcePath("index.css", IDR_VIDEO_SNIFFER_WEBUI_INDEX_CSS);
  source->AddResourcePath("index.js", IDR_VIDEO_SNIFFER_WEBUI_INDEX_JS);
  source->SetDefaultResource(IDR_VIDEO_SNIFFER_WEBUI_INDEX_HTML);

  web_ui->RegisterMessageCallback(
      "getSniffedMedia",
      base::BindRepeating(&VideoSnifferWebUIController::HandleGetSniffedMedia,
                          base::Unretained(this)));

  web_ui->RegisterMessageCallback(
      "clearSniffedMedia",
      base::BindRepeating(&VideoSnifferWebUIController::HandleClearSniffedMedia,
                          base::Unretained(this)));

  web_ui->RegisterMessageCallback(
      "downloadMedia",
      base::BindRepeating(&VideoSnifferWebUIController::HandleDownloadMedia,
                          base::Unretained(this)));

  web_ui->RegisterMessageCallback(
      "mergeMedia",
      base::BindRepeating(&VideoSnifferWebUIController::HandleMergeMedia,
                          base::Unretained(this)));
}

VideoSnifferWebUIController::~VideoSnifferWebUIController() = default;

void VideoSnifferWebUIController::HandleGetSniffedMedia(
    const base::ListValue& args) {
  auto groups = VideoSnifferManager::GetInstance()->GetGroups();
  
  base::ListValue result_list;
  for (const auto& group : groups) {
    base::DictValue item;
    item.Set("mimeType", group.mime_type);
    item.Set("url", group.main_url);
    item.Set("segmentCount", static_cast<int>(group.segments.size()));
    result_list.Append(std::move(item));
  }

  web_ui()->CallJavascriptFunctionUnsafe(
      "receiveSniffedMedia", base::Value(std::move(result_list)));
}

void VideoSnifferWebUIController::HandleClearSniffedMedia(
    const base::ListValue& args) {
  VideoSnifferManager::GetInstance()->ClearMedia();
}

void VideoSnifferWebUIController::HandleDownloadMedia(
    const base::ListValue& args) {
  if (args.empty() || !args[0].is_string())
    return;

  std::string url_str = args[0].GetString();
  GURL url(url_str);
  if (!url.is_valid())
    return;

  if (url_str.find(".m3u8") != std::string::npos) {
    LOG(INFO) << "[Xenon] Triggering HLS batch download for: " << url_str;
    VideoSnifferManager::GetInstance()->SynthesizeGroup(url_str, web_ui()->GetWebContents()->GetBrowserContext());
    return;
  }

  content::WebContents* web_contents = web_ui()->GetWebContents();
  content::BrowserContext* browser_context = web_contents->GetBrowserContext();
  content::DownloadManager* download_manager =
      browser_context->GetDownloadManager();

  std::unique_ptr<download::DownloadUrlParameters> params =
      content::DownloadRequestUtils::CreateDownloadForWebContentsMainFrame(
          web_contents, url, MISSING_TRAFFIC_ANNOTATION);
  
  download_manager->DownloadUrl(std::move(params));
  LOG(INFO) << "[Xenon Video Sniffer] Triggered direct download for: " << url;
}

void VideoSnifferWebUIController::HandleMergeMedia(
    const base::ListValue& args) {
  if (args.empty() || !args[0].is_string())
    return;

  std::string url_str = args[0].GetString();
  LOG(INFO) << "[Xenon Video Sniffer] Triggered synthesis/merge for: " << url_str;
  
  VideoSnifferManager::GetInstance()->SynthesizeGroup(url_str, web_ui()->GetWebContents()->GetBrowserContext());
  
  // 向前端发送一个模拟的成功提示
  web_ui()->CallJavascriptFunctionUnsafe("onSynthesisTriggered", base::Value(url_str));
}

VideoSnifferWebUIConfig::VideoSnifferWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kHost) {}

VideoSnifferWebUIConfig::~VideoSnifferWebUIConfig() = default;

std::unique_ptr<content::WebUIController>
VideoSnifferWebUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                               const GURL& url) {
  return std::make_unique<VideoSnifferWebUIController>(web_ui);
}

}  // namespace xenon
