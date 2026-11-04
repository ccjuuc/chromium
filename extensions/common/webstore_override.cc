// Copyright 2024 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "extensions/common/webstore_override.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "extensions/common/context_data.h"
#include "extensions/common/extension.h"
#include "extensions/common/extension_urls.h"
#include "extensions/common/features/feature.h"
#include "extensions/common/mojom/context_type.mojom.h"
#include "extensions/common/switches.h"
#include "url/gurl.h"
#include "url/origin.h"

namespace {

// The set of features which this delegated availability check should apply to,
// which correspond to the features used by the webstore.
constexpr static std::array<std::string_view, 2> kWebstoreOverrideFeatureList =
    {
        // LINT.IfChange
        "webstorePrivate", "management",
        // LINT.ThenChange(chrome/common/extensions/extension_test_util.cc)
};

// Returns true for the trusted XL store origin, or for a test store selected
// with the "apps-gallery-url" command line flag.
//
// The XL store check enforces an exact HTTPS origin because URL match patterns
// alone do not restrict ports. The override is used both for our own API tests
// and externally by the webstore team.
//
// Called by `SimpleFeature::IsAvailableToContextImpl()`, once that function
// determines that the feature has a "delegated availability check" and runs
// the check that was installed by `CreateAvailabilityCheckMap()` when the
// Extensions client was initialized.
bool AreWebstoreFeaturesAvailable(const std::string& api_full_name,
                                  const extensions::Extension* extension,
                                  extensions::mojom::ContextType context,
                                  const GURL& url,
                                  extensions::Feature::Platform platform,
                                  int context_id,
                                  bool check_developer_mode,
                                  const extensions::ContextData& context_data) {
  if (extension || context != extensions::mojom::ContextType::kWebPage ||
      !std::ranges::contains(kWebstoreOverrideFeatureList, api_full_name)) {
    return false;
  }

  if (extension_urls::IsXlWebstoreOrigin(url::Origin::Create(url))) {
    return true;
  }

  // We use a static local variable here to save on recreating it for repeated
  // calls. This is safe since the command line flags are constant for the
  // duration of the process.
  static base::NoDestructor<GURL> override_url([]() {
    std::string override_url_str =
        base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
            extensions::switches::kAppsGalleryURL);

    // Empty string means the command line switch was not used.
    if (override_url_str.empty()) {
      return GURL();
    }

    return GURL(override_url_str);
  }());

  if (!override_url->is_valid()) {
    // No override URL was specified. Early out.
    return false;
  }

  // We only consider the scheme, domain and port of the supplied override URL,
  // which must match the current URL.
  if (!url.SchemeIs(override_url->GetScheme())) {
    return false;
  }
  if (!url.DomainIs(override_url->GetHost())) {
    return false;
  }
  // We only compare port if one was supplied in the override URL, otherwise we
  // ignore it completely.
  if (override_url->has_port() && url.GetPort() != override_url->GetPort()) {
    return false;
  }
  return true;
}

}  // namespace

namespace extensions::webstore_override {

Feature::FeatureDelegatedAvailabilityCheckMap CreateAvailabilityCheckMap() {
  Feature::FeatureDelegatedAvailabilityCheckMap map;
  for (const auto item : kWebstoreOverrideFeatureList) {
    map.emplace(item, base::BindRepeating(&AreWebstoreFeaturesAvailable));
  }
  return map;
}

}  // namespace extensions::webstore_override
