// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_CRASH_REPORTER_BRIDGE_H_
#define XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_CRASH_REPORTER_BRIDGE_H_

#include <string>

#include "base/containers/span.h"
#include "base/values.h"
#include "xenon_overlay/chrome/browser/ipc/xenon_electron_api_bridge.h"

namespace crash_reporter {
struct Report;
}

namespace xenon::ipc {

// The host owns collection, upload policy and destination. This bridge only
// reads the shared Browser database, on a worker that permits disk access.
void CallCrashReporterApi(const std::string& operation,
                          ElectronApiCallback callback);

// Converts only successfully uploaded reports, newest crash first. Exposed for
// tests that must not create real crashes or modify the user's crash database.
base::ListValue UploadedCrashReportsToValue(
    base::span<const crash_reporter::Report> reports);

}  // namespace xenon::ipc

#endif  // XENON_OVERLAY_CHROME_BROWSER_IPC_XENON_CRASH_REPORTER_BRIDGE_H_
