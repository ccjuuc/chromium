// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_GM_GM_SYSTEM_TRUST_WIN_H_
#define CHROME_BROWSER_GM_GM_SYSTEM_TRUST_WIN_H_

#include <windows.h>

#include <wincrypt.h>

#include "base/functional/callback.h"
#include "services/network/public/mojom/ssl_config.mojom.h"

namespace gm_browser {

// Blocking local-store enumeration. Call on a MayBlock worker, never for each
// connection. Reads registry-backed ROOT/CA/Disallowed stores only; it cannot
// retrieve roots, intermediate certificates, or revocation data from a server.
// Any incomplete snapshot fails closed with an enabled, empty trust snapshot.
network::mojom::GmAutoConfigPtr ReadGmSystemTrust();

// Owns independent, continuously rearmed Windows store notifications on a
// blocking sequence. The callback runs on the caller's sequence: true after
// monitoring starts and after each change, false if monitoring fails. Run the
// returned closure to stop; destruction also releases the monitor. No ordinary
// TLS verification or CertDatabase reader is needed to rearm notifications.
base::OnceClosure StartGmSystemTrustMonitoring(
    base::RepeatingCallback<void(bool)> on_change);

// Exercises the same event/resync/rearm state machine without changing OS
// trust. The control receives CERT_STORE_CTRL_NOTIFY_CHANGE or RESYNC and the
// manual-reset event registered with the stores. Called on a blocking worker.
base::OnceClosure StartGmSystemTrustMonitoringForTesting(
    base::RepeatingCallback<bool(DWORD, HANDLE*)> store_control,
    base::RepeatingCallback<void(bool)> on_change);

// The same conversion over caller-owned memory stores. Does not access the
// machine's stores, making trust/usage/distrust tests hermetic.
network::mojom::GmAutoConfigPtr ReadGmSystemTrustFromStoresForTesting(
    HCERTSTORE roots,
    HCERTSTORE intermediates,
    HCERTSTORE disallowed);

// Browser-test-only replacement of the worker's snapshot result. Changes are
// forwarded through CertDatabase and the production SSLConfig Mojo path.
// nullptr restores real local-store reads. Never changes any Windows store.
void SetGmSystemTrustSnapshotForTesting(
    network::mojom::GmAutoConfigPtr snapshot);

}  // namespace gm_browser

#endif  // CHROME_BROWSER_GM_GM_SYSTEM_TRUST_WIN_H_
