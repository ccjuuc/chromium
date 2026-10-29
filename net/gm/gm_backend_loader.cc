// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/gm/gm_backend_loader.h"

#include "base/check_op.h"
#include "net/gm/gm_buildflags.h"

namespace net::gm {

const GmBackendApi* GetBackendApi() {
#if BUILDFLAG(XENON_GM)
  // Function-local initialization is thread safe. All entries belong to the
  // same binary and CFI graph as the caller; no filesystem lookup is involved.
  static const GmBackendApi api = [] {
    GmBackendApi result = {};
    result.struct_size = sizeof(result);
    CHECK_EQ(gm_backend_get_api(GM_BACKEND_ABI_VERSION, &result), GM_OK);
    return result;
  }();
  return &api;
#else
  return nullptr;
#endif
}

#if !BUILDFLAG(XENON_GM)
GmStatus CreateBackendConnection(GmContext*,
                                 const GmConnectionConfig*,
                                 base::span<const uint16_t>,
                                 GmConnection** out) {
  if (out) {
    *out = nullptr;
  }
  return GM_ERROR_UNSUPPORTED;
}
#endif

}  // namespace net::gm
