// Copyright 2021 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/updater/external_constants_default.h"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "base/base64.h"
#include "base/check.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "base/values.h"
#include "build/branding_buildflags.h"
#include "build/build_config.h"
#include "chrome/updater/constants.h"
#include "chrome/updater/external_constants.h"
#include "chrome/updater/updater_branding.h"
#include "components/crx_file/crx_verifier.h"
#include "url/gurl.h"

namespace updater {
namespace {

std::optional<std::vector<uint8_t>> GetCrxPublicKeyHash() {
#if UPDATER_IS_XENON_BRANDED
  auto hash = base::Base64Decode(CRX_PKHASH);
  CHECK(hash && hash->size() == 32u)
      << "Xenon Updater requires a pinned SHA-256 CRX publisher key.";
  return hash;
#else
  if constexpr (std::string_view(CRX_PKHASH).empty()) {
    return std::nullopt;
  }
  return base::Base64Decode(CRX_PKHASH);
#endif
}

class DefaultExternalConstants : public ExternalConstants {
 public:
  DefaultExternalConstants() : ExternalConstants(nullptr) {}

  // Overrides of ExternalConstants:
  std::vector<GURL> UpdateURL() const override {
    const GURL url(UPDATE_CHECK_URL);
#if UPDATER_IS_XENON_BRANDED
    CHECK(url.is_valid() && !url.has_username() && !url.has_password() &&
          !url.has_ref());
    CHECK(url.SchemeIs("https") ||
          (XENON_UPDATER_ALLOW_INSECURE_LOCALHOST && url.SchemeIs("http") &&
           (url.host() == "localhost" || url.host() == "127.0.0.1" ||
            url.host() == "[::1]")));
#endif
    return std::vector<GURL>{url};
  }

  GURL CrashUploadURL() const override { return GURL(CRASH_UPLOAD_URL); }

  GURL AppLogoURL() const override { return GURL(APP_LOGO_URL); }

  GURL EventLoggingURL() const override {
    return GURL(UPDATER_EVENT_LOGGING_URL);
  }

  bool UseCUP() const override { return !UPDATER_IS_XENON_BRANDED; }

  base::TimeDelta InitialDelay() const override { return kInitialDelay; }

  base::TimeDelta ServerKeepAliveTime() const override {
    return kServerKeepAliveTime;
  }

  crx_file::VerifierFormat CrxVerifierFormat() const override {
#if UPDATER_IS_XENON_BRANDED
    // Xenon pins its own CRX signing key, not Google's publisher proof.
    return crx_file::VerifierFormat::CRX3;
#else
    return crx_file::VerifierFormat::CRX3_WITH_PUBLISHER_PROOF;
#endif
  }

  std::optional<std::vector<uint8_t>> CrxPublicKeyHash() const override {
    return GetCrxPublicKeyHash();
  }

  base::DictValue DictPolicies() const override { return base::DictValue(); }

  base::TimeDelta OverinstallTimeout() const override {
    return base::Minutes(2);
  }

  base::TimeDelta IdleCheckPeriod() const override { return base::Minutes(5); }

  std::optional<bool> IsMachineManaged() const override { return std::nullopt; }

  base::TimeDelta CecaConnectionTimeout() const override {
    return kCecaConnectionTimeout;
  }

  base::TimeDelta MinimumEventLoggingCooldown() const override {
    return kMinimumEventLoggingCooldown;
  }

  std::optional<EventLoggingPermissionProvider>
  GetEventLoggingPermissionProvider() const override {
#if BUILDFLAG(GOOGLE_CHROME_BRANDING) && \
    (BUILDFLAG(IS_WIN) || BUILDFLAG(IS_MAC))
    return EventLoggingPermissionProvider{
        .app_id = BROWSER_APPID,
#if BUILDFLAG(IS_MAC)
        .directory_name = BROWSER_NAME_STRING,
#endif
    };
#else
    return std::nullopt;
#endif
  }

 private:
  ~DefaultExternalConstants() override = default;
};

}  // namespace

scoped_refptr<ExternalConstants> CreateDefaultExternalConstants() {
  return base::MakeRefCounted<DefaultExternalConstants>();
}

}  // namespace updater
