// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/ui/views/side_panel/xenon_ai_side_panel_registration.h"

#include "base/functional/bind.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/side_panel/side_panel_entry.h"
#include "chrome/browser/ui/side_panel/side_panel_registry.h"
#include "xenon_overlay/chrome/browser/ui/views/side_panel/xenon_ai_side_panel_web_view.h"

namespace xenon {

void RegisterXenonAiGlobalSidePanelEntry(BrowserWindowInterface* browser,
                                         SidePanelRegistry* window_registry) {
  if (!browser || browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      !window_registry) {
    return;
  }
  window_registry->Register(std::make_unique<SidePanelEntry>(
      SidePanelEntry::Key(SidePanelEntry::Id::kXenonAI),
      base::BindRepeating(&XenonAiSidePanelWebView::Create,
                          browser->GetProfile()),
      base::NullCallback()));
}

}  // namespace xenon
