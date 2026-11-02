// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_backend_loader.h"

// Test-only bridge for the independent Tongsuo peer executable. Production
// calls GetBackendApi directly and never loads this library.
extern "C" GmStatus gm_boringssl_test_get_api(uint32_t version,
                                              GmBackendApi* api) {
  if (!api || api->struct_size != sizeof(*api) ||
      version != GM_BACKEND_ABI_VERSION) {
    return GM_ERROR_ABI;
  }
  const GmBackendApi* implementation = net::gm::GetBackendApi();
  if (!implementation) {
    return GM_ERROR_UNSUPPORTED;
  }
  *api = *implementation;
  return GM_OK;
}
