// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Brand-specific constants and install modes for Chromium.

#include "chrome/install_static/chromium_install_modes.h"

#include <stdlib.h>

#include "chrome/install_static/install_modes.h"

namespace install_static {

const wchar_t kCompanyPathName[] = L"";

#if defined(CUSTOM_CHROME_PRODUCT_PATH_NAME_W)
const wchar_t kProductPathName[] = CUSTOM_CHROME_PRODUCT_PATH_NAME_W;
#else
const wchar_t kProductPathName[] = L"Chromium";
#endif

const size_t kProductPathNameLength = _countof(kProductPathName) - 1;

#if defined(CUSTOM_CHROME_SAFE_BROWSING_NAME)
const char kSafeBrowsingName[] = CUSTOM_CHROME_SAFE_BROWSING_NAME;
#else
const char kSafeBrowsingName[] = "chromium";
#endif

}  // namespace install_static
