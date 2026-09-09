// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_INSTALL_STATIC_LOCALIZED_PRODUCT_NAME_H_
#define CHROME_INSTALL_STATIC_LOCALIZED_PRODUCT_NAME_H_

#include <windows.h>

namespace install_static {

inline bool UseChineseProductName() {
  return PRIMARYLANGID(::GetUserDefaultUILanguage()) == LANG_CHINESE;
}

// Display names only; installation paths and registration IDs stay stable.
inline const wchar_t* LocalizedProductName(const wchar_t* fallback) {
#if defined(CUSTOM_CHROME_PRODUCT_NAME_W)
  return UseChineseProductName() ? L"\u8FC5\u96F7153" : L"xlb153";
#else
  return fallback;
#endif
}

}  // namespace install_static

#endif  // CHROME_INSTALL_STATIC_LOCALIZED_PRODUCT_NAME_H_
