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
    Close(entry->type(), SidePanelEntryHideReason::kSidePanelClosed,
          /*suppress_animations=*/false);
    return;
  }

  if (entry && IsSidePanelShowing(entry->type()) &&
      waiter(entry->type())->loading_entry() == entry) {
    waiter(entry->type())->ResetLoadingEntryIfNecessary();
    Close(entry->type(), SidePanelEntryHideReason::kSidePanelClosed,
          /*suppress_animations=*/false);
    return;
  }

  if (GetUniqueKeyForKey(key).has_value()) {
    // Use the public `Show(SidePanelEntryKey, ...)` entry point so the call
    // dispatches to WebUIBrowserSidePanelUI::Show(UniqueKey, ...).
    SidePanelUIBase::Show(key, open_trigger, /*suppress_animations=*/false);
  }
