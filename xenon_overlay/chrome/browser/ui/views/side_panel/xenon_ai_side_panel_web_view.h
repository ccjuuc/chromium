// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_XENON_AI_SIDE_PANEL_WEB_VIEW_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_XENON_AI_SIDE_PANEL_WEB_VIEW_H_

#include <memory>

#include "base/functional/callback_forward.h"
#include "chrome/browser/ui/views/side_panel/side_panel_web_ui_view.h"
#include "chrome/browser/ui/webui/top_chrome/webui_contents_wrapper.h"
#include "ui/base/metadata/metadata_header_macros.h"

class Profile;
class SidePanelEntryScope;

namespace views {
class View;
}  // namespace views

namespace xenon {

class XenonAiSidePanelUI;

// SidePanelWebUIView adapter; WebUI + domain service live under //xenon_overlay.
class XenonAiSidePanelWebView : public SidePanelWebUIViewT<XenonAiSidePanelUI> {
  using SidePanelWebUIViewT_XenonAiSidePanelUI =
      SidePanelWebUIViewT<XenonAiSidePanelUI>;
  METADATA_HEADER(XenonAiSidePanelWebView,
                  SidePanelWebUIViewT_XenonAiSidePanelUI)

 public:
  static std::unique_ptr<views::View> Create(Profile* profile,
                                             SidePanelEntryScope& scope);

  XenonAiSidePanelWebView(const XenonAiSidePanelWebView&) = delete;
  XenonAiSidePanelWebView& operator=(const XenonAiSidePanelWebView&) = delete;
  ~XenonAiSidePanelWebView() override;

 private:
  XenonAiSidePanelWebView(
      SidePanelEntryScope& scope,
      base::RepeatingClosure close_cb,
      std::unique_ptr<WebUIContentsWrapperT<XenonAiSidePanelUI>>
          contents_wrapper);
};

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_XENON_AI_SIDE_PANEL_WEB_VIEW_H_
