// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_ADAPTER_H_
#define XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_ADAPTER_H_

#include <string>
#include <string_view>

#include "base/files/file_path.h"
#include "base/memory/raw_span.h"
#include "base/values.h"
#include "xenon_overlay/chrome/browser/ui/xenon_mac_player_callbacks.h"

namespace xenon {

struct MacPlayerSubscriptionContract {
  std::string_view attach;
  std::string_view detach;
};

// An explicitly registered SDK contract, separate from window ownership and
// IPC transport. All strings/spans have static lifetime. A bundle declaration
// selects a version; it cannot supply executable paths or native method names.
struct MacPlayerAdapter {
  std::string_view id;
  int version;
  std::string_view module_relative_path;
  std::string_view initialize_export;
  std::string_view shutdown_export;
  std::string_view root_constructor;
  std::string_view bind_view_export;
  std::string_view close_media_method;
  std::string_view close_view_method;
  base::raw_span<const std::string_view> initialization_identity_keys;
  base::raw_span<const std::string_view> allowed_exports;
  base::raw_span<const std::string_view> excluded_instance_methods;
  base::raw_span<const MacPlayerSubscriptionContract> subscriptions;
  base::raw_span<const MacPlayerCallbackContract> callbacks;

  bool AllowsExport(std::string_view name) const;
  bool IsProviderRegistration(std::string_view name) const;
  std::string_view DetachMethod(std::string_view method) const;
  bool ExcludesInstanceMethod(std::string_view method) const;
};

// Returns only compile-time registered adapter versions.
const MacPlayerAdapter* FindMacPlayerAdapter(std::string_view id, int version);

struct ResolvedMacPlayerAdapter {
  raw_ptr<const MacPlayerAdapter> adapter = nullptr;
  std::string module_path;
};

// Blocking: reads xenon-native-player.json from the resources directory of an
// app embedded directly under the outer bundle's Contents/Resources/xenon-apps.
// Canonical containment is checked for the app, declaration, and native module.
// External-addon flags and user-provided descriptors cannot extend this trust.
ResolvedMacPlayerAdapter ResolveBundledMacPlayerAdapter(
    const base::FilePath& outer_bundle,
    const base::FilePath& resources);

}  // namespace xenon

#endif  // XENON_OVERLAY_CHROME_BROWSER_UI_XENON_MAC_PLAYER_ADAPTER_H_
