// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/dev_test_pages_webui.h"

#include "base/functional/bind.h"
#include "base/values.h"
#include "chrome/common/beijing/render_dll_names.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {
namespace {

constexpr char kLocalVideoTestHost[] = "local-video-test";

void ApplyInlinePageCsp(content::WebUIDataSource* source, bool allow_https_media) {
  source->DisableTrustedTypesCSP();
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::ScriptSrc,
      "script-src chrome://resources 'self' 'unsafe-inline';");
  source->OverrideContentSecurityPolicy(
      network::mojom::CSPDirectiveName::StyleSrc,
      "style-src 'self' 'unsafe-inline';");
  if (allow_https_media) {
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::MediaSrc,
        "media-src https: http: blob: 'self';");
    source->OverrideContentSecurityPolicy(
        network::mojom::CSPDirectiveName::ConnectSrc,
        "connect-src https: http: 'self';");
  }
}

}  // namespace

LocalVideoTestWebUIController::LocalVideoTestWebUIController(
    content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kLocalVideoTestHost);
  ApplyInlinePageCsp(source, /*allow_https_media=*/true);
  source->SetDefaultResource(IDR_LOCAL_VIDEO_TEST_HTML);
}

LocalVideoTestWebUIController::~LocalVideoTestWebUIController() = default;

LocalVideoTestWebUIConfig::LocalVideoTestWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kLocalVideoTestHost) {}

LocalVideoTestWebUIConfig::~LocalVideoTestWebUIConfig() = default;

std::unique_ptr<content::WebUIController>
LocalVideoTestWebUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                                 const GURL& url) {
  return std::make_unique<LocalVideoTestWebUIController>(web_ui);
}

RenderDllTestWebUIController::RenderDllTestWebUIController(
    content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(),
      beijing::kRenderDllTestHost);
  ApplyInlinePageCsp(source, /*allow_https_media=*/false);
  source->SetDefaultResource(IDR_RENDER_DLL_TEST_HTML);

  web_ui->RegisterMessageCallback(
      "getDefaultDllPath",
      base::BindRepeating(&RenderDllTestWebUIController::HandleGetDefaultDllPath,
                          base::Unretained(this)));
}

RenderDllTestWebUIController::~RenderDllTestWebUIController() = default;

void RenderDllTestWebUIController::HandleGetDefaultDllPath(
    const base::ListValue& args) {
  const base::FilePath path = beijing::RenderDllPathNextToExe();
  web_ui()->CallJavascriptFunctionUnsafe(
      "__setDefaultDllPath", base::Value(path.AsUTF8Unsafe()));
}

RenderDllTestWebUIConfig::RenderDllTestWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme,
                           beijing::kRenderDllTestHost) {}

RenderDllTestWebUIConfig::~RenderDllTestWebUIConfig() = default;

std::unique_ptr<content::WebUIController>
RenderDllTestWebUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                                const GURL& url) {
  return std::make_unique<RenderDllTestWebUIController>(web_ui);
}

}  // namespace xenon
