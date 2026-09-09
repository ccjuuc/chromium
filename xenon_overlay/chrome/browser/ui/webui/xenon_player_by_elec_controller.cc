// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_player_by_elec_controller.h"

#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

namespace {

constexpr char kPlayerByElecHost[] = "xenon-player-by-elec";

}  // namespace

XenonPlayerByElecController::XenonPlayerByElecController(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kPlayerByElecHost);
  source->AddResourcePath("index.css", IDR_XENON_PLAYER_BY_ELEC_CSS);
  source->AddResourcePath("index.js", IDR_XENON_PLAYER_BY_ELEC_JS);
  source->SetDefaultResource(IDR_XENON_PLAYER_BY_ELEC_HTML);
}

XenonPlayerByElecController::~XenonPlayerByElecController() = default;

XenonPlayerByElecConfig::XenonPlayerByElecConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kPlayerByElecHost) {}

XenonPlayerByElecConfig::~XenonPlayerByElecConfig() = default;

std::unique_ptr<content::WebUIController>
XenonPlayerByElecConfig::CreateWebUIController(content::WebUI* web_ui,
                                               const GURL& url) {
  return std::make_unique<XenonPlayerByElecController>(web_ui);
}

}  // namespace xenon
