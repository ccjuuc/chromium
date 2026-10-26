// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_MANAGED_CERTIFICATE_MERGER_H_
#define XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_MANAGED_CERTIFICATE_MERGER_H_

#include "services/network/public/mojom/cert_verifier_service_updater.mojom-forward.h"

namespace xenon::certificates {
struct Snapshot;

// Append a validated snapshot after native policies and user certificates have
// been assembled. This never modifies their trust, constraints or distrust.
void MergeManagedCertificates(
    const Snapshot* snapshot,
    cert_verifier::mojom::AdditionalCertificates* certificates);
}  // namespace xenon::certificates

#endif  // XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_MANAGED_CERTIFICATE_MERGER_H_
