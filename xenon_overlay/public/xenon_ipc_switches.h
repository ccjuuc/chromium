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
inline constexpr char kHostedAppName[] = "xenon-hosted-app-name";

}  // namespace xenon::ipc::switches

#endif  // XENON_OVERLAY_PUBLIC_XENON_IPC_SWITCHES_H_
