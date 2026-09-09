// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_thunder_2025_controller.h"

#include <memory>
#include <string>
#include <vector>

#include "content/public/browser/web_ui.h"
#include "content/public/common/url_constants.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_player_electron_controller.h"

namespace xenon {

namespace {

constexpr char kThunder2025Host[] = "thunder-2025";
constexpr char kThunderFrontendDirSwitch[] =
    "xenon-thunder-2025-frontend-dir";

}  // namespace

XenonThunder2025Config::XenonThunder2025Config()
    : content::WebUIConfig(content::kChromeUIScheme, kThunder2025Host) {}

XenonThunder2025Config::~XenonThunder2025Config() = default;

std::unique_ptr<content::WebUIController>
XenonThunder2025Config::CreateWebUIController(content::WebUI* web_ui,
                                              const GURL& url) {
  // The hosted Electron controller owns the generic Node Mojo lifecycle.
  // Product-specific controllers only supply routing and asset policy.
  return std::make_unique<XenonPlayerElectronController>(
      web_ui, kThunder2025Host, kThunderFrontendDirSwitch,
      "thunder_2025/resources/app/renderer.asar/main-renderer",
      std::vector<std::string>{"xunlei.com"});
}

}  // namespace xenon
