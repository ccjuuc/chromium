// Copyright 2026 The Shenzhen Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_COMMON_EXTENSIONS_SHENZHENAPI_AVAILABILITY_H_
#define CHROME_COMMON_EXTENSIONS_SHENZHENAPI_AVAILABILITY_H_

#include <string>
#include <vector>
#include "extensions/common/features/feature.h"

namespace extensions::shenzhenapi_availability {

// Creates the availability check map for chrome.shenzhenapi API features.
Feature::FeatureDelegatedAvailabilityCheckMap CreateAvailabilityCheckMap();

// Process-local, in-memory getter and setter for allowed domains
// (thread-safe). Browser owns the source policy; Renderer receives a snapshot
// through chrome.mojom.RendererConfiguration.
std::vector<std::string> GetAllowedDomains();
void SetAllowedDomains(const std::vector<std::string>& domains);

}  // namespace extensions::shenzhenapi_availability

#endif  // CHROME_COMMON_EXTENSIONS_SHENZHENAPI_AVAILABILITY_H_
