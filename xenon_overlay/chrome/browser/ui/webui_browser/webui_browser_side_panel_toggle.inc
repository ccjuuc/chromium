// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Included inside WebUIBrowserSidePanelUI::Toggle when ENABLE_XENON_SERVICE.

  SidePanelEntry* entry = GetActiveContextualEntryForKey(key);
  if (!entry) {
    entry = SidePanelRegistry::From(browser())->GetEntryForKey(key);
  }

  // Mirrors SidePanelCoordinator::Toggle (no views::SidePanel::IsClosing;
  // Webium closes asynchronously via webshell).
  if (entry && IsSidePanelEntryShowing(key)) {
    Close(SidePanelEntryHideReason::kSidePanelClosed,
          /*suppress_animations=*/false);
    return;
  }

  if (entry && IsSidePanelShowing() && waiter()->loading_entry() == entry) {
    waiter()->ResetLoadingEntryIfNecessary();
    Close(SidePanelEntryHideReason::kSidePanelClosed,
          /*suppress_animations=*/false);
    return;
  }

  std::optional<UniqueKey> unique_key = GetUniqueKeyForKey(key);
  if (unique_key.has_value()) {
    Show(unique_key.value(), open_trigger, /*suppress_animations=*/false);
  }
