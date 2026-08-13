// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/webui/simple_webui_controller.h"

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_ui.h"
#include "content/public/browser/web_ui_data_source.h"
#include "content/public/common/url_constants.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

namespace xenon {

namespace {
// The host name for this WebUI (chrome://simple-webui/)
constexpr char kHost[] = "simple-webui";
}  // namespace

// =============================================================================
// SimpleWebUIController
// =============================================================================

SimpleWebUIController::SimpleWebUIController(content::WebUI* web_ui)
    : content::WebUIController(web_ui) {
  // Create and configure the data source for serving resources
  content::WebUIDataSource* source = content::WebUIDataSource::CreateAndAdd(
      web_ui->GetWebContents()->GetBrowserContext(), kHost);

  // Add static resources (CSS, JS files)
  source->AddResourcePath("index.css", IDR_SIMPLE_WEBUI_INDEX_CSS);
  source->AddResourcePath("index.js", IDR_SIMPLE_WEBUI_INDEX_JS);

  // Set the default resource (served when accessing the root URL)
  source->SetDefaultResource(IDR_SIMPLE_WEBUI_INDEX_HTML);

  // ==========================================================================
  // Register message callbacks for JavaScript chrome.send() calls.
  // ==========================================================================

  web_ui->RegisterMessageCallback(
      "getSystemInfo",
      base::BindRepeating(&SimpleWebUIController::HandleGetSystemInfo,
                          base::Unretained(this)));

  web_ui->RegisterMessageCallback(
      "logMessage",
      base::BindRepeating(&SimpleWebUIController::HandleLogMessage,
                          base::Unretained(this)));

  web_ui->RegisterMessageCallback(
      "performAction",
      base::BindRepeating(&SimpleWebUIController::HandlePerformAction,
                          base::Unretained(this)));

  LOG(INFO) << "SimpleWebUIController: Initialized with message handlers";
}

SimpleWebUIController::~SimpleWebUIController() = default;

// -----------------------------------------------------------------------------
// Message Handlers
// -----------------------------------------------------------------------------

void SimpleWebUIController::HandleGetSystemInfo(
    const base::ListValue& args) {
  // The first argument is the callback ID for response
  if (args.empty() || !args[0].is_string()) {
    LOG(ERROR) << "HandleGetSystemInfo: Missing callback ID";
    return;
  }

  const std::string& callback_id = args[0].GetString();

  // Use mock data instead of real system info
  base::DictValue system_info;
  system_info.Set("operatingSystem", "MockOS");
  system_info.Set("osVersion", "1.0.0");
  system_info.Set("architecture", "x64");
  system_info.Set("cpuCount", 8);
  system_info.Set("physicalMemoryMB", 16384);
  system_info.Set("timestamp", base::Time::Now().InSecondsFSinceUnixEpoch());

  LOG(INFO) << "SimpleWebUIController: Sending mock system info to JavaScript";

  // Send response back to JavaScript using CallJavascriptFunctionUnsafe
  web_ui()->CallJavascriptFunctionUnsafe(
      "cr.webUIResponse", base::Value(callback_id), base::Value(true),
      base::Value(std::move(system_info)));
}

void SimpleWebUIController::HandleLogMessage(const base::ListValue& args) {
  // Expected format: chrome.send('logMessage', ['message text', 'level'])
  if (args.empty() || !args[0].is_string()) {
    LOG(ERROR) << "HandleLogMessage: Invalid arguments";
    return;
  }

  const std::string& message = args[0].GetString();
  std::string level = "info";
  if (args.size() >= 2 && args[1].is_string()) {
    level = args[1].GetString();
  }

  // Log with appropriate level
  if (level == "error") {
    LOG(ERROR) << "[WebUI] " << message;
  } else if (level == "warning") {
    LOG(WARNING) << "[WebUI] " << message;
  } else {
    LOG(INFO) << "[WebUI] " << message;
  }
}

void SimpleWebUIController::HandlePerformAction(
    const base::ListValue& args) {
  // Expected: [callback_id, action_name, ...params]
  if (args.size() < 2 || !args[0].is_string() || !args[1].is_string()) {
    LOG(ERROR) << "HandlePerformAction: Missing callback ID or action name";
    return;
  }

  const std::string& callback_id = args[0].GetString();
  const std::string& action_name = args[1].GetString();

  LOG(INFO) << "SimpleWebUIController: Performing action: " << action_name;

  base::DictValue result;
  result.Set("success", true);
  result.Set("action", action_name);
  result.Set("timestamp", base::Time::Now().InSecondsFSinceUnixEpoch());

  // Handle different actions
  if (action_name == "greet") {
    std::string name = "User";
    if (args.size() >= 3 && args[2].is_string()) {
      name = args[2].GetString();
    }
    result.Set("message", "Hello, " + name + "! This message came from C++.");
  } else if (action_name == "calculate") {
    int a = (args.size() >= 3 && args[2].is_int()) ? args[2].GetInt() : 0;
    int b = (args.size() >= 4 && args[3].is_int()) ? args[3].GetInt() : 0;
    result.Set("result", a + b);
    result.Set("message", "Calculated " + std::to_string(a) + " + " +
                              std::to_string(b) + " = " + std::to_string(a + b));
  } else {
    result.Set("message", "Action '" + action_name + "' executed successfully.");
  }

  // Send response back to JavaScript
  web_ui()->CallJavascriptFunctionUnsafe(
      "cr.webUIResponse", base::Value(callback_id), base::Value(true),
      base::Value(std::move(result)));
}

// =============================================================================
// SimpleWebUIConfig
// =============================================================================

SimpleWebUIConfig::SimpleWebUIConfig()
    : content::WebUIConfig(content::kChromeUIScheme, kHost) {}

SimpleWebUIConfig::~SimpleWebUIConfig() = default;

std::unique_ptr<content::WebUIController>
SimpleWebUIConfig::CreateWebUIController(content::WebUI* web_ui,
                                          const GURL& url) {
  return std::make_unique<SimpleWebUIController>(web_ui);
}

}  // namespace xenon
