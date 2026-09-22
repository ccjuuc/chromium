// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/xenon_update_ui_controller.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/weak_ptr.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/net/system_network_context_manager.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/browser/web_ui_message_handler.h"
#include "content/public/common/url_constants.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "url/gurl.h"
#include "xenon_overlay/chrome/browser/updater/xenon_update_manager.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

namespace {

constexpr char kUpdateHost[] = "xenon-update";

constexpr net::NetworkTrafficAnnotationTag kScenarioTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("xenon_update_scenario", R"(
        semantics {
          sender: "Xenon Application Updater Scenario Switcher"
          description: "Switches mock scenario on the local update server."
          trigger: "User clicked scenario button in chrome://xenon-update."
          data: "None."
          destination: LOCAL
        }
        policy {
          cookies_allowed: NO
          setting: "This feature cannot be disabled."
          policy_exception_justification: "Development and testing only."
        })");

class XenonUpdateMessageHandler
    : public content::WebUIMessageHandler,
      public updater::XenonUpdateManager::Observer {
 public:
  XenonUpdateMessageHandler() = default;
  ~XenonUpdateMessageHandler() override {
    updater::XenonUpdateManager::GetInstance()->RemoveObserver(this);
  }

  void RegisterMessages() override {
    updater::XenonUpdateManager::GetInstance()->AddObserver(this);

    web_ui()->RegisterMessageCallback(
        "checkForUpdates",
        base::BindRepeating(&XenonUpdateMessageHandler::HandleCheckForUpdates,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "downloadUpdate",
        base::BindRepeating(&XenonUpdateMessageHandler::HandleDownloadUpdate,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "quitAndInstall",
        base::BindRepeating(&XenonUpdateMessageHandler::HandleQuitAndInstall,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "setFeedURL",
        base::BindRepeating(&XenonUpdateMessageHandler::HandleSetFeedURL,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "getInitialState",
        base::BindRepeating(&XenonUpdateMessageHandler::HandleGetInitialState,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "switchScenario",
        base::BindRepeating(&XenonUpdateMessageHandler::HandleSwitchScenario,
                            base::Unretained(this)));

    web_ui()->RegisterMessageCallback(
        "cleanupOldVersions",
        base::BindRepeating(&XenonUpdateMessageHandler::HandleCleanupOldVersions,
                            base::Unretained(this)));
  }

  void OnJavascriptAllowed() override {}
  void OnJavascriptDisallowed() override {
    scenario_loader_.reset();
  }

  // XenonUpdateManager::Observer:
  void OnCheckingForUpdate() override {
    if (IsJavascriptAllowed()) {
      FireWebUIListener("checking-for-update");
    }
  }

  void OnUpdateAvailable(const updater::UpdateManifest& manifest) override {
    if (IsJavascriptAllowed()) {
      FireWebUIListener("update-available", manifest.ToValue());
    }
  }

  void OnUpdateNotAvailable(const std::string& current_version) override {
    if (IsJavascriptAllowed()) {
      FireWebUIListener("update-not-available", base::Value(current_version));
    }
  }

  void OnDownloadProgress(const updater::DownloadProgress& progress) override {
    if (IsJavascriptAllowed()) {
      base::DictValue dict;
      dict.Set("percent", progress.percent);
      dict.Set("bytes_downloaded", static_cast<double>(progress.bytes_downloaded));
      dict.Set("total_bytes", static_cast<double>(progress.total_bytes));
      FireWebUIListener("download-progress", dict);
    }
  }

  void OnUpdateDownloaded(const updater::UpdateManifest& manifest) override {
    if (IsJavascriptAllowed()) {
      FireWebUIListener("update-downloaded", manifest.ToValue());
    }
  }

  void OnUpdateError(const std::string& error_message) override {
    if (IsJavascriptAllowed()) {
      FireWebUIListener("update-error", base::Value(error_message));
    }
  }

 private:
  void HandleCheckForUpdates(const base::ListValue& args) {
    AllowJavascript();
    std::string feed_url;
    if (!args.empty() && args[0].is_string()) {
      feed_url = args[0].GetString();
    }
    updater::XenonUpdateManager::GetInstance()->CheckForUpdates(feed_url);
  }

  void HandleDownloadUpdate(const base::ListValue& args) {
    AllowJavascript();
    updater::XenonUpdateManager::GetInstance()->DownloadUpdate();
  }

  void HandleQuitAndInstall(const base::ListValue& args) {
    AllowJavascript();
    updater::XenonUpdateManager::GetInstance()->QuitAndInstall();
  }

  void HandleCleanupOldVersions(const base::ListValue& args) {
    AllowJavascript();
    updater::XenonUpdateManager::GetInstance()->CleanupOldVersions();
  }

  void HandleSetFeedURL(const base::ListValue& args) {
    AllowJavascript();
    if (!args.empty() && args[0].is_string()) {
      updater::XenonUpdateManager::GetInstance()->SetFeedURL(args[0].GetString());
    }
  }

  void HandleGetInitialState(const base::ListValue& args) {
    AllowJavascript();
    auto* manager = updater::XenonUpdateManager::GetInstance();
    base::DictValue info;
    info.Set("current_version", manager->GetCurrentVersion());
    info.Set("feed_url", manager->GetFeedURL());
    info.Set("state", updater::UpdateStateToString(manager->GetState()));
    if (manager->GetManifest()) {
      info.Set("manifest", manager->GetManifest()->ToValue().Clone());
    }
    FireWebUIListener("initial-state", info);
  }

  void HandleSwitchScenario(const base::ListValue& args) {
    AllowJavascript();
    if (args.empty() || !args[0].is_string()) {
      return;
    }
    std::string mode = args[0].GetString();
    std::string feed_url = "http://127.0.0.1:8999/api/v1/update/check";
    if (args.size() > 1 && args[1].is_string()) {
      std::string custom_url = args[1].GetString();
      if (!custom_url.empty()) {
        feed_url = custom_url;
      }
    }

    GURL base_gurl(feed_url);
    GURL scenario_url = base_gurl.Resolve("/api/v1/scenario?mode=" + mode);

    auto request = std::make_unique<network::ResourceRequest>();
    request->url = scenario_url;
    request->method = "GET";

    if (!g_browser_process || !g_browser_process->system_network_context_manager()) {
      return;
    }
    auto factory = g_browser_process->system_network_context_manager()
                       ->GetSharedURLLoaderFactory();
    if (!factory) {
      return;
    }

    scenario_loader_ = network::SimpleURLLoader::Create(
        std::move(request), kScenarioTrafficAnnotation);
    scenario_loader_->DownloadToString(
        factory.get(),
        base::BindOnce(&XenonUpdateMessageHandler::OnScenarioResponse,
                       weak_ptr_factory_.GetWeakPtr(), mode, feed_url),
        64 * 1024);
  }

  void OnScenarioResponse(std::string mode,
                          std::string feed_url,
                          std::optional<std::string> response) {
    scenario_loader_.reset();
    if (IsJavascriptAllowed()) {
      FireWebUIListener("scenario-switched", base::Value(mode));
    }
    updater::XenonUpdateManager::GetInstance()->CheckForUpdates(feed_url);
  }

  std::unique_ptr<network::SimpleURLLoader> scenario_loader_;
  base::WeakPtrFactory<XenonUpdateMessageHandler> weak_ptr_factory_{this};
};

}  // namespace

XenonUpdateUIController::XenonUpdateUIController(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kUpdateHost);
  source->AddResourcePath("index.css", IDR_XENON_UPDATE_CSS);
  source->AddResourcePath("index.js", IDR_XENON_UPDATE_JS);
  source->SetDefaultResource(IDR_XENON_UPDATE_HTML);
  source->DisableTrustedTypesCSP();

  web_ui->AddMessageHandler(std::make_unique<XenonUpdateMessageHandler>());
}

XenonUpdateUIController::~XenonUpdateUIController() = default;

XenonUpdateConfig::XenonUpdateConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kUpdateHost) {}

XenonUpdateConfig::~XenonUpdateConfig() = default;

std::unique_ptr<content::WebUIController>
XenonUpdateConfig::CreateWebUIController(content::WebUI* web_ui,
                                         const GURL& url) {
  return std::make_unique<XenonUpdateUIController>(web_ui);
}

}  // namespace xenon
