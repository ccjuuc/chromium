// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/views/side_panel/xenon_ai_side_panel_web_view.h"

#include "base/memory/ptr_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/side_panel/side_panel_entry_scope.h"
#include "chrome/common/webui_url_constants.h"
#include "ui/base/metadata/metadata_impl_macros.h"
#include "xenon_overlay/chrome/browser/ui/webui/xenon_ai/xenon_ai_side_panel_ui.h"
#include "xenon_overlay/resources/grit/xenon_resources.h"

using SidePanelWebUIViewT_XenonAiSidePanelUI =
    SidePanelWebUIViewT<xenon::XenonAiSidePanelUI>;
BEGIN_TEMPLATE_METADATA(SidePanelWebUIViewT_XenonAiSidePanelUI, SidePanelWebUIViewT)
END_METADATA

namespace xenon {

// static
std::unique_ptr<views::View> XenonAiSidePanelWebView::Create(
    Profile* profile,
    SidePanelEntryScope& scope) {
  return base::WrapUnique(new XenonAiSidePanelWebView(
      scope, base::RepeatingClosure(),
      std::make_unique<WebUIContentsWrapperT<XenonAiSidePanelUI>>(
          GURL(chrome::kChromeUIXenonAISidePanelURL), profile,
          IDS_XENON_AI_SIDE_PANEL_TASK_MANAGER_TITLE,
          /*esc_closes_ui=*/false)));
}

XenonAiSidePanelWebView::XenonAiSidePanelWebView(
    SidePanelEntryScope& scope,
    base::RepeatingClosure close_cb,
    std::unique_ptr<WebUIContentsWrapperT<XenonAiSidePanelUI>> contents_wrapper)
    : SidePanelWebUIViewT<XenonAiSidePanelUI>(
          scope,
          base::RepeatingClosure(),
          std::move(close_cb),
          std::move(contents_wrapper)) {}

XenonAiSidePanelWebView::~XenonAiSidePanelWebView() = default;

bool XenonAiSidePanelWebView::HandleContextMenu(
    content::RenderFrameHost& /*render_frame_host*/,
    const content::ContextMenuParams& /*params*/) {
  // WebUIContentsWrapper::Host default implementation ignores context menus.
  // Returning false here allows the wrapped WebContents to show its default
  // context menu (including DevTools actions).
  return false;
}

BEGIN_METADATA(XenonAiSidePanelWebView)
END_METADATA

}  // namespace xenon
