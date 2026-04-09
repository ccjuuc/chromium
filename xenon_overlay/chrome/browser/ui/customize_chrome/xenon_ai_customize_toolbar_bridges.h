// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_CUSTOMIZE_CHROME_XENON_AI_CUSTOMIZE_TOOLBAR_BRIDGES_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_CUSTOMIZE_CHROME_XENON_AI_CUSTOMIZE_TOOLBAR_BRIDGES_H_

#include <utility>

#include "chrome/browser/ui/actions/chrome_action_id.h"
#include "chrome/browser/ui/webui/side_panel/customize_chrome/customize_toolbar/customize_toolbar.mojom.h"
#include "xenon_overlay/buildflags/buildflags.h"

namespace xenon {

// Appends Xenon AI to Customize Toolbar list (handler keeps switch/case style
// for Mojo ↔ Chrome action mapping).
#if BUILDFLAG(ENABLE_XENON_AI)
template <typename AddActionFn>
void AppendXenonAiCustomizeToolbarListActions(AddActionFn&& add_action) {
  std::forward<AddActionFn>(add_action)(
      kActionSidePanelShowXenonAI,
      side_panel::customize_chrome::mojom::CategoryId::kTools);
}
#else
template <typename AddActionFn>
void AppendXenonAiCustomizeToolbarListActions(AddActionFn&&) {}
#endif

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_CUSTOMIZE_CHROME_XENON_AI_CUSTOMIZE_TOOLBAR_BRIDGES_H_
