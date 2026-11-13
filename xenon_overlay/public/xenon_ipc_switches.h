// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_PUBLIC_XENON_IPC_SWITCHES_H_
#define XENON_OVERLAY_PUBLIC_XENON_IPC_SWITCHES_H_

namespace xenon::ipc::switches {

inline constexpr char kMainScript[] = "xenon-main-js";
inline constexpr char kElectronApp[] = "xenon-electron-app";
inline constexpr char kEnable[] = "xenon-electron-ipc";
inline constexpr char kAllowAllOrigins[] = "xenon-ipc-allow-all-origins";
inline constexpr char kAllowedOrigins[] = "xenon-ipc-allowed-origins";
// Browser-set eligibility hint for a renderer in an Electron storage partition.
// It only avoids unnecessary probes; document IPC still authorizes every call.
inline constexpr char kRendererIpcCandidate[] = "xenon-renderer-ipc-candidate";
inline constexpr char kHostedAppName[] = "xenon-hosted-app-name";
// Browser-set marker for a Utility dedicated to one Electron application.
inline constexpr char kIsolatedElectronService[] = "xenon-isolated-electron-service";
// Browser-set marker for an independent Electron utilityProcess.fork child.
inline constexpr char kUtilityProcess[] = "xenon-utility-process";

}  // namespace xenon::ipc::switches

#endif  // XENON_OVERLAY_PUBLIC_XENON_IPC_SWITCHES_H_
