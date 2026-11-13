// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_COMMON_IPC_XENON_RENDERER_COMMAND_LINE_H_
#define XENON_OVERLAY_COMMON_IPC_XENON_RENDERER_COMMAND_LINE_H_

#include <string_view>
#include <vector>

#include "base/command_line.h"
#include "base/values.h"
#include "url/origin.h"

namespace xenon::ipc {

// Send Browser-owned eligibility hints, never an authorization capability.
// Ordinary renderers can skip configuration IPC unless their main document
// matches an explicit host allowlist. Application partitions still ask Browser.
// In-process renderer diagnostics cannot carry a per-child partition hint and
// conservatively retain the Browser probe.
void AppendElectronIpcRendererHints(const base::CommandLine& browser,
                                    bool application_partition,
                                    base::CommandLine* renderer);

class ElectronRendererIpcHint {
 public:
  explicit ElectronRendererIpcHint(const base::CommandLine& command_line);
  ~ElectronRendererIpcHint();

  bool MayRequestConfig(const url::Origin& origin, bool main_frame) const;

 private:
  bool enabled_ = false;
  bool application_partition_ = false;
  bool allow_all_origins_ = false;
  std::vector<url::Origin> allowed_origins_;
};

// Only renderer configuration is inherited by an Electron application's child
// renderers. Browser/GPU/transport/sandbox switches belong to the shared host.
bool IsElectronRendererSwitch(std::string_view name);

// Transport overrides separately because Content appends its own browser
// switches after ContentBrowserClient::AppendExtraCommandLineSwitches().
void AppendElectronRendererOverrides(const base::DictValue& overrides,
                                     base::CommandLine* command_line);
// Called in the child before Content creates the FeatureList or starts Blink.
// Invalid input is ignored as a whole; process identity cannot be overridden.
void ApplyElectronRendererOverrides(base::CommandLine* command_line);

}  // namespace xenon::ipc
#endif
