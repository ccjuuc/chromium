// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_XENON_AI_SIDE_PANEL_REGISTRATION_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_XENON_AI_SIDE_PANEL_REGISTRATION_H_

class BrowserWindowInterface;
class SidePanelRegistry;

namespace xenon {

// Registers the global Xenon AI side panel entry (before WebUI-browser return).
void RegisterXenonAiGlobalSidePanelEntry(BrowserWindowInterface* browser,
                                         SidePanelRegistry* window_registry);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_VIEWS_SIDE_PANEL_XENON_AI_SIDE_PANEL_REGISTRATION_H_
