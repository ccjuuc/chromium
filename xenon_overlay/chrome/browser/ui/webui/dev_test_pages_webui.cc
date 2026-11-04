// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/dev_test_pages_webui.h"

#include <string>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/functional/bind.h"
#include "base/path_service.h"
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
constexpr char kXlExtensionStoreTestHost[] = "xenon-extension-store";

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

XlExtensionStoreTestWebUIController::XlExtensionStoreTestWebUIController(
    content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  auto* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kXlExtensionStoreTestHost);
  source->SetDefaultResource(IDR_XENON_EXTENSION_STORE_HTML);
  source->AddResourcePath("index.css", IDR_XENON_EXTENSION_STORE_CSS);
  source->AddResourcePath("index.js", IDR_XENON_EXTENSION_STORE_JS);

  base::FilePath executable;
  base::PathService::Get(base::FILE_EXE, &executable);
  source->AddString("browserExecutable", executable.AsUTF8Unsafe());
  // Source builds normally live in src/out/<configuration>. Packaged builds
  // leave the suggestion empty so the page can ask for the source location.
  const base::FilePath output_root = executable.DirName().DirName();
  source->AddString(
      "suggestedSourceRoot",
      output_root.BaseName() == base::FilePath(FILE_PATH_LITERAL("out"))
          ? output_root.DirName().AsUTF8Unsafe()
          : std::string());
  source->UseStringsJs();
}

XlExtensionStoreTestWebUIController::~XlExtensionStoreTestWebUIController() =
    default;

XlExtensionStoreTestWebUIConfig::XlExtensionStoreTestWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme,
                           kXlExtensionStoreTestHost) {}

XlExtensionStoreTestWebUIConfig::~XlExtensionStoreTestWebUIConfig() = default;

std::unique_ptr<content::WebUIController>
XlExtensionStoreTestWebUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                                       const GURL& url) {
  return std::make_unique<XlExtensionStoreTestWebUIController>(web_ui);
}

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
