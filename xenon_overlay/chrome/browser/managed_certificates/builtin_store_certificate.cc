// Copyright 2026 The Xenon Overlay Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "xenon_overlay/chrome/browser/managed_certificates/builtin_store_certificate.h"

#include <string_view>
#include <utility>

#include "base/check.h"
#include "base/check_op.h"
#include "base/containers/span.h"
#include "base/no_destructor.h"
#include "base/strings/string_view_util.h"
#include "net/cert/x509_certificate.h"
#include "net/cert/x509_util.h"
#include "third_party/boringssl/src/pki/parsed_certificate.h"
#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_merger.h"
#include "xenon_overlay/chrome/browser/managed_certificates/managed_certificate_types.h"

namespace xenon::certificates {
namespace {

// Public certificate only. Keep in sync with
// tools/xl_extension_store/xl_store_root_ca.pem. Never load this from the build
// output, user profile, or the network: initial TLS verification needs it.
constexpr std::string_view kRootPem = R"pem(-----BEGIN CERTIFICATE-----
MIIDQDCCAiigAwIBAgIUQ1SKSb8t+wwGpNyvE3wald93LKQwDQYJKoZIhvcNAQEL
BQAwJjEkMCIGA1UEAwwbWEwgTG9jYWwgRXh0ZW5zaW9uIFN0b3JlIENBMB4XDTI2
MTEwNDAyNDU1MVoXDTM2MTEwMTAyNDU1MVowJjEkMCIGA1UEAwwbWEwgTG9jYWwg
RXh0ZW5zaW9uIFN0b3JlIENBMIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKC
AQEA4Hj0cAl01Gh+HORiVS8GpEYYFomZPyWtjlh2dRIPS3YFhrtYVbUiPqKlfb+k
D+Iru4JjHX+1UfKIDt/ejS30YRtr5sonZnZTh+apD569/QGy0EW/FVntNxOtb9ui
w6Yvd9r+ORW0WqIM5H1Vz+V4HVSSq7Kuce6CYNol2zbrRN12SO+Ifuyt1/7gb4lN
hgW0Xmn7htPN7ZUPqS1UlBG+UefucOnYCF6lp27ofIAvgOQ1mRrszTIoel2LGJVn
c1eBD0RwZZs1xUxHld0RTvQ8xDuAVWhsibo2DWDjdkTN+iEXsqZFjA055xYZSti6
8wPjCxQ6stdVGSSHN9IUTJtvQQIDAQABo2YwZDASBgNVHRMBAf8ECDAGAQH/AgEA
MA4GA1UdDwEB/wQEAwIBBjAdBgNVHQ4EFgQUOyBk4pMl+qWt9TaY9koM/I5iPLcw
HwYDVR0jBBgwFoAUOyBk4pMl+qWt9TaY9koM/I5iPLcwDQYJKoZIhvcNAQELBQAD
ggEBAAlDepLRt/jsU7aTuhMy8qK0/TPr2LLphRSz+T15SXHwKNBLAHFfO5uGsf/Z
IE2vAzzusg6ryk6IXvpOsAijdpz/8wjehNOvAI+P1JN9VchD0AfVpmS7VHoeQ9i+
4tq0D22ThqaPcVsj9Tn0OD1xZVnaB8S7iNvCVEN30I/zrq38RrigjkV6YGV8Ntao
hsdqp1eisr7qiF2WFgFfmAAP6amVG3nIaLh/m6CDXoGZdPOYKfBqIrBlxUKXWLDG
0837Ca6jHT6LSDkBJzERK84SISdkiKSyMMyIoWZISHiOFt1eGkGACV2CHg/S+5es
Sph+4O1x00yaKwxZuhj76yR8e7Y=
-----END CERTIFICATE-----
)pem";

Snapshot CreateBuiltinSnapshot() {
  const auto certificates = net::X509Certificate::CreateCertificateListFromBytes(
      base::as_byte_span(kRootPem), net::X509Certificate::FORMAT_PEM_CERT_SEQUENCE);
  CHECK_EQ(certificates.size(), 1u);
  auto parsed = bssl::ParsedCertificate::Create(
      net::x509_util::CreateCryptoBuffer(certificates[0]->cert_span()), {},
      nullptr);
  CHECK(parsed);
  CHECK(parsed->signature_algorithm());
  CHECK(parsed->has_basic_constraints());
  CHECK(parsed->basic_constraints().is_ca);
  CHECK(parsed->has_key_usage());
  CHECK(parsed->key_usage().AssertsBit(bssl::KEY_USAGE_BIT_KEY_CERT_SIGN));

  Certificate certificate;
  certificate.name = "XL Local Extension Store CA";
  const auto der = parsed->der_cert();
  certificate.der.assign(der.begin(), der.end());
  const auto spki = parsed->tbs().spki_tlv;
  certificate.spki.assign(spki.begin(), spki.end());
  CHECK_EQ(Sha256Hex(base::as_string_view(certificate.der)),
           "9365a8c6469624fc2c720439bd1c65dc56bc82bbd38b10b8813b37dc0c9d1da2");
  CHECK_EQ(Sha256Hex(base::as_string_view(certificate.spki)),
           "e685858a1f8beb98eeb4cc7b5a310d7aed0881d2f4385be1a1d9d6c501bacb51");
  Snapshot snapshot;
  snapshot.certificates.push_back(std::move(certificate));
  return snapshot;
}

const Snapshot& GetBuiltinSnapshot() {
  static const base::NoDestructor<Snapshot> snapshot(CreateBuiltinSnapshot());
  return *snapshot;
}

}  // namespace

const Certificate& GetBuiltinStoreCertificate() {
  return GetBuiltinSnapshot().certificates[0];
}

void MergeBuiltinStoreCertificate(
    cert_verifier::mojom::AdditionalCertificates* certificates) {
  MergeManagedCertificates(&GetBuiltinSnapshot(), certificates);
}

}  // namespace xenon::certificates
