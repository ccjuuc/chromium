// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_DEV_TEST_PAGES_WEBUI_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_DEV_TEST_PAGES_WEBUI_H_

#include <memory>

#include "base/values.h"
#include "content/public/browser/web_ui_controller.h"
#include "content/public/browser/webui_config.h"

namespace xenon {

// chrome://local-video-test/ — remote mp4 playback smoke test.
class LocalVideoTestWebUIController : public content::WebUIController {
 public:
  explicit LocalVideoTestWebUIController(content::WebUI* web_ui);
  ~LocalVideoTestWebUIController() override;

  LocalVideoTestWebUIController(const LocalVideoTestWebUIController&) = delete;
  LocalVideoTestWebUIController& operator=(
      const LocalVideoTestWebUIController&) = delete;
};

class LocalVideoTestWebUIConfig : public content::WebUIConfig {
 public:
  LocalVideoTestWebUIConfig();
  ~LocalVideoTestWebUIConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

// chrome://render-dll-test/ — window.renderDll load/invoke smoke test.
class RenderDllTestWebUIController : public content::WebUIController {
 public:
  explicit RenderDllTestWebUIController(content::WebUI* web_ui);
  ~RenderDllTestWebUIController() override;

  RenderDllTestWebUIController(const RenderDllTestWebUIController&) = delete;
  RenderDllTestWebUIController& operator=(const RenderDllTestWebUIController&) =
      delete;

 private:
  void HandleGetDefaultDllPath(const base::ListValue& args);
};

class RenderDllTestWebUIConfig : public content::WebUIConfig {
 public:
  RenderDllTestWebUIConfig();
  ~RenderDllTestWebUIConfig() override;

  std::unique_ptr<content::WebUIController> CreateWebUIController(
      content::WebUI* web_ui,
      const GURL& url) override;
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_WEBUI_DEV_TEST_PAGES_WEBUI_H_
