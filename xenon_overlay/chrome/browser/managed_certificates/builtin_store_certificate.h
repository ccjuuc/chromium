// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_BUILTIN_STORE_CERTIFICATE_H_
#define XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_BUILTIN_STORE_CERTIFICATE_H_

#include "services/network/public/mojom/cert_verifier_service_updater.mojom-forward.h"

namespace xenon::certificates {
struct Certificate;

// The public CA is compiled into the browser and parsed once. Standard TLS
// certificate chain, validity and hostname checks apply.
const Certificate& GetBuiltinStoreCertificate();

// Call after assembling native trust and before merging downloaded managed
// certificates, including for the first verifier configuration. Native trust
// and SPKI distrust retain precedence.
void MergeBuiltinStoreCertificate(
    cert_verifier::mojom::AdditionalCertificates* certificates);
}  // namespace xenon::certificates

#endif  // XENON_OVERLAY_CHROME_BROWSER_MANAGED_CERTIFICATES_BUILTIN_STORE_CERTIFICATE_H_
