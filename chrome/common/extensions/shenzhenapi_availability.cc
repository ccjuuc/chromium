// Copyright 2026 The Shenzhen Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/common/extensions/shenzhenapi_availability.h"

#include <string>
#include <vector>
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/no_destructor.h"
#include "base/strings/string_util.h"
#include "base/synchronization/lock.h"
#include "extensions/common/context_data.h"
#include "extensions/common/extension.h"
#include "extensions/common/mojom/context_type.mojom.h"
#include "url/gurl.h"

namespace extensions::shenzhenapi_availability {

namespace {

// Thread-safe storage lock
base::Lock& GetAllowedDomainsLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

// In-memory list of whitelisted domains (defaults to *.baidu.com if not set)
std::vector<std::string>& GetAllowedDomainsStorage() {
  static base::NoDestructor<std::vector<std::string>> domains([]() {
    return std::vector<std::string>{"*.so.com"};
  }());
  return *domains;
}

// Checks if a given host matches a domain pattern (e.g. "*.baidu.com", "baidu.com", "*").
bool MatchDomainPattern(const std::string& host, const std::string& pattern) {
  std::string trimmed_pattern(base::TrimWhitespaceASCII(pattern, base::TRIM_ALL));
  if (trimmed_pattern == "*") {
    return true;
  }
  if (base::StartsWith(trimmed_pattern, "*.", base::CompareCase::INSENSITIVE_ASCII)) {
    std::string domain = trimmed_pattern.substr(2);
    return host == domain || base::EndsWith(host, "." + domain, base::CompareCase::INSENSITIVE_ASCII);
  }
  return base::EqualsCaseInsensitiveASCII(host, trimmed_pattern);
}

bool IsShenzhenApiAvailable(const std::string& api_full_name,
                            const extensions::Extension* extension,
                            extensions::mojom::ContextType context,
                            const GURL& url,
                            extensions::Feature::Platform platform,
                            int context_id,
                            bool check_developer_mode,
                            const extensions::ContextData& context_data) {
  if (context != extensions::mojom::ContextType::kWebPage) {
    return false;
  }

  // We check if it matches "shenzhen" namespace
  if (api_full_name != "shenzhen") {
    return false;
  }

  std::string host(url.host());
  for (const std::string& pattern : GetAllowedDomains()) {
    if (MatchDomainPattern(host, pattern)) {
      return true;
    }
  }

  return false;
}

}  // namespace

Feature::FeatureDelegatedAvailabilityCheckMap CreateAvailabilityCheckMap() {
  Feature::FeatureDelegatedAvailabilityCheckMap map;
  map.emplace("shenzhen", base::BindRepeating(&IsShenzhenApiAvailable));
  return map;
}

std::vector<std::string> GetAllowedDomains() {
  base::AutoLock lock(GetAllowedDomainsLock());
  return GetAllowedDomainsStorage();
}

void SetAllowedDomains(const std::vector<std::string>& domains) {
  base::AutoLock lock(GetAllowedDomainsLock());
  GetAllowedDomainsStorage() = domains;
}

}  // namespace extensions::shenzhenapi_availability
